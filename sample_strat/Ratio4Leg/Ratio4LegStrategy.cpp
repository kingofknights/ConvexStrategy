#include "Ratio4LegStrategy.hpp"

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

Ratio4LegStrategy::Ratio4LegStrategy(MinixStrategy* ms_, uint32_t strategyId_, const nlohmann::json& json_) : _ms(ms_), _strategyId(strategyId_) {
    char filename[128];
    std::snprintf(filename, sizeof(filename), "Ratio4Leg_%u.log", _strategyId);
    _logFile = std::fopen(filename, "w");

    ParamUpdate(json_);
    _uid.composite_id_.client_id   = static_cast<uint32_t>(_ms->client);
    _uid.composite_id_.strategy_id = strategyId_;

    for (int token : _tokens) {
        _ms->subscribeProduct(token, _ms->flags);
    }

    ProductDetails details[4];
    for (size_t i = 0; i < 4; ++i) {
        ms_->getProductDetails(_tokens[i], details[i]);
    }

    _gap      = std::abs(details[0].strike_price_ - details[1].strike_price_) / 100;
    _lotSize  = details[0].lot_size_;
    _tickSize = details[0].tick_size_;

    for (size_t i = 0; i < 4; ++i) {
        _longOrders._order[i]  = std::make_unique<OrderObjectT>(_tokens[i], _longSide[i], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
        _shortOrders._order[i] = std::make_unique<OrderObjectT>(_tokens[i], _shortSide[i], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    }
}

Ratio4LegStrategy::~Ratio4LegStrategy() {
    if (_logFile) {
        std::fclose(_logFile);
    }
}

void Ratio4LegStrategy::ParamUpdate(const nlohmann::json& json_) {
    // ── Legs ─────────────────────────────────────────────────────────────────
    auto legs = json_["Legs"];

    std::vector<TokenInfo> legsInfo;
    for (auto& item : legs) {
        int         token = item.value("Token", 0);
        std::string side  = item.value("Side", "BUY");
        size_t      legId = item.value("LegID", 0U);
        bool        bid   = item.value("EnableBid", false);

        if (bid && legId > 0) {
            _biddingLeg = legId - 1;
        }
        legsInfo.push_back(TokenInfo{
            ._token = token,
            ._side  = side == "BUY" ? BUY_SIDE : SELL_SIDE,
            ._bid   = bid,
        });
    }
    _biddingLeg = std::min(_biddingLeg, static_cast<size_t>(3));

    // ── Ratio (nested under "Ratio" object) ───────────────────────────────────
    for (size_t i = 0; i < 4 && i < legsInfo.size(); ++i) {
        TokenInfo info = legsInfo[i];
        _tokens[i]     = info._token;
        _longSide[i]   = info._side;
        _shortSide[i]  = info._side == SELL_SIDE ? BUY_SIDE : SELL_SIDE;
    }

    // ── Params ────────────────────────────────────────────────────────────────
    if (json_.contains("Params")) {
        const auto& parmas = json_["Params"];

        _longParam._quantity      = parmas.value("LongBuySoQ", 0);
        _longParam._totalQuantity = parmas.value("LongBuyQty", 0);
        _longParam._spread        = parmas.value("LongBuyPrice", 0.0F) * 100.0F;

        _shortParam._quantity      = parmas.value("ShortSellSoQ", 0);
        _shortParam._totalQuantity = parmas.value("ShortSellQty", 0);
        _shortParam._spread        = parmas.value("ShortSellPrice", 0.0F) * 100.0F;

        _minTickChange      = parmas.value("TickSize", 0U);
        _orderDepth         = parmas.value("OrderDepth", 1U);
        _priceDepth         = parmas.value("PriceDepth", 1U);
        _allowedBidDepth    = parmas.value("AllowedBidDepth", 1U);
        _thresholdQty       = parmas.value("ThresholdQty", 100);
        _allowedSlippage    = parmas.value("AllowedSlippage", 0) * 100;
        _tradeGear          = parmas.value("TradeGear", 0);
        _marketOrderRetries = parmas.value("MarketOrderRetries", 0U);

        _orderDepth      = std::min<size_t>(_orderDepth, 5U);
        _priceDepth      = std::min<size_t>(_priceDepth, 5U);
        _allowedBidDepth = std::min<size_t>(_allowedBidDepth, 5U);
    }

    // ── Strategy meta ─────────────────────────────────────────────────────────
    if (json_.contains("Strategy")) {
        const auto& strategy = json_["Strategy"];
        _isBidding           = strategy.value("IsBidding", false);
        std::string status   = strategy.value("Status", "None");
        _active              = status == "Applied";
    }
}

void Ratio4LegStrategy::OnTick(const Quote& event_, int64_t nowTs_) {
    int  token  = event_.header.product_id;
    bool status = false;
    size_t index = 0;
    for (size_t i = 0; i < 4; ++i) {
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

    auto evaluateBidding = [&](MarketBidding& object_, ParamLots& param_, WindRate rate_, std::string name_) {
        bool hedgeLegsOk = true;
        for (size_t h = 0; h < 4; ++h) {
            if (h == _biddingLeg) continue;
            ORDER_SIDE hedgeSide = object_._order[h]->get_side();
            bool orderOk = CheckOrderDepth(_qoute[h], _orderDepth, hedgeSide);
            bool priceOk = CheckPriceDepth(_qoute[h], _priceDepth, hedgeSide);

            double thresholdPct = _thresholdQty > 0 ? _thresholdQty : 100.0;
            double targetQty    = (param_._quantity * _lotSize) * (thresholdPct / 100.0);
            bool qtyOk          = GetAvailableQuantity(_qoute[h], _orderDepth, hedgeSide) >= targetQty;

            if (!orderOk || !priceOk || !qtyOk) {
                hedgeLegsOk = false;
                break;
            }
        }

        ORDER_SIDE mainSide = object_._order[_biddingLeg]->get_side();
        bool biddingLegOk   = CheckPriceDepth(_qoute[_biddingLeg], _allowedBidDepth, mainSide);

        if (hedgeLegsOk && biddingLegOk) {
            OrderBiddingLogic(object_, param_, rate_, name_);
        } else {
            object_._order[_biddingLeg]->cancel_order();
        }
    };

    evaluateBidding(_longOrders, _longParam, GetBCmp(), "Long");
    evaluateBidding(_shortOrders, _shortParam, GetSCmp(), "Short");
}

void Ratio4LegStrategy::OnBcast(const aef::infra::product::product_data& pd_, int64_t nowTs_) {
}

void Ratio4LegStrategy::OnOrderResponse(const oms_transaction& resp_) {
    if (_strategyId != resp_.hdr_.uid_.composite_id_.strategy_id) {
        return;
    }
    _ms->sendOrderResponse(resp_, "4LegRatio");
    bool traded   = resp_.hdr_.transaction_code == OMS_TRADE;
    auto price    = resp_.packet_.price_;
    auto quantity = resp_.packet_.quantity_;
    auto lot      = quantity / _lotSize;
    auto value    = static_cast<uint64_t>(price * quantity);

    auto handleTrade = [&](MarketBidding& object_, size_t index_) -> void {
        object_._order[index_]->handle_confirmation(resp_);
        object_._tradedLot[index_] += traded ? lot : 0;
        object_._tradeValue[index_] += traded ? value : 0;
    };

    auto checkSlippage = [&](MarketBidding& object_) {
        if (traded && object_._lastBiddingFillPrice > 0 && _allowedSlippage > 0) {
            double leg0AveragePrice = static_cast<double>(object_._tradeValue[0]) / (object_._tradedLot[0] > 0 ? object_._tradedLot[0] * _lotSize : 1);
            double leg1AveragePrice = static_cast<double>(object_._tradeValue[1]) / (object_._tradedLot[1] > 0 ? object_._tradedLot[1] * _lotSize : 1);
            double leg2AveragePrice = static_cast<double>(object_._tradeValue[2]) / (object_._tradedLot[2] > 0 ? object_._tradedLot[2] * _lotSize : 1);
            double leg3AveragePrice = static_cast<double>(object_._tradeValue[3]) / (object_._tradedLot[3] > 0 ? object_._tradedLot[3] * _lotSize : 1);

            double executedSpread = leg0AveragePrice - leg1AveragePrice - leg2AveragePrice + leg3AveragePrice;
            double slippage       = 0.0;
            if (&object_ == &_longOrders) {
                slippage = executedSpread - _longParam._spread;
            } else {
                slippage = _shortParam._spread - executedSpread;
            }
            if (slippage > _allowedSlippage) {
                writeLog("[SLIPPAGE 4Leg] Slippage {} > AllowedSlippage {}. Stopping strategy.\n", slippage, _allowedSlippage);
                _active = false;
                for (size_t i = 0; i < 4; ++i) {
                    _longOrders._order[i]->cancel_order();
                    _shortOrders._order[i]->cancel_order();
                }
            }
        }
    };

    auto processOrderResponse = [&](MarketBidding& object_, size_t index_) -> bool {
        if (resp_.hdr_.uid_.id_ == object_._uniqueID[index_]) {
            handleTrade(object_, index_);
            if (_biddingLeg == index_) {
                if (traded) {
                    object_._lastBiddingFillPrice = price;
                }
            } else {
                checkSlippage(object_);
            }
            return true;
        }
        return false;
    };

    for (size_t i = 0; i < 4; ++i) {
        if (processOrderResponse(_longOrders, i) || processOrderResponse(_shortOrders, i)) {
            break;
        }
    }

    if (traded) {
        SecondOrderBidding(_longOrders, _longParam);
        SecondOrderBidding(_shortOrders, _shortParam);
    }
}

auto Ratio4LegStrategy::GetBCmp() const -> WindRate {
    int leg0Price = GetPrice(_qoute[0], _shortSide[0], 0);
    int leg1Price = GetPrice(_qoute[1], _shortSide[1], 0);
    int leg2Price = GetPrice(_qoute[2], _shortSide[2], 0);
    int leg3Price = GetPrice(_qoute[3], _shortSide[3], 0);
    double spread = 0;
    spread += _longSide[0] == BUY_SIDE ? -leg0Price : leg0Price;
    spread += _longSide[1] == BUY_SIDE ? -leg1Price : leg1Price;
    spread += _longSide[2] == BUY_SIDE ? -leg2Price : leg2Price;
    spread += _longSide[3] == BUY_SIDE ? -leg3Price : leg3Price;
    return WindRate{
        ._price  = {leg0Price, leg1Price, leg2Price, leg3Price},
        ._spread = static_cast<float>(spread),
    };
}

auto Ratio4LegStrategy::GetSCmp() const -> WindRate {
    int leg0Price = GetPrice(_qoute[0], _longSide[0], 0);
    int leg1Price = GetPrice(_qoute[1], _longSide[1], 0);
    int leg2Price = GetPrice(_qoute[2], _longSide[2], 0);
    int leg3Price = GetPrice(_qoute[3], _longSide[3], 0);
    double spread = 0;
    spread += _shortSide[0] == BUY_SIDE ? -leg0Price : leg0Price;
    spread += _shortSide[1] == BUY_SIDE ? -leg1Price : leg1Price;
    spread += _shortSide[2] == BUY_SIDE ? -leg2Price : leg2Price;
    spread += _shortSide[3] == BUY_SIDE ? -leg3Price : leg3Price;
    return WindRate{
        ._price  = {leg0Price, leg1Price, leg2Price, leg3Price},
        ._spread = static_cast<float>(spread),
    };
}

auto Ratio4LegStrategy::GetStrategyID() const -> uint32_t { return _strategyId; }
auto Ratio4LegStrategy::GetGap() const -> int { return _gap; }

auto Ratio4LegStrategy::GetLongTradedLots() const -> int {
    int leg0TradedPacks = _longOrders._tradedLot[0];
    int leg1TradedPacks = _longOrders._tradedLot[1];
    int leg2TradedPacks = _longOrders._tradedLot[2];
    int leg3TradedPacks = _longOrders._tradedLot[3];
    int totalPacks      = std::min({leg0TradedPacks, leg1TradedPacks, leg2TradedPacks, leg3TradedPacks});
    return totalPacks;
}

auto Ratio4LegStrategy::GetShortTradedLots() const -> int {
    int leg0TradedPacks = _shortOrders._tradedLot[0];
    int leg1TradedPacks = _shortOrders._tradedLot[1];
    int leg2TradedPacks = _shortOrders._tradedLot[2];
    int leg3TradedPacks = _shortOrders._tradedLot[3];
    int totalPacks      = std::min({leg0TradedPacks, leg1TradedPacks, leg2TradedPacks, leg3TradedPacks});
    return totalPacks;
}

auto Ratio4LegStrategy::GetBATP() const -> double {
    uint64_t leg0TradedValue = _longOrders._tradeValue[0];
    uint64_t leg1TradedValue = _longOrders._tradeValue[1];
    uint64_t leg2TradedValue = _longOrders._tradeValue[2];
    uint64_t leg3TradedValue = _longOrders._tradeValue[3];
    uint64_t leg0TradedLots  = static_cast<uint64_t>(_longOrders._tradedLot[0]);
    uint64_t leg1TradedLots  = static_cast<uint64_t>(_longOrders._tradedLot[1]);
    uint64_t leg2TradedLots  = static_cast<uint64_t>(_longOrders._tradedLot[2]);
    uint64_t leg3TradedLots  = static_cast<uint64_t>(_longOrders._tradedLot[3]);

    if (leg0TradedLots == 0 or leg1TradedLots == 0 or leg2TradedLots == 0 or leg3TradedLots == 0 or _lotSize == 0) {
        return 0.0;
    }
    double leg0AveragePrice = static_cast<double>(leg0TradedValue) / static_cast<double>(leg0TradedLots * _lotSize);
    double leg1AveragePrice = static_cast<double>(leg1TradedValue) / static_cast<double>(leg1TradedLots * _lotSize);
    double leg2AveragePrice = static_cast<double>(leg2TradedValue) / static_cast<double>(leg2TradedLots * _lotSize);
    double leg3AveragePrice = static_cast<double>(leg3TradedValue) / static_cast<double>(leg3TradedLots * _lotSize);
    return leg0AveragePrice - leg1AveragePrice - leg2AveragePrice + leg3AveragePrice;
}

auto Ratio4LegStrategy::GetSATP() const -> double {
    uint64_t leg0TradedValue = _shortOrders._tradeValue[0];
    uint64_t leg1TradedValue = _shortOrders._tradeValue[1];
    uint64_t leg2TradedValue = _shortOrders._tradeValue[2];
    uint64_t leg3TradedValue = _shortOrders._tradeValue[3];
    uint64_t leg0TradedLots  = static_cast<uint64_t>(_shortOrders._tradedLot[0]);
    uint64_t leg1TradedLots  = static_cast<uint64_t>(_shortOrders._tradedLot[1]);
    uint64_t leg2TradedLots  = static_cast<uint64_t>(_shortOrders._tradedLot[2]);
    uint64_t leg3TradedLots  = static_cast<uint64_t>(_shortOrders._tradedLot[3]);

    if (leg0TradedLots == 0 or leg1TradedLots == 0 or leg2TradedLots == 0 or leg3TradedLots == 0 or _lotSize == 0) {
        return 0.0;
    }
    double leg0AveragePrice = static_cast<double>(leg0TradedValue) / static_cast<double>(leg0TradedLots * _lotSize);
    double leg1AveragePrice = static_cast<double>(leg1TradedValue) / static_cast<double>(leg1TradedLots * _lotSize);
    double leg2AveragePrice = static_cast<double>(leg2TradedValue) / static_cast<double>(leg2TradedLots * _lotSize);
    double leg3AveragePrice = static_cast<double>(leg3TradedValue) / static_cast<double>(leg3TradedLots * _lotSize);
    return leg0AveragePrice - leg1AveragePrice - leg2AveragePrice + leg3AveragePrice;
}

auto Ratio4LegStrategy::GetRLP() const -> double {
    double totalRLP = 0.0;
    for (size_t i = 0; i < 4; ++i) {
        int64_t buyQty = 0;
        uint64_t buyVal = 0;
        int64_t sellQty = 0;
        uint64_t sellVal = 0;

        if (_longSide[i] == BUY_SIDE) {
            buyQty += static_cast<int64_t>(_longOrders._tradedLot[i]) * _lotSize;
            buyVal += _longOrders._tradeValue[i];
        } else {
            sellQty += static_cast<int64_t>(_longOrders._tradedLot[i]) * _lotSize;
            sellVal += _longOrders._tradeValue[i];
        }

        if (_shortSide[i] == BUY_SIDE) {
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

auto Ratio4LegStrategy::GetCutPL() const -> double { return GetRLP(); }

auto Ratio4LegStrategy::GetM2M() const -> int {
    double totalM2M = 0.0;
    for (size_t i = 0; i < 4; ++i) {
        int64_t buyQty = 0;
        uint64_t buyVal = 0;
        int64_t sellQty = 0;
        uint64_t sellVal = 0;

        if (_longSide[i] == BUY_SIDE) {
            buyQty += static_cast<int64_t>(_longOrders._tradedLot[i]) * _lotSize;
            buyVal += _longOrders._tradeValue[i];
        } else {
            sellQty += static_cast<int64_t>(_longOrders._tradedLot[i]) * _lotSize;
            sellVal += _longOrders._tradeValue[i];
        }

        if (_shortSide[i] == BUY_SIDE) {
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

auto Ratio4LegStrategy::GetNetPL() const -> double { return GetRLP() + static_cast<double>(GetM2M()); }
auto Ratio4LegStrategy::GetFLP() const -> int { return 0; }
auto Ratio4LegStrategy::GetCost() const -> double { return 0.0; }

void Ratio4LegStrategy::OrderBiddingLogic(MarketBidding& object_, ParamLots param_, WindRate rate_, std::string name_) {
    int biddingPacks = object_._tradedLot[_biddingLeg];
    for (size_t h = 0; h < 4; ++h) {
        if (h == _biddingLeg) continue;
        int hedgePacks = object_._tradedLot[h];
        if (biddingPacks != hedgePacks) {
            SecondOrderBidding(object_, param_);
            return;
        }
    }

    if ((param_._spread < rate_._spread) || (object_._tradedLot[_biddingLeg] >= param_._totalQuantity)) {
        object_._order[_biddingLeg]->cancel_order();
        return;
    }

    OrderObjectPtrT& order             = object_._order[_biddingLeg];
    int              basePrice         = GetPrice(_qoute[_biddingLeg], order->get_side(), 0);
    int              priceOffset       = _tradeGear * _tickSize;
    int              marketPrice       = order->get_side() == BUY_SIDE ? (basePrice + priceOffset) : (basePrice - priceOffset);
    int              currentPlacePrice = order->get_open_price();
    int              diff              = std::abs(currentPlacePrice - marketPrice);
    int              quantity          = param_._quantity * _lotSize;

    if (diff >= static_cast<int>(_minTickChange * _tickSize)) {
        if (quantity <= 0) return;
        auto status = _ms->update_order(order, _tokens[_biddingLeg], marketPrice, quantity, _uid);
        if (status != 0) {
            object_._uniqueID[_biddingLeg] = _uid.id_;
            object_._windRate              = rate_;
        }
    }
}

void Ratio4LegStrategy::SecondOrderBidding(MarketBidding& object_, ParamLots param_) {
    int biddingPacks = object_._tradedLot[_biddingLeg];

    for (size_t leg = 0; leg < 4; ++leg) {
        if (leg == _biddingLeg) continue;
        int hedgePacks = object_._tradedLot[leg];
        int diff       = biddingPacks - hedgePacks;
        if (diff <= 0) {
            object_._hedgeRetryCount = 0;
            continue;
        }

        if (_marketOrderRetries > 0 && object_._hedgeRetryCount >= _marketOrderRetries) {
            writeLog("[HEDGE RETRY EXHAUSTED 4Leg] Terminating strategy & cancelling all orders.\n");
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

[[nodiscard]] auto Ratio4LegStrategy::GetPrice(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].price : event_.message.ask_levels[index_].price;
}
[[nodiscard]] auto Ratio4LegStrategy::GetQuantity(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].qty : event_.message.ask_levels[index_].qty;
}
[[nodiscard]] auto Ratio4LegStrategy::GetOrderCount(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].order_count_ : event_.message.ask_levels[index_].order_count_;
}
[[nodiscard]] auto Ratio4LegStrategy::GetAvailableQuantity(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> int {
    int quantity = 0;
    for (size_t index = 0; index < depth_; ++index) {
        quantity += GetQuantity(event_, side_, index);
    }
    return quantity;
}
[[nodiscard]] auto Ratio4LegStrategy::CheckOrderDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool {
    int totalOrders = 0;
    for (size_t index = 0; index < 5; ++index) {
        totalOrders += GetOrderCount(event_, side_, index);
    }
    return static_cast<size_t>(totalOrders) >= depth_;
}
[[nodiscard]] auto Ratio4LegStrategy::CheckPriceDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool {
    size_t validPriceLevels = 0;
    for (size_t index = 0; index < 5; ++index) {
        if (GetPrice(event_, side_, index) > 0) {
            validPriceLevels++;
        }
    }
    return validPriceLevels >= depth_;
}
