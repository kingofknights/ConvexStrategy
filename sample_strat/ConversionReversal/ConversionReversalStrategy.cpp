#include "ConversionReversalStrategy.hpp"

#include "AlgoBase.hpp"
#include "MinixStrategy.hpp"
#include "Utils.hpp"
#include "oms_api.hpp"

#define FMT_HEADER_ONLY
#include <fmt/format.h>
#include <fmt/ostream.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

ConversionReversalStrategy::ConversionReversalStrategy(MinixStrategy* ms_, uint32_t strategyId_, const nlohmann::json& json_)
    : _ms(ms_), _strategyId(strategyId_) {
    char filename[128];
    std::snprintf(filename, sizeof(filename), "ConversionReversal_%u.log", _strategyId);
    _logFile = std::fopen(filename, "w");

    ParamUpdate(json_);
    _uid.composite_id_.client_id   = static_cast<uint32_t>(_ms->client);
    _uid.composite_id_.strategy_id = strategyId_;

    for (int token : _tokens) {
        if (token > 0) {
            _ms->subscribeProduct(token, _ms->flags);
        }
    }

    ProductDetails details[3];
    for (size_t i = 0; i < 3; ++i) {
        if (_tokens[i] > 0) {
            ms_->getProductDetails(_tokens[i], details[i]);
        }
    }

    _lotSize  = details[0].lot_size_ > 0 ? details[0].lot_size_ : 1;
    _tickSize = details[0].tick_size_ > 0 ? details[0].tick_size_ : 5;

    // Long Side (Conversion): Short CE, Long PE, Long Fut
    // Short Side (Reversion):  Long CE, Short PE, Short Fut
    ORDER_SIDE longSide[3]  = {SELL_SIDE, BUY_SIDE, BUY_SIDE};
    ORDER_SIDE shortSide[3] = {BUY_SIDE, SELL_SIDE, SELL_SIDE};

    for (size_t i = 0; i < 3; ++i) {
        _longOrders._order[i]  = std::make_unique<OrderObjectT>(_tokens[i], longSide[i], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
        _shortOrders._order[i] = std::make_unique<OrderObjectT>(_tokens[i], shortSide[i], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    }
}

ConversionReversalStrategy::~ConversionReversalStrategy() {
    if (_logFile) {
        std::fclose(_logFile);
    }
}

void ConversionReversalStrategy::ParamUpdate(const nlohmann::json& json_) {
    if (json_.contains("Params")) {
        const auto& parmas = json_["Params"];

        _orderStrike = parmas.value("OrderStrike", 0);

        _longParam._quantity      = parmas.value("LongBuySoQ", parmas.value("OrderLot", 0));
        _longParam._totalQuantity = parmas.value("LongBuyQty", parmas.value("TotalLot", 0));
        _longParam._spread        = parmas.value("LongBuyPrice", 0.0F) * 100.0F;

        _shortParam._quantity      = parmas.value("ShortSellSoQ", parmas.value("OrderLot", 0));
        _shortParam._totalQuantity = parmas.value("ShortSellQty", parmas.value("TotalLot", 0));
        _shortParam._spread        = parmas.value("ShortSellPrice", 0.0F) * 100.0F;

        _marketOrderRetries = parmas.value("MarketOrderRetries", 0U);
    }

    if (json_.contains("Legs")) {
        const auto& legs = json_["Legs"];
        for (const auto& item : legs) {
            std::string optionType = item.value("OptionType", "");
            int token              = item.value("Token", 0);
            if (optionType == "CE" || optionType == "CALL") {
                _tokens[0] = token;
            } else if (optionType == "PE" || optionType == "PUT") {
                _tokens[1] = token;
            } else {
                _tokens[2] = token; // Future
            }
        }
    }

    if (json_.contains("Strategy")) {
        const auto& strategy = json_["Strategy"];
        _isBidding           = strategy.value("IsBidding", false);
        std::string status   = strategy.value("Status", "None");
        _active              = status == "Applied";
    }
}

void ConversionReversalStrategy::OnTick(const Quote& event_, int64_t nowTs_) {
    int  token  = event_.header.product_id;
    bool status = false;
    size_t index = 0;
    for (size_t i = 0; i < 3; ++i) {
        if (token == _tokens[i]) {
            status = true;
            index  = i;
            break;
        }
    }
    if (!status) {
        return;
    }
    _qoute[index] = event_;
    if (!_active) {
        return;
    }

    // Long Bidding = Conversion spread
    WindRate longRate = GetBCmp();
    if (longRate._valid) {
        OrderBiddingLogic(_longOrders, _longParam, longRate, "LongConversion");
    } else {
        size_t biddingLeg = (_qoute[2].message.ltp_ > _orderStrike) ? 0 : 1;
        _longOrders._order[biddingLeg]->cancel_order();
    }

    // Short Bidding = Reversion spread
    WindRate shortRate = GetSCmp();
    if (shortRate._valid) {
        OrderBiddingLogic(_shortOrders, _shortParam, shortRate, "ShortReversion");
    } else {
        size_t biddingLeg = (_qoute[2].message.ltp_ > _orderStrike) ? 0 : 1;
        _shortOrders._order[biddingLeg]->cancel_order();
    }
}

void ConversionReversalStrategy::OnBcast(const aef::infra::product::product_data& pd_, int64_t nowTs_) {
}

void ConversionReversalStrategy::OnOrderResponse(const oms_transaction& resp_) {
    if (_strategyId != resp_.hdr_.uid_.composite_id_.strategy_id) {
        return;
    }
    _ms->sendOrderResponse(resp_, "ConversionReversal");
    bool traded   = resp_.hdr_.transaction_code == OMS_TRADE;
    auto price    = resp_.packet_.price_;
    auto quantity = resp_.packet_.quantity_;
    auto lot      = quantity / _lotSize;
    auto value    = static_cast<uint64_t>(price * quantity);

    size_t biddingLeg = (_qoute[2].message.ltp_ > _orderStrike) ? 0 : 1;

    auto processResponse = [&](MarketBidding& object_) -> bool {
        for (size_t i = 0; i < 3; ++i) {
            if (resp_.hdr_.uid_.id_ == object_._uniqueID[i]) {
                object_._order[i]->handle_confirmation(resp_);
                object_._tradedLot[i] += traded ? lot : 0;
                object_._tradeValue[i] += traded ? value : 0;
                if (i == biddingLeg && traded) {
                    object_._lastBiddingFillPrice = price;
                }
                return true;
            }
        }
        return false;
    };

    processResponse(_longOrders);
    processResponse(_shortOrders);

    if (traded) {
        SecondOrderBidding(_longOrders, _longParam);
        SecondOrderBidding(_shortOrders, _shortParam);
    }
}

// ── Pure Spread Calculations from con_rev_BIDDING.docx ─────────────────────

// Conversion bid in Call (Fut_LTP > Order_Strike)
auto ConversionReversalStrategy::CalculateConversionCallWindRate() const -> WindRate {
    int ceAsk  = _qoute[0].message.ask_levels[0].price;
    int peAsk  = _qoute[1].message.ask_levels[0].price;
    int futAsk = _qoute[2].message.ask_levels[0].price;

    if (ceAsk <= 0 || peAsk <= 0 || futAsk <= 0) {
        return WindRate{._biddingPrice = 0, ._windRate = 0.0f, ._valid = false};
    }

    float windRate     = static_cast<float>(ceAsk - peAsk - futAsk - _tickSize + _orderStrike);
    int   biddingPrice = ceAsk - _tickSize;
    return WindRate{._biddingPrice = biddingPrice, ._windRate = windRate, ._valid = true};
}

// Reversion bid in Call (Fut_LTP > Order_Strike)
auto ConversionReversalStrategy::CalculateReversionCallWindRate() const -> WindRate {
    int ceBid  = _qoute[0].message.bid_levels[0].price;
    int peBid  = _qoute[1].message.bid_levels[0].price;
    int futBid = _qoute[2].message.bid_levels[0].price;

    if (ceBid <= 0 || peBid <= 0 || futBid <= 0) {
        return WindRate{._biddingPrice = 0, ._windRate = 0.0f, ._valid = false};
    }

    float windRate     = static_cast<float>(-ceBid + peBid + futBid - _orderStrike - _tickSize);
    int   biddingPrice = ceBid + _tickSize;
    return WindRate{._biddingPrice = biddingPrice, ._windRate = windRate, ._valid = true};
}

// Conversion bid in Put (Fut_LTP <= Order_Strike)
auto ConversionReversalStrategy::CalculateConversionPutWindRate() const -> WindRate {
    int peBid  = _qoute[1].message.bid_levels[0].price;
    int ceBid  = _qoute[0].message.bid_levels[0].price;
    int futAsk = _qoute[2].message.ask_levels[0].price;

    if (peBid <= 0 || ceBid <= 0 || futAsk <= 0) {
        return WindRate{._biddingPrice = 0, ._windRate = 0.0f, ._valid = false};
    }

    float windRate     = static_cast<float>(ceBid - peBid - futAsk + _orderStrike - _tickSize);
    int   biddingPrice = peBid + _tickSize;
    return WindRate{._biddingPrice = biddingPrice, ._windRate = windRate, ._valid = true};
}

// Reversion bid in Put (Fut_LTP <= Order_Strike)
auto ConversionReversalStrategy::CalculateReversionPutWindRate() const -> WindRate {
    int peAsk  = _qoute[1].message.ask_levels[0].price;
    int ceAsk  = _qoute[0].message.ask_levels[0].price;
    int futBid = _qoute[2].message.bid_levels[0].price;

    if (peAsk <= 0 || ceAsk <= 0 || futBid <= 0) {
        return WindRate{._biddingPrice = 0, ._windRate = 0.0f, ._valid = false};
    }

    float windRate     = static_cast<float>(peAsk - ceAsk + futBid - _orderStrike - _tickSize);
    int   biddingPrice = peAsk - _tickSize;
    return WindRate{._biddingPrice = biddingPrice, ._windRate = windRate, ._valid = true};
}

auto ConversionReversalStrategy::GetBCmp() const -> WindRate {
    int futLtp = _qoute[2].message.ltp_;
    return (futLtp > _orderStrike) ? CalculateConversionCallWindRate() : CalculateConversionPutWindRate();
}

auto ConversionReversalStrategy::GetSCmp() const -> WindRate {
    int futLtp = _qoute[2].message.ltp_;
    return (futLtp > _orderStrike) ? CalculateReversionCallWindRate() : CalculateReversionPutWindRate();
}

auto ConversionReversalStrategy::GetStrategyID() const -> uint32_t { return _strategyId; }
auto ConversionReversalStrategy::GetGap() const -> int { return _gap; }

auto ConversionReversalStrategy::GetBuyTradedQuantity() const -> int {
    return std::min({_longOrders._tradedLot[0], _longOrders._tradedLot[1], _longOrders._tradedLot[2]});
}
auto ConversionReversalStrategy::GetSellTradedQuantity() const -> int {
    return std::min({_shortOrders._tradedLot[0], _shortOrders._tradedLot[1], _shortOrders._tradedLot[2]});
}

auto ConversionReversalStrategy::GetBATP() const -> double {
    if (_longOrders._tradedLot[0] == 0 || _longOrders._tradedLot[1] == 0 || _longOrders._tradedLot[2] == 0 || _lotSize == 0) return 0.0;
    double pCe  = static_cast<double>(_longOrders._tradeValue[0]) / static_cast<double>(_longOrders._tradedLot[0] * _lotSize);
    double pPe  = static_cast<double>(_longOrders._tradeValue[1]) / static_cast<double>(_longOrders._tradedLot[1] * _lotSize);
    double pFut = static_cast<double>(_longOrders._tradeValue[2]) / static_cast<double>(_longOrders._tradedLot[2] * _lotSize);
    return (pFut + pPe - pCe - _orderStrike);
}

auto ConversionReversalStrategy::GetSATP() const -> double {
    if (_shortOrders._tradedLot[0] == 0 || _shortOrders._tradedLot[1] == 0 || _shortOrders._tradedLot[2] == 0 || _lotSize == 0) return 0.0;
    double pCe  = static_cast<double>(_shortOrders._tradeValue[0]) / static_cast<double>(_shortOrders._tradedLot[0] * _lotSize);
    double pPe  = static_cast<double>(_shortOrders._tradeValue[1]) / static_cast<double>(_shortOrders._tradedLot[1] * _lotSize);
    double pFut = static_cast<double>(_shortOrders._tradeValue[2]) / static_cast<double>(_shortOrders._tradedLot[2] * _lotSize);
    return (pCe - pPe - pFut + _orderStrike);
}

auto ConversionReversalStrategy::GetRLP() const -> double {
    int buyLots  = GetBuyTradedQuantity();
    int sellLots = GetSellTradedQuantity();
    return static_cast<double>(std::min(buyLots, sellLots)) * (GetSATP() - GetBATP()) * static_cast<double>(_lotSize);
}

auto ConversionReversalStrategy::GetCutPL() const -> double { return GetRLP(); }

auto ConversionReversalStrategy::GetM2M() const -> int {
    int buyLots  = GetBuyTradedQuantity();
    int sellLots = GetSellTradedQuantity();
    double m2m   = 0.0;
    if (buyLots > sellLots) {
        m2m = static_cast<double>(buyLots - sellLots) * (GetBCmp()._windRate - GetBATP()) * static_cast<double>(_lotSize);
    } else if (sellLots > buyLots) {
        m2m = static_cast<double>(sellLots - buyLots) * (GetSATP() - GetSCmp()._windRate) * static_cast<double>(_lotSize);
    }
    return static_cast<int>(m2m);
}

auto ConversionReversalStrategy::GetNetPL() const -> double { return GetRLP() + static_cast<double>(GetM2M()); }
auto ConversionReversalStrategy::GetFLP() const -> int { return 0; }
auto ConversionReversalStrategy::GetCost() const -> double { return 0.0; }

void ConversionReversalStrategy::OrderBiddingLogic(MarketBidding& object_, ParamLots param_, WindRate rate_, std::string name_) {
    size_t biddingLeg = (_qoute[2].message.ltp_ > _orderStrike) ? 0 : 1; // 0 = CE, 1 = PE

    int biddingLots = object_._tradedLot[biddingLeg];
    for (size_t h = 0; h < 3; ++h) {
        if (h == biddingLeg) continue;
        if (biddingLots != object_._tradedLot[h]) {
            SecondOrderBidding(object_, param_);
            return;
        }
    }

    if (biddingLots >= param_._totalQuantity) {
        object_._order[biddingLeg]->cancel_order();
        return;
    }

    OrderObjectPtrT& order             = object_._order[biddingLeg];
    int              marketPrice       = rate_._biddingPrice;
    int              currentPlacePrice = order->get_open_price();

    if (marketPrice > 0 && marketPrice != currentPlacePrice) {
        int quantity = param_._quantity * _lotSize;
        auto status  = _ms->update_order(order, _tokens[biddingLeg], marketPrice, quantity, _uid);
        if (status != 0) {
            object_._uniqueID[biddingLeg] = _uid.id_;
            object_._windRate             = rate_;
        }
    }
}

void ConversionReversalStrategy::SecondOrderBidding(MarketBidding& object_, ParamLots param_) {
    size_t biddingLeg = (_qoute[2].message.ltp_ > _orderStrike) ? 0 : 1;
    int    biddingLot = object_._tradedLot[biddingLeg];

    for (size_t leg = 0; leg < 3; ++leg) {
        if (leg == biddingLeg) continue;
        int hedgeLot = object_._tradedLot[leg];
        int diff     = biddingLot - hedgeLot;
        if (diff <= 0) {
            object_._hedgeRetryCount = 0;
            continue;
        }

        if (_marketOrderRetries > 0 && object_._hedgeRetryCount >= _marketOrderRetries) {
            writeLog("[HEDGE RETRY EXHAUSTED ConRev] Terminating strategy & cancelling all orders.\n");
            _active = false;
            for (size_t i = 0; i < 3; ++i) {
                _longOrders._order[i]->cancel_order();
                _shortOrders._order[i]->cancel_order();
            }
            return;
        }

        int              quantity          = std::min(diff, param_._quantity) * _lotSize;
        OrderObjectPtrT& order             = object_._order[leg];
        int              currentPlacePrice = order->get_open_price();

        ORDER_SIDE hedgeMarketSide = order->get_side() == BUY_SIDE ? SELL_SIDE : BUY_SIDE;
        int        marketPrice     = GetPrice(_qoute[leg], hedgeMarketSide, 0);

        if (marketPrice > 0 && marketPrice != currentPlacePrice) {
            auto status = _ms->update_order(order, _tokens[leg], marketPrice, quantity, _uid);
            if (status != 0) {
                object_._uniqueID[leg] = _uid.id_;
                object_._hedgeRetryCount++;
            }
        }
    }
}

[[nodiscard]] auto ConversionReversalStrategy::GetPrice(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].price : event_.message.ask_levels[index_].price;
}
[[nodiscard]] auto ConversionReversalStrategy::GetQuantity(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].qty : event_.message.ask_levels[index_].qty;
}
[[nodiscard]] auto ConversionReversalStrategy::GetOrderCount(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].order_count_ : event_.message.ask_levels[index_].order_count_;
}
