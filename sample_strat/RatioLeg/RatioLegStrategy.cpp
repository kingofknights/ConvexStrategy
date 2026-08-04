#include "RatioLegStrategy.hpp"

#include "AlgoBase.hpp"
#include "MinixStrategy.hpp"
#include "ProductInfo.hpp"
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

RatioLegStrategy::RatioLegStrategy(MinixStrategy* ms_, uint32_t strategyId_, const nlohmann::json& json_, size_t numLegs_)
    : _ms(ms_), _strategyId(strategyId_), _numLegs(numLegs_) {
    _qoute.resize(_numLegs);
    _tokens.assign(_numLegs, 0);
    _tokensParam.assign(_numLegs, 0);
    _ratios.assign(_numLegs, 1);
    _ratiosParam.assign(_numLegs, 1);
    _isOption.assign(_numLegs, false);
    _longSide.resize(_numLegs);
    _shortSide.resize(_numLegs);
    _longSideParam.resize(_numLegs);
    _shortSideParam.resize(_numLegs);
    _longOrders.resize(_numLegs);
    _shortOrders.resize(_numLegs);

    ParamUpdate(json_);
    _uid.composite_id_.client_id   = static_cast<uint32_t>(_ms->client);
    _uid.composite_id_.strategy_id = _strategyId;

    for (size_t i = 0; i < _numLegs; ++i) {
        _tokens[i]    = _tokensParam[i];
        _ratios[i]    = _ratiosParam[i];
        _longSide[i]  = _longSideParam[i];
        _shortSide[i] = _shortSideParam[i];
    }

    for (int token : _tokens) {
        _ms->subscribeProduct(token, _ms->flags);
    }

    std::vector<ProductDetails> details(_numLegs);
    for (size_t i = 0; i < _numLegs; ++i) {
        _ms->getProductDetails(_tokens[i], details[i]);
        _isOption[i] = details[i].opt_type_ != aef::infra::product::OPTION_TYPE::FUTXX;
    }

    _gap      = std::abs(details[0].strike_price_ - details[1].strike_price_) / 100;
    _lotSize  = details[0].lot_size_;
    _tickSize = details[0].tick_size_;

    for (size_t i = 0; i < _numLegs; ++i) {
        _longOrders._order[i]  = std::make_unique<OrderObjectT>(_tokens[i], _longSide[i], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
        _shortOrders._order[i] = std::make_unique<OrderObjectT>(_tokens[i], _shortSide[i], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    }
}

RatioLegStrategy::~RatioLegStrategy() {}

void RatioLegStrategy::ParamUpdate(const nlohmann::json& json_) {
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
    _biddingLeg = std::min(_biddingLeg, _numLegs - 1);

    // ── Ratio (nested under "Ratio" object) ───────────────────────────────────
    for (size_t i = 0; i < _numLegs && i < legsInfo.size(); ++i) {
        TokenInfo info     = legsInfo[i];
        _tokensParam[i]    = info._token;
        _longSideParam[i]  = info._side;
        _shortSideParam[i] = info._side == SELL_SIDE ? BUY_SIDE : SELL_SIDE;
    }

    if (json_.contains("Ratio") && json_["Ratio"].contains("LegRatios")) {
        auto legRatiosJson = json_["Ratio"]["LegRatios"];
        for (size_t i = 0; i < _numLegs && i < legRatiosJson.size(); ++i) {
            _ratiosParam[i] = legRatiosJson[i].get<int>();
        }
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

void RatioLegStrategy::OnTick(const Quote& event_, int64_t nowTs_) {
    int    token  = event_.header.product_id;
    bool   status = false;
    size_t index  = 0;
    for (size_t i = 0; i < _numLegs; ++i) {
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
        for (size_t h = 0; h < _numLegs; ++h) {
            if (h == _biddingLeg) continue;
            ORDER_SIDE hedgeSide = object_._order[h]->get_side();
            bool       orderOk   = CheckOrderDepth(_qoute[h], _orderDepth, hedgeSide);
            bool       priceOk   = CheckPriceDepth(_qoute[h], _priceDepth, hedgeSide);

            double thresholdPct = _thresholdQty > 0 ? _thresholdQty : 100.0;
            double targetQty    = (param_._quantity * _ratios[h] * _lotSize) * (thresholdPct / 100.0);
            bool   qtyOk        = GetAvailableQuantity(_qoute[h], _orderDepth, hedgeSide) >= targetQty;

            if (!orderOk || !priceOk || !qtyOk) {
                hedgeLegsOk = false;
                break;
            }
        }

        ORDER_SIDE mainSide     = object_._order[_biddingLeg]->get_side();
        bool       biddingLegOk = CheckPriceDepth(_qoute[_biddingLeg], _allowedBidDepth, mainSide);

        if (hedgeLegsOk && biddingLegOk) {
            OrderBiddingLogic(object_, param_, rate_, name_);
        } else {
            object_._order[_biddingLeg]->cancel_order();
        }
    };

    evaluateBidding(_longOrders, _longParam, GetBCmp(), "Long");
    evaluateBidding(_shortOrders, _shortParam, GetSCmp(), "Short");
}

void RatioLegStrategy::OnBcast(const aef::infra::product::product_data& pd_, int64_t nowTs_) {}

void RatioLegStrategy::OnOrderResponse(const oms_transaction& resp_) {
    if (_strategyId != resp_.hdr_.uid_.composite_id_.strategy_id) {
        return;
    }
    _ms->sendOrderResponse(resp_, std::to_string(_numLegs) + "LegRatio");
    bool traded   = resp_.hdr_.transaction_code == OMS_TRADE;
    auto price    = resp_.packet_.price_;
    auto quantity = resp_.packet_.quantity_;
    auto lot      = quantity / _lotSize;
    auto value    = static_cast<uint64_t>(price * quantity);

    auto handleTrade = [&](MarketBidding& object_, size_t index_) -> void {
        object_._order[index_]->handle_confirmation(resp_);
        object_._tradedLot[index_] += traded ? lot : 0;
        object_._tradeValue[index_] += traded ? value : 0;
        object_._cycleTradedLot[index_] += traded ? lot : 0;
        object_._cycleTradeValue[index_] += traded ? value : 0;
    };

    auto checkSlippage = [&](MarketBidding& object_, const std::vector<ORDER_SIDE>& sides_) {
        if (traded && _allowedSlippage > 0 && object_._cycleTradedLot[0] > 0) {
            bool allEqual = true;
            for (size_t i = 1; i < _numLegs; ++i) {
                if (object_._cycleTradedLot[0] / _ratios[0] != object_._cycleTradedLot[i] / _ratios[i]) {
                    allEqual = false;
                    break;
                }
            }
            if (!allEqual) return;

            bool expectedPricesOk = true;
            for (size_t i = 0; i < _numLegs; ++i) {
                if (i != _biddingLeg && object_._windRate._price[i] <= 0) {
                    expectedPricesOk = false;
                    break;
                }
            }
            if (!expectedPricesOk) return;

            double slippage = 0.0;
            for (size_t i = 0; i < _numLegs; ++i) {
                if (i == _biddingLeg) continue;
                double legAveragePrice = static_cast<double>(object_._cycleTradeValue[i]) / (object_._cycleTradedLot[i] * _lotSize);
                double expectedHedgePrice = object_._windRate._price[i];
                double actualHedgePrice   = legAveragePrice;
                ORDER_SIDE hedgeSide_ = sides_[i];

                if (hedgeSide_ == BUY_SIDE) {
                    slippage += (actualHedgePrice - expectedHedgePrice) * _ratios[i];
                } else {
                    slippage += (expectedHedgePrice - actualHedgePrice) * _ratios[i];
                }
            }

            // Reset cycle accumulators for the next cycle
            for (size_t i = 0; i < _numLegs; ++i) {
                object_._cycleTradedLot[i] = 0;
                object_._cycleTradeValue[i] = 0;
            }

            if (slippage > _allowedSlippage) {
                writeLog("[SLIPPAGE {}Leg] Slippage {} > AllowedSlippage {}. Stopping strategy.\n", _numLegs, slippage, _allowedSlippage);
                _active = false;
                for (size_t i = 0; i < _numLegs; ++i) {
                    _longOrders._order[i]->cancel_order();
                    _shortOrders._order[i]->cancel_order();
                }
            }
        }
    };

    auto processOrderResponse = [&](MarketBidding& object_, const std::vector<ORDER_SIDE>& sides_, size_t index_) -> bool {
        if (resp_.hdr_.uid_.id_ == object_._uniqueID[index_]) {
            handleTrade(object_, index_);
            if (_biddingLeg == index_) {
                if (traded) {
                    object_._lastBiddingFillPrice = price;
                }
            }
            checkSlippage(object_, sides_);
            return true;
        }
        return false;
    };

    for (size_t i = 0; i < _numLegs; ++i) {
        if (processOrderResponse(_longOrders, _longSide, i) || processOrderResponse(_shortOrders, _shortSide, i)) {
            break;
        }
    }

    if (traded) {
        SecondOrderBidding(_longOrders, _longParam);
        SecondOrderBidding(_shortOrders, _shortParam);
    }
}

auto RatioLegStrategy::GetBCmp() const -> WindRate {
    std::vector<int> prices(_numLegs);
    for (size_t i = 0; i < _numLegs; ++i) {
        prices[i] = GetPrice(_qoute[i], _longSide[i] == BUY_SIDE ? SELL_SIDE : BUY_SIDE, 0);
    }
    double spread = 0;
    for (size_t i = 0; i < _numLegs; ++i) {
        spread += (_longSide[i] == BUY_SIDE ? -prices[i] : prices[i]) * _ratios[i];
    }
    return WindRate{
        ._price  = prices,
        ._spread = static_cast<float>(spread),
    };
}

auto RatioLegStrategy::GetSCmp() const -> WindRate {
    std::vector<int> prices(_numLegs);
    for (size_t i = 0; i < _numLegs; ++i) {
        prices[i] = GetPrice(_qoute[i], _shortSide[i] == BUY_SIDE ? SELL_SIDE : BUY_SIDE, 0);
    }
    double spread = 0;
    for (size_t i = 0; i < _numLegs; ++i) {
        spread += (_shortSide[i] == BUY_SIDE ? -prices[i] : prices[i]) * _ratios[i];
    }
    return WindRate{
        ._price  = prices,
        ._spread = static_cast<float>(spread),
    };
}

auto RatioLegStrategy::GetStrategyID() const -> uint32_t { return _strategyId; }
auto RatioLegStrategy::GetGap() const -> int { return _gap; }

auto RatioLegStrategy::GetLongTradedLots() const -> int {
    if (_numLegs == 0) return 0;
    int totalPacks = _longOrders._tradedLot[0] / _ratios[0];
    for (size_t i = 1; i < _numLegs; ++i) {
        totalPacks = std::min(totalPacks, _longOrders._tradedLot[i] / _ratios[i]);
    }
    return totalPacks;
}

auto RatioLegStrategy::GetShortTradedLots() const -> int {
    if (_numLegs == 0) return 0;
    int totalPacks = _shortOrders._tradedLot[0] / _ratios[0];
    for (size_t i = 1; i < _numLegs; ++i) {
        totalPacks = std::min(totalPacks, _shortOrders._tradedLot[i] / _ratios[i]);
    }
    return totalPacks;
}

auto RatioLegStrategy::GetBATP() const -> double {
    double totalBATP = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        if (_longOrders._tradedLot[i] > 0) {
            totalBATP += static_cast<double>(_longOrders._tradeValue[i]) / (_longOrders._tradedLot[i] * _lotSize);
        }
    }
    return totalBATP;
}

auto RatioLegStrategy::GetSATP() const -> double {
    double totalSATP = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        if (_shortOrders._tradedLot[i] > 0) {
            totalSATP += static_cast<double>(_shortOrders._tradeValue[i]) / (_shortOrders._tradedLot[i] * _lotSize);
        }
    }
    return totalSATP;
}

auto RatioLegStrategy::GetRLP() const -> double {
    double totalRLP = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        int64_t  buyQty  = 0;
        uint64_t buyVal  = 0;
        int64_t  sellQty = 0;
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

        double avgBuyPrice  = buyQty > 0 ? static_cast<double>(buyVal) / buyQty : 0.0;
        double avgSellPrice = sellQty > 0 ? static_cast<double>(sellVal) / sellQty : 0.0;

        if (buyQty > sellQty) {
            totalRLP += static_cast<double>(sellQty) * (avgSellPrice - avgBuyPrice);
        } else {
            totalRLP += static_cast<double>(buyQty) * (avgSellPrice - avgBuyPrice);
        }
    }
    return totalRLP;
}

auto RatioLegStrategy::GetCutPL() const -> double { return GetRLP(); }

auto RatioLegStrategy::GetM2M() const -> int {
    double totalM2M = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        int64_t  buyQty  = 0;
        uint64_t buyVal  = 0;
        int64_t  sellQty = 0;
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

        double avgBuyPrice  = buyQty > 0 ? static_cast<double>(buyVal) / buyQty : 0.0;
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

auto RatioLegStrategy::GetNetPL() const -> double { return GetRLP() + static_cast<double>(GetM2M()); }
auto RatioLegStrategy::GetFLP() const -> int { return _qoute[_biddingLeg].message.ltp_; }

auto RatioLegStrategy::GetCost() const -> double {
    constexpr static double OptionBuyCost  = 0.000060000000000000001;
    constexpr static double OptionSellCost = 0.000070000000000000007;
    constexpr static double FutureBuyCost  = 0.0;
    constexpr static double FutureSellCost = 0.0;

    double totalCost = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        double buyPrice = _qoute[i].message.bid_levels[0].price * (_isOption[i] ? OptionBuyCost : FutureBuyCost);
        double sellPrice = _qoute[i].message.ask_levels[0].price * (_isOption[i] ? OptionSellCost : FutureSellCost);
        totalCost += buyPrice + sellPrice;
    }
    return totalCost;
}

void RatioLegStrategy::OrderBiddingLogic(MarketBidding& object_, ParamLots param_, WindRate rate_, std::string name_) {
    int biddingPacks = object_._tradedLot[_biddingLeg] / _ratios[_biddingLeg];
    for (size_t h = 0; h < _numLegs; ++h) {
        if (h == _biddingLeg) continue;
        int hedgePacks = object_._tradedLot[h] / _ratios[h];
        if (biddingPacks != hedgePacks) {
            SecondOrderBidding(object_, param_);
            return;
        }
    }

    if ((param_._spread < rate_._spread) || (object_._tradedLot[_biddingLeg] >= (param_._totalQuantity * _ratios[_biddingLeg]))) {
        object_._order[_biddingLeg]->cancel_order();
        return;
    }

    OrderObjectPtrT& order             = object_._order[_biddingLeg];
    int              basePrice         = GetPrice(_qoute[_biddingLeg], order->get_side(), 0);
    int              priceOffset       = _tradeGear * _tickSize;
    int              marketPrice       = order->get_side() == BUY_SIDE ? (basePrice + priceOffset) : (basePrice - priceOffset);
    int              currentPlacePrice = order->get_open_price();
    int              diff              = std::abs(currentPlacePrice - marketPrice);
    int              quantity          = param_._quantity * _ratios[_biddingLeg] * _lotSize;

    if (diff >= static_cast<int>(_minTickChange * _tickSize)) {
        if (quantity <= 0) return;
        auto status = _ms->update_order(order, _tokens[_biddingLeg], marketPrice, quantity, _uid);
        if (status != 0) {
            object_._uniqueID[_biddingLeg] = _uid.id_;
            object_._windRate              = rate_;
        }
    }
}

void RatioLegStrategy::SecondOrderBidding(MarketBidding& object_, ParamLots param_) {
    int biddingPacks = object_._tradedLot[_biddingLeg] / _ratios[_biddingLeg];

    for (size_t leg = 0; leg < _numLegs; ++leg) {
        if (leg == _biddingLeg) continue;
        int hedgePacks = object_._tradedLot[leg] / _ratios[leg];
        int targetHedgeLots = biddingPacks * _ratios[leg];
        int diff            = targetHedgeLots - object_._tradedLot[leg];
        if (diff <= 0) {
            object_._hedgeRetryCount = 0;
            continue;
        }

        if (_marketOrderRetries > 0 && object_._hedgeRetryCount >= _marketOrderRetries) {
            writeLog("[HEDGE RETRY EXHAUSTED {}Leg] Terminating strategy & cancelling all orders.\n", _numLegs);
            _active = false;
            for (size_t i = 0; i < _numLegs; ++i) {
                _longOrders._order[i]->cancel_order();
                _shortOrders._order[i]->cancel_order();
            }
            return;
        }

        int              maxHedgeSlice     = param_._quantity * _ratios[leg];
        int              quantity          = std::min(diff, maxHedgeSlice) * _lotSize;
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

auto RatioLegStrategy::GetPrice(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    if (side_ == BUY_SIDE) {
        return event_.message.bid_levels[index_].price;
    }
    return event_.message.ask_levels[index_].price;
}

auto RatioLegStrategy::GetQuantity(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    if (side_ == BUY_SIDE) {
        return event_.message.bid_levels[index_].qty;
    }
    return event_.message.ask_levels[index_].qty;
}

auto RatioLegStrategy::GetOrderCount(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    if (side_ == BUY_SIDE) {
        return event_.message.bid_levels[index_].qty;
    }
    return event_.message.ask_levels[index_].qty;
}

auto RatioLegStrategy::GetAvailableQuantity(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> int {
    int totalQty = 0;
    for (size_t i = 0; i < depth_; ++i) {
        totalQty += GetQuantity(event_, side_, i);
    }
    return totalQty;
}

auto RatioLegStrategy::CheckOrderDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool {
    for (size_t i = 0; i < depth_; ++i) {
        if (GetOrderCount(event_, side_, i) <= 0) {
            return false;
        }
    }
    return true;
}

auto RatioLegStrategy::CheckPriceDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool {
    for (size_t i = 0; i < depth_; ++i) {
        if (GetPrice(event_, side_, i) <= 0) {
            return false;
        }
    }
    return true;
}
