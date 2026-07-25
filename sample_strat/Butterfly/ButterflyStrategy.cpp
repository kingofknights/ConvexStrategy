#include "ButterflyStrategy.hpp"

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

ButterflyStrategy::ButterflyStrategy(MinixStrategy* ms_, uint32_t strategyId_, const nlohmann::json& json_) : _ms(ms_), _strategyId(strategyId_) {
    ParamUpdate(json_);
    _uid.composite_id_.client_id   = static_cast<uint32_t>(_ms->client);
    _uid.composite_id_.strategy_id = strategyId_;

    for (size_t i = 0; i < _numLegs; ++i) {
        if (_tokens[i] > 0) {
            _ms->subscribeProduct(_tokens[i], _ms->flags);
        }
    }

    ProductDetails details[4];
    for (size_t i = 0; i < _numLegs; ++i) {
        if (_tokens[i] > 0) {
            ms_->getProductDetails(_tokens[i], details[i]);
        }
    }

    if (_numLegs >= 2 && details[0].strike_price_ > 0 && details[1].strike_price_ > 0) {
        _gap = std::abs(details[0].strike_price_ - details[1].strike_price_) / 100;
    }
    _lotSize  = details[0].lot_size_ > 0 ? details[0].lot_size_ : 1;
    _tickSize = details[0].tick_size_ > 0 ? details[0].tick_size_ : 5;

    for (size_t i = 0; i < _numLegs; ++i) {
        _longOrders._order[i]  = std::make_unique<OrderObjectT>(_tokens[i], _longSide[i], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
        _shortOrders._order[i] = std::make_unique<OrderObjectT>(_tokens[i], _shortSide[i], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    }
}

void ButterflyStrategy::ParamUpdate(const nlohmann::json& json_) {
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

    _numLegs = std::min<size_t>(std::max<size_t>(legsInfo.size(), 3U), 4U);
    _biddingLeg = std::min(_biddingLeg, _numLegs - 1);

    // ── Ratio (nested under "Ratio" object) ───────────────────────────────────
    std::vector<int> ratio;
    if (json_.contains("Ratio") && json_["Ratio"].contains("LegRatios")) {
        for (const auto& item : json_["Ratio"]["LegRatios"]) {
            ratio.push_back(item.get<int>());
        }
    }

    // Default Butterfly ratio: 1:2:1 for 3 legs
    std::array<int, 4> defaultRatios = {1, 2, 1, 1};

    for (size_t i = 0; i < _numLegs; ++i) {
        if (i < legsInfo.size()) {
            TokenInfo info = legsInfo[i];
            _tokens[i]     = info._token;
            _longSide[i]   = info._side;
            _shortSide[i]  = info._side == SELL_SIDE ? BUY_SIDE : SELL_SIDE;
        }
        _ratio[i] = i < ratio.size() ? ratio[i] : defaultRatios[i];
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

void ButterflyStrategy::OnTick(const Quote& event_, int64_t nowTs_) {
    int  token  = event_.header.product_id;
    bool status = false;
    size_t index = 0;
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
            bool orderOk = CheckOrderDepth(_qoute[h], _orderDepth, hedgeSide);
            bool priceOk = CheckPriceDepth(_qoute[h], _priceDepth, hedgeSide);

            double thresholdPct = _thresholdQty > 0 ? _thresholdQty : 100.0;
            double targetQty    = (param_._quantity * _ratio[h] * _lotSize) * (thresholdPct / 100.0);
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

    evaluateBidding(_longOrders, _longParam, GetBCmp(), "LongButterfly");
    evaluateBidding(_shortOrders, _shortParam, GetSCmp(), "ShortButterfly");
}

void ButterflyStrategy::OnBcast(const aef::infra::product::product_data& pd_, int64_t nowTs_) {
}

void ButterflyStrategy::OnOrderResponse(const oms_transaction& resp_) {
    if (_strategyId != resp_.hdr_.uid_.composite_id_.strategy_id) {
        return;
    }
    _ms->sendOrderResponse(resp_, "Butterfly");
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
            double executedSpread = 0.0;
            for (size_t i = 0; i < _numLegs; ++i) {
                double avgPrice = static_cast<double>(object_._tradeValue[i]) / (object_._tradedLot[i] > 0 ? object_._tradedLot[i] * _lotSize : 1);
                if (object_._order[i]->get_side() == BUY_SIDE) {
                    executedSpread += avgPrice * _ratio[i];
                } else {
                    executedSpread -= avgPrice * _ratio[i];
                }
            }

            double slippage = 0.0;
            if (&object_ == &_longOrders) {
                slippage = executedSpread - _longParam._spread;
            } else {
                slippage = _shortParam._spread - executedSpread;
            }
            if (slippage > _allowedSlippage) {
                fmt::print("[SLIPPAGE Butterfly] Slippage {} > AllowedSlippage {}. Stopping strategy.\n", slippage, _allowedSlippage);
                _active = false;
                for (size_t i = 0; i < _numLegs; ++i) {
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

    for (size_t i = 0; i < _numLegs; ++i) {
        if (processOrderResponse(_longOrders, i) || processOrderResponse(_shortOrders, i)) {
            break;
        }
    }

    if (traded) {
        SecondOrderBidding(_longOrders, _longParam);
        SecondOrderBidding(_shortOrders, _shortParam);
    }
}

auto ButterflyStrategy::GetBCmp() const -> WindRate {
    WindRate rate;
    float spread = 0.0f;
    for (size_t i = 0; i < _numLegs; ++i) {
        int legPrice = GetPrice(_qoute[i], _shortSide[i], 0);
        rate._price[i] = legPrice;
        if (_longSide[i] == BUY_SIDE) {
            spread += static_cast<float>(legPrice * _ratio[i]);
        } else {
            spread -= static_cast<float>(legPrice * _ratio[i]);
        }
    }
    rate._spread = spread;
    return rate;
}

auto ButterflyStrategy::GetSCmp() const -> WindRate {
    WindRate rate;
    float spread = 0.0f;
    for (size_t i = 0; i < _numLegs; ++i) {
        int legPrice = GetPrice(_qoute[i], _longSide[i], 0);
        rate._price[i] = legPrice;
        if (_shortSide[i] == SELL_SIDE) {
            spread += static_cast<float>(legPrice * _ratio[i]);
        } else {
            spread -= static_cast<float>(legPrice * _ratio[i]);
        }
    }
    rate._spread = spread;
    return rate;
}

auto ButterflyStrategy::GetStrategyID() const -> uint32_t { return _strategyId; }
auto ButterflyStrategy::GetGap() const -> int { return _gap; }

auto ButterflyStrategy::GetBuyTradedQuantity() const -> int {
    int totalPacks = 999999;
    for (size_t i = 0; i < _numLegs; ++i) {
        int packs = _longOrders._tradedLot[i] / (_ratio[i] > 0 ? _ratio[i] : 1);
        totalPacks = std::min(totalPacks, packs);
    }
    return totalPacks == 999999 ? 0 : totalPacks;
}

auto ButterflyStrategy::GetSellTradedQuantity() const -> int {
    int totalPacks = 999999;
    for (size_t i = 0; i < _numLegs; ++i) {
        int packs = _shortOrders._tradedLot[i] / (_ratio[i] > 0 ? _ratio[i] : 1);
        totalPacks = std::min(totalPacks, packs);
    }
    return totalPacks == 999999 ? 0 : totalPacks;
}

auto ButterflyStrategy::GetBATP() const -> double {
    for (size_t i = 0; i < _numLegs; ++i) {
        if (_longOrders._tradedLot[i] == 0 || _lotSize == 0) return 0.0;
    }

    double spread = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        double avgPrice = static_cast<double>(_longOrders._tradeValue[i]) / static_cast<double>(_longOrders._tradedLot[i] * _lotSize);
        if (_longSide[i] == BUY_SIDE) {
            spread += avgPrice * _ratio[i];
        } else {
            spread -= avgPrice * _ratio[i];
        }
    }
    return spread;
}

auto ButterflyStrategy::GetSATP() const -> double {
    for (size_t i = 0; i < _numLegs; ++i) {
        if (_shortOrders._tradedLot[i] == 0 || _lotSize == 0) return 0.0;
    }

    double spread = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        double avgPrice = static_cast<double>(_shortOrders._tradeValue[i]) / static_cast<double>(_shortOrders._tradedLot[i] * _lotSize);
        if (_shortSide[i] == SELL_SIDE) {
            spread += avgPrice * _ratio[i];
        } else {
            spread -= avgPrice * _ratio[i];
        }
    }
    return spread;
}

auto ButterflyStrategy::GetRLP() const -> double {
    int buyPacks  = GetBuyTradedQuantity();
    int sellPacks = GetSellTradedQuantity();

    double realizedPnL = static_cast<double>(std::min(buyPacks, sellPacks)) * (GetSATP() - GetBATP()) * static_cast<double>(_lotSize);
    if (buyPacks == sellPacks && buyPacks > 0) {
        double totalSellValue = static_cast<double>(sellPacks * _lotSize) * GetSATP();
        double totalBuyValue  = static_cast<double>(buyPacks * _lotSize) * GetBATP();
        realizedPnL           = totalSellValue - totalBuyValue;
    }
    return realizedPnL;
}

auto ButterflyStrategy::GetCutPL() const -> double { return GetRLP(); }

auto ButterflyStrategy::GetM2M() const -> int {
    int buyPacks  = GetBuyTradedQuantity();
    int sellPacks = GetSellTradedQuantity();

    double markToMarketPnL = 0.0;
    if (buyPacks > sellPacks) {
        double currentMarketSpread = static_cast<double>(GetBCmp()._spread);
        markToMarketPnL            = static_cast<double>(buyPacks - sellPacks) * (currentMarketSpread - GetBATP()) * static_cast<double>(_lotSize);
    } else if (sellPacks > buyPacks) {
        double currentMarketSpread = static_cast<double>(GetSCmp()._spread);
        markToMarketPnL            = static_cast<double>(sellPacks - buyPacks) * (GetSATP() - currentMarketSpread) * static_cast<double>(_lotSize);
    }
    return static_cast<int>(markToMarketPnL);
}

auto ButterflyStrategy::GetNetPL() const -> double { return GetRLP() + static_cast<double>(GetM2M()); }
auto ButterflyStrategy::GetFLP() const -> int { return 0; }
auto ButterflyStrategy::GetCost() const -> double { return 0.0; }

void ButterflyStrategy::OrderBiddingLogic(MarketBidding& object_, ParamLots param_, WindRate rate_, std::string name_) {
    int biddingPacks = object_._tradedLot[_biddingLeg] / (_ratio[_biddingLeg] > 0 ? _ratio[_biddingLeg] : 1);
    for (size_t h = 0; h < _numLegs; ++h) {
        if (h == _biddingLeg) continue;
        int hedgePacks = object_._tradedLot[h] / (_ratio[h] > 0 ? _ratio[h] : 1);
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
    int              quantity          = param_._quantity * _lotSize * _ratio[_biddingLeg];

    if (diff >= static_cast<int>(_minTickChange * _tickSize)) {
        if (quantity <= 0) return;
        auto status = _ms->update_order(order, _tokens[_biddingLeg], marketPrice, quantity, _uid);
        if (status != 0) {
            object_._uniqueID[_biddingLeg] = _uid.id_;
            object_._windRate              = rate_;
        }
    }
}

void ButterflyStrategy::SecondOrderBidding(MarketBidding& object_, ParamLots param_) {
    int biddingPacks = object_._tradedLot[_biddingLeg] / (_ratio[_biddingLeg] > 0 ? _ratio[_biddingLeg] : 1);

    for (size_t leg = 0; leg < _numLegs; ++leg) {
        if (leg == _biddingLeg) continue;
        int hedgePacks = object_._tradedLot[leg] / (_ratio[leg] > 0 ? _ratio[leg] : 1);
        int diff       = biddingPacks - hedgePacks;
        if (diff <= 0) {
            object_._hedgeRetryCount = 0;
            continue;
        }

        if (_marketOrderRetries > 0 && object_._hedgeRetryCount >= _marketOrderRetries) {
            fmt::print("[HEDGE RETRY EXHAUSTED Butterfly] Terminating strategy & cancelling all orders.\n");
            _active = false;
            for (size_t i = 0; i < _numLegs; ++i) {
                _longOrders._order[i]->cancel_order();
                _shortOrders._order[i]->cancel_order();
            }
            return;
        }

        int              quantity          = std::min(diff, param_._quantity) * _lotSize * _ratio[leg];
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

[[nodiscard]] auto ButterflyStrategy::GetPrice(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].price : event_.message.ask_levels[index_].price;
}
[[nodiscard]] auto ButterflyStrategy::GetQuantity(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].qty : event_.message.ask_levels[index_].qty;
}
[[nodiscard]] auto ButterflyStrategy::GetOrderCount(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].order_count_ : event_.message.ask_levels[index_].order_count_;
}
[[nodiscard]] auto ButterflyStrategy::GetAvailableQuantity(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> int {
    int quantity = 0;
    for (size_t index = 0; index < depth_; ++index) {
        quantity += GetQuantity(event_, side_, index);
    }
    return quantity;
}
[[nodiscard]] auto ButterflyStrategy::CheckOrderDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool {
    int totalOrders = 0;
    for (size_t index = 0; index < 5; ++index) {
        totalOrders += GetOrderCount(event_, side_, index);
    }
    return static_cast<size_t>(totalOrders) >= depth_;
}
[[nodiscard]] auto ButterflyStrategy::CheckPriceDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool {
    size_t validPriceLevels = 0;
    for (size_t index = 0; index < 5; ++index) {
        if (GetPrice(event_, side_, index) > 0) {
            validPriceLevels++;
        }
    }
    return validPriceLevels >= depth_;
}
