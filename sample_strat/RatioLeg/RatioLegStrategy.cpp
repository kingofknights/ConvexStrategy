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
#include <filesystem>

constexpr static std::string_view kLegRatioNames[] = {"0", "1", "2LegRatio", "3LegRatio", "4LegRatio", "5LegRatio", "6LegRatio"};

RatioLegStrategy::RatioLegStrategy(MinixStrategy* ms_, uint32_t strategyId_, int32_t interface_, const nlohmann::json& json_, size_t numLegs_, bool gapDiff_)
    : _ms(ms_), _strategyId(strategyId_), _interface(interface_), _numLegs(numLegs_), _gapDiff(gapDiff_) {
    const auto        now = std::chrono::system_clock::now();
    const std::time_t tt  = std::chrono::system_clock::to_time_t(now);
    std::tm           local_tm{};
    localtime_r(&tt, &local_tm);
    char dateBuf[16];
    std::strftime(dateBuf, sizeof(dateBuf), "%d%m%Y", &local_tm);
    char timeBuf[16];
    std::strftime(timeBuf, sizeof(timeBuf), "%H%M%S", &local_tm);

    const std::string logDir = fmt::format("log/{}", dateBuf);
    std::error_code   ec;
    std::filesystem::create_directories(logDir, ec);

    _logFileName = fmt::format("{}/{}Ratio_{}_{}.log", logDir, _numLegs, _strategyId, timeBuf);
    _logFile     = std::fopen(_logFileName.c_str(), "a");
    WriteLog(">>> [INIT] Opened log file: {}\n", _logFileName);

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

    for (const int token : _tokens) {
        _ms->subscribeProduct(token, _ms->_flags);
    }

    std::vector<ProductDetails> details(_numLegs);
    for (size_t i = 0; i < _numLegs; ++i) {
        _ms->getProductDetails(_tokens[i], details[i]);
        _isOption[i] = details[i].opt_type_ != aef::infra::product::OPTION_TYPE::FUTXX;
        WriteLog("[{}LegRatios] {}Leg Token: {} Symbol: {} strike = {}, lot = {} ticksize = {}\n", _numLegs, i, _tokens[i], details[i].symbol, int(details[i].strike_price_), int(details[i].lot_size_), int(details[i].tick_size_));
    }

    if (_numLegs > 0) {
        std::memset(_tracer._symbol, '\0', sizeof(_tracer._symbol));
        const size_t symIdx = (_biddingLeg < _numLegs) ? _biddingLeg : 0;
        std::strncpy(_tracer._symbol, details[symIdx].symbol, sizeof(_tracer._symbol) - 1);
    }

    _gap      = std::abs(details[0].strike_price_ - details[1].strike_price_);
    _lotSize  = details[0].lot_size_;
    _tickSize = details[0].tick_size_;

    for (size_t i = 0; i < _numLegs; ++i) {
        _longOrders._order[i]  = std::make_unique<OrderObjectT>(_tokens[i], _longSide[i], _lotSize, _ms->_client, _ms->_algoid, _ms->_omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
        _shortOrders._order[i] = std::make_unique<OrderObjectT>(_tokens[i], _shortSide[i], _lotSize, _ms->_client, _ms->_algoid, _ms->_omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    }
    RebuildCache();
}

RatioLegStrategy::~RatioLegStrategy() {
    WriteLog(">>> [DESTROY] Closing log file: {}\n", _logFileName);
    if (_logFile) {
        std::fclose(_logFile);
        _logFile = nullptr;
    }
}

void RatioLegStrategy::UpdateUnhedgedStatus(MarketBidding& object_) noexcept {
    if (_biddingLeg >= _numLegs || _ratios[_biddingLeg] <= 0) return;
    const int biddingPacks = object_._tradedLot[_biddingLeg] / _ratios[_biddingLeg];
    for (size_t h = 0; h < _numLegs; ++h) {
        if (h != _biddingLeg && _ratios[h] > 0 && biddingPacks != (object_._tradedLot[h] / _ratios[h])) {
            object_._isUnhedged = true;
            return;
        }
    }
    object_._isUnhedged = false;
}

void RatioLegStrategy::RebuildCache() {
    if (_lotSize <= 0) return;

    _tradeGearPriceOffset = _tradeGear * _tickSize;
    _minTickDiffThreshold = static_cast<int>(_minTickChange * _tickSize);

    const double thresholdPct = _thresholdQty > 0 ? _thresholdQty : 100.0;

    const auto updateSideCache = [&](LegSideCache& cache_, const ParamLots& param_, const std::vector<ORDER_SIDE>& sides_, const int mult_) {
        cache_._targetRawSpread = _gapDiff ? (param_._spread - (mult_ * _gap)) : param_._spread;
        for (size_t i = 0; i < _numLegs; ++i) {
            cache_._oppQuoteSide[i]        = (sides_[i] == BUY_SIDE) ? SELL_SIDE : BUY_SIDE;
            cache_._signedRatio[i]         = (sides_[i] == BUY_SIDE ? -1 : 1) * _ratios[i];
            cache_._hedgeTargetDepthQty[i] = (param_._quantity * _ratios[i] * _lotSize) * (thresholdPct / 100.0);
            cache_._sliceQty[i]            = param_._quantity * _ratios[i] * _lotSize;
        }
    };

    updateSideCache(_longCache, _longParam, _longSide, 1);
    updateSideCache(_shortCache, _shortParam, _shortSide, -1);

    for (size_t i = 0; i < _numLegs; ++i) {
        _buyCostCoeff[i]  = _ratios[i] * (_isOption[i] ? OptionBuyCost : FutureBuyCost);
        _sellCostCoeff[i] = _ratios[i] * (_isOption[i] ? OptionSellCost : FutureSellCost);
    }
}

void RatioLegStrategy::ParamUpdate(const nlohmann::json& json_) {
    // ── Legs ─────────────────────────────────────────────────────────────────
    const auto& legs = json_["Legs"];

    std::vector<TokenInfo> legsInfo;
    for (const auto& item : legs) {
        const int         token = item.value("Token", 0);
        const std::string side  = item.value("Side", "BUY");
        const size_t      legId = item.value("LegID", 0U);
        const bool        bid   = item.value("EnableBid", false);

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
        const TokenInfo& info = legsInfo[i];
        _tokensParam[i]       = info._token;
        _longSideParam[i]     = info._side;
        _shortSideParam[i]    = info._side == SELL_SIDE ? BUY_SIDE : SELL_SIDE;
    }

    if (json_.contains("Ratio") && json_["Ratio"].contains("LegRatios")) {
        const auto& legRatiosJson = json_["Ratio"]["LegRatios"];
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

        WriteLog(
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
        const auto& strategy     = json_["Strategy"];
        _isBidding               = strategy.value("IsBidding", false);
        const std::string status = strategy.value("Status", "None");
        _name                    = strategy.value("SubType", "Garbaged");

        _status = StringToStrategyStatus(status);

        std::cout << "[RatioLeg:UpdateConfig] strat=" << _strategyId
                  << " raw_status='" << status
                  << "' -> _status=" << static_cast<int>(_status)
                  << " (" << StrategyStatusToString(_status) << ")" << std::endl;
        WriteLog("[RatioLeg:UpdateConfig] strat={} raw_status='{}' -> _status={} ({})\n",
                 _strategyId, status, static_cast<int>(_status), StrategyStatusToString(_status));

        if (_status == StrategyStatus_INACTIVE) {
            Stop();
        }
    }
    RebuildCache();
}

void RatioLegStrategy::Stop() {
    _status = StrategyStatus_INACTIVE;
    std::cout << "[RatioLeg:Stop] strat=" << _strategyId << " _status=INACTIVE" << std::endl;
    WriteLog("[RatioLeg:Stop] strat={} _status=INACTIVE\n", _strategyId);
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
    _longOrders._isUnhedged  = false;
    _shortOrders._isUnhedged = false;
}

auto RatioLegStrategy::CheckHedgeLegsDepth(const MarketBidding& object_, const LegSideCache& cache_) const -> bool {
    for (size_t h = 0; h < _numLegs; ++h) {
        if (h == _biddingLeg) continue;
        const ORDER_SIDE hedgeSide = object_._order[h]->get_side();
        if (!CheckOrderDepth(_qoute[h], _orderDepth, hedgeSide) ||
            !CheckPriceDepth(_qoute[h], _priceDepth, hedgeSide) ||
            GetAvailableQuantity(_qoute[h], _orderDepth, hedgeSide) < cache_._hedgeTargetDepthQty[h]) {
            return false;
        }
    }
    return true;
}

auto RatioLegStrategy::CheckBiddingLegDepth(const MarketBidding& object_) const -> bool {
    const ORDER_SIDE mainSide = object_._order[_biddingLeg]->get_side();
    return CheckPriceDepth(_qoute[_biddingLeg], _allowedBidDepth, mainSide);
}

void RatioLegStrategy::EvaluateBidding(MarketBidding& object_, const ParamLots& param_, const LegSideCache& cache_, const WindRate& rate_, int multiplier_, const char* name_) {
    if (object_._isUnhedged) {
        object_._order[_biddingLeg]->cancel_order();
        SecondOrderBidding(object_, cache_, param_, multiplier_);
        return;
    }

    if (CheckHedgeLegsDepth(object_, cache_) && CheckBiddingLegDepth(object_)) {
        OrderBiddingLogic(object_, param_, cache_, rate_, multiplier_, name_);
    } else {
        object_._order[_biddingLeg]->cancel_order();
    }
}

void RatioLegStrategy::OnTick(const Quote& event_, int64_t nowTs_) {
    const int idx = FindLegIndex(event_.header.product_id);
    if (idx < 0) return;

    ++_eventCount;
    _qoute[idx] = event_;
    if (!IsActive()) return;

    // ponytail: O(1) unhedged check - zero division/loops on tick hot path
    if (_longOrders._isUnhedged || _shortOrders._isUnhedged) {
        if (_longOrders._isUnhedged) {
            _longOrders._order[_biddingLeg]->cancel_order();
            SecondOrderBidding(_longOrders, _longCache, _longParam, 1);
        }
        if (_shortOrders._isUnhedged) {
            _shortOrders._order[_biddingLeg]->cancel_order();
            SecondOrderBidding(_shortOrders, _shortCache, _shortParam, -1);
        }
        return;
    }

    EvaluateBidding(_longOrders, _longParam, _longCache, GetBCmp(), 1, "Long");
    EvaluateBidding(_shortOrders, _shortParam, _shortCache, GetSCmp(), -1, "Short");
}

void RatioLegStrategy::OnBcast(const aef::infra::product::product_data& pd_, int64_t nowTs_) {}

void RatioLegStrategy::CheckSlippageThreshold(MarketBidding& object_, const std::vector<ORDER_SIDE>& sides_, const ParamLots& param_, int sideOfPack_, const oms_transaction& resp_) {
    if (_numLegs == 0 || _lotSize <= 0 || _ratios[0] <= 0) return;

    // ponytail: fast pack calculation across legs
    int cyclePacks = object_._cycleTradedLot[0] / _ratios[0];
    for (size_t i = 1; i < _numLegs; ++i) {
        if (_ratios[i] <= 0) return;
        cyclePacks = std::min(cyclePacks, object_._cycleTradedLot[i] / _ratios[i]);
    }
    if (cyclePacks <= 0) return;

    float tradedSpread = 0.0f;
    for (size_t i = 0; i < _numLegs; ++i) {
        const float legAvgPrice = static_cast<float>(object_._cycleTradeValue[i]) / (object_._cycleTradedLot[i] * _lotSize);
        WriteLog("[SLIPPAGE {}Leg] [strat = {}] [leg = {}] Val: {}, Lot: {}, LotSize: {}, AvgPrice: {}\n",
                 _numLegs, _strategyId, i, object_._cycleTradeValue[i], object_._cycleTradedLot[i], _lotSize, legAvgPrice);
        tradedSpread += (sides_[i] == BUY_SIDE ? -legAvgPrice : legAvgPrice) * _ratios[i];
    }

    const float adjustedTradedSpread = static_cast<float>(AdjustGap(tradedSpread));

    _tracer._orderId      = resp_.packet_.exchange_order_id;
    _tracer._time         = resp_.hdr_.exchange_timestamp;
    _tracer._qtyRemaining = 0;
    _tracer._strategyId   = _strategyId;
    _tracer._side         = sideOfPack_;
    _tracer._price        = param_._spread / 100.0F;
    _tracer._ltp          = adjustedTradedSpread / 100.0F;
    _tracer._ltq          = cyclePacks;
    _tracer._slippage     = (param_._spread - adjustedTradedSpread) / 100.0F;

    if (_tracer._symbol[0] == '\0') {
        ProductDetails d{};
        const size_t   symIdx = (_biddingLeg < _numLegs) ? _biddingLeg : 0;
        if (_ms->getProductDetails(_tokens[symIdx], d) && d.symbol[0] != '\0') {
            std::strncpy(_tracer._symbol, d.symbol, sizeof(_tracer._symbol) - 1);
        } else if (!_name.empty()) {
            std::strncpy(_tracer._symbol, _name.c_str(), sizeof(_tracer._symbol) - 1);
        } else {
            auto defSym = fmt::format("{}Ratio", _numLegs);
            std::strncpy(_tracer._symbol, defSym.c_str(), sizeof(_tracer._symbol) - 1);
        }
    }

    WriteLog("Tracer [{}Leg] symbol {} stratId {} orderId {} time {} side {} price {} ltp {} ltq {} slippage {}\n",
             _numLegs, _tracer._symbol, _strategyId, _tracer._orderId, _tracer._time, sideOfPack_, _tracer._price, _tracer._ltp, _tracer._ltq, _tracer._slippage);

    const float slippage = _tracer._slippage;
    _ms->sendTradeTracerToUI(_tracer, _interface);

    for (size_t i = 0; i < _numLegs; ++i) {
        const int usedLots = cyclePacks * _ratios[i];
        if (object_._cycleTradedLot[i] > usedLots) {
            const uint64_t avgValPerLot = object_._cycleTradeValue[i] / object_._cycleTradedLot[i];
            object_._cycleTradedLot[i] -= usedLots;
            object_._cycleTradeValue[i] -= static_cast<uint64_t>(usedLots) * avgValPerLot;
        } else {
            object_._cycleTradedLot[i]  = 0;
            object_._cycleTradeValue[i] = 0;
        }
    }

    if (_allowedSlippage > 0 && slippage > _allowedSlippage) {
        WriteLog("[SLIPPAGE {}Leg] Slippage {} > AllowedSlippage {}. Stopping strategy.\n", _numLegs, slippage, _allowedSlippage);
        Stop();
        _ms->Registerfortermination(_strategyId);
    }
}

void RatioLegStrategy::OnOrderResponse(const oms_transaction& resp_) {
    if (_strategyId != resp_.hdr_.uid_.composite_id_.strategy_id) return;

    const std::string_view stratName = (_numLegs < std::size(kLegRatioNames)) ? kLegRatioNames[_numLegs] : "RatioLeg";
    _ms->sendOrderResponse(resp_, _interface, stratName);

    const int idx = FindLegIndex(resp_.packet_.product_id_);
    if (idx < 0) return;

    const auto side   = static_cast<ORDER_SIDE>(resp_.packet_.flags_.order_side);
    const bool isLong = (side == _longSide[idx]);
    if (!isLong && side != _shortSide[idx]) return;

    MarketBidding& object = isLong ? _longOrders : _shortOrders;

    // ponytail: fast O(1) order confirmation handling
    object._order[idx]->handle_confirmation(resp_);
    object._uniqueID[idx] = resp_.hdr_.uid_.id_;

    if (resp_.hdr_.transaction_code != OMS_TRADE) return;

    const auto     price    = resp_.packet_.price_;
    const auto     quantity = resp_.packet_.quantity_;
    const int      lot      = (_lotSize > 0) ? (quantity / _lotSize) : 0;
    const uint64_t value    = static_cast<uint64_t>(price) * quantity;

    WriteLog("[TRADE EVENT] index: {}, token: {}, side: {} ({}), price: {}, qty: {}\n",
             idx, resp_.packet_.product_id_, (side == BUY_SIDE ? "BUY" : "SELL"), static_cast<int>(side), price, quantity);

    object._tradedLot[idx] += lot;
    object._tradeValue[idx] += value;
    object._cycleTradedLot[idx] += lot;
    object._cycleTradeValue[idx] += value;

    if (static_cast<size_t>(idx) == _biddingLeg) {
        object._lastBiddingFillPrice = price;
    }

    UpdateUnhedgedStatus(object);

    const auto& sides      = isLong ? _longSide : _shortSide;
    const auto& param      = isLong ? _longParam : _shortParam;
    const auto& cache      = isLong ? _longCache : _shortCache;
    const int   sideOfPack = isLong ? BUY_SIDE : SELL_SIDE;
    const int   multiplier = isLong ? 1 : -1;

    CheckSlippageThreshold(object, sides, param, sideOfPack, resp_);

    if (object._isUnhedged) {
        object._order[_biddingLeg]->cancel_order();
        SecondOrderBidding(object, cache, param, multiplier);
    }
}

auto RatioLegStrategy::ComputeRawSpread(const LegSideCache& cache_, std::array<int, MAX_LEGS>& prices_) const -> double {
    double spread = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        prices_[i] = GetPrice(_qoute[i], cache_._oppQuoteSide[i], 0);
        spread += prices_[i] * cache_._signedRatio[i];
    }
    return spread;
}

auto RatioLegStrategy::AdjustGap(double spread_) const noexcept -> double {
    if (!_gapDiff) return spread_;
    return (spread_ < 0) ? (spread_ + _gap) : (spread_ - _gap);
}

auto RatioLegStrategy::GetBCmp() const -> WindRate {
    std::array<int, MAX_LEGS> prices{};
    const double              rawSpread = ComputeRawSpread(_longCache, prices);
    return WindRate{
        ._price  = prices,
        ._spread = static_cast<float>(AdjustGap(rawSpread)),
    };
}

auto RatioLegStrategy::GetSCmp() const -> WindRate {
    std::array<int, MAX_LEGS> prices{};
    const double              rawSpread = ComputeRawSpread(_shortCache, prices);
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
        const double avgPrice = static_cast<double>(_longOrders._tradeValue[i]) / (_longOrders._tradedLot[i] * _lotSize);
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
        const double avgPrice = static_cast<double>(_shortOrders._tradeValue[i]) / (_shortOrders._tradedLot[i] * _lotSize);
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

        const double avgBuyPrice  = buyQty > 0 ? static_cast<double>(buyVal) / buyQty : 0.0;
        const double avgSellPrice = sellQty > 0 ? static_cast<double>(sellVal) / sellQty : 0.0;

        if (buyQty > sellQty) {
            totalRLP += static_cast<double>(sellQty) * (avgSellPrice - avgBuyPrice);
        } else {
            totalRLP += static_cast<double>(buyQty) * (avgSellPrice - avgBuyPrice);
        }

        // ponytail: calculate transaction cost on actual traded value (paise)
        const double buyCostCoeff  = _isOption[i] ? OptionBuyCost : FutureBuyCost;
        const double sellCostCoeff = _isOption[i] ? OptionSellCost : FutureSellCost;
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

        const double avgBuyPrice  = buyQty > 0 ? static_cast<double>(buyVal) / buyQty : 0.0;
        const double avgSellPrice = sellQty > 0 ? static_cast<double>(sellVal) / sellQty : 0.0;

        const int64_t netQty = buyQty - sellQty;
        if (netQty != 0) {
            const double markPrice = (netQty > 0) ? _qoute[i].message.bid_levels[0].price : _qoute[i].message.ask_levels[0].price;
            const double avgPrice  = (netQty > 0) ? avgBuyPrice : avgSellPrice;
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
        totalCost += (_qoute[i].message.bid_levels[0].price * _buyCostCoeff[i]) +
                     (_qoute[i].message.ask_levels[0].price * _sellCostCoeff[i]);
    }
    return totalCost;
}

void RatioLegStrategy::OrderBiddingLogic(MarketBidding& object_, const ParamLots& param_, const LegSideCache& cache_, const WindRate& rate_, int multiplier_, const char* name_) {
    if (object_._isUnhedged) {
        SecondOrderBidding(object_, cache_, param_, multiplier_);
        return;
    }

    if ((param_._spread > rate_._spread) || (object_._tradedLot[_biddingLeg] >= param_._totalQuantity)) {
        object_._order[_biddingLeg]->cancel_order();
        return;
    }

    OrderObjectPtrT& order             = object_._order[_biddingLeg];
    const int        basePrice         = GetPrice(_qoute[_biddingLeg], order->get_side(), 0);
    const int        marketPrice       = (order->get_side() == BUY_SIDE) ? (basePrice + _tradeGearPriceOffset) : (basePrice - _tradeGearPriceOffset);
    const int        currentPlacePrice = order->get_open_price();
    const int        diff              = std::abs(currentPlacePrice - marketPrice);
    const int        remainingLot      = param_._totalQuantity - object_._tradedLot[_biddingLeg];
    const int        quantity          = std::min(cache_._sliceQty[_biddingLeg], remainingLot * _lotSize);

    if (quantity > 0 && diff >= _minTickDiffThreshold) {
        const auto status = _ms->update_order(order, _tokens[_biddingLeg], marketPrice, quantity, _uid);
        if (status != 0) {
            WriteLog("[RatioLeg] {} StragegyId: {} spread [._user = {}, ._market = {}, ._multiplier = {}, ._name = {}], {}\n",
                     __FUNCTION__, _strategyId, param_._spread, rate_._spread, multiplier_, name_, marketPrice);
            object_._uniqueID[_biddingLeg] = status;
            object_._windRate              = rate_;
        } else {
            WriteLog("[RatioLeg] {} StragegyId: {} failed to place order [._status = {}, ._price = {}, ._quantity = {}]\n", _numLegs, _strategyId, status, marketPrice, quantity);
        }
    }
}

auto RatioLegStrategy::CalculateHedgePrice(const MarketBidding& object_, const LegSideCache& cache_, int multiplier_, size_t hedgeLeg_) const -> int {
    const auto& sides = (multiplier_ == 1) ? _longSide : _shortSide;

    double otherSpread = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        if (i == hedgeLeg_) continue;
        int legPrice = 0;
        if (object_._cycleTradedLot[i] > 0) {
            legPrice = (i == _biddingLeg && object_._lastBiddingFillPrice > 0)
                           ? object_._lastBiddingFillPrice
                           : static_cast<int>(object_._cycleTradeValue[i] / (object_._cycleTradedLot[i] * _lotSize));
        } else {
            legPrice = GetPrice(_qoute[i], cache_._oppQuoteSide[i], 0);
        }
        otherSpread += legPrice * cache_._signedRatio[i];
    }

    const double hedgeSign         = (sides[hedgeLeg_] == BUY_SIDE ? -1.0 : 1.0);
    const double neededHedgeSpread = cache_._targetRawSpread - otherSpread;
    const double rawPrice          = (neededHedgeSpread / hedgeSign) / _ratios[hedgeLeg_];

    const int targetPrice = static_cast<int>(std::round(rawPrice / _tickSize)) * _tickSize;
    return (targetPrice <= 0) ? _tickSize : targetPrice;
}

void RatioLegStrategy::ExecuteHedgeLeg(MarketBidding& object_, const LegSideCache& cache_, const ParamLots& param_, int multiplier_, size_t leg_, int targetHedgeLots_) {
    const int diff = targetHedgeLots_ - object_._tradedLot[leg_];
    if (diff <= 0) {
        object_._hedgeRetryCount[leg_] = 0;
        return;
    }

    const size_t retryCount   = object_._hedgeRetryCount[leg_];
    const bool   isAggressive = (_marketOrderRetries > 0 && retryCount >= _marketOrderRetries);

    const int        quantity          = std::min(diff * _lotSize, cache_._sliceQty[leg_]);
    OrderObjectPtrT& order             = object_._order[leg_];
    const int        currentPlacePrice = order->get_open_price();

    int targetOrderPrice = 0;
    if (isAggressive) {
        targetOrderPrice = GetPrice(_qoute[leg_], cache_._oppQuoteSide[leg_], 0);
    } else {
        int storedPrice = object_._windRate._price[leg_];
        if (storedPrice <= 0) {
            storedPrice = CalculateHedgePrice(object_, cache_, multiplier_, leg_);
        }
        const int tickDelta = static_cast<int>(retryCount * _tickSize);
        targetOrderPrice    = (order->get_side() == BUY_SIDE) ? (storedPrice + tickDelta) : (storedPrice - tickDelta);
        if (targetOrderPrice <= 0) {
            targetOrderPrice = _tickSize;
        }
    }

    if (targetOrderPrice > 0 && targetOrderPrice != currentPlacePrice) {
        const auto status = _ms->update_order(order, _tokens[leg_], targetOrderPrice, quantity, _uid);
        if (status != 0) {
            WriteLog("SecondOrderBidding [{}LegRatio] [mode = {}, leg = {}, retry = {}/{}, token = {}, price = {}, qty = {}]\n",
                     _numLegs, isAggressive ? "AGGRESSIVE_OPPOSITE" : "STORED_PLUS_TICKS", leg_, retryCount, _marketOrderRetries, _tokens[leg_], targetOrderPrice, quantity);
            object_._uniqueID[leg_] = status;
            object_._hedgeRetryCount[leg_]++;
        }
    }
}

void RatioLegStrategy::SecondOrderBidding(MarketBidding& object_, const LegSideCache& cache_, const ParamLots& param_, int multiplier_) {
    const int biddingPacks = object_._tradedLot[_biddingLeg] / _ratios[_biddingLeg];

    for (size_t leg = 0; leg < _numLegs; ++leg) {
        if (leg == _biddingLeg) continue;
        ExecuteHedgeLeg(object_, cache_, param_, multiplier_, leg, biddingPacks * _ratios[leg]);
    }
}

void RatioLegStrategy::CheckHedgeTimeout() {
    if (!IsActive()) return;
    if (_longOrders._isUnhedged) {
        _longOrders._order[_biddingLeg]->cancel_order();
        SecondOrderBidding(_longOrders, _longCache, _longParam, 1);
    }
    if (_shortOrders._isUnhedged) {
        _shortOrders._order[_biddingLeg]->cancel_order();
        SecondOrderBidding(_shortOrders, _shortCache, _shortParam, -1);
    }
}

auto RatioLegStrategy::GetPrice(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return (side_ == BUY_SIDE) ? event_.message.bid_levels[index_].price : event_.message.ask_levels[index_].price;
}

auto RatioLegStrategy::GetQuantity(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return (side_ == BUY_SIDE) ? event_.message.bid_levels[index_].qty : event_.message.ask_levels[index_].qty;
}

auto RatioLegStrategy::GetOrderCount(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return (side_ == BUY_SIDE) ? event_.message.bid_levels[index_].order_count_ : event_.message.ask_levels[index_].order_count_;
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

auto RatioLegStrategy::IsActive() const -> bool { return _status == StrategyStatus_APPLIED; }
auto RatioLegStrategy::IsStopped() const -> bool { return _status == StrategyStatus_INACTIVE; }
auto RatioLegStrategy::GetStatus() const -> StrategyStatus { return _status; }

void RatioLegStrategy::SetStatus(StrategyStatus status_) {
    _status = status_;
    std::cout << "[RatioLeg:SetStatus] strat=" << _strategyId
              << " _status=" << static_cast<int>(_status)
              << " (" << StrategyStatusToString(_status) << ")" << std::endl;
    WriteLog("[RatioLeg:SetStatus] strat={} _status={} ({})\n",
             _strategyId, static_cast<int>(_status), StrategyStatusToString(_status));
}

void RatioLegStrategy::Print() {
    WriteLog("------------------- Ratio StragegyId: {} [._eventCount = {}]\n", _strategyId, _eventCount);
    for (size_t index = 0; index < _numLegs; ++index) {
        WriteLog("token {} Buy [._price = {}] Sell [._price = {}] LTP = {}, LTQ = {}\n", _tokens[index], int(_qoute[index].message.bid_levels[0].price), int(_qoute[index].message.ask_levels[0].price),
                 int(_qoute[index].message.ltp_), int(_qoute[index].message.ltq_));
    }
    _eventCount = 0;
}
