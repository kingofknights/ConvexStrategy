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

    // Default sides:
    // Conversion: Long Fut (+), Long PE (+), Short CE (-)
    // Reversion:  Short Fut (-), Short PE (-), Long CE (+)
    ORDER_SIDE ceSide  = (_stratType == StrategyType::CONVERSION) ? SELL_SIDE : BUY_SIDE;
    ORDER_SIDE peSide  = (_stratType == StrategyType::CONVERSION) ? BUY_SIDE : SELL_SIDE;
    ORDER_SIDE futSide = (_stratType == StrategyType::CONVERSION) ? BUY_SIDE : SELL_SIDE;

    _biddingOrders._order[0] = std::make_unique<OrderObjectT>(_tokens[0], ceSide, _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    _biddingOrders._order[1] = std::make_unique<OrderObjectT>(_tokens[1], peSide, _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    _biddingOrders._order[2] = std::make_unique<OrderObjectT>(_tokens[2], futSide, _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
}

void ConversionReversalStrategy::ParamUpdate(const nlohmann::json& json_) {
    if (json_.contains("Params")) {
        const auto& parmas = json_["Params"];

        std::string typeStr = parmas.value("StrategyType", "Conversion");
        _stratType = (typeStr == "Reversion" || typeStr == "reversion") ? StrategyType::REVERSION : StrategyType::CONVERSION;

        _orderStrike        = parmas.value("OrderStrike", 0);
        _quantity           = parmas.value("OrderLot", 0);
        _totalQuantity      = parmas.value("TotalLot", 0);
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

    WindRate activeRate = GetBCmp();
    if (activeRate._valid) {
        OrderBiddingLogic(_biddingOrders, activeRate, "ConRev");
    } else {
        size_t biddingLeg = (_qoute[2].message.ltp_ > _orderStrike) ? 0 : 1;
        _biddingOrders._order[biddingLeg]->cancel_order();
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

    for (size_t i = 0; i < 3; ++i) {
        if (resp_.hdr_.uid_.id_ == _biddingOrders._uniqueID[i]) {
            _biddingOrders._order[i]->handle_confirmation(resp_);
            _biddingOrders._tradedLot[i] += traded ? lot : 0;
            _biddingOrders._tradeValue[i] += traded ? value : 0;
            if (i == biddingLeg && traded) {
                _biddingOrders._lastBiddingFillPrice = price;
            }
            break;
        }
    }

    if (traded) {
        SecondOrderBidding(_biddingOrders);
    }
}

// ── Pure Spread Calculation Functions from con_rev_BIDDING.docx ────────────

// 1. Conversion bid in Call (Fut_LTP > Order_Strike)
// Cur_wind_rate = order_ce_ask - order_pe_ask - future_ask - 1tick + strike_pr
// FIRST PRICE   = order_ce_ask - 1tick
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

// 2. Reversion bid in Call (Fut_LTP > Order_Strike)
// Cur_wind_rate = - order_ce_bid + order_pe_bid + future_bid - strike_pr - 1tick
// BIDDING PRICE = order_ce_bid + 1tick
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

// 3. Conversion bid in Put (Fut_LTP <= Order_Strike)
// Cur_wind_rate = order_ce_bid - order_pe_bid - future_ask + strike_pr - 1tick
// BIDDING PRICE = order_pe_bid + 1tick
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

// 4. Reversion bid in Put (Fut_LTP <= Order_Strike)
// Cur_wind_rate = order_pe_ask - order_ce_ask + future_bid - strike_pr - 1tick
// FIRST PRICE   = order_pe_ask - 1tick
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
    if (futLtp > _orderStrike) {
        // Fut_LTP > Order_Strike -> Order in Call
        return (_stratType == StrategyType::CONVERSION) 
            ? CalculateConversionCallWindRate() 
            : CalculateReversionCallWindRate();
    } else {
        // Fut_LTP <= Order_Strike -> Order in Put
        return (_stratType == StrategyType::CONVERSION) 
            ? CalculateConversionPutWindRate() 
            : CalculateReversionPutWindRate();
    }
}

auto ConversionReversalStrategy::GetSCmp() const -> WindRate {
    return GetBCmp();
}

auto ConversionReversalStrategy::GetStrategyID() const -> uint32_t { return _strategyId; }
auto ConversionReversalStrategy::GetGap() const -> int { return _gap; }

auto ConversionReversalStrategy::GetBuyTradedQuantity() const -> int {
    return std::min({_biddingOrders._tradedLot[0], _biddingOrders._tradedLot[1], _biddingOrders._tradedLot[2]});
}
auto ConversionReversalStrategy::GetSellTradedQuantity() const -> int {
    return GetBuyTradedQuantity();
}

auto ConversionReversalStrategy::GetBATP() const -> double {
    if (_biddingOrders._tradedLot[0] == 0 || _biddingOrders._tradedLot[1] == 0 || _biddingOrders._tradedLot[2] == 0 || _lotSize == 0) return 0.0;
    double pCe  = static_cast<double>(_biddingOrders._tradeValue[0]) / static_cast<double>(_biddingOrders._tradedLot[0] * _lotSize);
    double pPe  = static_cast<double>(_biddingOrders._tradeValue[1]) / static_cast<double>(_biddingOrders._tradedLot[1] * _lotSize);
    double pFut = static_cast<double>(_biddingOrders._tradeValue[2]) / static_cast<double>(_biddingOrders._tradedLot[2] * _lotSize);
    return (_stratType == StrategyType::CONVERSION) ? (pFut + pPe - pCe - _orderStrike) : (pCe - pPe - pFut + _orderStrike);
}

auto ConversionReversalStrategy::GetSATP() const -> double { return GetBATP(); }

auto ConversionReversalStrategy::GetRLP() const -> double {
    int matchedLots = GetBuyTradedQuantity();
    return GetBATP() * static_cast<double>(matchedLots * _lotSize);
}

auto ConversionReversalStrategy::GetCutPL() const -> double { return GetRLP(); }

auto ConversionReversalStrategy::GetM2M() const -> int {
    int openLots = _biddingOrders._tradedLot[0] - GetBuyTradedQuantity();
    if (openLots <= 0) return 0;
    double currentSpread = static_cast<double>(GetBCmp()._windRate);
    return static_cast<int>((currentSpread - GetBATP()) * static_cast<double>(openLots * _lotSize));
}

auto ConversionReversalStrategy::GetNetPL() const -> double { return GetRLP() + static_cast<double>(GetM2M()); }
auto ConversionReversalStrategy::GetFLP() const -> int { return 0; }
auto ConversionReversalStrategy::GetCost() const -> double { return 0.0; }

void ConversionReversalStrategy::OrderBiddingLogic(MarketBidding& object_, WindRate rate_, std::string name_) {
    size_t biddingLeg = (_qoute[2].message.ltp_ > _orderStrike) ? 0 : 1; // 0 = CE, 1 = PE

    if (object_._tradedLot[biddingLeg] >= _totalQuantity) {
        object_._order[biddingLeg]->cancel_order();
        return;
    }

    OrderObjectPtrT& order             = object_._order[biddingLeg];
    int              marketPrice       = rate_._biddingPrice;
    int              currentPlacePrice = order->get_open_price();

    if (marketPrice > 0 && marketPrice != currentPlacePrice) {
        int quantity = _quantity * _lotSize;
        auto status  = _ms->update_order(order, _tokens[biddingLeg], marketPrice, quantity, _uid);
        if (status != 0) {
            object_._uniqueID[biddingLeg] = _uid.id_;
            object_._windRate             = rate_;
        }
    }
}

void ConversionReversalStrategy::SecondOrderBidding(MarketBidding& object_) {
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
            fmt::print("[HEDGE RETRY EXHAUSTED ConRev] Terminating strategy & cancelling all orders.\n");
            _active = false;
            for (size_t i = 0; i < 3; ++i) {
                _biddingOrders._order[i]->cancel_order();
            }
            return;
        }

        int              quantity          = std::min(diff, _quantity) * _lotSize;
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
