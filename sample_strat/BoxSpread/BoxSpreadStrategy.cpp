#include "BoxSpreadStrategy.hpp"

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

BoxSpreadStrategy::BoxSpreadStrategy(MinixStrategy* ms_, uint32_t strategyId_, const nlohmann::json& json_)
    : _ms(ms_), _strategyId(strategyId_) {
    ParamUpdate(json_);
    _uid.composite_id_.client_id   = static_cast<uint32_t>(_ms->client);
    _uid.composite_id_.strategy_id = strategyId_;

    for (int token : _tokens) {
        if (token > 0) {
            _ms->subscribeProduct(token, _ms->flags);
        }
    }

    ProductDetails details[4];
    for (size_t i = 0; i < 4; ++i) {
        if (_tokens[i] > 0) {
            ms_->getProductDetails(_tokens[i], details[i]);
        }
    }

    _lotSize  = details[0].lot_size_ > 0 ? details[0].lot_size_ : 1;
    _tickSize = details[0].tick_size_ > 0 ? details[0].tick_size_ : 5;

    // Long Box (Conversion): Buy Order_CE, Sell Order_PE, Sell ATM_CE, Buy ATM_PE
    // Short Box (Reversion):  Sell Order_CE, Buy Order_PE, Buy ATM_CE, Sell ATM_PE
    ORDER_SIDE longSide[4]  = {BUY_SIDE, SELL_SIDE, SELL_SIDE, BUY_SIDE};
    ORDER_SIDE shortSide[4] = {SELL_SIDE, BUY_SIDE, BUY_SIDE, SELL_SIDE};

    for (size_t i = 0; i < 4; ++i) {
        _longOrders._order[i]  = std::make_unique<OrderObjectT>(_tokens[i], longSide[i], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
        _shortOrders._order[i] = std::make_unique<OrderObjectT>(_tokens[i], shortSide[i], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    }
}

BoxSpreadStrategy::~BoxSpreadStrategy() {}

void BoxSpreadStrategy::ParamUpdate(const nlohmann::json& json_) {
    if (json_.contains("Params")) {
        const auto& parmas = json_["Params"];

        _orderStrike = parmas.value("OrderStrike", 0);
        _atmStrike   = parmas.value("AtmStrike", 0);

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
            int strike             = item.value("Strike", 0);
            
            if (optionType == "CE" || optionType == "CALL") {
                if (strike == _orderStrike || _tokens[0] == 0) {
                    _tokens[0] = token; // Order_CE
                } else {
                    _tokens[2] = token; // ATM_CE
                }
            } else if (optionType == "PE" || optionType == "PUT") {
                if (strike == _orderStrike || _tokens[1] == 0) {
                    _tokens[1] = token; // Order_PE
                } else {
                    _tokens[3] = token; // ATM_PE
                }
            } else {
                _tokens[4] = token; // Future (for ITM determination)
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

void BoxSpreadStrategy::OnTick(const Quote& event_, int64_t nowTs_) {
    int  token  = event_.header.product_id;
    bool status = false;
    size_t index = 0;
    for (size_t i = 0; i < 5; ++i) {
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

    // Long Bidding = Conversion Box
    WindRate longRate = GetBCmp();
    if (longRate._valid) {
        OrderBiddingLogic(_longOrders, _longParam, longRate, "LongBoxConversion");
    } else {
        size_t biddingLeg = (_qoute[4].message.ltp_ > _orderStrike) ? 0 : 1;
        _longOrders._order[biddingLeg]->cancel_order();
    }

    // Short Bidding = Reversion Box
    WindRate shortRate = GetSCmp();
    if (shortRate._valid) {
        OrderBiddingLogic(_shortOrders, _shortParam, shortRate, "ShortBoxReversion");
    } else {
        size_t biddingLeg = (_qoute[4].message.ltp_ > _orderStrike) ? 0 : 1;
        _shortOrders._order[biddingLeg]->cancel_order();
    }
}

void BoxSpreadStrategy::OnBcast(const aef::infra::product::product_data& pd_, int64_t nowTs_) {
}

void BoxSpreadStrategy::OnOrderResponse(const oms_transaction& resp_) {
    if (_strategyId != resp_.hdr_.uid_.composite_id_.strategy_id) {
        return;
    }
    _ms->sendOrderResponse(resp_, "BoxSpread");
    bool traded   = resp_.hdr_.transaction_code == OMS_TRADE;
    auto price    = resp_.packet_.price_;
    auto quantity = resp_.packet_.quantity_;
    auto lot      = quantity / _lotSize;
    auto value    = static_cast<uint64_t>(price * quantity);

    size_t biddingLeg = (_qoute[4].message.ltp_ > _orderStrike) ? 0 : 1;

    auto processResponse = [&](MarketBidding& object_) -> bool {
        for (size_t i = 0; i < 4; ++i) {
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

// ── Spread Calculations directly from Documents.md ────────────────────────

// Conversion bid in Call (Fut_LTP > Order_Strike)
auto BoxSpreadStrategy::CalculateConversionCallWindRate() const -> WindRate {
    int orderCeAsk = _qoute[0].message.ask_levels[0].price;
    int orderPeAsk = _qoute[1].message.ask_levels[0].price;
    int atmCeAsk   = _qoute[2].message.ask_levels[0].price;
    int atmPeBid   = _qoute[3].message.bid_levels[0].price;

    if (orderCeAsk <= 0 || orderPeAsk <= 0 || atmCeAsk <= 0 || atmPeBid <= 0) {
        return WindRate{._biddingPrice = 0, ._windRate = 0.0f, ._valid = false};
    }

    float windRate     = static_cast<float>(orderCeAsk - orderPeAsk - atmCeAsk + atmPeBid + _orderStrike - _atmStrike - _tickSize);
    int   biddingPrice = orderCeAsk - _tickSize;
    return WindRate{._biddingPrice = biddingPrice, ._windRate = windRate, ._valid = true};
}

// Reversion bid in Call (Fut_LTP > Order_Strike)
auto BoxSpreadStrategy::CalculateReversionCallWindRate() const -> WindRate {
    int orderCeBid = _qoute[0].message.bid_levels[0].price;
    int orderPeBid = _qoute[1].message.bid_levels[0].price;
    int atmCeBid   = _qoute[2].message.bid_levels[0].price;
    int atmPeAsk   = _qoute[3].message.ask_levels[0].price;

    if (orderCeBid <= 0 || orderPeBid <= 0 || atmCeBid <= 0 || atmPeAsk <= 0) {
        return WindRate{._biddingPrice = 0, ._windRate = 0.0f, ._valid = false};
    }

    float windRate     = static_cast<float>(-orderCeBid + orderPeBid + atmCeBid - atmPeAsk - _orderStrike + _atmStrike - _tickSize);
    int   biddingPrice = orderCeBid + _tickSize;
    return WindRate{._biddingPrice = biddingPrice, ._windRate = windRate, ._valid = true};
}

// Conversion bid in Put (Fut_LTP <= Order_Strike)
auto BoxSpreadStrategy::CalculateConversionPutWindRate() const -> WindRate {
    int orderCeBid = _qoute[0].message.bid_levels[0].price;
    int orderPeBid = _qoute[1].message.bid_levels[0].price;
    int atmCeAsk   = _qoute[2].message.ask_levels[0].price;
    int atmPeBid   = _qoute[3].message.bid_levels[0].price;

    if (orderCeBid <= 0 || orderPeBid <= 0 || atmCeAsk <= 0 || atmPeBid <= 0) {
        return WindRate{._biddingPrice = 0, ._windRate = 0.0f, ._valid = false};
    }

    float windRate     = static_cast<float>(orderCeBid - orderPeBid - atmCeAsk + atmPeBid + _orderStrike - _atmStrike - _tickSize);
    int   biddingPrice = orderPeBid + _tickSize;
    return WindRate{._biddingPrice = biddingPrice, ._windRate = windRate, ._valid = true};
}

// Reversion bid in Put (Fut_LTP <= Order_Strike)
auto BoxSpreadStrategy::CalculateReversionPutWindRate() const -> WindRate {
    int orderPeAsk = _qoute[1].message.ask_levels[0].price;
    int orderCeAsk = _qoute[0].message.ask_levels[0].price;
    int atmCeBid   = _qoute[2].message.bid_levels[0].price;
    int atmPeAsk   = _qoute[3].message.ask_levels[0].price;

    if (orderPeAsk <= 0 || orderCeAsk <= 0 || atmCeBid <= 0 || atmPeAsk <= 0) {
        return WindRate{._biddingPrice = 0, ._windRate = 0.0f, ._valid = false};
    }

    float windRate     = static_cast<float>(orderPeAsk - orderCeAsk + atmCeBid - atmPeAsk - _orderStrike + _atmStrike - _tickSize);
    int   biddingPrice = orderPeAsk - _tickSize;
    return WindRate{._biddingPrice = biddingPrice, ._windRate = windRate, ._valid = true};
}

auto BoxSpreadStrategy::GetBCmp() const -> WindRate {
    int futLtp = _qoute[4].message.ltp_;
    return (futLtp > _orderStrike) ? CalculateConversionCallWindRate() : CalculateConversionPutWindRate();
}

auto BoxSpreadStrategy::GetSCmp() const -> WindRate {
    int futLtp = _qoute[4].message.ltp_;
    return (futLtp > _orderStrike) ? CalculateReversionCallWindRate() : CalculateReversionPutWindRate();
}

auto BoxSpreadStrategy::GetStrategyID() const -> uint32_t { return _strategyId; }
auto BoxSpreadStrategy::GetGap() const -> int { return _gap; }

auto BoxSpreadStrategy::GetLongTradedLots() const -> int {
    return std::min({_longOrders._tradedLot[0], _longOrders._tradedLot[1], _longOrders._tradedLot[2], _longOrders._tradedLot[3]});
}
auto BoxSpreadStrategy::GetShortTradedLots() const -> int {
    return std::min({_shortOrders._tradedLot[0], _shortOrders._tradedLot[1], _shortOrders._tradedLot[2], _shortOrders._tradedLot[3]});
}

auto BoxSpreadStrategy::GetBATP() const -> double {
    if (_longOrders._tradedLot[0] == 0 || _longOrders._tradedLot[1] == 0 || _longOrders._tradedLot[2] == 0 || _longOrders._tradedLot[3] == 0 || _lotSize == 0) return 0.0;
    double pOrderCe = static_cast<double>(_longOrders._tradeValue[0]) / static_cast<double>(_longOrders._tradedLot[0] * _lotSize);
    double pOrderPe = static_cast<double>(_longOrders._tradeValue[1]) / static_cast<double>(_longOrders._tradedLot[1] * _lotSize);
    double pAtmCe   = static_cast<double>(_longOrders._tradeValue[2]) / static_cast<double>(_longOrders._tradedLot[2] * _lotSize);
    double pAtmPe   = static_cast<double>(_longOrders._tradeValue[3]) / static_cast<double>(_longOrders._tradedLot[3] * _lotSize);

    return (pOrderCe - pOrderPe - pAtmCe + pAtmPe + _orderStrike - _atmStrike);
}

auto BoxSpreadStrategy::GetSATP() const -> double {
    if (_shortOrders._tradedLot[0] == 0 || _shortOrders._tradedLot[1] == 0 || _shortOrders._tradedLot[2] == 0 || _shortOrders._tradedLot[3] == 0 || _lotSize == 0) return 0.0;
    double pOrderCe = static_cast<double>(_shortOrders._tradeValue[0]) / static_cast<double>(_shortOrders._tradedLot[0] * _lotSize);
    double pOrderPe = static_cast<double>(_shortOrders._tradeValue[1]) / static_cast<double>(_shortOrders._tradedLot[1] * _lotSize);
    double pAtmCe   = static_cast<double>(_shortOrders._tradeValue[2]) / static_cast<double>(_shortOrders._tradedLot[2] * _lotSize);
    double pAtmPe   = static_cast<double>(_shortOrders._tradeValue[3]) / static_cast<double>(_shortOrders._tradedLot[3] * _lotSize);

    return (-pOrderCe + pOrderPe + pAtmCe - pAtmPe - _orderStrike + _atmStrike);
}

auto BoxSpreadStrategy::GetRLP() const -> double {
    double totalRLP = 0.0;
    // Fixed sides for Box Spread:
    // Long Orders: Leg 0 (BUY), Leg 1 (SELL), Leg 2 (SELL), Leg 3 (BUY)
    // Short Orders: Leg 0 (SELL), Leg 1 (BUY), Leg 2 (BUY), Leg 3 (SELL)
    const ORDER_SIDE longSides[4] = {BUY_SIDE, SELL_SIDE, SELL_SIDE, BUY_SIDE};
    const ORDER_SIDE shortSides[4] = {SELL_SIDE, BUY_SIDE, BUY_SIDE, SELL_SIDE};

    for (size_t i = 0; i < 4; ++i) {
        int64_t buyQty = 0;
        uint64_t buyVal = 0;
        int64_t sellQty = 0;
        uint64_t sellVal = 0;

        if (longSides[i] == BUY_SIDE) {
            buyQty += static_cast<int64_t>(_longOrders._tradedLot[i]) * _lotSize;
            buyVal += _longOrders._tradeValue[i];
        } else {
            sellQty += static_cast<int64_t>(_longOrders._tradedLot[i]) * _lotSize;
            sellVal += _longOrders._tradeValue[i];
        }

        if (shortSides[i] == BUY_SIDE) {
            buyQty += static_cast<int64_t>(_shortOrders._tradedLot[i]) * _lotSize;
            buyVal += _shortOrders._tradeValue[i];
        } else {
            sellQty += static_cast<int64_t>(_shortOrders._tradedLot[i]) * _lotSize;
            sellVal += _shortOrders._tradeValue[i];
        }

        double avgBuyPrice = buyQty > 0 ? static_cast<double>(buyVal) / buyQty : 0.0;
        double avgSellPrice = sellQty > 0 ? static_cast<double>(sellVal) / sellQty : 0.0;

        if (buyQty > sellQty) {
            totalRLP += static_cast<double>(sellQty) * (avgSellPrice - avgBuyPrice);
        } else {
            totalRLP += static_cast<double>(buyQty) * (avgSellPrice - avgBuyPrice);
        }
    }
    return totalRLP;
}

auto BoxSpreadStrategy::GetCutPL() const -> double { return GetRLP(); }

auto BoxSpreadStrategy::GetM2M() const -> int {
    double totalM2M = 0.0;
    const ORDER_SIDE longSides[4] = {BUY_SIDE, SELL_SIDE, SELL_SIDE, BUY_SIDE};
    const ORDER_SIDE shortSides[4] = {SELL_SIDE, BUY_SIDE, BUY_SIDE, SELL_SIDE};

    for (size_t i = 0; i < 4; ++i) {
        int64_t buyQty = 0;
        uint64_t buyVal = 0;
        int64_t sellQty = 0;
        uint64_t sellVal = 0;

        if (longSides[i] == BUY_SIDE) {
            buyQty += static_cast<int64_t>(_longOrders._tradedLot[i]) * _lotSize;
            buyVal += _longOrders._tradeValue[i];
        } else {
            sellQty += static_cast<int64_t>(_longOrders._tradedLot[i]) * _lotSize;
            sellVal += _longOrders._tradeValue[i];
        }

        if (shortSides[i] == BUY_SIDE) {
            buyQty += static_cast<int64_t>(_shortOrders._tradedLot[i]) * _lotSize;
            buyVal += _shortOrders._tradeValue[i];
        } else {
            sellQty += static_cast<int64_t>(_shortOrders._tradedLot[i]) * _lotSize;
            sellVal += _shortOrders._tradeValue[i];
        }

        double avgBuyPrice = buyQty > 0 ? static_cast<double>(buyVal) / buyQty : 0.0;
        double avgSellPrice = sellQty > 0 ? static_cast<double>(sellVal) / sellQty : 0.0;

        int64_t netQty = buyQty - sellQty;
        if (netQty != 0) {
            double markPrice = 0.0;
            if (netQty > 0) {
                markPrice = _qoute[i].message.bid_levels[0].price;
            } else {
                markPrice = _qoute[i].message.ask_levels[0].price;
            }

            double avgPrice = netQty > 0 ? avgBuyPrice : avgSellPrice;
            totalM2M += static_cast<double>(netQty) * (markPrice - avgPrice);
        }
    }
    return static_cast<int>(totalM2M);
}

auto BoxSpreadStrategy::GetNetPL() const -> double { return GetRLP() + static_cast<double>(GetM2M()); }
auto BoxSpreadStrategy::GetFLP() const -> int { return 0; }
auto BoxSpreadStrategy::GetCost() const -> double { return 0.0; }

void BoxSpreadStrategy::OrderBiddingLogic(MarketBidding& object_, ParamLots param_, WindRate rate_, std::string name_) {
    size_t biddingLeg = (_qoute[4].message.ltp_ > _orderStrike) ? 0 : 1; // 0 = Order_CE, 1 = Order_PE

    int biddingLots = object_._tradedLot[biddingLeg];
    for (size_t h = 0; h < 4; ++h) {
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

void BoxSpreadStrategy::SecondOrderBidding(MarketBidding& object_, ParamLots param_) {
    size_t biddingLeg = (_qoute[4].message.ltp_ > _orderStrike) ? 0 : 1;
    int    biddingLot = object_._tradedLot[biddingLeg];

    for (size_t leg = 0; leg < 4; ++leg) {
        if (leg == biddingLeg) continue;
        int hedgeLot = object_._tradedLot[leg];
        int diff     = biddingLot - hedgeLot;
        if (diff <= 0) {
            object_._hedgeRetryCount = 0;
            continue;
        }

        if (_marketOrderRetries > 0 && object_._hedgeRetryCount >= _marketOrderRetries) {
            writeLog("[HEDGE RETRY EXHAUSTED BoxSpread] Terminating strategy & cancelling all orders.\n");
            _active = false;
            for (size_t i = 0; i < 4; ++i) {
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

[[nodiscard]] auto BoxSpreadStrategy::GetPrice(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].price : event_.message.ask_levels[index_].price;
}
[[nodiscard]] auto BoxSpreadStrategy::GetQuantity(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].qty : event_.message.ask_levels[index_].qty;
}
[[nodiscard]] auto BoxSpreadStrategy::GetOrderCount(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].order_count_ : event_.message.ask_levels[index_].order_count_;
}
