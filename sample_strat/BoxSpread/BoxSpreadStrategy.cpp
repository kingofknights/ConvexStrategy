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

    // Sides for Box Conversion: Long Order_CE, Short Order_PE, Short ATM_CE, Long ATM_PE
    // Sides for Box Reversion: Short Order_CE, Long Order_PE, Long ATM_CE, Short ATM_PE
    ORDER_SIDE orderCeSide = (_stratType == BoxStrategyType::CONVERSION) ? BUY_SIDE : SELL_SIDE;
    ORDER_SIDE orderPeSide = (_stratType == BoxStrategyType::CONVERSION) ? SELL_SIDE : BUY_SIDE;
    ORDER_SIDE atmCeSide   = (_stratType == BoxStrategyType::CONVERSION) ? SELL_SIDE : BUY_SIDE;
    ORDER_SIDE atmPeSide   = (_stratType == BoxStrategyType::CONVERSION) ? BUY_SIDE : SELL_SIDE;

    _biddingOrders._order[0] = std::make_unique<OrderObjectT>(_tokens[0], orderCeSide, _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    _biddingOrders._order[1] = std::make_unique<OrderObjectT>(_tokens[1], orderPeSide, _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    _biddingOrders._order[2] = std::make_unique<OrderObjectT>(_tokens[2], atmCeSide, _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    _biddingOrders._order[3] = std::make_unique<OrderObjectT>(_tokens[3], atmPeSide, _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
}

void BoxSpreadStrategy::ParamUpdate(const nlohmann::json& json_) {
    if (json_.contains("Params")) {
        const auto& parmas = json_["Params"];

        std::string typeStr = parmas.value("StrategyType", "Conversion");
        _stratType = (typeStr == "Reversion" || typeStr == "reversion") ? BoxStrategyType::REVERSION : BoxStrategyType::CONVERSION;

        _orderStrike        = parmas.value("OrderStrike", 0);
        _atmStrike          = parmas.value("AtmStrike", 0);
        _quantity           = parmas.value("OrderLot", 0);
        _totalQuantity      = parmas.value("TotalLot", 0);
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

    WindRate activeRate = GetBCmp();
    if (activeRate._valid) {
        OrderBiddingLogic(_biddingOrders, activeRate, "BoxSpread");
    } else {
        size_t biddingLeg = (_qoute[4].message.ltp_ > _orderStrike) ? 0 : 1;
        _biddingOrders._order[biddingLeg]->cancel_order();
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

    for (size_t i = 0; i < 4; ++i) {
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

// ── Spread Calculations directly from Documents.md ────────────────────────

// 1. Conversion bid in Call (Fut_LTP > Order_Strike)
// Cur_wind_rate = order_ce_ask - order_pe_ask - atm_ce_ask + atm_pe_bid + order_strike - atm_strike - 1tick
// FIRST PRICE   = order_ce_ask - 1tick
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

// 2. Reversion bid in Call (Fut_LTP > Order_Strike)
// Cur_wind_rate = - order_ce_bid + order_pe_bid + atm_ce_bid - atm_pe_ask - order_strike + atm_strike - 1tick
// BIDDING PRICE = order_ce_bid + 1tick
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

// 3. Conversion bid in Put (Fut_LTP <= Order_Strike)
// Cur_wind_rate = order_ce_bid - order_pe_bid - atm_ce_ask + atm_pe_bid + order_strike - atm_strike - 1tick
// BIDDING PRICE = order_pe_bid + 1tick
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

// 4. Reversion bid in Put (Fut_LTP <= Order_Strike)
// Cur_wind_rate = order_pe_ask - order_ce_ask + atm_ce_bid - atm_pe_ask - order_strike + atm_strike - 1tick
// FIRST PRICE   = order_pe_ask - 1tick
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
    if (futLtp > _orderStrike) {
        // Fut_LTP > Order_Strike -> Order in Call
        return (_stratType == BoxStrategyType::CONVERSION) 
            ? CalculateConversionCallWindRate() 
            : CalculateReversionCallWindRate();
    } else {
        // Fut_LTP <= Order_Strike -> Order in Put
        return (_stratType == BoxStrategyType::CONVERSION) 
            ? CalculateConversionPutWindRate() 
            : CalculateReversionPutWindRate();
    }
}

auto BoxSpreadStrategy::GetSCmp() const -> WindRate {
    return GetBCmp();
}

auto BoxSpreadStrategy::GetStrategyID() const -> uint32_t { return _strategyId; }
auto BoxSpreadStrategy::GetGap() const -> int { return _gap; }

auto BoxSpreadStrategy::GetBuyTradedQuantity() const -> int {
    return std::min({_biddingOrders._tradedLot[0], _biddingOrders._tradedLot[1], _biddingOrders._tradedLot[2], _biddingOrders._tradedLot[3]});
}
auto BoxSpreadStrategy::GetSellTradedQuantity() const -> int {
    return GetBuyTradedQuantity();
}

auto BoxSpreadStrategy::GetBATP() const -> double {
    if (_biddingOrders._tradedLot[0] == 0 || _biddingOrders._tradedLot[1] == 0 || _biddingOrders._tradedLot[2] == 0 || _biddingOrders._tradedLot[3] == 0 || _lotSize == 0) return 0.0;
    double pOrderCe = static_cast<double>(_biddingOrders._tradeValue[0]) / static_cast<double>(_biddingOrders._tradedLot[0] * _lotSize);
    double pOrderPe = static_cast<double>(_biddingOrders._tradeValue[1]) / static_cast<double>(_biddingOrders._tradedLot[1] * _lotSize);
    double pAtmCe   = static_cast<double>(_biddingOrders._tradeValue[2]) / static_cast<double>(_biddingOrders._tradedLot[2] * _lotSize);
    double pAtmPe   = static_cast<double>(_biddingOrders._tradeValue[3]) / static_cast<double>(_biddingOrders._tradedLot[3] * _lotSize);

    return (_stratType == BoxStrategyType::CONVERSION) 
        ? (pOrderCe - pOrderPe - pAtmCe + pAtmPe + _orderStrike - _atmStrike) 
        : (-pOrderCe + pOrderPe + pAtmCe - pAtmPe - _orderStrike + _atmStrike);
}

auto BoxSpreadStrategy::GetSATP() const -> double { return GetBATP(); }

auto BoxSpreadStrategy::GetRLP() const -> double {
    int matchedLots = GetBuyTradedQuantity();
    return GetBATP() * static_cast<double>(matchedLots * _lotSize);
}

auto BoxSpreadStrategy::GetCutPL() const -> double { return GetRLP(); }

auto BoxSpreadStrategy::GetM2M() const -> int {
    int openLots = _biddingOrders._tradedLot[0] - GetBuyTradedQuantity();
    if (openLots <= 0) return 0;
    double currentSpread = static_cast<double>(GetBCmp()._windRate);
    return static_cast<int>((currentSpread - GetBATP()) * static_cast<double>(openLots * _lotSize));
}

auto BoxSpreadStrategy::GetNetPL() const -> double { return GetRLP() + static_cast<double>(GetM2M()); }
auto BoxSpreadStrategy::GetFLP() const -> int { return 0; }
auto BoxSpreadStrategy::GetCost() const -> double { return 0.0; }

void BoxSpreadStrategy::OrderBiddingLogic(MarketBidding& object_, WindRate rate_, std::string name_) {
    size_t biddingLeg = (_qoute[4].message.ltp_ > _orderStrike) ? 0 : 1; // 0 = Order_CE, 1 = Order_PE

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

void BoxSpreadStrategy::SecondOrderBidding(MarketBidding& object_) {
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
            fmt::print("[HEDGE RETRY EXHAUSTED BoxSpread] Terminating strategy & cancelling all orders.\n");
            _active = false;
            for (size_t i = 0; i < 4; ++i) {
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

[[nodiscard]] auto BoxSpreadStrategy::GetPrice(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].price : event_.message.ask_levels[index_].price;
}
[[nodiscard]] auto BoxSpreadStrategy::GetQuantity(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].qty : event_.message.ask_levels[index_].qty;
}
[[nodiscard]] auto BoxSpreadStrategy::GetOrderCount(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].order_count_ : event_.message.ask_levels[index_].order_count_;
}
