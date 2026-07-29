#include "Ratio5LegStrategy.hpp"

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

Ratio5LegStrategy::Ratio5LegStrategy(MinixStrategy* ms_, uint32_t strategyId_, const nlohmann::json& json_) : _ms(ms_), _strategyId(strategyId_) {
    ParamUpdate(json_);
    _uid.composite_id_.client_id   = static_cast<uint32_t>(_ms->client);
    _uid.composite_id_.strategy_id = strategyId_;

    for (int token : _tokens) {
        _ms->subscribeProduct(token, _ms->flags);
    }

    ProductDetails details[5];
    for (size_t i = 0; i < 5; ++i) {
        ms_->getProductDetails(_tokens[i], details[i]);
    }

    _gap      = std::abs(details[0].strike_price_ - details[1].strike_price_) / 100;
    _lotSize  = details[0].lot_size_;
    _tickSize = details[0].tick_size_;

    for (size_t i = 0; i < 5; ++i) {
        _longOrders._order[i]  = std::make_unique<OrderObjectT>(_tokens[i], _longSide[i], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
        _shortOrders._order[i] = std::make_unique<OrderObjectT>(_tokens[i], _shortSide[i], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    }
}

Ratio5LegStrategy::~Ratio5LegStrategy() {}

void Ratio5LegStrategy::ParamUpdate(const nlohmann::json& json_) {
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
    _biddingLeg = std::min(_biddingLeg, static_cast<size_t>(4));

    // ── Ratio (nested under "Ratio" object) ───────────────────────────────────
    for (size_t i = 0; i < 5 && i < legsInfo.size(); ++i) {
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

void Ratio5LegStrategy::OnTick(const Quote& event_, int64_t nowTs_) {
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

    auto evaluateBidding = [&](MarketBidding& object_, ParamLots& param_, WindRate rate_, std::string name_) {
        bool hedgeLegsOk = true;
        for (size_t h = 0; h < 5; ++h) {
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

void Ratio5LegStrategy::OnBcast(const aef::infra::product::product_data& pd_, int64_t nowTs_) {
}

void Ratio5LegStrategy::OnOrderResponse(const oms_transaction& resp_) {
    if (_strategyId != resp_.hdr_.uid_.composite_id_.strategy_id) {
        return;
    }
    _ms->sendOrderResponse(resp_, "5LegRatio");
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

    auto checkSlippage = [&](MarketBidding& object_, const std::array<ORDER_SIDE, 5>& sides_) {
        if (traded && _allowedSlippage > 0 && 
            object_._cycleTradedLot[0] == object_._cycleTradedLot[1] && 
            object_._cycleTradedLot[1] == object_._cycleTradedLot[2] && 
            object_._cycleTradedLot[2] == object_._cycleTradedLot[3] && 
            object_._cycleTradedLot[3] == object_._cycleTradedLot[4] && 
            object_._cycleTradedLot[0] > 0) {
            
            bool expectedPricesOk = true;
            for (size_t i = 0; i < 5; ++i) {
                if (i != _biddingLeg && object_._windRate._price[i] <= 0) {
                    expectedPricesOk = false;
                    break;
                }
            }
            if (!expectedPricesOk) return;

            double slippage = 0.0;
            for (size_t i = 0; i < 5; ++i) {
                if (i == _biddingLeg) continue;
                double legAveragePrice = static_cast<double>(object_._cycleTradeValue[i]) / (object_._cycleTradedLot[i] * _lotSize);
                double expectedHedgePrice = object_._windRate._price[i];
                double actualHedgePrice   = legAveragePrice;
                ORDER_SIDE hedgeSide_ = sides_[i];

                if (hedgeSide_ == BUY_SIDE) {
                    slippage += (actualHedgePrice - expectedHedgePrice);
                } else {
                    slippage += (expectedHedgePrice - actualHedgePrice);
                }
            }

            // Reset cycle accumulators for the next cycle
            for (size_t i = 0; i < 5; ++i) {
                object_._cycleTradedLot[i] = 0;
                object_._cycleTradeValue[i] = 0;
            }

            if (slippage > _allowedSlippage) {
                writeLog("[SLIPPAGE 5Leg] Slippage {} > AllowedSlippage {}. Stopping strategy.\n", slippage, _allowedSlippage);
                _active = false;
                for (size_t i = 0; i < 5; ++i) {
                    _longOrders._order[i]->cancel_order();
                    _shortOrders._order[i]->cancel_order();
                }
            }
        }
    };

    auto processOrderResponse = [&](MarketBidding& object_, const std::array<ORDER_SIDE, 5>& sides_, size_t index_) -> bool {
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

    for (size_t i = 0; i < 5; ++i) {
        if (processOrderResponse(_longOrders, _longSide, i) || processOrderResponse(_shortOrders, _shortSide, i)) {
            break;
        }
    }

    if (traded) {
        SecondOrderBidding(_longOrders, _longParam);
        SecondOrderBidding(_shortOrders, _shortParam);
    }
}

auto Ratio5LegStrategy::GetBCmp() const -> WindRate {
    int leg0Price = GetPrice(_qoute[0], _shortSide[0], 0);
    int leg1Price = GetPrice(_qoute[1], _shortSide[1], 0);
    int leg2Price = GetPrice(_qoute[2], _shortSide[2], 0);
    int leg3Price = GetPrice(_qoute[3], _shortSide[3], 0);
    int leg4Price = GetPrice(_qoute[4], _shortSide[4], 0);
    double spread = 0;
    spread += _longSide[0] == BUY_SIDE ? -leg0Price : leg0Price;
    spread += _longSide[1] == BUY_SIDE ? -leg1Price : leg1Price;
    spread += _longSide[2] == BUY_SIDE ? -leg2Price : leg2Price;
    spread += _longSide[3] == BUY_SIDE ? -leg3Price : leg3Price;
    spread += _longSide[4] == BUY_SIDE ? -leg4Price : leg4Price;
    return WindRate{
        ._price  = {leg0Price, leg1Price, leg2Price, leg3Price, leg4Price},
        ._spread = static_cast<float>(spread),
    };
}

auto Ratio5LegStrategy::GetSCmp() const -> WindRate {
    int leg0Price = GetPrice(_qoute[0], _longSide[0], 0);
    int leg1Price = GetPrice(_qoute[1], _longSide[1], 0);
    int leg2Price = GetPrice(_qoute[2], _longSide[2], 0);
    int leg3Price = GetPrice(_qoute[3], _longSide[3], 0);
    int leg4Price = GetPrice(_qoute[4], _longSide[4], 0);
    double spread = 0;
    spread += _shortSide[0] == BUY_SIDE ? -leg0Price : leg0Price;
    spread += _shortSide[1] == BUY_SIDE ? -leg1Price : leg1Price;
    spread += _shortSide[2] == BUY_SIDE ? -leg2Price : leg2Price;
    spread += _shortSide[3] == BUY_SIDE ? -leg3Price : leg3Price;
    spread += _shortSide[4] == BUY_SIDE ? -leg4Price : leg4Price;
    return WindRate{
        ._price  = {leg0Price, leg1Price, leg2Price, leg3Price, leg4Price},
        ._spread = static_cast<float>(spread),
    };
}

auto Ratio5LegStrategy::GetStrategyID() const -> uint32_t { return _strategyId; }
auto Ratio5LegStrategy::GetGap() const -> int { return _gap; }

auto Ratio5LegStrategy::GetLongTradedLots() const -> int {
    int leg0TradedPacks = _longOrders._tradedLot[0];
    int leg1TradedPacks = _longOrders._tradedLot[1];
    int leg2TradedPacks = _longOrders._tradedLot[2];
    int leg3TradedPacks = _longOrders._tradedLot[3];
    int leg4TradedPacks = _longOrders._tradedLot[4];
    int totalPacks      = std::min({leg0TradedPacks, leg1TradedPacks, leg2TradedPacks, leg3TradedPacks, leg4TradedPacks});
    return totalPacks;
}

auto Ratio5LegStrategy::GetShortTradedLots() const -> int {
    int leg0TradedPacks = _shortOrders._tradedLot[0];
    int leg1TradedPacks = _shortOrders._tradedLot[1];
    int leg2TradedPacks = _shortOrders._tradedLot[2];
    int leg3TradedPacks = _shortOrders._tradedLot[3];
    int leg4TradedPacks = _shortOrders._tradedLot[4];
    int totalPacks      = std::min({leg0TradedPacks, leg1TradedPacks, leg2TradedPacks, leg3TradedPacks, leg4TradedPacks});
    return totalPacks;
}

auto Ratio5LegStrategy::GetBATP() const -> double {
    uint64_t leg0TradedValue = _longOrders._tradeValue[0];
    uint64_t leg1TradedValue = _longOrders._tradeValue[1];
    uint64_t leg2TradedValue = _longOrders._tradeValue[2];
    uint64_t leg3TradedValue = _longOrders._tradeValue[3];
    uint64_t leg4TradedValue = _longOrders._tradeValue[4];
    uint64_t leg0TradedLots  = static_cast<uint64_t>(_longOrders._tradedLot[0]);
    uint64_t leg1TradedLots  = static_cast<uint64_t>(_longOrders._tradedLot[1]);
    uint64_t leg2TradedLots  = static_cast<uint64_t>(_longOrders._tradedLot[2]);
    uint64_t leg3TradedLots  = static_cast<uint64_t>(_longOrders._tradedLot[3]);
    uint64_t leg4TradedLots  = static_cast<uint64_t>(_longOrders._tradedLot[4]);

    if (leg0TradedLots == 0 or leg1TradedLots == 0 or leg2TradedLots == 0 or leg3TradedLots == 0 or leg4TradedLots == 0 or _lotSize == 0) {
        return 0.0;
    }
    double leg0AveragePrice = static_cast<double>(leg0TradedValue) / static_cast<double>(leg0TradedLots * _lotSize);
    double leg1AveragePrice = static_cast<double>(leg1TradedValue) / static_cast<double>(leg1TradedLots * _lotSize);
    double leg2AveragePrice = static_cast<double>(leg2TradedValue) / static_cast<double>(leg2TradedLots * _lotSize);
    double leg3AveragePrice = static_cast<double>(leg3TradedValue) / static_cast<double>(leg3TradedLots * _lotSize);
    double leg4AveragePrice = static_cast<double>(leg4TradedValue) / static_cast<double>(leg4TradedLots * _lotSize);
    return leg0AveragePrice - leg1AveragePrice - leg2AveragePrice + leg3AveragePrice - leg4AveragePrice;
}

auto Ratio5LegStrategy::GetSATP() const -> double {
    uint64_t leg0TradedValue = _shortOrders._tradeValue[0];
    uint64_t leg1TradedValue = _shortOrders._tradeValue[1];
    uint64_t leg2TradedValue = _shortOrders._tradeValue[2];
    uint64_t leg3TradedValue = _shortOrders._tradeValue[3];
    uint64_t leg4TradedValue = _shortOrders._tradeValue[4];
    uint64_t leg0TradedLots  = static_cast<uint64_t>(_shortOrders._tradedLot[0]);
    uint64_t leg1TradedLots  = static_cast<uint64_t>(_shortOrders._tradedLot[1]);
    uint64_t leg2TradedLots  = static_cast<uint64_t>(_shortOrders._tradedLot[2]);
    uint64_t leg3TradedLots  = static_cast<uint64_t>(_shortOrders._tradedLot[3]);
    uint64_t leg4TradedLots  = static_cast<uint64_t>(_shortOrders._tradedLot[4]);

    if (leg0TradedLots == 0 or leg1TradedLots == 0 or leg2TradedLots == 0 or leg3TradedLots == 0 or leg4TradedLots == 0 or _lotSize == 0) {
        return 0.0;
    }
    double leg0AveragePrice = static_cast<double>(leg0TradedValue) / static_cast<double>(leg0TradedLots * _lotSize);
    double leg1AveragePrice = static_cast<double>(leg1TradedValue) / static_cast<double>(leg1TradedLots * _lotSize);
    double leg2AveragePrice = static_cast<double>(leg2TradedValue) / static_cast<double>(leg2TradedLots * _lotSize);
    double leg3AveragePrice = static_cast<double>(leg3TradedValue) / static_cast<double>(leg3TradedLots * _lotSize);
    double leg4AveragePrice = static_cast<double>(leg4TradedValue) / static_cast<double>(leg4TradedLots * _lotSize);
    return leg0AveragePrice - leg1AveragePrice - leg2AveragePrice + leg3AveragePrice - leg4AveragePrice;
}

auto Ratio5LegStrategy::GetRLP() const -> double {
    double totalRLP = 0.0;
    for (size_t i = 0; i < 5; ++i) {
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

auto Ratio5LegStrategy::GetCutPL() const -> double { return GetRLP(); }

auto Ratio5LegStrategy::GetM2M() const -> int {
    double totalM2M = 0.0;
    for (size_t i = 0; i < 5; ++i) {
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

auto Ratio5LegStrategy::GetNetPL() const -> double { return GetRLP() + static_cast<double>(GetM2M()); }
auto Ratio5LegStrategy::GetFLP() const -> int { return _qoute[_biddingLeg].message.ltp_; }
auto Ratio5LegStrategy::GetCost() const -> double {
    constexpr static double buyCostPercentage  = 0.002079734;
    constexpr static double sellCostPercentage = 0.000609734;

    std::array buyPrice = {
        _qoute[0].message.bid_levels[0].price,
        _qoute[1].message.bid_levels[0].price,
        _qoute[2].message.bid_levels[0].price,
        _qoute[3].message.bid_levels[0].price,
        _qoute[4].message.bid_levels[0].price,
    };
    std::array sellPrice = {
        _qoute[0].message.ask_levels[0].price,
        _qoute[1].message.ask_levels[0].price,
        _qoute[2].message.ask_levels[0].price,
        _qoute[3].message.ask_levels[0].price,
        _qoute[4].message.ask_levels[0].price,
    };

    double buyCost  = (static_cast<double>(buyPrice[0] + buyPrice[1] + buyPrice[2] + buyPrice[3] + buyPrice[4]) * buyCostPercentage);
    double sellCost = (static_cast<double>(sellPrice[0] + sellPrice[1] + sellPrice[2] + sellPrice[3] + sellPrice[4]) * sellCostPercentage);
    return (buyCost + sellCost);
}

void Ratio5LegStrategy::OrderBiddingLogic(MarketBidding& object_, ParamLots param_, WindRate rate_, std::string name_) {
    int biddingPacks = object_._tradedLot[_biddingLeg];
    for (size_t h = 0; h < 5; ++h) {
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

void Ratio5LegStrategy::SecondOrderBidding(MarketBidding& object_, ParamLots param_) {
    int biddingPacks = object_._tradedLot[_biddingLeg];

    for (size_t leg = 0; leg < 5; ++leg) {
        if (leg == _biddingLeg) continue;
        int hedgePacks = object_._tradedLot[leg];
        int diff       = biddingPacks - hedgePacks;
        if (diff <= 0) {
            object_._hedgeRetryCount = 0;
            continue;
        }

        if (_marketOrderRetries > 0 && object_._hedgeRetryCount >= _marketOrderRetries) {
            writeLog("[HEDGE RETRY EXHAUSTED 5Leg] Terminating strategy & cancelling all orders.\n");
            _active = false;
            for (size_t i = 0; i < 5; ++i) {
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

[[nodiscard]] auto Ratio5LegStrategy::GetPrice(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].price : event_.message.ask_levels[index_].price;
}
[[nodiscard]] auto Ratio5LegStrategy::GetQuantity(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].qty : event_.message.ask_levels[index_].qty;
}
[[nodiscard]] auto Ratio5LegStrategy::GetOrderCount(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].order_count_ : event_.message.ask_levels[index_].order_count_;
}
[[nodiscard]] auto Ratio5LegStrategy::GetAvailableQuantity(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> int {
    int quantity = 0;
    for (size_t index = 0; index < depth_; ++index) {
        quantity += GetQuantity(event_, side_, index);
    }
    return quantity;
}
[[nodiscard]] auto Ratio5LegStrategy::CheckOrderDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool {
    int totalOrders = 0;
    for (size_t index = 0; index < 5; ++index) {
        totalOrders += GetOrderCount(event_, side_, index);
    }
    return static_cast<size_t>(totalOrders) >= depth_;
}
[[nodiscard]] auto Ratio5LegStrategy::CheckPriceDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool {
    size_t validPriceLevels = 0;
    for (size_t index = 0; index < 5; ++index) {
        if (GetPrice(event_, side_, index) > 0) {
            validPriceLevels++;
        }
    }
    return validPriceLevels >= depth_;
}
