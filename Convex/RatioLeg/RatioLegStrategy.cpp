// RatioLegStrategy: lifecycle, params, events and the per-pack state machine.
#include "RatioLegStrategy.hpp"
#include "RatioLegHelpers.hpp"

#include "MinixStrategy.hpp"
#include "ProductInfo.hpp"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {

    // GUI prices are rupees; round so 195.05 becomes 19505 paise, not 19504.999.
    auto ToPaise(double rupees_) noexcept -> float { return static_cast<float>(std::round(rupees_ * 100.0)); }

}  // namespace

using namespace ratioleg;

// ═══ Lifecycle ═══════════════════════════════════════════════════════════════

RatioLegStrategy::RatioLegStrategy(MinixStrategy* hub_, uint32_t strategyId_, int32_t interface_, const nlohmann::json& json_, size_t legCount_, bool hasStrikeGap_)
    : _hub(hub_), _strategyId(strategyId_), _interface(interface_), _legCount(legCount_), _hasStrikeGap(hasStrikeGap_) {
    if (_legCount < 2 || _legCount > MAX_LEGS) {
        throw std::runtime_error(fmt::format("strategy {}: {} legs, supported 2..{}", _strategyId, _legCount, MAX_LEGS));
    }

    const std::time_t now = std::time(nullptr);
    std::tm           localTime{};
    localtime_r(&now, &localTime);
    _utcOffsetSeconds = localTime.tm_gmtoff;
    char dateText[16];
    char timeText[16];
    std::strftime(dateText, sizeof(dateText), "%Y%m%d", &localTime);
    std::strftime(timeText, sizeof(timeText), "%H%M%S", &localTime);

    const std::string logDir = fmt::format("log/{}", dateText);
    std::error_code   ignored;
    std::filesystem::create_directories(logDir, ignored);

    // The portfolio is the strategy id, as on the UI order echo.
    const int userId = json_.contains("Strategy") ? json_["Strategy"].value("UserId", 0) : 0;
    _logFileName     = fmt::format("{}/user_{}_portfolio_{}_{}.log", logDir, userId, _strategyId, timeText);
    _logFile.reset(std::fopen(_logFileName.c_str(), "a"));
    if (_logFile) {
        // Fully buffered: a line-buffered log made one write() syscall per line on the order path.
        // The hub flushes every doWork(), so a crash loses at most the lines since the last loop pass.
        std::setvbuf(_logFile.get(), nullptr, _IOFBF, LOG_BUFFER_BYTES);
    } else {
        // Without this the strategy runs with every WriteLog silently dropped.
        std::cerr << "[RatioLeg] strat=" << _strategyId << " cannot open log '" << _logFileName << "': " << std::strerror(errno) << std::endl;
    }
    WriteLog(">>> [INIT] Opened log file: {}\n", _logFileName);

    _longPack._packSide  = BUY_SIDE;
    _shortPack._packSide = SELL_SIDE;
    _ratios.fill(1);

    ParseLegs(json_);
    LoadProductDetails();
    ParamUpdate(json_);

    _clientUid.composite_id_.client_id   = static_cast<uint32_t>(_hub->_clientId);
    _clientUid.composite_id_.strategy_id = _strategyId;
    for (Pack* pack : {&_longPack, &_shortPack}) {
        for (size_t leg = 0; leg < _legCount; ++leg) {
            pack->_orders[leg]    = std::make_unique<OrderObjectT>(_tokens[leg], pack->_sides[leg], _lotSize, _hub->_clientId, _hub->_algoId, _hub->_omsId, ORDER_TYPE::LIMIT_ORDER_TYPE, _hub);
            pack->_cutOrders[leg] = std::make_unique<OrderObjectT>(_tokens[leg], Opposite(pack->_sides[leg]), _lotSize, _hub->_clientId, _hub->_algoId, _hub->_omsId, ORDER_TYPE::IOC_ORDER_TYPE, _hub);
        }
    }

    // Subscribe last: a throw above must not leave feeds pointing at a strategy that never existed.
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const bool subscribed = _hub->subscribeProduct(_tokens[leg], _hub->_feedFlags);
        WriteLog("[FEED] leg={} token={} subscribe={}\n", leg, _tokens[leg], subscribed ? "ok" : "FAILED");
        _hub->getLastQuote(_tokens[leg], _quote[leg]);
    }
}

RatioLegStrategy::~RatioLegStrategy() {
    WriteLog(">>> [DESTROY] Closing log file: {}\n", _logFileName);
}

// Tokens, sides and ratios are fixed at creation; later config updates only change params.
void RatioLegStrategy::ParseLegs(const nlohmann::json& json_) {
    const auto& legs = json_.at("Legs");
    for (size_t leg = 0; leg < _legCount && leg < legs.size(); ++leg) {
        _tokens[leg]           = legs[leg].value("Token", 0);
        _longPack._sides[leg]  = legs[leg].value("Side", "BUY") == "BUY" ? BUY_SIDE : SELL_SIDE;
        _shortPack._sides[leg] = Opposite(_longPack._sides[leg]);
    }

    if (json_.contains("Ratio") && json_["Ratio"].contains("LegRatios")) {
        const auto& legRatios = json_["Ratio"]["LegRatios"];
        for (size_t leg = 0; leg < _legCount && leg < legRatios.size(); ++leg) {
            _ratios[leg] = legRatios[leg].get<int>();
        }
    }
}

// Lot and tick size divide order quantities; trading without them would size orders from garbage.
void RatioLegStrategy::LoadProductDetails() {
    LegArray<ProductDetails> details{};
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (!_hub->getProductDetails(_tokens[leg], details[leg]) || details[leg].lot_size_ <= 0 || details[leg].tick_size_ <= 0) {
            WriteLog("[INIT] leg {} token {}: product details unavailable, strategy not created\n", leg, _tokens[leg]);
            throw std::runtime_error(fmt::format("strategy {}: leg {} token {} has no product details", _strategyId, leg, _tokens[leg]));
        }
        _isOption[leg] = details[leg].opt_type_ != aef::infra::product::OPTION_TYPE::FUTXX;
        WriteLog("[{}LegRatios] {}Leg Token: {} Symbol: {} strike = {}, lot = {} ticksize = {} ratio = {} longSide = {}\n",
                 _legCount, leg, _tokens[leg], details[leg].symbol, int(details[leg].strike_price_), int(details[leg].lot_size_), int(details[leg].tick_size_), _ratios[leg], SideName(_longPack._sides[leg]));
    }

    std::strncpy(_tradeTracer._symbol, details[BIDDING_LEG].symbol, sizeof(_tradeTracer._symbol) - 1);

    _strikeGap = std::abs(details[0].strike_price_ - details[1].strike_price_);
    _lotSize   = details[0].lot_size_;
    _tickSize  = details[0].tick_size_;
}

void RatioLegStrategy::ParamUpdate(const nlohmann::json& json_) {
    if (const auto found = json_.find("Params"); found != json_.end()) {
        const auto& params = *found;

        _longPack._params._slicePacks   = params.value("LongBuySoQ", 0);
        _longPack._params._totalPacks   = params.value("LongBuyQty", 0);
        _longPack._params._targetSpread = ToPaise(params.value("LongBuyPrice", 0.0));

        _shortPack._params._slicePacks   = params.value("ShortSellSoQ", 0);
        _shortPack._params._totalPacks   = params.value("ShortSellQty", 0);
        _shortPack._params._targetSpread = ToPaise(params.value("ShortSellPrice", 0.0));

        _repriceTicks         = params.value("TickSize", 0U);
        _orderDepth           = std::min<size_t>(params.value("OrderDepth", 1U), aef::infra::quote::QUOTE_LEVELS);
        _priceDepth           = std::min<size_t>(params.value("PriceDepth", 1U), aef::infra::quote::QUOTE_LEVELS);
        _hedgeDepthPercent    = params.value("ThresholdQty", 100);
        _allowedSlippage      = params.value("AllowedSlippage", 0);
        _tradeGear            = params.value("TradeGear", 0);
        _allowedBidDepth      = params.value("AllowedBidDepth", 0);
        const int revertBidMs = params.value("TimeToRevertBidMs", 0);
        _revertBidMs          = revertBidMs > 0 ? revertBidMs : DEFAULT_REVERT_BID_MS;

        WriteLog(
            "[RatioLeg] StrategyId: {} | Params parsed: "
            "\nLongBuySoQ: {}, \nLongBuyQty: {}, \nLongBuyPrice: {}; "
            "\nShortSellSoQ: {}, \nShortSellQty: {}, \nShortSellPrice: {}; "
            "\nTickSize: {}, \nOrderDepth: {}, \nPriceDepth: {}, "
            "\nThresholdQty: {}, \nAllowedSlippage: {}, \nTradeGear: {}, \nAllowedBidDepth: {}, \nTimeToRevertBidMs: {}, \n_biddingLeg: {}\n",
            _strategyId,
            _longPack._params._slicePacks, _longPack._params._totalPacks, _longPack._params._targetSpread,
            _shortPack._params._slicePacks, _shortPack._params._totalPacks, _shortPack._params._targetSpread,
            _repriceTicks, _orderDepth, _priceDepth,
            _hedgeDepthPercent, _allowedSlippage, _tradeGear, _allowedBidDepth, _revertBidMs, BIDDING_LEG);
    }
    RebuildCache();
}

void RatioLegStrategy::RebuildCache() {
    _tradeGearOffset  = _tradeGear * _tickSize;
    _repriceThreshold = static_cast<int>(_repriceTicks) * _tickSize;

    const double hedgeDepthFraction = (_hedgeDepthPercent > 0 ? _hedgeDepthPercent : 100) / 100.0;

    for (Pack* pack : {&_longPack, &_shortPack}) {
        LegCache& cache = pack->_cache;
        for (size_t leg = 0; leg < _legCount; ++leg) {
            cache._takeSide[leg]           = Opposite(pack->_sides[leg]);
            cache._signedRatio[leg]        = (pack->_sides[leg] == BUY_SIDE ? -1 : 1) * _ratios[leg];
            cache._sliceQuantity[leg]      = pack->_params._slicePacks * _ratios[leg] * _lotSize;
            cache._requiredHedgeDepth[leg] = cache._sliceQuantity[leg] * hedgeDepthFraction;

            WriteLog("updateSideCache [.slice = {}, ._ratio = {}, ._index = {}, ._packSide = {}]\n",
                     cache._sliceQuantity[leg], _ratios[leg], leg, static_cast<int>(pack->_packSide));
        }
    }

    for (size_t leg = 0; leg < _legCount; ++leg) {
        _buyCostRate[leg]  = _ratios[leg] * (_isOption[leg] ? OptionBuyCost : FutureBuyCost);
        _sellCostRate[leg] = _ratios[leg] * (_isOption[leg] ? OptionSellCost : FutureSellCost);
    }
}

void RatioLegStrategy::SetStatus(StrategyStatus status_) {
    if (status_ == StrategyStatus_INACTIVE) {
        Stop("status INACTIVE requested");
        return;
    }
    _status             = status_;
    _deleted            = false;
    _longPack._bidGate  = BidGate::UNKNOWN;  // log the first gate under the new status
    _shortPack._bidGate = BidGate::UNKNOWN;
    std::cout << "[RatioLeg:SetStatus] strat=" << _strategyId << " _status=" << static_cast<int>(_status)
              << " (" << StrategyStatusToString(_status) << ")" << std::endl;
    WriteLog("[RatioLeg:SetStatus] strat={} _status={} ({})\n", _strategyId, static_cast<int>(_status), StrategyStatusToString(_status));
}

void RatioLegStrategy::Stop(std::string_view reason_) {
    _status = StrategyStatus_ACTIVE;
    WriteLog("[RatioLeg:Stop] strat={} _status=ACTIVE reason='{}' long_unhedged={} short_unhedged={} long_lots=[{}] short_lots=[{}]\n",
             _strategyId, reason_, _longPack._isUnhedged, _shortPack._isUnhedged, LotsText(_longPack), LotsText(_shortPack));

    // Stop new entries only. A pack that is not whole keeps completing or cutting until it is,
    // so stopping never leaves a naked leg. A cancel refused here is retried by ProcessPack.
    for (Pack* pack : {&_longPack, &_shortPack}) {
        for (size_t leg = 0; leg < _legCount; ++leg) {
            if (leg != BIDDING_LEG && !IsWhole(*pack)) continue;
            PullOrder(*pack, leg, reason_);
        }
        pack->_bidGate = BidGate::UNKNOWN;
    }
}

// The square-off flattened every position from the trade DB, this strategy's too: drop them, so no hedge or
// cut acts on lots that are gone. The packs then read whole, so Stop() pulls every working order.
// ponytail: fills the DB had not seen at square-off time are dropped too; flatten from strategy counts if that bites.
void RatioLegStrategy::Forget(std::string_view reason_) {
    WriteLog("[RatioLeg:Forget] strat={} reason='{}' dropping long_lots=[{}] short_lots=[{}]\n", _strategyId, reason_, LotsText(_longPack), LotsText(_shortPack));
    for (Pack* pack : {&_longPack, &_shortPack}) {
        pack->_tradedLots.fill(0);
        pack->_tradedValue.fill(0);
        pack->_unreportedLots.fill(0);
        pack->_unreportedValue.fill(0);
        pack->_hedgeWaiting.fill(false);
        pack->_isUnhedged      = false;
        pack->_tradedSpread    = 0.0;
        pack->_incompleteSinceNs = 0;
    }
    Stop(reason_);
}

auto RatioLegStrategy::IsIdle() const -> bool {
    for (const Pack* pack : {&_longPack, &_shortPack}) {
        if (!IsWhole(*pack)) return false;
        for (size_t leg = 0; leg < _legCount; ++leg) {
            if (pack->_orders[leg]->get_current_state() != IDLE || pack->_cutOrders[leg]->get_current_state() != IDLE) return false;
        }
    }
    return true;
}

// ═══ Events ══════════════════════════════════════════════════════════════════

void RatioLegStrategy::OnTick(const Quote& quote_) {
    const size_t leg = FindLegIndex(quote_.header.product_id);
    if (leg == NO_LEG) return;

    // Until every leg has ticked the bid gate stays silent, so say when each feed goes live.
    if (_quote[leg].header.product_id == 0) {
        WriteLog("[FEED] leg={} token={} first tick bid={} ask={}\n", leg, _tokens[leg],
                 int{quote_.message.bid_levels[0].price}, int{quote_.message.ask_levels[0].price});
    }
    _quote[leg] = quote_;
    _eventCount += 1;
    ProcessPack(_longPack);
    ProcessPack(_shortPack);
}

void RatioLegStrategy::OnOrderResponse(const oms_transaction& response_) {
    const size_t leg  = FindLegIndex(response_.packet_.product_id_);
    const auto   side = static_cast<ORDER_SIDE>(response_.packet_.flags_.order_side);
    if (leg == NO_LEG || (side != BUY_SIDE && side != SELL_SIDE)) {
        WriteLog("[ORDER-IN] ignored: token={} side={} code={} uid={} (not a leg of this strategy)\n",
                 int{response_.packet_.product_id_}, static_cast<int>(side), int{response_.hdr_.transaction_code}, uint32_t{response_.hdr_.uid_.id_});
        return;
    }

    if (response_.hdr_.transaction_code == OMS_REQ_REJ && response_.hdr_.error_code == THROTTLE_LIMIT) {
        const uint64_t now = aef::infra::get_time_in_nanos();
        if (now >= _bidPausedUntilNs) WriteLog("[THROTTLE] uid={} rejected by the message limit: new bids paused {} ms\n", uint32_t{response_.hdr_.uid_.id_}, THROTTLE_PAUSE_NS / 1'000'000);
        _bidPausedUntilNs  = now + THROTTLE_PAUSE_NS;
        _throttleRetryAtNs = now + THROTTLE_RETRY_NS;
    }

    // A cut IOC trades the other pack's side of its leg, so its uid names its pack (read before a fill resets it).
    const uint32_t uid = response_.hdr_.uid_.id_;
    for (Pack* cutPack : {&_longPack, &_shortPack}) {
        OrderObjectT& cut = *cutPack->_cutOrders[leg];
        if (cut.get_uid() == 0 || cut.get_uid() != uid) continue;
        cut.handle_confirmation(response_);
        LogResponse(*cutPack, leg, cut, response_, true);
        if (response_.hdr_.transaction_code == OMS_TRADE) {
            OnCutTrade(*cutPack, leg, response_);
        } else if (response_.hdr_.transaction_code != OMS_REQ_REJ) {
            ProcessPack(*cutPack);
        }
        return;
    }

    // Long and short packs trade opposite sides of every leg, so the side names the pack.
    Pack&         pack  = (side == _longPack._sides[leg]) ? _longPack : _shortPack;
    OrderObjectT& order = *pack._orders[leg];

    // Only the order a response answers updates its state: a late reject of an old uid must not reset a newer order.
    if (order.get_uid() == uid) order.handle_confirmation(response_);
    LogResponse(pack, leg, order, response_, false);

    switch (response_.hdr_.transaction_code) {
        case OMS_TRADE:
            OnTrade(pack, leg, response_);
            break;
        case OMS_REQ_REJ:
            // Wait for the next tick, so a persistent reject (price band, margin) cannot resend at OMS speed.
            break;
        default:
            // An ack or cancel frees the order: act now instead of waiting for a tick.
            ProcessPack(pack);
            break;
    }
}

// A whole pack quotes while APPLIED. A pack that is not whole alternates, in any status, between completing
// (hedge the legs, or let the bid finish leg 0's part pack) and reverse-cutting the extra lots, each for
// TimeToRevertBidMs, until it is whole. A cut IOC in flight pauses the pack until it is answered.
void RatioLegStrategy::ProcessPack(Pack& pack_) {
    // Resending on every tick after a THROTTLE_LIMIT reject sent 25k rejected hedges in 0.7 s (14:31:36 log).
    if (aef::infra::get_time_in_nanos() < _throttleRetryAtNs) return;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (pack_._cutOrders[leg]->get_current_state() != IDLE) return;
    }
    const bool isWhole = IsWhole(pack_);
    if (isWhole) {
        pack_._incompleteSinceNs = 0;
    } else if (InCutWindow(pack_)) {
        // A hedge that could not fill in a whole window would fail again on re-entry: stop. A part pack keeps bidding.
        if (pack_._isUnhedged && IsActive()) Stop("hedge failed, reverse cut");
        CutPack(pack_);
        return;
    }
    if (pack_._isUnhedged) {
        HedgePack(pack_);
        return;
    }
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (leg != BIDDING_LEG) PullOrder(pack_, leg, "no hedge needed");
    }
    const bool hasPartLots = pack_._tradedLots[BIDDING_LEG] % _ratios[BIDDING_LEG] != 0;
    if (IsActive() && (isWhole || hasPartLots)) {
        EvaluateBidding(pack_);
    } else {
        PullOrder(pack_, BIDDING_LEG, IsActive() ? "pack not whole, waiting to cut" : "strategy not APPLIED");
    }
}

void RatioLegStrategy::OnTrade(Pack& pack_, size_t leg_, const oms_transaction& response_) {
    const int32_t  price    = response_.packet_.price_;
    const int32_t  quantity = response_.packet_.quantity_;
    const int32_t  lots     = quantity / _lotSize;
    const uint64_t value    = static_cast<uint64_t>(price) * static_cast<uint64_t>(quantity);

    const ORDER_SIDE side = pack_._sides[leg_];
    pack_._tradedLots[leg_] += lots;
    pack_._tradedValue[leg_] += value;
    pack_._unreportedLots[leg_] += lots;
    pack_._unreportedValue[leg_] += value;

    WriteLog("[TRADE EVENT] index: {}, token: {}, side: {} ({}), price: {}, quantity: {}, pack: {}, lots: [{}]\n",
             leg_, _tokens[leg_], SideName(side), static_cast<int>(side), price, quantity, PackName(pack_), LotsText(pack_));
    if (quantity % _lotSize != 0) {
        WriteLog("[TRADE EVENT] WARNING quantity {} is not a multiple of lot size {}; {} units not counted\n", quantity, _lotSize, quantity % _lotSize);
    }

    const bool wasUnhedged = pack_._isUnhedged;
    UpdateUnhedgedStatus(pack_);
    if (pack_._isUnhedged != wasUnhedged) {
        WriteLog("[PACK] {} {} | lots=[{}] completedPacks={}\n", PackName(pack_), pack_._isUnhedged ? "UNHEDGED" : "HEDGED", LotsText(pack_), CompletedPacks(pack_));
        pack_._bidGate = BidGate::UNKNOWN;
    }
    if (!pack_._isUnhedged) {
        pack_._tradedSpread = TradedSpread(pack_);
        CheckSlippageThreshold(pack_, response_);
    }
    ProcessPack(pack_);
}

// ═══ Status ══════════════════════════════════════════════════════════════════

auto RatioLegStrategy::IsActive() const -> bool { return _status == StrategyStatus_APPLIED; }
auto RatioLegStrategy::GetStatus() const -> StrategyStatus { return _status; }
