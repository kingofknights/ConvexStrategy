#include "RatioLegStrategy.hpp"

#include "AlgoBase.hpp"
#include "MinixStrategy.hpp"
#include "ProductInfo.hpp"
#include "Utils.hpp"
#include "oms_api.hpp"

#define FMT_HEADER_ONLY
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

RatioLegStrategy::RatioLegStrategy(MinixStrategy* ms_, uint32_t strategyId_, int32_t interface_, const nlohmann::json& json_, size_t numLegs_)
    : _ms(ms_), _strategyId(strategyId_), _interface(interface_), _numLegs(numLegs_) {
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
    _uid.composite_id_.client_id   = static_cast<uint32_t>(_ms->_client);
    _uid.composite_id_.strategy_id = _strategyId;

    for (size_t i = 0; i < _numLegs; ++i) {
        _tokens[i]    = _tokensParam[i];
        _ratios[i]    = _ratiosParam[i];
        _longSide[i]  = _longSideParam[i];
        _shortSide[i] = _shortSideParam[i];
    }

    for (int token : _tokens) {
        _ms->subscribeProduct(token, _ms->_flags);
    }

    std::vector<ProductDetails> details(_numLegs);
    for (size_t i = 0; i < _numLegs; ++i) {
        _ms->getProductDetails(_tokens[i], details[i]);
        _isOption[i] = details[i].opt_type_ != aef::infra::product::OPTION_TYPE::FUTXX;
        std::memset(_tracer._symbol, '\0', 11);
        std::memcpy(_tracer._symbol, details[i].symbol, 11);
        writeLog("[{}LegRatios] {}Leg Token: {} Symbol: {} strike = {}, lot = {} ticksize = {}\n", _numLegs, i, _tokens[i], _tracer._symbol, int(details[i].strike_price_), int(details[i].lot_size_), int(details[i].tick_size_));
    }

    _gap      = std::abs(details[0].strike_price_ - details[1].strike_price_) / 100;
    _lotSize  = details[0].lot_size_;
    _tickSize = details[0].tick_size_;

    for (size_t i = 0; i < _numLegs; ++i) {
        _longOrders._order[i]  = std::make_unique<OrderObjectT>(_tokens[i], _longSide[i], _lotSize, _ms->_client, _ms->_algoid, _ms->_omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
        _shortOrders._order[i] = std::make_unique<OrderObjectT>(_tokens[i], _shortSide[i], _lotSize, _ms->_client, _ms->_algoid, _ms->_omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
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
        _allowedSlippage    = parmas.value("AllowedSlippage", 0);
        _tradeGear          = parmas.value("TradeGear", 0);
        _marketOrderRetries = parmas.value("MarketOrderRetries", 0U);

        _orderDepth      = std::min<size_t>(_orderDepth, 5U);
        _priceDepth      = std::min<size_t>(_priceDepth, 5U);
        _allowedBidDepth = std::min<size_t>(_allowedBidDepth, 5U);

        writeLog(
            "[RatioLeg] StrategyId: {} | Params parsed: "
            "\nLongBuySoQ: {}, \nLongBuyQty: {}, \nLongBuyPrice: {}; "
            "\nShortSellSoQ: {}, \nShortSellQty: {}, \nShortSellPrice: {}; "
            "\nTickSize: {}, \nOrderDepth: {}, \nPriceDepth: {}, \nAllowedBidDepth: {}, "
            "\nThresholdQty: {}, \nAllowedSlippage: {}, \nTradeGear: {}, \nMarketOrderRetries: {}\n, _biddingLeg: {}\n",
            _strategyId,
            _longParam._quantity, _longParam._totalQuantity, _longParam._spread,
            _shortParam._quantity, _shortParam._totalQuantity, _shortParam._spread,
            _minTickChange, _orderDepth, _priceDepth, _allowedBidDepth,
            _thresholdQty, _allowedSlippage, _tradeGear, _marketOrderRetries, _biddingLeg);
    }

    // ── Strategy meta ─────────────────────────────────────────────────────────
    if (json_.contains("Strategy")) {
        const auto& strategy = json_["Strategy"];
        _isBidding           = strategy.value("IsBidding", false);
        std::string status   = strategy.value("Status", "None");
        if (status == "Applied") {
            _active = true;
        } else if (status == "Unsubscribed") {
            Stop();
        }
    }
}

void RatioLegStrategy::Stop() {
    _active = false;
    for (size_t i = 0; i < _numLegs; ++i) {
        if (_longOrders._order[i]) {
            _longOrders._order[i]->cancel_order();
        }
        if (_shortOrders._order[i]) {
            _shortOrders._order[i]->cancel_order();
        }
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
    ++_eventCount;
    _qoute[index] = event_;
    if (!_active) {
        return;
    }

    auto evaluateBidding = [&](MarketBidding& object_, ParamLots& param_, WindRate rate_, int multiplier_, std::string name_) {
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
            OrderBiddingLogic(object_, param_, rate_, multiplier_, name_);
        } else {
            object_._order[_biddingLeg]->cancel_order();
            writeLog("evaluateBidding {} StragegyId: {} [.hedgeLegsOk = {}, biddingLegOk = {}\n", _numLegs, _strategyId, hedgeLegsOk, biddingLegOk);
        }
    };

    evaluateBidding(_longOrders, _longParam, GetBCmp(), 1, "Long");
    evaluateBidding(_shortOrders, _shortParam, GetSCmp(), -1, "Short");
}

void RatioLegStrategy::OnBcast(const aef::infra::product::product_data& pd_, int64_t nowTs_) {}

void RatioLegStrategy::OnOrderResponse(const oms_transaction& resp_) {
    if (_strategyId != resp_.hdr_.uid_.composite_id_.strategy_id) {
        return;
    }
    _ms->sendOrderResponse(resp_, _interface, std::to_string(_numLegs) + "LegRatio");
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

    auto checkSlippage = [&](MarketBidding& object_, const std::vector<ORDER_SIDE>& sides_, const ParamLots& param_, const std::string& sideName, int sideOfPack_) {
        if (traded && object_._cycleTradedLot[0] > 0) {
            int firstLegLot = object_._cycleTradedLot[0] / _ratios[0];
            if (firstLegLot <= 0) return;
            bool allEqual = true;
            for (size_t i = 0; i < _numLegs; ++i) {
                if (object_._cycleTradedLot[i] != firstLegLot * _ratios[i]) {
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

            float tradedSpread = 0.0;
            float slippage     = 0.0;
            for (size_t i = 0; i < _numLegs; ++i) {
                float legAveragePrice = static_cast<float>(object_._cycleTradeValue[i]) / (object_._cycleTradedLot[i] * _lotSize);
                writeLog("[SLIPPAGE {}Leg]  [strategy = {}] [leg = {}]  Trade Value: {}, Traded Lot: {}, Lot Size: {}, Leg Average Price: {}\n",
                         _numLegs, _strategyId, i, object_._cycleTradeValue[i], object_._cycleTradedLot[i], _lotSize, legAveragePrice);
                tradedSpread += (sides_[i] == BUY_SIDE ? -legAveragePrice : legAveragePrice) * _ratios[i];
            }

            _tracer._orderId      = resp_.packet_.exchange_order_id;
            _tracer._time         = resp_.hdr_.exchange_timestamp;
            _tracer._qtyRemaining = 0;
            _tracer._strategyId   = _strategyId;
            _tracer._side         = sideOfPack_;
            _tracer._price        = param_._spread / 100.0F;
            _tracer._ltp          = static_cast<float>(tradedSpread) / 100.0F;
            _tracer._ltq          = object_._cycleTradedLot[0] / _ratios[0];
            _tracer._slippage     = (param_._spread - tradedSpread) / 100.0F;

            writeLog("Tracer [{}Leg] symbol {}  strategyId {} orderId {} time {} qtyRemaining {} side {} price {} ltp {} ltq {} slippage {}\n",
                     _numLegs, _tracer._symbol, _strategyId, _tracer._orderId, _tracer._time, _tracer._qtyRemaining, sideOfPack_, _tracer._price, _tracer._ltp, _tracer._ltq, _tracer._slippage);
            slippage = _tracer._slippage;
            _ms->sendTradeTracerToUI(_tracer, _interface);
            for (size_t i = 0; i < _numLegs; ++i) {
                object_._cycleTradedLot[i]  = 0;
                object_._cycleTradeValue[i] = 0;
            }

            if (_allowedSlippage > 0 && slippage > _allowedSlippage) {
                writeLog("[SLIPPAGE {}Leg] Slippage {} > AllowedSlippage {}. Stopping strategy.\n", _numLegs, slippage, _allowedSlippage);
                Stop();
                _ms->Registerfortermination(_strategyId);
            }
        }
    };

    auto processOrderResponse = [&](MarketBidding& object_, const std::vector<ORDER_SIDE>& sides_, const ParamLots& param_, const std::string& sideName, size_t index_, int sideOfPack_) -> bool {
        if (resp_.hdr_.uid_.id_ == object_._uniqueID[index_]) {
            handleTrade(object_, index_);
            if (_biddingLeg == index_) {
                if (traded) {
                    object_._lastBiddingFillPrice = price;
                }
            }
            checkSlippage(object_, sides_, param_, sideName, sideOfPack_);
            return true;
        }
        return false;
    };

    for (size_t i = 0; i < _numLegs; ++i) {
        if (processOrderResponse(_longOrders, _longSide, _longParam, "Long", i, BUY_SIDE) || processOrderResponse(_shortOrders, _shortSide, _shortParam, "Short", i, SELL_SIDE)) {
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

    // fmt::print("{} port {} {} prices [{}] ratio [{}] _bid {}\n", __FUNCTION__, _strategyId, _numLegs, prices, _ratios, _biddingLeg);
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
    // fmt::print("{} port {} {} prices [{}] ratio [{}] _bid {}\n", __FUNCTION__, _strategyId, _numLegs, prices, _ratios, _biddingLeg);

    return WindRate{
        ._price  = prices,
        ._spread = static_cast<float>(spread),
    };
}

auto RatioLegStrategy::GetStrategyID() const -> uint32_t { return _strategyId; }
auto RatioLegStrategy::GetInterface() const -> int32_t { return _interface; }
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
    for (size_t i = 0; i < _numLegs; ++i) {
        if (_longOrders._tradedLot[i] == 0) {
            return 0.0;
        }
    }
    double spread = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        double avgPrice = static_cast<double>(_longOrders._tradeValue[i]) / (_longOrders._tradedLot[i] * _lotSize);
        spread += (_longSide[i] == BUY_SIDE ? -avgPrice : avgPrice) * _ratios[i];
    }
    return spread;
}

auto RatioLegStrategy::GetSATP() const -> double {
    for (size_t i = 0; i < _numLegs; ++i) {
        if (_shortOrders._tradedLot[i] == 0) {
            return 0.0;
        }
    }
    double spread = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        double avgPrice = static_cast<double>(_shortOrders._tradeValue[i]) / (_shortOrders._tradedLot[i] * _lotSize);
        spread += (_shortSide[i] == BUY_SIDE ? -avgPrice : avgPrice) * _ratios[i];
    }
    return spread;
}

auto RatioLegStrategy::GetRLP() const -> double {
    double totalRLP  = 0.0;
    double totalCost = 0.0;
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

        // ponytail: calculate transaction cost on actual traded value (paise)
        double buyCostCoeff  = _isOption[i] ? OptionBuyCost : FutureBuyCost;
        double sellCostCoeff = _isOption[i] ? OptionSellCost : FutureSellCost;
        totalCost += (static_cast<double>(buyVal) * buyCostCoeff) + (static_cast<double>(sellVal) * sellCostCoeff);
    }
    return totalRLP - totalCost;
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
    double totalCost = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        double buyPrice  = _qoute[i].message.bid_levels[0].price * _ratios[i] * (_isOption[i] ? OptionBuyCost : FutureBuyCost);
        double sellPrice = _qoute[i].message.ask_levels[0].price * _ratios[i] * (_isOption[i] ? OptionSellCost : FutureSellCost);
        totalCost += buyPrice + sellPrice;
    }
    return totalCost;
}

void RatioLegStrategy::OrderBiddingLogic(MarketBidding& object_, ParamLots param_, WindRate rate_, int multiplier_, std::string name_) {
    int biddingPacks = object_._tradedLot[_biddingLeg] / _ratios[_biddingLeg];
    for (size_t h = 0; h < _numLegs; ++h) {
        if (h == _biddingLeg) continue;
        int hedgePacks = object_._tradedLot[h] / _ratios[h];
        if (biddingPacks != hedgePacks) {
            SecondOrderBidding(object_, param_);
            return;
        }
    }

    if ((param_._spread > rate_._spread) || (object_._tradedLot[_biddingLeg] >= (param_._totalQuantity))) {
        object_._order[_biddingLeg]->cancel_order();
        writeLog("[RatioLeg] {} StragegyId: {} user spread > market spread {} > {}", _numLegs, _strategyId, param_._spread, rate_._spread);
        return;
    }

    OrderObjectPtrT& order             = object_._order[_biddingLeg];
    int              basePrice         = GetPrice(_qoute[_biddingLeg], order->get_side(), 0);
    int              priceOffset       = _tradeGear * _tickSize;
    int              marketPrice       = order->get_side() == BUY_SIDE ? (basePrice + priceOffset) : (basePrice - priceOffset);
    int              currentPlacePrice = order->get_open_price();
    int              diff              = std::abs(currentPlacePrice - marketPrice);
    int              quantity          = param_._quantity * _ratios[_biddingLeg];
    int              remainQty         = param_._totalQuantity - object_._tradedLot[_biddingLeg];

    quantity = std::min(quantity, remainQty) * _lotSize;

    if (diff >= static_cast<int>(_minTickChange * _tickSize)) {
        if (quantity <= 0) return;
        auto status = _ms->update_order(order, _tokens[_biddingLeg], marketPrice, quantity, _uid);
        if (status != 0) {
            writeLog("[RatioLeg] {} StragegyId: {} spread [._user = {}, ._market = {}, ._multiplier = {}, ._name = {}], {}\n",
                     __FUNCTION__, _strategyId, param_._spread, rate_._spread, multiplier_, name_, marketPrice);
            object_._uniqueID[_biddingLeg] = _uid.id_;
            object_._windRate              = rate_;
        } else {
            writeLog("[RatioLeg] {} StragegyId: {} failed to place order [._status = {}, ._price = {}, ._quantity = {}]", _numLegs, _strategyId, status, marketPrice, quantity);
        }
    }
}

void RatioLegStrategy::SecondOrderBidding(MarketBidding& object_, ParamLots param_) {
    int biddingPacks = object_._tradedLot[_biddingLeg] / _ratios[_biddingLeg];

    for (size_t leg = 0; leg < _numLegs; ++leg) {
        if (leg == _biddingLeg) continue;
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
                writeLog("SecondOrderBidding [{}LegRatio] [._index = {}, ._tokens = {}, ._price = {}, ._quantity = {}]\n", _numLegs, leg, _tokens[leg], marketPrice, quantity);
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

auto RatioLegStrategy::IsActive() const -> bool {
    return _active;
}

auto RatioLegStrategy::IsStopped() const -> bool {
    return !_active;
}

void RatioLegStrategy::Print() {
    writeLog("------------------- Ratio StragegyId: {} [._eventCount = {}]", _strategyId, _eventCount);
    for (size_t index = 0; index < _numLegs; ++index) {
        writeLog("token {} Buy [._price = {}] Sell [._price = {}] LTP = {}, LTQ = {}", _tokens[index], int(_qoute[index].message.bid_levels[0].price), int(_qoute[index].message.ask_levels[0].price),
                 int(_qoute[index].message.ltp_), int(_qoute[index].message.ltq_));
    }
    _eventCount = 0;
}
