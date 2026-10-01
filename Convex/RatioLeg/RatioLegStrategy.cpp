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

// ═══ Lifecycle ═══════════════════════════════════════════════════════════════

RatioLegStrategy::RatioLegStrategy(MinixStrategy* ms_, uint32_t strategyId_, int32_t interface_, const nlohmann::json& json_, size_t numLegs_, bool gapDiff_)
    : _ms(ms_), _strategyId(strategyId_), _interface(interface_), _numLegs(numLegs_), _gapDiff(gapDiff_) {
    const auto        now = std::chrono::system_clock::now();
    const std::time_t tt  = std::chrono::system_clock::to_time_t(now);
    std::tm           local_tm{};
    localtime_r(&tt, &local_tm);
    char dateBuf[16];
    std::strftime(dateBuf, sizeof(dateBuf), "%Y%m%d", &local_tm);
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
    _ratios.assign(_numLegs, 1);
    _isOption.assign(_numLegs, false);
    _long.resize(_numLegs);
    _short.resize(_numLegs);
    _long._packSide  = BUY_SIDE;
    _short._packSide = SELL_SIDE;

    ParseLegs(json_);
    ParamUpdate(json_);
    _uid.composite_id_.client_id   = static_cast<uint32_t>(_ms->_client);
    _uid.composite_id_.strategy_id = _strategyId;

    for (const int token : _tokens) {
        _ms->subscribeProduct(token, _ms->_flags);
    }

    std::vector<ProductDetails> details(_numLegs);
    for (size_t i = 0; i < _numLegs; ++i) {
        _ms->getProductDetails(_tokens[i], details[i]);
        _isOption[i] = details[i].opt_type_ != aef::infra::product::OPTION_TYPE::FUTXX;
        WriteLog("[{}LegRatios] {}Leg Token: {} Symbol: {} strike = {}, lot = {} ticksize = {} ratio = {}\n",
                 _numLegs, i, _tokens[i], details[i].symbol, int(details[i].strike_price_), int(details[i].lot_size_), int(details[i].tick_size_), _ratios[i]);
    }

    std::memset(_tracer._symbol, '\0', sizeof(_tracer._symbol));
    std::strncpy(_tracer._symbol, details[_biddingLeg].symbol, sizeof(_tracer._symbol) - 1);

    _gap      = std::abs(details[0].strike_price_ - details[1].strike_price_);
    _lotSize  = details[0].lot_size_;
    _tickSize = details[0].tick_size_;

    for (Pack* pack : {&_long, &_short}) {
        for (size_t i = 0; i < _numLegs; ++i) {
            pack->_order[i] = std::make_unique<OrderObjectT>(_tokens[i], pack->_sides[i], _lotSize, _ms->_client, _ms->_algoid, _ms->_omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
        }
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

// Tokens, sides and ratios are fixed at creation; later config updates only change params.
void RatioLegStrategy::ParseLegs(const nlohmann::json& json_) {
    const auto& legs = json_.at("Legs");
    for (size_t i = 0; i < _numLegs && i < legs.size(); ++i) {
        _tokens[i]       = legs[i].value("Token", 0);
        _long._sides[i]  = legs[i].value("Side", "BUY") == "BUY" ? BUY_SIDE : SELL_SIDE;
        _short._sides[i] = _long._sides[i] == SELL_SIDE ? BUY_SIDE : SELL_SIDE;
    }

    if (json_.contains("Ratio") && json_["Ratio"].contains("LegRatios")) {
        const auto& legRatiosJson = json_["Ratio"]["LegRatios"];
        for (size_t i = 0; i < _numLegs && i < legRatiosJson.size(); ++i) {
            _ratios[i] = legRatiosJson[i].get<int>();
        }
    }
}

void RatioLegStrategy::ParamUpdate(const nlohmann::json& json_) {
    if (json_.contains("Params")) {
        const auto& parmas = json_["Params"];

        _long._param._quantity      = parmas.value("LongBuySoQ", 0);
        _long._param._totalQuantity = parmas.value("LongBuyQty", 0);
        _long._param._spread        = parmas.value("LongBuyPrice", 0.0F) * 100.0F;

        _short._param._quantity      = parmas.value("ShortSellSoQ", 0);
        _short._param._totalQuantity = parmas.value("ShortSellQty", 0);
        _short._param._spread        = parmas.value("ShortSellPrice", 0.0F) * 100.0F;

        _minTickChange      = parmas.value("TickSize", 0U);
        _orderDepth         = std::min<size_t>(parmas.value("OrderDepth", 1U), 5U);
        _priceDepth         = std::min<size_t>(parmas.value("PriceDepth", 1U), 5U);
        _allowedBidDepth    = std::min<size_t>(parmas.value("AllowedBidDepth", 1U), 5U);
        _thresholdQuantity  = parmas.value("ThresholdQty", 100);
        _allowedSlippage    = parmas.value("AllowedSlippage", 0);
        _tradeGear          = parmas.value("TradeGear", 0);
        _marketOrderRetries = parmas.value("MarketOrderRetries", 0U);

        WriteLog(
            "[RatioLeg] StrategyId: {} | Params parsed: "
            "\nLongBuySoQ: {}, \nLongBuyQty: {}, \nLongBuyPrice: {}; "
            "\nShortSellSoQ: {}, \nShortSellQty: {}, \nShortSellPrice: {}; "
            "\nTickSize: {}, \nOrderDepth: {}, \nPriceDepth: {}, \nAllowedBidDepth: {}, "
            "\nThresholdQty: {}, \nAllowedSlippage: {}, \nTradeGear: {}, \nMarketOrderRetries: {}\n, _biddingLeg: {}\n",
            _strategyId,
            _long._param._quantity, _long._param._totalQuantity, _long._param._spread,
            _short._param._quantity, _short._param._totalQuantity, _short._param._spread,
            _minTickChange, _orderDepth, _priceDepth, _allowedBidDepth,
            _thresholdQuantity, _allowedSlippage, _tradeGear, _marketOrderRetries, _biddingLeg);
    }

    if (json_.contains("Strategy")) {
        const std::string status = json_["Strategy"].value("Status", "None");
        _status                  = StringToStrategyStatus(status);

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

void RatioLegStrategy::RebuildCache() {
    _tradeGearPriceOffset = _tradeGear * _tickSize;
    _minTickDiffThreshold = static_cast<int>(_minTickChange * _tickSize);

    if (_lotSize <= 0) {
        return;
    }

    const double thresholdPercentage = _thresholdQuantity > 0 ? _thresholdQuantity : 100.0;

    for (Pack* pack : {&_long, &_short}) {
        LegSideCache&    cache = pack->_cache;
        const ParamLots& param = pack->_param;
        for (size_t i = 0; i < _numLegs; ++i) {
            cache._oppQuoteSide[i]             = (pack->_sides[i] == BUY_SIDE) ? SELL_SIDE : BUY_SIDE;
            cache._signedRatio[i]              = (pack->_sides[i] == BUY_SIDE ? -1 : 1) * _ratios[i];
            cache._hedgeTargetDepthQuantity[i] = (param._quantity * _ratios[i] * _lotSize) * (thresholdPercentage / 100.0);
            cache._sliceQuantity[i]            = param._quantity * _ratios[i] * _lotSize;

            WriteLog("updateSideCache [.slice = {}, ._ratio = {}, ._index = {}, ._packSide = {}]\n",
                     cache._sliceQuantity[i], _ratios[i], i, static_cast<int>(pack->_packSide));
        }
    }

    for (size_t i = 0; i < _numLegs; ++i) {
        _buyCostCoeff[i]  = _ratios[i] * (_isOption[i] ? OptionBuyCost : FutureBuyCost);
        _sellCostCoeff[i] = _ratios[i] * (_isOption[i] ? OptionSellCost : FutureSellCost);
    }
}

void RatioLegStrategy::Stop() {
    _status = StrategyStatus_INACTIVE;
    WriteLog("[RatioLeg:Stop] strat={} _status=INACTIVE long_unhedged={} short_unhedged={}\n",
             _strategyId, _long._isUnhedged, _short._isUnhedged);

    // Stop new entries only. An unhedged pack keeps its hedge orders working (OnTick /
    // OnOrderResponse) until flat, so stopping never leaves a naked leg.
    for (Pack* pack : {&_long, &_short}) {
        for (size_t i = 0; i < _numLegs; ++i) {
            if (!pack->_order[i] || (i != _biddingLeg && pack->_isUnhedged)) continue;
            pack->_order[i]->cancel_order();
            pack->_hedgeRetryCount[i] = 0;
        }
    }
}

// ═══ Events ══════════════════════════════════════════════════════════════════

void RatioLegStrategy::OnTick(const Quote& event_) {
    const size_t instrumentIndex = FindLegIndex(event_.header.product_id);
    if (instrumentIndex == NO_LEG) {
        return;
    }

    ++_eventCount;
    _qoute[instrumentIndex] = event_;

    // Hedging runs in every status so a stopped strategy still goes flat; new entries only when APPLIED.
    const bool active = IsActive();
    ProcessPack(_long, active);
    ProcessPack(_short, active);
}

void RatioLegStrategy::OnOrderResponse(const oms_transaction& resp_) {
    const size_t instrumentIndex = FindLegIndex(resp_.packet_.product_id_);
    if (instrumentIndex == NO_LEG) return;

    const auto side    = static_cast<ORDER_SIDE>(resp_.packet_.flags_.order_side);
    Pack*      packPtr = (side == _long._sides[instrumentIndex])    ? &_long
                         : (side == _short._sides[instrumentIndex]) ? &_short
                                                                    : nullptr;
    if (!packPtr) return;
    Pack& pack = *packPtr;

    // ponytail: fast O(1) order confirmation handling
    pack._order[instrumentIndex]->handle_confirmation(resp_);

    if (resp_.hdr_.transaction_code != OMS_TRADE) {
        // Ack/cancel frees the order: push the next hedge step now, do not wait for a tick.
        // A reject waits for the next tick so a persistent reject (price band, margin) cannot resend at OMS speed.
        if (pack._isUnhedged && resp_.hdr_.transaction_code != OMS_REQ_REJ) {
            Rehedge(pack);
        }
        return;
    }

    const auto     price    = resp_.packet_.price_;
    const auto     quantity = resp_.packet_.quantity_;
    const int      lot      = (_lotSize > 0) ? (quantity / _lotSize) : 0;
    const uint64_t value    = static_cast<uint64_t>(price) * static_cast<uint64_t>(quantity);

    WriteLog("[TRADE EVENT] index: {}, token: {}, side: {} ({}), price: {}, quantity: {}\n", instrumentIndex, resp_.packet_.product_id_, (side == BUY_SIDE ? "BUY" : "SELL"), static_cast<int>(side), price, quantity);

    pack._tradedLot[instrumentIndex] += lot;
    pack._tradeValue[instrumentIndex] += value;
    pack._cycleTradedLot[instrumentIndex] += lot;
    pack._cycleTradeValue[instrumentIndex] += value;

    UpdateUnhedgedStatus(pack);

    if (pack._isUnhedged) {
        Rehedge(pack);
    } else {
        pack._atp = ComputeATP(pack);
        CheckSlippageThreshold(pack, resp_);
    }
}

// ═══ Entry: bidding leg ══════════════════════════════════════════════════════

void RatioLegStrategy::ProcessPack(Pack& pack_, bool active_) {
    // ponytail: O(1) unhedged check - zero division/loops on tick hot path
    if (pack_._isUnhedged) {
        Rehedge(pack_);
    } else if (active_) {
        EvaluateBidding(pack_);
    }
}

void RatioLegStrategy::EvaluateBidding(Pack& pack_) {
    if (CheckHedgeLegsDepth(pack_) && CheckBiddingLegDepth(pack_)) {
        OrderBiddingLogic(pack_, ComputeRate(pack_._cache));
    } else {
        pack_._order[_biddingLeg]->cancel_order();
    }
}

auto RatioLegStrategy::CheckHedgeLegsDepth(const Pack& pack_) const -> bool {
    for (size_t hedgeIndex = 0; hedgeIndex < _numLegs; ++hedgeIndex) {
        if (hedgeIndex == _biddingLeg) continue;
        // Hedge crosses the book, so check liquidity on the side it will take (BUY hedge -> asks).
        const ORDER_SIDE takeSide = pack_._cache._oppQuoteSide[hedgeIndex];
        if (!CheckOrderDepth(_qoute[hedgeIndex], _orderDepth, takeSide) ||
            !CheckPriceDepth(_qoute[hedgeIndex], _priceDepth, takeSide) ||
            GetAvailableQuantity(_qoute[hedgeIndex], _orderDepth, takeSide) < pack_._cache._hedgeTargetDepthQuantity[hedgeIndex]) {
            return false;
        }
    }
    return true;
}

auto RatioLegStrategy::CheckBiddingLegDepth(const Pack& pack_) const -> bool {
    return CheckPriceDepth(_qoute[_biddingLeg], _allowedBidDepth, pack_._sides[_biddingLeg]);
}

void RatioLegStrategy::OrderBiddingLogic(Pack& pack_, const WindRate& rate_) {
    const ParamLots& param            = pack_._param;
    const int        biddingRatio     = _ratios[_biddingLeg] > 0 ? _ratios[_biddingLeg] : 1;
    const int        totalBiddingLots = param._totalQuantity * biddingRatio;
    const int        tradedLots       = pack_._tradedLot[_biddingLeg];
    OrderObjectPtrT& order            = pack_._order[_biddingLeg];

    if (!rate_._valid || (param._spread > rate_._spread) || (tradedLots >= totalBiddingLots)) {
        order->cancel_order();
        return;
    }

    const int marketPrice       = GetPrice(_qoute[_biddingLeg], pack_._sides[_biddingLeg], 0);
    const int diff              = std::abs(order->get_open_price() - marketPrice);
    const int remainingLot      = totalBiddingLots - tradedLots;
    const int unhedgedRemainder = tradedLots % biddingRatio;
    const int packRemainderLots = (unhedgedRemainder > 0) ? (biddingRatio - unhedgedRemainder) : (pack_._cache._sliceQuantity[_biddingLeg] / _lotSize);
    const int quantity          = std::min(packRemainderLots, remainingLot) * _lotSize;

    if (quantity > 0 && diff >= _minTickDiffThreshold) {
        if (_ms->UpdateOrder(order, _tokens[_biddingLeg], marketPrice, quantity, _uid) != 0) {
            WriteLog("[RatioLeg] {} StragegyId: {} order placed [._user = {}, ._market = {}, ], [._price = {}, ._quantity = {}]\n", __FUNCTION__, _strategyId, param._spread, rate_._spread, marketPrice, quantity);
            pack_._windRate = rate_;
        }
    }
}

// ═══ Hedge legs ══════════════════════════════════════════════════════════════

void RatioLegStrategy::UpdateUnhedgedStatus(Pack& pack_) noexcept {
    if (_ratios[_biddingLeg] <= 0) return;
    const int biddingPacks = pack_._tradedLot[_biddingLeg] / _ratios[_biddingLeg];
    for (size_t leg = 0; leg < _numLegs; ++leg) {
        if (leg == _biddingLeg) continue;
        if (_ratios[leg] > 0 && pack_._tradedLot[leg] < (biddingPacks * _ratios[leg])) {
            pack_._isUnhedged = true;
            return;
        }
    }
    pack_._isUnhedged = false;
}

void RatioLegStrategy::Rehedge(Pack& pack_) {
    pack_._order[_biddingLeg]->cancel_order();
    HedgePack(pack_);
}

void RatioLegStrategy::HedgePack(Pack& pack_) {
    const int biddingPacks = pack_._tradedLot[_biddingLeg] / _ratios[_biddingLeg];
    for (size_t leg = 0; leg < _numLegs; ++leg) {
        if (leg == _biddingLeg) continue;
        ExecuteHedgeLeg(pack_, leg, biddingPacks * _ratios[leg]);
    }
}

void RatioLegStrategy::ExecuteHedgeLeg(Pack& pack_, size_t leg_, int targetHedgeLots_) {
    const int diff = targetHedgeLots_ - pack_._tradedLot[leg_];
    if (diff <= 0) {
        pack_._hedgeRetryCount[leg_] = 0;
        return;
    }

    OrderObjectPtrT& order = pack_._order[leg_];
    if (order->is_response_pending()) {
        return;
    }

    const size_t retryCount     = pack_._hedgeRetryCount[leg_];
    const bool   isAggressive   = (_marketOrderRetries == 0 || retryCount >= _marketOrderRetries);
    const int    quantity       = std::min(diff * _lotSize, pack_._cache._sliceQuantity[leg_]);
    const int    sideMultiplier = (pack_._sides[leg_] == BUY_SIDE) ? 1 : -1;

    // ponytail: hedge legs place aggressive orders offset by _tradeGear ticks (+ retry step)
    const int storedPrice = pack_._windRate._price[leg_];
    const int basePrice   = (isAggressive || storedPrice <= 0) ? GetPrice(_qoute[leg_], pack_._cache._oppQuoteSide[leg_], 0) : storedPrice;
    if (basePrice <= 0) {
        return;  // opposite side empty: wait for a real price instead of resting near zero
    }
    const int tickDelta = isAggressive ? 0 : static_cast<int>(retryCount) * _tickSize;

    int targetOrderPrice = basePrice + (sideMultiplier * (_tradeGearPriceOffset + tickDelta));
    if (targetOrderPrice <= 0) {
        targetOrderPrice = _tickSize;
    }

    if (targetOrderPrice != order->get_open_price() || quantity != order->get_open_qty()) {
        if (_ms->UpdateOrder(order, _tokens[leg_], targetOrderPrice, quantity, _uid) != 0) {
            WriteLog("SecondOrderBidding [{}LegRatio] [mode = {}, leg = {}, retry = {}/{}, token = {}, price = {}, quantity = {}]\n",
                     _numLegs, isAggressive ? "AGGRESSIVE_OPPOSITE" : "STORED_PLUS_TICKS", leg_, retryCount, _marketOrderRetries, _tokens[leg_], targetOrderPrice, quantity);
            pack_._hedgeRetryCount[leg_]++;
        }
    }
}

void RatioLegStrategy::CheckSlippageThreshold(Pack& pack_, const oms_transaction& resp_) {
    // ponytail: fast pack calculation across legs
    int cyclePacks = pack_._cycleTradedLot[0] / _ratios[0];
    for (size_t i = 1; i < _numLegs; ++i) {
        cyclePacks = std::min(cyclePacks, pack_._cycleTradedLot[i] / _ratios[i]);
    }
    if (cyclePacks <= 0) {
        return;
    }

    double tradedSpread = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        const double legAvgPrice = static_cast<double>(pack_._cycleTradeValue[i]) / static_cast<double>(pack_._cycleTradedLot[i] * _lotSize);
        WriteLog("[SLIPPAGE {}Leg] [strat = {}] [leg = {}] Val: {}, Lot: {}, LotSize: {}, AvgPrice: {}\n",
                 _numLegs, _strategyId, i, pack_._cycleTradeValue[i], pack_._cycleTradedLot[i], _lotSize, legAvgPrice);
        tradedSpread += (pack_._sides[i] == BUY_SIDE ? -legAvgPrice : legAvgPrice) * static_cast<double>(_ratios[i]);
    }

    const auto  adjustedTradedSpread = static_cast<float>(AdjustGap(tradedSpread));
    const float slippage             = (pack_._param._spread - adjustedTradedSpread) / 100.0F;

    _tracer._orderId      = resp_.packet_.exchange_order_id;
    _tracer._time         = resp_.hdr_.exchange_timestamp;
    _tracer._qtyRemaining = pack_._param._totalQuantity - CalculateTradedLots(pack_);
    _tracer._strategyId   = _strategyId;
    _tracer._side         = pack_._packSide;
    _tracer._price        = pack_._param._spread / 100.0F;
    _tracer._ltp          = adjustedTradedSpread / 100.0F;
    _tracer._ltq          = cyclePacks;
    _tracer._slippage     = slippage >= static_cast<float>(_allowedSlippage) ? slippage : 0;

    WriteLog("Tracer [{}Leg] symbol {} stratId {} orderId {} time {} side {} price {} ltp {} ltq {} slippage {}\n",
             _numLegs, _tracer._symbol, _strategyId, _tracer._orderId, _tracer._time, _tracer._side, _tracer._price, _tracer._ltp, _tracer._ltq, _tracer._slippage);

    _ms->SendTradeTracerToUi(_tracer, _interface);

    for (size_t i = 0; i < _numLegs; ++i) {
        const int usedLots = cyclePacks * _ratios[i];
        if (pack_._cycleTradedLot[i] > usedLots) {
            const uint64_t avgValPerLot = pack_._cycleTradeValue[i] / pack_._cycleTradedLot[i];
            pack_._cycleTradedLot[i] -= usedLots;
            pack_._cycleTradeValue[i] -= static_cast<uint64_t>(usedLots) * avgValPerLot;
        } else {
            pack_._cycleTradedLot[i]  = 0;
            pack_._cycleTradeValue[i] = 0;
        }
    }

    if (_allowedSlippage > 0 && slippage > static_cast<float>(_allowedSlippage)) {
        WriteLog("[SLIPPAGE {}Leg] Slippage {} > AllowedSlippage {}. Stopping strategy.\n", _numLegs, slippage, _allowedSlippage);
        Stop();
    }
}

// ═══ Pricing ═════════════════════════════════════════════════════════════════

auto RatioLegStrategy::ComputeRate(const LegSideCache& cache_) const -> WindRate {
    WindRate rate{};
    double   spread = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        rate._price[i] = GetPrice(_qoute[i], cache_._oppQuoteSide[i], 0);
        // Missing price on any leg means the spread is not tradable: report 0.
        if (rate._price[i] <= 0) {
            rate._spread = 0.0F;
            return rate;
        }
        spread += rate._price[i] * cache_._signedRatio[i];
    }
    rate._spread = static_cast<float>(AdjustGap(spread));
    rate._valid  = true;
    return rate;
}

auto RatioLegStrategy::AdjustGap(double spread_) const noexcept -> double {
    if (!_gapDiff) {
        return spread_;
    }
    return (spread_ < 0) ? (spread_ + _gap) : (spread_ - _gap);
}

auto RatioLegStrategy::GetBCmp() const -> WindRate { return ComputeRate(_long._cache); }
auto RatioLegStrategy::GetSCmp() const -> WindRate { return ComputeRate(_short._cache); }

// ═══ Positions / PnL (UI) ════════════════════════════════════════════════════

auto RatioLegStrategy::CalculateTradedLots(const Pack& pack_) const noexcept -> int {
    int totalPacks = pack_._tradedLot[0] / _ratios[0];
    for (size_t i = 1; i < _numLegs; ++i) {
        totalPacks = std::min(totalPacks, pack_._tradedLot[i] / _ratios[i]);
    }
    return totalPacks;
}

auto RatioLegStrategy::GetLongTradedLots() const -> int { return CalculateTradedLots(_long); }
auto RatioLegStrategy::GetShortTradedLots() const -> int { return CalculateTradedLots(_short); }

auto RatioLegStrategy::ComputeATP(const Pack& pack_) const -> double {
    for (size_t i = 0; i < _numLegs; ++i) {
        if (pack_._tradedLot[i] == 0) {
            return 0.0;
        }
    }
    double spread = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        const double avgPrice = static_cast<double>(pack_._tradeValue[i]) / (pack_._tradedLot[i] * _lotSize);
        spread += (pack_._sides[i] == BUY_SIDE ? -avgPrice : avgPrice) * _ratios[i];
    }
    return AdjustGap(spread);
}

// ATP is cached on hedged fills so in-flight unhedged lots do not distort it.
auto RatioLegStrategy::GetBATP() const -> double { return _long._atp; }
auto RatioLegStrategy::GetSATP() const -> double { return _short._atp; }

auto RatioLegStrategy::PositionOf(size_t leg_) const -> LegPosition {
    LegPosition position;
    for (const Pack* pack : {&_long, &_short}) {
        const int64_t  quantity = static_cast<int64_t>(pack->_tradedLot[leg_]) * _lotSize;
        const uint64_t value    = pack->_tradeValue[leg_];
        if (pack->_sides[leg_] == BUY_SIDE) {
            position._buyQuantity += quantity;
            position._buyValue += value;
        } else {
            position._sellQuantity += quantity;
            position._sellValue += value;
        }
    }
    return position;
}

auto RatioLegStrategy::GetRLP() const -> double {
    double totalRLP  = 0.0;
    double totalCost = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        const LegPosition position = PositionOf(i);
        totalRLP += static_cast<double>(std::min(position._buyQuantity, position._sellQuantity)) * (position.AvgSell() - position.AvgBuy());

        // ponytail: calculate transaction cost on actual traded value (paise)
        const double buyCostCoeff  = _isOption[i] ? OptionBuyCost : FutureBuyCost;
        const double sellCostCoeff = _isOption[i] ? OptionSellCost : FutureSellCost;
        totalCost += (static_cast<double>(position._buyValue) * buyCostCoeff) + (static_cast<double>(position._sellValue) * sellCostCoeff);
    }
    return totalRLP - totalCost;
}

auto RatioLegStrategy::GetCutPL() const -> double { return GetRLP(); }

auto RatioLegStrategy::GetM2M() const -> double {
    double totalM2M = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        const LegPosition position    = PositionOf(i);
        const int64_t     netQuantity = position._buyQuantity - position._sellQuantity;
        if (netQuantity != 0) {
            const double markPrice = (netQuantity > 0) ? _qoute[i].message.bid_levels[0].price : _qoute[i].message.ask_levels[0].price;
            const double avgPrice  = (netQuantity > 0) ? position.AvgBuy() : position.AvgSell();
            totalM2M += static_cast<double>(netQuantity) * (markPrice - avgPrice);
        }
    }
    return totalM2M;
}

auto RatioLegStrategy::GetNetPL() const -> double { return GetRLP() + GetM2M(); }
auto RatioLegStrategy::GetFLP() const -> int { return _qoute[_biddingLeg].message.ltp_; }

auto RatioLegStrategy::GetCost() const -> double {
    double totalCost = 0.0;
    for (size_t i = 0; i < _numLegs; ++i) {
        totalCost += (_qoute[i].message.bid_levels[0].price * _buyCostCoeff[i]) +
                     (_qoute[i].message.ask_levels[0].price * _sellCostCoeff[i]);
    }
    return totalCost;
}

auto RatioLegStrategy::GetStrategyID() const -> uint32_t { return _strategyId; }
auto RatioLegStrategy::GetInterface() const -> int32_t { return _interface; }
auto RatioLegStrategy::GetGap() const -> int { return _gap / 100; }

// ═══ Status ══════════════════════════════════════════════════════════════════

auto RatioLegStrategy::IsActive() const -> bool { return _status == StrategyStatus_APPLIED; }
auto RatioLegStrategy::GetStatus() const -> StrategyStatus { return _status; }

void RatioLegStrategy::SetStatus(StrategyStatus status_) {
    _status = status_;
    std::cout << "[RatioLeg:SetStatus] strat=" << _strategyId
              << " _status=" << static_cast<int>(_status)
              << " (" << StrategyStatusToString(_status) << ")" << '\n';
    WriteLog("[RatioLeg:SetStatus] strat={} _status={} ({})\n",
             _strategyId, static_cast<int>(_status), StrategyStatusToString(_status));
}

void RatioLegStrategy::Print() {
    if (++_printCount < 10) {
        return;
    }
    WriteLog("------------------- Ratio StragegyId: {} [._eventCount = {}]\n", _strategyId, _eventCount);
    for (size_t index = 0; index < _numLegs; ++index) {
        WriteLog("token {} Buy [._price = {}] Sell [._price = {}] LTP = {}, LTQ = {}\n", _tokens[index], int(_qoute[index].message.bid_levels[0].price), int(_qoute[index].message.ask_levels[0].price),
                 int(_qoute[index].message.ltp_), int(_qoute[index].message.ltq_));
    }
    _printCount = 0;
    _eventCount = 0;
}

// ═══ Order book helpers ══════════════════════════════════════════════════════

auto RatioLegStrategy::GetPrice(const Quote& event_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int {
    return (side_ == BUY_SIDE) ? event_.message.bid_levels[levelIndex_].price : event_.message.ask_levels[levelIndex_].price;
}

auto RatioLegStrategy::GetQuantity(const Quote& event_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int {
    return (side_ == BUY_SIDE) ? event_.message.bid_levels[levelIndex_].qty : event_.message.ask_levels[levelIndex_].qty;
}

auto RatioLegStrategy::GetOrderCount(const Quote& event_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int {
    return (side_ == BUY_SIDE) ? event_.message.bid_levels[levelIndex_].order_count_ : event_.message.ask_levels[levelIndex_].order_count_;
}

auto RatioLegStrategy::GetAvailableQuantity(const Quote& event_, size_t depth_, ORDER_SIDE side_) noexcept -> int {
    int totalQuantity = 0;
    for (size_t levelIndex = 0; levelIndex < depth_; ++levelIndex) {
        totalQuantity += GetQuantity(event_, side_, levelIndex);
    }
    return totalQuantity;
}

auto RatioLegStrategy::CheckOrderDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) noexcept -> bool {
    for (size_t levelIndex = 0; levelIndex < depth_; ++levelIndex) {
        if (GetOrderCount(event_, side_, levelIndex) <= 0) {
            return false;
        }
    }
    return true;
}

auto RatioLegStrategy::CheckPriceDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) noexcept -> bool {
    for (size_t levelIndex = 0; levelIndex < depth_; ++levelIndex) {
        if (GetPrice(event_, side_, levelIndex) <= 0) {
            return false;
        }
    }
    return true;
}
