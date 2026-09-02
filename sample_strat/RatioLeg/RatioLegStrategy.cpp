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
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>

constexpr static std::string_view kLegRatioNames[] = {"0", "1", "2LegRatio", "3LegRatio", "4LegRatio", "5LegRatio", "6LegRatio"};

RatioLegStrategy::RatioLegStrategy(MinixStrategy* ms_, uint32_t strategyId_, int32_t interface_, const nlohmann::json& json_, size_t numLegs_, bool gapDiff_)
    : _ms(ms_), _strategyId(strategyId_), _interface(interface_), _numLegs(numLegs_), _gapDiff(gapDiff_) {
    // ── Strategy Name & Portfolio for Log File ──────────────────────────────
    if (json_.contains("Strategy")) {
        const auto& stratJson = json_["Strategy"];
        _stratName = stratJson.value("SubType", stratJson.value("StrategyName", ""));
        if (stratJson.contains("Portfolio")) {
            if (stratJson["Portfolio"].is_string()) {
                _portfolio = stratJson["Portfolio"].get<std::string>();
            } else if (stratJson["Portfolio"].is_number()) {
                _portfolio = std::to_string(stratJson["Portfolio"].get<int>());
            }
        } else if (stratJson.contains("PortfolioName")) {
            _portfolio = stratJson["PortfolioName"].get<std::string>();
        }
    }
    if (_stratName.empty()) {
        if (_gapDiff) {
            _stratName = (_numLegs == 4) ? "Box" : "ConRev";
        } else {
            _stratName = (_numLegs == 3) ? "Butterfly" : ((_numLegs < std::size(kLegRatioNames)) ? std::string(kLegRatioNames[_numLegs]) : "RatioLeg");
        }
    }
    if (_portfolio.empty()) {
        if (json_.contains("Portfolio")) {
            if (json_["Portfolio"].is_string()) {
                _portfolio = json_["Portfolio"].get<std::string>();
            } else if (json_["Portfolio"].is_number()) {
                _portfolio = std::to_string(json_["Portfolio"].get<int>());
            }
        } else if (json_.contains("PortfolioName")) {
            _portfolio = json_["PortfolioName"].get<std::string>();
        }
    }
    if (_portfolio.empty()) {
        _portfolio = std::to_string(_strategyId);
    }

    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_r(&tt, &local_tm);
    char timeBuf[16];
    std::strftime(timeBuf, sizeof(timeBuf), "%H%M%S", &local_tm);

    _logFileName = fmt::format("{}_{}_{}.log", _stratName, _portfolio, timeBuf);
    _logFile.open(_logFileName, std::ios::out | std::ios::app);
    writeLog(">>> [INIT] Opened log file: {}\n", _logFileName);

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

    _gap      = std::abs(details[0].strike_price_ - details[1].strike_price_);
    _lotSize  = details[0].lot_size_;
    _tickSize = details[0].tick_size_;

    for (size_t i = 0; i < _numLegs; ++i) {
        _longOrders._order[i]  = std::make_unique<OrderObjectT>(_tokens[i], _longSide[i], _lotSize, _ms->_client, _ms->_algoid, _ms->_omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
        _shortOrders._order[i] = std::make_unique<OrderObjectT>(_tokens[i], _shortSide[i], _lotSize, _ms->_client, _ms->_algoid, _ms->_omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    }
}

RatioLegStrategy::~RatioLegStrategy() {
    writeLog(">>> [DESTROY] Closing log file: {}\n", _logFileName);
    if (_logFile.is_open()) {
        _logFile.close();
    }
}

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
        _name                = strategy.value("SubType", "Garbaged");
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
    _longOrders._hedgeRetryCount.assign(_numLegs, 0);
    _shortOrders._hedgeRetryCount.assign(_numLegs, 0);
}

auto RatioLegStrategy::HasUnhedgedLots(const MarketBidding& object_) const noexcept -> bool {
    int biddingPacks = object_._tradedLot[_biddingLeg] / _ratios[_biddingLeg];
    for (size_t h = 0; h < _numLegs; ++h) {
        if (h == _biddingLeg) continue;
        if (biddingPacks != (object_._tradedLot[h] / _ratios[h])) return true;
    }
    return false;
}

auto RatioLegStrategy::CheckHedgeLegsDepth(const MarketBidding& object_, const ParamLots& param_) const -> bool {
    double thresholdPct = _thresholdQty > 0 ? _thresholdQty : 100.0;
    for (size_t h = 0; h < _numLegs; ++h) {
        if (h == _biddingLeg) continue;
        ORDER_SIDE hedgeSide = object_._order[h]->get_side();
        if (!CheckOrderDepth(_qoute[h], _orderDepth, hedgeSide) ||
            !CheckPriceDepth(_qoute[h], _priceDepth, hedgeSide)) {
            return false;
        }
        double targetQty = (param_._quantity * _ratios[h] * _lotSize) * (thresholdPct / 100.0);
        if (GetAvailableQuantity(_qoute[h], _orderDepth, hedgeSide) < targetQty) {
            return false;
        }
    }
    return true;
}

auto RatioLegStrategy::CheckBiddingLegDepth(const MarketBidding& object_) const -> bool {
    ORDER_SIDE mainSide = object_._order[_biddingLeg]->get_side();
    return CheckPriceDepth(_qoute[_biddingLeg], _allowedBidDepth, mainSide);
}

void RatioLegStrategy::EvaluateBidding(MarketBidding& object_, const ParamLots& param_, const WindRate& rate_, int multiplier_, const char* name_) {
    if (HasUnhedgedLots(object_)) {
        object_._order[_biddingLeg]->cancel_order();
        SecondOrderBidding(object_, param_, multiplier_);
        return;
    }

    bool hedgeOk   = CheckHedgeLegsDepth(object_, param_);
    bool biddingOk = CheckBiddingLegDepth(object_);

    if (hedgeOk && biddingOk) {
        OrderBiddingLogic(object_, param_, rate_, multiplier_, name_);
    } else {
        object_._order[_biddingLeg]->cancel_order();
        writeLog("evaluateBidding {} StragegyId: {} [.hedgeLegsOk = {}, biddingLegOk = {}\n", _numLegs, _strategyId, hedgeOk, biddingOk);
    }
}

void RatioLegStrategy::OnTick(const Quote& event_, int64_t nowTs_) {
    int idx = FindLegIndex(event_.header.product_id);
    if (idx < 0) return;

    ++_eventCount;
    _qoute[idx] = event_;
    if (!_active) return;

    // ponytail: check and place hedge legs FIRST if unhedged position exists
    bool longUnhedged  = HasUnhedgedLots(_longOrders);
    bool shortUnhedged = HasUnhedgedLots(_shortOrders);

    if (longUnhedged || shortUnhedged) {
        if (longUnhedged) {
            _longOrders._order[_biddingLeg]->cancel_order();
            SecondOrderBidding(_longOrders, _longParam, 1);
        }
        if (shortUnhedged) {
            _shortOrders._order[_biddingLeg]->cancel_order();
            SecondOrderBidding(_shortOrders, _shortParam, -1);
        }
        return;
    }

    EvaluateBidding(_longOrders, _longParam, GetBCmp(), 1, "Long");
    EvaluateBidding(_shortOrders, _shortParam, GetSCmp(), -1, "Short");
}

void RatioLegStrategy::OnBcast(const aef::infra::product::product_data& pd_, int64_t nowTs_) {}

void RatioLegStrategy::ProcessTradeFill(MarketBidding& object_, size_t index_, bool traded_, int lot_, uint64_t value_) noexcept {
    if (traded_) {
        object_._tradedLot[index_] += lot_;
        object_._tradeValue[index_] += value_;
        object_._cycleTradedLot[index_] += lot_;
        object_._cycleTradeValue[index_] += value_;
    }
}

void RatioLegStrategy::CheckSlippageThreshold(MarketBidding& object_, const std::vector<ORDER_SIDE>& sides_, const ParamLots& param_, int sideOfPack_, const oms_transaction& resp_) {
    if (object_._cycleTradedLot[0] <= 0) return;
    int firstLegLot = object_._cycleTradedLot[0] / _ratios[0];
    if (firstLegLot <= 0) return;

    for (size_t i = 0; i < _numLegs; ++i) {
        if (object_._cycleTradedLot[i] != firstLegLot * _ratios[i]) return;
        if (i != _biddingLeg && object_._windRate._price[i] <= 0) return;
    }

    float tradedSpread = 0.0f;
    for (size_t i = 0; i < _numLegs; ++i) {
        float legAvgPrice = static_cast<float>(object_._cycleTradeValue[i]) / (object_._cycleTradedLot[i] * _lotSize);
        writeLog("[SLIPPAGE {}Leg] [strat = {}] [leg = {}] Val: {}, Lot: {}, LotSize: {}, AvgPrice: {}\n",
                 _numLegs, _strategyId, i, object_._cycleTradeValue[i], object_._cycleTradedLot[i], _lotSize, legAvgPrice);
        tradedSpread += (sides_[i] == BUY_SIDE ? -legAvgPrice : legAvgPrice) * _ratios[i];
    }

    _tracer._orderId      = resp_.packet_.exchange_order_id;
    _tracer._time         = resp_.hdr_.exchange_timestamp;
    _tracer._qtyRemaining = 0;
    _tracer._strategyId   = _strategyId;
    _tracer._side         = sideOfPack_;
    _tracer._price        = param_._spread / 100.0F;
    _tracer._ltp          = tradedSpread / 100.0F;
    _tracer._ltq          = object_._cycleTradedLot[0] / _ratios[0];
    _tracer._slippage     = (param_._spread - tradedSpread) / 100.0F;

    writeLog("Tracer [{}Leg] symbol {} stratId {} orderId {} time {} side {} price {} ltp {} ltq {} slippage {}\n",
             _numLegs, _tracer._symbol, _strategyId, _tracer._orderId, _tracer._time, sideOfPack_, _tracer._price, _tracer._ltp, _tracer._ltq, _tracer._slippage);

    float slippage = _tracer._slippage;
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

auto RatioLegStrategy::ProcessLegResponse(MarketBidding& object_, const std::vector<ORDER_SIDE>& sides_, const ParamLots& param_, const char* sideName_, size_t index_, int sideOfPack_, const oms_transaction& resp_, bool traded_, int lot_, uint64_t value_, int price_) -> bool {
    if (resp_.hdr_.uid_.id_ != object_._uniqueID[index_]) return false;

    object_._order[index_]->handle_confirmation(resp_);
    ProcessTradeFill(object_, index_, traded_, lot_, value_);
    if (_biddingLeg == index_ && traded_) {
        object_._lastBiddingFillPrice = price_;
    }
    if (traded_) {
        CheckSlippageThreshold(object_, sides_, param_, sideOfPack_, resp_);
    }
    return true;
}

void RatioLegStrategy::OnOrderResponse(const oms_transaction& resp_) {
    if (_strategyId != resp_.hdr_.uid_.composite_id_.strategy_id) return;

    std::string_view stratName = (_numLegs < std::size(kLegRatioNames)) ? kLegRatioNames[_numLegs] : "RatioLeg";
    _ms->sendOrderResponse(resp_, _interface, stratName);

    bool     traded   = (resp_.hdr_.transaction_code == OMS_TRADE);
    auto     price    = resp_.packet_.price_;
    auto     quantity = resp_.packet_.quantity_;
    int      lot      = quantity / _lotSize;
    uint64_t value    = static_cast<uint64_t>(price) * quantity;

    for (size_t i = 0; i < _numLegs; ++i) {
        if (ProcessLegResponse(_longOrders, _longSide, _longParam, "Long", i, BUY_SIDE, resp_, traded, lot, value, price) ||
            ProcessLegResponse(_shortOrders, _shortSide, _shortParam, "Short", i, SELL_SIDE, resp_, traded, lot, value, price)) {
            break;
        }
    }

    if (traded) {
        if (HasUnhedgedLots(_longOrders)) {
            _longOrders._order[_biddingLeg]->cancel_order();
            SecondOrderBidding(_longOrders, _longParam, 1);
        }
        if (HasUnhedgedLots(_shortOrders)) {
            _shortOrders._order[_biddingLeg]->cancel_order();
            SecondOrderBidding(_shortOrders, _shortParam, -1);
        }
    }
}

auto RatioLegStrategy::ComputeRawSpread(const std::vector<ORDER_SIDE>& sides_, std::array<int, MAX_LEGS>& prices_) const -> double {
    double spread = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        prices_[i] = GetPrice(_qoute[i], sides_[i] == BUY_SIDE ? SELL_SIDE : BUY_SIDE, 0);
        spread += (sides_[i] == BUY_SIDE ? -prices_[i] : prices_[i]) * _ratios[i];
    }
    return spread;
}

auto RatioLegStrategy::AdjustGap(double spread_) const noexcept -> double {
    if (!_gapDiff) return spread_;
    return (spread_ < 0) ? (spread_ + _gap) : (spread_ - _gap);
}

auto RatioLegStrategy::GetBCmp() const -> WindRate {
    std::array<int, MAX_LEGS> prices{};
    double rawSpread = ComputeRawSpread(_longSide, prices);
    return WindRate{
        ._price  = prices,
        ._spread = static_cast<float>(AdjustGap(rawSpread)),
    };
}

auto RatioLegStrategy::GetSCmp() const -> WindRate {
    std::array<int, MAX_LEGS> prices{};
    double rawSpread = ComputeRawSpread(_shortSide, prices);
    return WindRate{
        ._price  = prices,
        ._spread = static_cast<float>(AdjustGap(rawSpread)),
    };
}

auto RatioLegStrategy::GetStrategyID() const -> uint32_t { return _strategyId; }
auto RatioLegStrategy::GetInterface() const -> int32_t { return _interface; }
auto RatioLegStrategy::GetGap() const -> int { return _gap / 100; }

auto RatioLegStrategy::CalculateTradedLots(const MarketBidding& object_) const noexcept -> int {
    if (_numLegs == 0) return 0;
    int totalPacks = object_._tradedLot[0] / _ratios[0];
    for (size_t i = 1; i < _numLegs; ++i) {
        totalPacks = std::min(totalPacks, object_._tradedLot[i] / _ratios[i]);
    }
    return totalPacks;
}

auto RatioLegStrategy::GetLongTradedLots() const -> int { return CalculateTradedLots(_longOrders); }
auto RatioLegStrategy::GetShortTradedLots() const -> int { return CalculateTradedLots(_shortOrders); }

auto RatioLegStrategy::GetBATP() const -> double {
    for (size_t i = 0; i < _numLegs; ++i) {
        if (_longOrders._tradedLot[i] == 0) return 0.0;
    }
    double spread = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        double avgPrice = static_cast<double>(_longOrders._tradeValue[i]) / (_longOrders._tradedLot[i] * _lotSize);
        spread += (_longSide[i] == BUY_SIDE ? -avgPrice : avgPrice) * _ratios[i];
    }
    return AdjustGap(spread);
}

auto RatioLegStrategy::GetSATP() const -> double {
    for (size_t i = 0; i < _numLegs; ++i) {
        if (_shortOrders._tradedLot[i] == 0) return 0.0;
    }
    double spread = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        double avgPrice = static_cast<double>(_shortOrders._tradeValue[i]) / (_shortOrders._tradedLot[i] * _lotSize);
        spread += (_shortSide[i] == BUY_SIDE ? -avgPrice : avgPrice) * _ratios[i];
    }
    return AdjustGap(spread);
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
            double markPrice = (netQty > 0) ? _qoute[i].message.bid_levels[0].price : _qoute[i].message.ask_levels[0].price;
            double avgPrice  = (netQty > 0) ? avgBuyPrice : avgSellPrice;
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

void RatioLegStrategy::OrderBiddingLogic(MarketBidding& object_, const ParamLots& param_, const WindRate& rate_, int multiplier_, const char* name_) {
    if (HasUnhedgedLots(object_)) {
        SecondOrderBidding(object_, param_, multiplier_);
        return;
    }

    if ((param_._spread > rate_._spread) || (object_._tradedLot[_biddingLeg] >= param_._totalQuantity)) {
        object_._order[_biddingLeg]->cancel_order();
        writeLog("[RatioLeg] {} StragegyId: {} user spread > market spread {} > {}", _numLegs, _strategyId, param_._spread, rate_._spread);
        return;
    }

    OrderObjectPtrT& order             = object_._order[_biddingLeg];
    int              basePrice         = GetPrice(_qoute[_biddingLeg], order->get_side(), 0);
    int              priceOffset       = _tradeGear * _tickSize;
    int              marketPrice       = (order->get_side() == BUY_SIDE) ? (basePrice + priceOffset) : (basePrice - priceOffset);
    int              currentPlacePrice = order->get_open_price();
    int              diff              = std::abs(currentPlacePrice - marketPrice);
    int              quantity          = std::min(param_._quantity * _ratios[_biddingLeg], param_._totalQuantity - object_._tradedLot[_biddingLeg]) * _lotSize;

    if (quantity > 0 && diff >= static_cast<int>(_minTickChange * _tickSize)) {
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

auto RatioLegStrategy::CalculateHedgePrice(const MarketBidding& object_, const ParamLots& param_, int multiplier_, size_t hedgeLeg_) const -> int {
    // ponytail: calculate limit price for hedge leg to strictly match user target spread
    double targetRawSpread = param_._spread;
    if (_gapDiff) {
        targetRawSpread = (multiplier_ == 1) ? (param_._spread - _gap) : (param_._spread + _gap);
    }

    const auto& sides = (multiplier_ == 1) ? _longSide : _shortSide;

    double otherSpread = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        if (i == hedgeLeg_) continue;
        int legPrice = 0;
        if (i == _biddingLeg) {
            if (object_._lastBiddingFillPrice > 0) {
                legPrice = object_._lastBiddingFillPrice;
            } else if (object_._cycleTradedLot[i] > 0) {
                legPrice = static_cast<int>(object_._cycleTradeValue[i] / (object_._cycleTradedLot[i] * _lotSize));
            } else {
                legPrice = GetPrice(_qoute[i], sides[i] == BUY_SIDE ? SELL_SIDE : BUY_SIDE, 0);
            }
        } else {
            if (object_._cycleTradedLot[i] > 0) {
                legPrice = static_cast<int>(object_._cycleTradeValue[i] / (object_._cycleTradedLot[i] * _lotSize));
            } else {
                legPrice = GetPrice(_qoute[i], sides[i] == BUY_SIDE ? SELL_SIDE : BUY_SIDE, 0);
            }
        }
        double sign = (sides[i] == BUY_SIDE ? -1.0 : 1.0);
        otherSpread += sign * legPrice * _ratios[i];
    }

    double hedgeSign         = (sides[hedgeLeg_] == BUY_SIDE ? -1.0 : 1.0);
    double neededHedgeSpread = targetRawSpread - otherSpread;
    double rawPrice          = (neededHedgeSpread / hedgeSign) / _ratios[hedgeLeg_];

    int targetPrice = static_cast<int>(std::round(rawPrice / _tickSize)) * _tickSize;
    if (targetPrice <= 0) {
        targetPrice = _tickSize;
    }
    return targetPrice;
}

void RatioLegStrategy::ExecuteHedgeLeg(MarketBidding& object_, const ParamLots& param_, int multiplier_, size_t leg_, int targetHedgeLots_) {
    int diff = targetHedgeLots_ - object_._tradedLot[leg_];
    if (diff <= 0) {
        object_._hedgeRetryCount[leg_] = 0;
        return;
    }

    size_t retryCount   = object_._hedgeRetryCount[leg_];
    bool   isAggressive = (_marketOrderRetries > 0 && retryCount >= _marketOrderRetries);

    int              quantity          = std::min(diff, param_._quantity * _ratios[leg_]) * _lotSize;
    OrderObjectPtrT& order             = object_._order[leg_];
    int              currentPlacePrice = order->get_open_price();

    int targetOrderPrice = 0;
    if (isAggressive) {
        // ponytail: when max retries exceeded, cross to opposite touch to guarantee fill (BUY at Ask, SELL at Bid)
        ORDER_SIDE marketOppositeSide = (order->get_side() == BUY_SIDE) ? SELL_SIDE : BUY_SIDE;
        targetOrderPrice              = GetPrice(_qoute[leg_], marketOppositeSide, 0);
    } else {
        // ponytail: place first order at stored snapshot price, then step aggressiveness by one tick per retry
        int storedPrice = object_._windRate._price[leg_];
        if (storedPrice <= 0) {
            storedPrice = CalculateHedgePrice(object_, param_, multiplier_, leg_);
        }
        if (order->get_side() == BUY_SIDE) {
            targetOrderPrice = storedPrice + static_cast<int>(retryCount * _tickSize);
        } else {
            targetOrderPrice = storedPrice - static_cast<int>(retryCount * _tickSize);
        }
        if (targetOrderPrice <= 0) {
            targetOrderPrice = _tickSize;
        }
    }

    if (targetOrderPrice > 0 && targetOrderPrice != currentPlacePrice) {
        auto status = _ms->update_order(order, _tokens[leg_], targetOrderPrice, quantity, _uid);
        if (status != 0) {
            writeLog("SecondOrderBidding [{}LegRatio] [mode = {}, leg = {}, retry = {}/{}, token = {}, price = {}, qty = {}]\n",
                     _numLegs, isAggressive ? "AGGRESSIVE_OPPOSITE" : "STORED_PLUS_TICKS", leg_, retryCount, _marketOrderRetries, _tokens[leg_], targetOrderPrice, quantity);
            object_._uniqueID[leg_] = _uid.id_;
            object_._hedgeRetryCount[leg_]++;
        }
    }
}

void RatioLegStrategy::SecondOrderBidding(MarketBidding& object_, const ParamLots& param_, int multiplier_) {
    int biddingPacks = object_._tradedLot[_biddingLeg] / _ratios[_biddingLeg];

    for (size_t leg = 0; leg < _numLegs; ++leg) {
        if (leg == _biddingLeg) continue;
        ExecuteHedgeLeg(object_, param_, multiplier_, leg, biddingPacks * _ratios[leg]);
    }
}

void RatioLegStrategy::CheckHedgeTimeout() {
    if (!_active) return;
    if (HasUnhedgedLots(_longOrders)) {
        _longOrders._order[_biddingLeg]->cancel_order();
        SecondOrderBidding(_longOrders, _longParam, 1);
    }
    if (HasUnhedgedLots(_shortOrders)) {
        _shortOrders._order[_biddingLeg]->cancel_order();
        SecondOrderBidding(_shortOrders, _shortParam, -1);
    }
}

auto RatioLegStrategy::GetPrice(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return (side_ == BUY_SIDE) ? event_.message.bid_levels[index_].price : event_.message.ask_levels[index_].price;
}

auto RatioLegStrategy::GetQuantity(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return (side_ == BUY_SIDE) ? event_.message.bid_levels[index_].qty : event_.message.ask_levels[index_].qty;
}

auto RatioLegStrategy::GetOrderCount(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return (side_ == BUY_SIDE) ? event_.message.bid_levels[index_].qty : event_.message.ask_levels[index_].qty;
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
        if (GetOrderCount(event_, side_, i) <= 0) return false;
    }
    return true;
}

auto RatioLegStrategy::CheckPriceDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool {
    for (size_t i = 0; i < depth_; ++i) {
        if (GetPrice(event_, side_, i) <= 0) return false;
    }
    return true;
}

auto RatioLegStrategy::IsActive() const -> bool { return _active; }
auto RatioLegStrategy::IsStopped() const -> bool { return !_active; }

void RatioLegStrategy::Print() {
    writeLog("------------------- Ratio StragegyId: {} [._eventCount = {}]", _strategyId, _eventCount);
    for (size_t index = 0; index < _numLegs; ++index) {
        writeLog("token {} Buy [._price = {}] Sell [._price = {}] LTP = {}, LTQ = {}", _tokens[index], int(_qoute[index].message.bid_levels[0].price), int(_qoute[index].message.ask_levels[0].price),
                 int(_qoute[index].message.ltp_), int(_qoute[index].message.ltq_));
    }
    _eventCount = 0;
}
