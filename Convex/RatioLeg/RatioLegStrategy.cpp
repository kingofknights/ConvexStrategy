#include "RatioLegStrategy.hpp"

#include "MinixStrategy.hpp"
#include "ProductInfo.hpp"
#include "RatioLegHelpers.hpp"
#include "Utils.hpp"

#include <fmt/format.h>
#include <oms_api.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace {
    auto ToPaise(double rupees_) -> float { return static_cast<float>(std::round(rupees_ * 100.0)); }
}  // namespace

using namespace ratioleg;

// ═══ Lifecycle ═══════════════════════════════════════════════════════════════

RatioLegStrategy::RatioLegStrategy(MinixStrategy* hub_, uint32_t strategyId_, int32_t interface_, const nlohmann::json& json_, size_t legCount_, bool hasStrikeGap_)
    : _hub(hub_),
      _strategyId(strategyId_),
      _interface(interface_),
      _legCount(legCount_),
      _hasStrikeGap(hasStrikeGap_) {
    _logFile = OpenLogFile(json_.contains("Strategy") ? json_["Strategy"].value("UserId", 0U) : 0U, _strategyId);
    WriteLog(">>> [INIT] Opened log file strategyId={}\n", _strategyId);

    _longPack._packSide  = BUY_SIDE;
    _shortPack._packSide = SELL_SIDE;
    _ratios.fill(1);

    const auto& legs = json_.at("Legs");

    for (size_t leg = 0; leg < _legCount and leg < legs.size(); ++leg) {
        _tokens[leg]            = legs[leg].value("Token", 0);
        ORDER_SIDE side         = legs[leg].value("Side", "BUY") == "BUY" ? BUY_SIDE : SELL_SIDE;
        ORDER_SIDE oppositeSide = Opposite(side);

        _longPack._sides[leg]         = side;
        _longPack._oppositeSide[leg]  = oppositeSide;
        _shortPack._sides[leg]        = oppositeSide;
        _shortPack._oppositeSide[leg] = side;

        if (!_hub->getProductDetails(_tokens[leg], _product_data[leg])) {
            WriteLog("[INIT] leg {} token {}: product details unavailable, strategy not created\n", leg, _tokens[leg]);
            throw std::runtime_error(fmt::format("strategy {}: leg {} token {} has no product details", _strategyId, leg, _tokens[leg]));
        }
        _isOption[leg] = _product_data[leg].opt_type_ != aef::infra::product::OPTION_TYPE::FUTXX;
    }

    if (json_.contains("Ratio") && json_["Ratio"].contains("LegRatios")) {
        const auto& legRatios = json_["Ratio"]["LegRatios"];
        for (size_t leg = 0; leg < _legCount && leg < legRatios.size(); ++leg) {
            int sign                     = _longPack._sides[leg] == ORDER_SIDE::BUY_SIDE ? -1 : 1;
            _ratios[leg]                 = legRatios[leg].get<int>();
            _longPack._signedRatio[leg]  = _ratios[leg] * sign * 1;
            _shortPack._signedRatio[leg] = _ratios[leg] * sign * -1;
        }
    }

    for (size_t leg = 0; leg < _legCount; ++leg) {
        WriteLog("[{}LegRatios] {}Leg Token: {} Symbol: {} strike = {}, lot = {} ticksize = {} ratio = {} longSide = {}\n",
                 _legCount, leg, _tokens[leg], _product_data[leg].symbol, int(_product_data[leg].strike_price_), int(_product_data[leg].lot_size_), int(_product_data[leg].tick_size_), _ratios[leg], SideName(_longPack._sides[leg]));
    }

    std::strncpy(_tradeTracer._symbol, _product_data[BiddingLeg].symbol, sizeof(_tradeTracer._symbol) - 1);
    _strikeGap          = std::abs(_product_data[0].strike_price_ - _product_data[1].strike_price_);
    _lotSize            = _product_data[0].lot_size_;
    _tickSize           = _product_data[0].tick_size_;
    _spread._strategyId = _strategyId;
    _spread._gap        = static_cast<float>(_strikeGap / 100);

    ParamUpdate(json_);

    _clientUid.composite_id_.client_id   = static_cast<uint32_t>(_hub->_clientId);
    _clientUid.composite_id_.strategy_id = _strategyId;

    for (Pack* pack : {&_longPack, &_shortPack}) {
        for (size_t leg = 0; leg < _legCount; ++leg) {
            pack->_orders[leg] = std::make_unique<OrderObjectT>(_tokens[leg], pack->_sides[leg], _lotSize, _hub->_clientId, _hub->_algoId, _hub->_omsId, ORDER_TYPE::LIMIT_ORDER_TYPE, _hub);
        }
        pack->_reverse = std::make_unique<OrderObjectT>(_tokens[BiddingLeg], Opposite(pack->_sides[BiddingLeg]), _lotSize, _hub->_clientId, _hub->_algoId, _hub->_omsId, ORDER_TYPE::LIMIT_ORDER_TYPE, _hub);
    }

    for (size_t leg = 0; leg < _legCount; ++leg) {
        const bool subscribed = _hub->subscribeProduct(_tokens[leg], _hub->_feedFlags);
        WriteLog("[FEED] leg={} token={} subscribe={}\n", leg, _tokens[leg], subscribed ? "ok" : "FAILED");
        _hub->getLastQuote(_tokens[leg], _quote[leg]);
    }
}

RatioLegStrategy::~RatioLegStrategy() {
    WriteLog(">>> [DESTROY] Closing log file\n");
}

void RatioLegStrategy::ParamUpdate(const nlohmann::json& json_) {
    if (const auto found = json_.find("Params"); found != json_.end()) {
        const auto& params = *found;

        _longPack._singleOrderPacks  = params.value("LongBuySoQ", 0);
        _longPack._totalAllowedPacks = params.value("LongBuyQty", 0);
        _longPack._targetSpread      = ToPaise(params.value("LongBuyPrice", 0.0));

        _shortPack._singleOrderPacks  = params.value("ShortSellSoQ", 0);
        _shortPack._totalAllowedPacks = params.value("ShortSellQty", 0);
        _shortPack._targetSpread      = ToPaise(params.value("ShortSellPrice", 0.0));

        _orderDepth        = params.value("OrderDepth", 1);
        _priceDepth        = std::min<size_t>(params.value("PriceDepth", 1U), aef::infra::quote::QUOTE_LEVELS);
        _hedgeDepthPercent = params.value("ThresholdQty", 100);
        _allowedSlippage   = params.value("AllowedSlippage", 0);
        _allowedBidDepth   = std::min<size_t>(params.value("AllowedBidDepth", 0U), aef::infra::quote::QUOTE_LEVELS);
        _minChangeTick     = std::max(params.value("TickSize", 2), 2);
        _sweepBufferLevel  = params.value("SweepBufferLevel", 150);
        _sweepStopLevel    = params.value("SweepStopLevel", 200);
        _sweepBuffer       = ToPaise(params.value("SweepBuffer", 0.25));
        _hedgeHoldNs       = static_cast<uint64_t>(std::max(params.value("HedgeHoldUs", 300), 0)) * 1000;
        _hedgeHoldCover    = params.value("HedgeHoldCover", 5);
        _maxConfirmNs      = static_cast<uint64_t>(std::max(params.value("MaxConfirmUs", 5000), 0)) * 1000;

        WriteLog(
            "[RatioLeg] StrategyId: {} | Params parsed: "
            "\nLongBuySoQ: {}, \nLongBuyQty: {}, \nLongBuyPrice: {}; "
            "\nShortSellSoQ: {}, \nShortSellQty: {}, \nShortSellPrice: {}; "
            "\nOrderDepth: {}, \nPriceDepth: {}, "
            "\nThresholdQty: {}, \nAllowedSlippage: {}, \nAllowedBidDepth: {}, \nTickSize: {}, \n_biddingLeg: {}, "
            "\nSweepBufferLevel: {}, \nSweepStopLevel: {}, \nSweepBuffer: {}, "
            "\nHedgeHoldUs: {}, \nHedgeHoldCover: {}, \nMaxConfirmUs: {}\n",
            _strategyId, _longPack._singleOrderPacks, _longPack._totalAllowedPacks, _longPack._targetSpread,
            _shortPack._singleOrderPacks, _shortPack._totalAllowedPacks, _shortPack._targetSpread, _orderDepth, _priceDepth,
            _hedgeDepthPercent, _allowedSlippage, _allowedBidDepth, _minChangeTick, BiddingLeg,
            _sweepBufferLevel, _sweepStopLevel, _sweepBuffer, _hedgeHoldNs / 1000, _hedgeHoldCover, _maxConfirmNs / 1000);
    }
    _depthCheckedNs = 0;
}

void RatioLegStrategy::SetStatus(StrategyStatus status_) {
    if (status_ == StrategyStatus_INACTIVE) {
        Stop("status INACTIVE requested");
        return;
    }
    _status  = status_;
    _deleted = false;
    if (_squaredOff && PacksWhole()) {
        _squaredOff = false;  // re-applied after a square-off with nothing left open: trades on from the kept lots
    }
    WriteLog("[RatioLeg:SetStatus] strat={} _status={} ({}){}\n", _strategyId, static_cast<int>(_status), StrategyStatusToString(_status),
             _squaredOff ? " squared off with an open pack: still no orders" : "");
}

void RatioLegStrategy::Stop(std::string_view reason_) {
    _status = StrategyStatus_ACTIVE;
    for (Pack* pack : {&_longPack, &_shortPack}) {
        Pull(*pack, BiddingLeg, reason_);
    }
    WriteLog("[RatioLeg:Stop] strat={} _status=ACTIVE reason='{}' long_lots=[{}] short_lots=[{}]\n",
             _strategyId, reason_, LotsText(_longPack), LotsText(_shortPack));
    FlushOrderOuts();  // reason_ is only borrowed: write the cancel lines before returning
}

// From doWork, once a second while the OMS is not throttling: a stopped strategy's leg 0 order whose cancel at Stop
// was throttled, or could not go yet (order still in flight), is cancelled now, with no tick needed.
void RatioLegStrategy::CancelIfStopped() {
    if (IsActive()) {
        return;
    }
    if (_squaredOff) {
        PullAll("squared off: cancel retry");
    } else {
        for (Pack* pack : {&_longPack, &_shortPack}) {
            Pull(*pack, BiddingLeg, "stopped: cancel retry");
        }
    }
    FlushOrderOuts();
}

// GUI square-off took the positions. Lots, values and P&L stay as traded, so the GUI keeps showing them, but the
// strategy sends nothing more: a hedge or reverse would re-open a leg square-off just closed. Live orders are cancelled.
void RatioLegStrategy::SquaredOff(std::string_view reason_) {
    _squaredOff = true;
    PullAll(reason_);
    Stop(reason_);
    WriteLog("[RatioLeg:SquaredOff] strat={} reason='{}' lots kept, no orders until re-applied with every pack whole\n", _strategyId, reason_);
}

// Every live order of the strategy, the reverse included.
void RatioLegStrategy::PullAll(std::string_view reason_) {
    for (Pack* pack : {&_longPack, &_shortPack}) {
        for (size_t leg = 0; leg < _legCount; ++leg) {
            Pull(*pack, leg, reason_);
        }
        OrderObjectT& reverse = *pack->_reverse;
        if (!_hub->Throttled() && reverse.is_order_confirmed() && !reverse.is_response_pending() && reverse.cancel_order()) {
            _hub->NoteRequest(reverse.get_uid());
            RecordOrderOut({pack, BiddingLeg, Opposite(pack->_sides[BiddingLeg]), "CANCEL", reverse.get_uid(), reverse.get_open_price(), reverse.get_open_price(),
                            reverse.get_open_qty(), 0, reason_});
        }
    }
}

// Every leg holds whole packs and nothing waits to be sent back.
auto RatioLegStrategy::PacksWhole() const -> bool {
    for (const Pack* pack : {&_longPack, &_shortPack}) {
        if (pack->_reverseLots != 0) {
            return false;
        }
        for (size_t leg = 0; leg < _legCount; ++leg) {
            if (pack->_tradedLots[leg] != pack->_tradedPack * _ratios[leg]) {
                return false;
            }
        }
    }
    return true;
}

// No order live, and every pack whole; after a square-off an open pack is square-off's, not ours.
auto RatioLegStrategy::IsIdle() const -> bool {
    for (const Pack* pack : {&_longPack, &_shortPack}) {
        if (pack->_reverse->get_current_state() != IDLE) {
            return false;
        }
        for (size_t leg = 0; leg < _legCount; ++leg) {
            if (pack->_orders[leg]->get_current_state() != IDLE) {
                return false;
            }
        }
    }
    return _squaredOff || PacksWhole();
}

// ═══ Events ══════════════════════════════════════════════════════════════════

void RatioLegStrategy::OnTick(const Quote& quote_) {
    const size_t leg = FindLegIndex(quote_.header.product_id);
    if (leg == NoLeg) {
        return;
    }
    _quote[leg] = quote_;
    TrackTouch(leg, quote_);
    _eventCount += 1;
    _sweep             = LegSweep();
    const int lastBand = _sweepBand;
    _sweepBand         = (_sweepStopLevel > 0 && _sweep > _sweepStopLevel) ? 2 : (_sweepBufferLevel > 0 && _sweep > _sweepBufferLevel) ? 1 : 0;
    // A band change is logged at most once per GateLogNs, after this tick's orders.
    uint64_t sweepLogGapNs = 0;
    bool     logSweep      = false;
    if (_sweepBand != lastBand) {
        ++_sweepChanges;
        if (const uint64_t now = _hub->LatestExchangeNs(); now >= _sweepLogNs + GateLogNs) {
            sweepLogGapNs = _sweepLogNs == 0 ? 0 : now - _sweepLogNs;
            _sweepLogNs   = now;
            logSweep      = true;
        }
    }
    if (_squaredOff) {
        PullAll("squared off");
        FlushOrderOuts();
        return;
    }
    if (leg == BiddingLeg && quote_.message.updatetype_.update_type == 'S') {
        TryEarlyHedge(_longPack, quote_.message.updatetype_.update_side);
        TryEarlyHedge(_shortPack, quote_.message.updatetype_.update_side);
    }
    for (Pack* pack : {&_longPack, &_shortPack}) {
        if (pack->_reverseLots > 0) {
            Reverse(*pack);
        }
    }
    if (_longPack._hedgeMode) {
        EvaluateHedgeMode(_longPack);
    } else {
        EvaluateBidding(_longPack);
    }
    if (_shortPack._hedgeMode) {
        EvaluateHedgeMode(_shortPack);
    } else {
        EvaluateBidding(_shortPack);
    }
    FlushOrderOuts();
    if (logSweep) {
        WriteLog("[SWEEP] strat={} sweep={} msgs/ms band {} -> {} ({}), {} band changes since the last line ({} ms)\n", _strategyId, _sweep, lastBand,
                 _sweepBand, _sweepBand == 2 ? "leg 0 holds, pulled below target" : _sweepBand == 1 ? "leg 0 buffer" : "normal",
                 std::exchange(_sweepChanges, 0), sweepLogGapNs / 1'000'000);
    }
}

void RatioLegStrategy::OnOrderResponse(const oms_transaction& response_) {
    const size_t leg  = FindLegIndex(response_.packet_.product_id_);
    const auto   side = static_cast<ORDER_SIDE>(response_.packet_.flags_.order_side);
    const auto   uid  = uint32_t{response_.hdr_.uid_.id_};
    const auto   code = int32_t{response_.hdr_.transaction_code};
    if (leg == NoLeg || (side != BUY_SIDE && side != SELL_SIDE)) {
        WriteLog("[ORDER-IN] ignored: token={} side={} code={} uid={} (not a leg of this strategy)\n", int{response_.packet_.product_id_}, static_cast<int>(side), code, uid);
        return;
    }

    for (Pack* reversing : {&_longPack, &_shortPack}) {
        OrderObjectT& reverse = *reversing->_reverse;
        if (leg == BiddingLeg && reverse.get_uid() == uid) {
            reverse.handle_confirmation(response_);
            if (code == OMS_REQ_REJ) {
                reversing->_reverseRejectedNs = aef::infra::get_time_in_nanos();
            }
            LogResponse(*reversing, leg, OrderSnapshot{reverse}, response_, true);
            if (code == OMS_TRADE) {
                OnReverseTrade(*reversing, response_);
            }
            return;
        }
    }

    Pack&         pack  = (side == _longPack._sides[leg]) ? _longPack : _shortPack;
    OrderObjectT& order = *pack._orders[leg];
    if (order.get_uid() == uid) {
        order.handle_confirmation(response_);
        if (code == OMS_REQ_REJ) {
            pack._rejectedNs[leg] = aef::infra::get_time_in_nanos();
        }
    }
    // Hedges for a fill go out before the [ORDER-IN] line; a hedge may reuse this order, so its state is taken now.
    const OrderSnapshot state{order};
    if (code == OMS_TRADE) {
        OnTrade(pack, leg, response_);
    }
    LogResponse(pack, leg, state, response_, false);
    FlushOrderOuts();
}

// ═══ Decision: everything is worked out again on every event ═════════════════

void RatioLegStrategy::EvaluateBidding(Pack& pack_) {
    if (pack_._reverseLots > 0) {
        Pull(pack_, BiddingLeg, "reversing leg 0");
        return;
    }
    // Leg 0 hedged early on an 'S' tick: it may be filled already, so no reprice or cancel. The early hedges follow the book until a fill decides.
    const OrderObjectT& bid = *pack_._orders[BiddingLeg];
    if (bid.get_uid() == pack_._earlyHedgeUid && bid.get_current_state() != IDLE) {
        HedgeEarly(pack_);
        return;
    }
    // Every leg is level here, so a hedge still working is an early hedge whose leg 0 went away unfilled.
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (leg != BiddingLeg) {
            Pull(pack_, leg, "early hedge without a leg 0 fill");
        }
    }
    if (not IsActive()) {
        Pull(pack_, BiddingLeg, "Strategy is not active");
        return;
    }
    LegArrayT<int>             prices{};
    LegArrayT<int>             deeper{};
    const std::optional<float> market = MarketSpread(pack_, &prices, 0);
    if (!market or !MarketSpread(pack_, &deeper, 2)) {
        Pull(pack_, BiddingLeg, "no price on a leg");
        return;
    }

    // Cushion: what the hedge legs lose two levels deeper, the level they are hedged at. The bidding leg's own depth is ours to set, so it is left out.
    // Refreshed at most once a second, so level-2 noise alone does not move the bid.
    if (const uint64_t now = aef::infra::get_time_in_nanos(); now - pack_._cushionNs >= CushionRefreshNs) {
        pack_._cushion   = 0.0;
        pack_._cushionNs = now;
        for (size_t leg = 0; leg < _legCount; ++leg) {
            if (leg != BiddingLeg) {
                pack_._cushion += static_cast<double>(prices[leg] - deeper[leg]) * pack_._signedRatio[leg];
            }
        }
    }
    const double cushion = pack_._cushion;

    // P1 = p0 + (target + cushion - market) / signedRatio0, rounded to the tick away from the market (BUY down, SELL up).
    const bool   buy     = pack_._sides[BiddingLeg] == BUY_SIDE;
    const auto   rounded = [&](double price_) { return static_cast<int>(buy ? std::floor(price_ / _tickSize) : std::ceil(price_ / _tickSize)) * _tickSize; };
    const double p1      = prices[BiddingLeg] + ((pack_._targetSpread + cushion - *market) / pack_._signedRatio[BiddingLeg]);
    int          price   = rounded(p1);
    if (price <= 0) {
        Pull(pack_, BiddingLeg, "target needs a price <= 0");
        return;
    }

    int trade     = pack_._tradedLots[0];
    int tradePack = pack_._tradedPack;

    int quantity = std::min({
                       (pack_._totalAllowedPacks * _ratios[0]) - trade,               // lots left overall
                       ((tradePack + pack_._singleOrderPacks) * _ratios[0]) - trade,  // lots left in this slice
                   }) *
                   _lotSize;
    if (quantity <= 0) {
        Pull(pack_, BiddingLeg, "nothing left to bid");
        return;
    }

    // Entry gate. A resting bid is never pulled for a sweep alone: queue place is what fills it. In a sweep it goes
    // only when its own price no longer makes the target at the hedge legs' live touch.
    const OrderObjectT& order  = *pack_._orders[BiddingLeg];
    const bool          live   = order.get_current_state() != IDLE;
    const int           open   = order.get_open_price();
    const auto          toward = [&](int price_) { return !live || (buy ? price_ > open : price_ < open); };
    if (_sweepBand == 2 && live && open > 0 && !SpreadPresent(pack_, open)) {
        Pull(pack_, BiddingLeg, "sweep above SweepStopLevel and the resting spread below target");
        return;
    }
    // A new bid or a move toward the market waits out a sweep or a slow exchange, takes the buffer in the buffer band,
    // and does not cross on a hedge touch that is being eaten or just appeared thin. Moves away and size changes always go through.
    if (toward(price)) {
        if (_sweepBand == 2) {
            Hold(pack_, "sweep above SweepStopLevel");
            return;
        }
        if (_maxConfirmNs > 0 && _hub->ExchangeLagNs() > _maxConfirmNs) {
            Hold(pack_, "exchange round trip above MaxConfirmUs");
            return;
        }
        if (_sweepBand == 1) {
            price = rounded(p1 + ((buy ? -1.0 : 1.0) * _sweepBuffer / std::abs(pack_._signedRatio[BiddingLeg])));
            if (!toward(price)) {
                Hold(pack_, "sweep buffer keeps the resting bid");
                return;
            }
        }
        if (const size_t leg = FreshHedge(pack_, quantity); leg != NoLeg && Crosses(pack_, price)) {
            Hold(pack_, "hedge touch younger than HedgeHoldUs", leg);
            return;
        }
    }
    Hold(pack_, {});

    // Never rest deeper than AllowedBidDepth levels on our own side; improving or crossing the touch is fine.
    // AllowedBidDepth 0, or that level empty in a thin book: no limit.
    if (_allowedBidDepth > 0) {
        const int deepest = Price(_quote[BiddingLeg], pack_._sides[BiddingLeg], _allowedBidDepth - 1);
        if (deepest > 0 && (pack_._sides[BiddingLeg] == BUY_SIDE ? price < deepest : price > deepest)) {
            Pull(pack_, BiddingLeg, "deeper than AllowedBidDepth levels");
            return;
        }
    }
    // A resting bid within _minChangeTick ticks of the new price stays, either way, while a fill at its price still makes the target.
    if (order.is_order_confirmed() && order.get_open_qty() == quantity && std::abs(price - open) < _minChangeTick * _tickSize && SpreadPresent(pack_, open)) {
        return;
    }
    UpdateOrder(pack_, BiddingLeg, price, quantity, "BID");
}

void RatioLegStrategy::EvaluateHedgeMode(Pack& pack_) {
    LegArrayT<int> tradedPacks{};
    for (size_t leg = 0; leg < _legCount; ++leg) {
        tradedPacks[leg] = pack_._tradedLots[leg] / _ratios[leg];
    }
    const auto minMax = std::ranges::minmax(std::span{tradedPacks}.first(_legCount));
    if (minMax.min == minMax.max) {
        pack_._hedgeMode  = false;
        pack_._tradedPack = minMax.max;
        return;
    }
    pack_._hedgeMode = true;

    const int      target = std::min(minMax.max, pack_._totalAllowedPacks);
    LegArrayT<int> missingLots{};
    for (size_t leg = 0; leg < _legCount; ++leg) {
        missingLots[leg] = (target * _ratios[leg]) - pack_._tradedLots[leg];
    }
    SendHedges(pack_, missingLots, "HEDGE");
}

// A synthetic trade on leg 0's own side that leaves our bid better than that side's top has probably filled it:
// hedge the whole open bid now instead of waiting for the fill. Once per leg 0 uid.
void RatioLegStrategy::TryEarlyHedge(Pack& pack_, int updateSide_) {
    const OrderObjectT& order = *pack_._orders[BiddingLeg];
    const ORDER_SIDE    side  = pack_._sides[BiddingLeg];
    if (not IsActive() || pack_._hedgeMode || updateSide_ != side || !order.is_order_confirmed() || order.is_response_pending() || order.get_uid() == pack_._earlyHedgeUid) {
        return;
    }
    const int top  = Price(_quote[BiddingLeg], side, 0);
    const int open = order.get_open_price();
    if (top > 0 && (side == BUY_SIDE ? open <= top : open >= top)) {
        return;
    }
    if (!SpreadPresent(pack_, open)) {
        WriteLog("[EARLY-HEDGE] {} 'S' tick: leg 0 {} uid={} open {}@{} beats top {}, spread gone: no hedge, the fill decides\n",
                 PackName(pack_), SideName(side), order.get_uid(), order.get_open_qty(), open, top);
        return;
    }
    pack_._earlyHedgeUid = order.get_uid();
    HedgeEarly(pack_);
    WriteLog("[EARLY-HEDGE] {} 'S' tick: leg 0 {} uid={} open {}@{} beats top {}: hedged\n",
             PackName(pack_), SideName(side), order.get_uid(), order.get_open_qty(), open, top);
}

// Hedges for leg 0's traded plus open lots, at the opposite side's index 2 like any hedge.
// Leg 0 itself is not chased here: EvaluateHedgeMode does that once a hedge fill puts the hedge legs ahead.
void RatioLegStrategy::HedgeEarly(Pack& pack_) {
    const int packs = (pack_._tradedLots[BiddingLeg] + (pack_._orders[BiddingLeg]->get_open_qty() / _lotSize)) / _ratios[BiddingLeg];
    LegArrayT<int> missingLots{};
    for (size_t leg = 0; leg < _legCount; ++leg) {
        missingLots[leg] = leg == BiddingLeg ? 0 : (packs * _ratios[leg]) - pack_._tradedLots[leg];
    }
    SendHedges(pack_, missingLots, "EARLY-HEDGE");
}

// Sends every leg with missing lots at the opposite side's index 2, highest price first, throttle or not.
void RatioLegStrategy::SendHedges(Pack& pack_, const LegArrayT<int>& missingLots_, std::string_view reason_) {
    LegArrayT<int>    prices{};
    LegArrayT<size_t> order{};
    for (size_t leg = 0; leg < _legCount; ++leg) {
        prices[leg] = Price(_quote[leg], pack_._oppositeSide[leg], 2);
        order[leg]  = leg;
    }
    const std::span legs = std::span{order}.first(_legCount);
    std::ranges::stable_sort(legs, std::ranges::greater{}, [&](size_t leg_) { return prices[leg_]; });
    for (const size_t leg : legs) {
        if (missingLots_[leg] > 0) {
            Send(pack_, *pack_._orders[leg], leg, pack_._sides[leg], prices[leg], missingLots_[leg] * _lotSize, reason_, true);
        }
    }
}

// Sends leg 0's unwanted lots back at its own side's index 2 (a BUY sells into the bids), repriced every tick until filled.
void RatioLegStrategy::Reverse(Pack& pack_) {
    const int price = Price(_quote[BiddingLeg], pack_._sides[BiddingLeg], 2);
    Send(pack_, *pack_._reverse, BiddingLeg, Opposite(pack_._sides[BiddingLeg]), price, pack_._reverseLots * _lotSize, "REVERSE");
}

void RatioLegStrategy::OnReverseTrade(Pack& pack_, const oms_transaction& response_) {
    const int32_t price = response_.packet_.price_;
    const int32_t lots  = std::min(response_.packet_.quantity_ / _lotSize, pack_._reverseLots);
    if (lots <= 0) {
        WriteLog("[REVERSE] {} fill {}@{} with no lots left to reverse: ignored\n", PackName(pack_), int32_t{response_.packet_.quantity_}, price);
        return;
    }
    const uint64_t entry  = pack_._reverseValue * static_cast<uint64_t>(lots) / static_cast<uint64_t>(pack_._reverseLots);
    const uint64_t exit   = static_cast<uint64_t>(price) * static_cast<uint64_t>(lots * _lotSize);
    const bool     bought = pack_._sides[BiddingLeg] == BUY_SIDE;
    const auto     buy    = static_cast<double>(bought ? entry : exit);
    const auto     sell   = static_cast<double>(bought ? exit : entry);
    _unwindPnL += sell - buy - (buy * (_isOption[BiddingLeg] ? OptionBuyCost : FutureBuyCost)) - (sell * (_isOption[BiddingLeg] ? OptionSellCost : FutureSellCost));

    pack_._reverseLots -= lots;
    pack_._reverseValue -= entry;
    WriteLog("[REVERSE] {} leg 0 {} lots back @{}, {} lots left to reverse, unwind P&L {}\n", PackName(pack_), lots, price, pack_._reverseLots, _unwindPnL);
    SendTracer(pack_, response_, pack_._targetSpread / 100.0F, static_cast<float>(price) / 100.0F, lots, 0, "Reverse");
}

void RatioLegStrategy::OnTrade(Pack& pack_, size_t leg_, const oms_transaction& response_) {
    const int32_t  price    = response_.packet_.price_;
    const int32_t  quantity = response_.packet_.quantity_;
    const int32_t  lots     = quantity / _lotSize;
    const uint64_t value    = static_cast<uint64_t>(price) * static_cast<uint64_t>(quantity);

    if (leg_ == BiddingLeg) {
        // Hedges already out for this fill (early on an 'S' tick, or leg 0 chased behind hedge fills): reversing would leave them naked.
        bool hedged = uint32_t{response_.hdr_.uid_.id_} == pack_._earlyHedgeUid;
        for (size_t leg = 0; leg < _legCount; ++leg) {
            hedged = hedged || pack_._tradedLots[leg] * _ratios[BiddingLeg] > pack_._tradedLots[BiddingLeg] * _ratios[leg];
        }
        if (!hedged && !SpreadPresent(pack_, price)) {
            pack_._reverseLots += lots;
            pack_._reverseValue += static_cast<uint64_t>(price) * static_cast<uint64_t>(lots * _lotSize);
            Pull(pack_, BiddingLeg, "spread gone, reversing leg 0");
            Reverse(pack_);
            WriteLog("[TRADE EVENT] index: 0, token: {}, {} {}@{}: spread gone at the hedge legs' top, reversing {} lots, book=[{}]\n",
                     _tokens[BiddingLeg], PackName(pack_), quantity, price, pack_._reverseLots, BookText());
            return;
        }
    }

    pack_._tradedLots[leg_] += lots;
    pack_._tradedValue[leg_] += value;

    // Hedges go out first, the trade is logged after them.
    EvaluateHedgeMode(pack_);

    const ORDER_SIDE side = pack_._sides[leg_];
    WriteLog("[TRADE EVENT] index: {}, token: {}, side: {} ({}), price: {}, quantity: {}, pack: {}, lots: [{}]\n",
             leg_, _tokens[leg_], SideName(side), static_cast<int>(side), price, quantity, PackName(pack_), LotsText(pack_));
    if (quantity % _lotSize != 0) {
        WriteLog("[TRADE EVENT] WARNING quantity {} is not a multiple of lot size {}; {} units not counted\n", quantity, _lotSize, quantity % _lotSize);
    }

    if (!pack_._hedgeMode) {
        double spread  = 0.0;
        bool   allLegs = true;
        for (size_t leg = 0; leg < _legCount; ++leg) {
            if (pack_._tradedLots[leg] == 0) {
                allLegs = false;
                break;
            }
            const double average = static_cast<double>(pack_._tradedValue[leg]) / (static_cast<double>(pack_._tradedLots[leg]) * _lotSize);
            spread += (pack_._sides[leg] == BUY_SIDE ? -average : average) * _ratios[leg];
        }
        pack_._tradedSpread = allLegs ? AdjustGap(spread) : 0.0;
        ReportPacks(pack_, response_);
    }
}

// One Trade Tracer row per batch of packs completed since the last row, with the slippage against the target.
void RatioLegStrategy::ReportPacks(Pack& pack_, const oms_transaction& response_) {
    const int newPacks = pack_._tradedPack - (pack_._reportedLots[BiddingLeg] / _ratios[BiddingLeg]);
    if (newPacks <= 0) {
        return;
    }

    double spread = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const int32_t  lots     = pack_._tradedLots[leg] - pack_._reportedLots[leg];
        const uint64_t value    = pack_._tradedValue[leg] - pack_._reportedValue[leg];
        const int32_t  newLots  = newPacks * _ratios[leg];
        const uint64_t newValue = value * static_cast<uint64_t>(newLots) / static_cast<uint64_t>(lots);
        const double   average  = static_cast<double>(newValue) / (static_cast<double>(newLots) * _lotSize);
        spread += (pack_._sides[leg] == BUY_SIDE ? -average : average) * _ratios[leg];

        pack_._reportedLots[leg] += newLots;
        pack_._reportedValue[leg] += newValue;
    }

    const auto  traded   = static_cast<float>(AdjustGap(spread));
    const float slippage = pack_._targetSpread - traded;
    const bool  slipped  = std::round(slippage) > static_cast<float>(_allowedSlippage);
    // Stop cancels the bid, so it goes before the Tracer row and the log.
    if (slipped) {
        Stop("slippage above AllowedSlippage");
    }
    SendTracer(pack_, response_, pack_._targetSpread / 100.0F, traded / 100.0F, newPacks, slipped ? slippage / 100.0F : 0, slipped ? "Slippage" : "");
    if (slipped) {
        WriteLog("[SLIPPAGE] {} slippage {} paise > AllowedSlippage {} paise: stopped\n", PackName(pack_), slippage, _allowedSlippage);
    }
}

void RatioLegStrategy::SendTracer(const Pack& pack_, const oms_transaction& response_, float price_, float ltp_, int ltq_, float slippage_, std::string_view message_) {
    _tradeTracer._orderId      = response_.packet_.exchange_order_id;
    _tradeTracer._time         = response_.hdr_.exchange_timestamp;
    _tradeTracer._qtyRemaining = pack_._totalAllowedPacks - pack_._tradedPack;
    _tradeTracer._strategyId   = _strategyId;
    _tradeTracer._side         = pack_._packSide;
    _tradeTracer._price        = price_;
    _tradeTracer._ltp          = ltp_;
    _tradeTracer._ltq          = ltq_;
    _tradeTracer._slippage     = slippage_;
    std::memset(_tradeTracer._message, 0, sizeof(_tradeTracer._message));
    std::memcpy(_tradeTracer._message, message_.data(), std::min(message_.size(), sizeof(_tradeTracer._message) - 1));

    WriteLog("Tracer [{}Leg] symbol {} stratId {} orderId {} time {} side {} price {} ltp {} ltq {} slippage {} message '{}'\n",
             _legCount, _tradeTracer._symbol, _strategyId, _tradeTracer._orderId, _tradeTracer._time, _tradeTracer._side, _tradeTracer._price, _tradeTracer._ltp,
             _tradeTracer._ltq, _tradeTracer._slippage, message_);
    _hub->SendToUi(UiMessageCode_TRADE_TRACER, _interface, _tradeTracer);
}

// ═══ UI ══════════════════════════════════════════════════════════════════════

auto RatioLegStrategy::SpreadUpdate(uint64_t nowNs_) -> std::optional<StrategySpreadUpdate> {
    double rlp  = _unwindPnL;
    double m2m  = 0.0;
    double cost = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        int64_t  buyQuantity  = 0;
        int64_t  sellQuantity = 0;
        uint64_t buyValue     = 0;
        uint64_t sellValue    = 0;
        for (const Pack* pack : {&_longPack, &_shortPack}) {
            const int64_t quantity = static_cast<int64_t>(pack->_tradedLots[leg]) * _lotSize;
            if (pack->_sides[leg] == BUY_SIDE) {
                buyQuantity += quantity;
                buyValue += pack->_tradedValue[leg];
            } else {
                sellQuantity += quantity;
                sellValue += pack->_tradedValue[leg];
            }
        }
        const double avgBuy   = buyQuantity > 0 ? static_cast<double>(buyValue) / buyQuantity : 0.0;
        const double avgSell  = sellQuantity > 0 ? static_cast<double>(sellValue) / sellQuantity : 0.0;
        const double buyRate  = _isOption[leg] ? OptionBuyCost : FutureBuyCost;
        const double sellRate = _isOption[leg] ? OptionSellCost : FutureSellCost;
        rlp += (static_cast<double>(std::min(buyQuantity, sellQuantity)) * (avgSell - avgBuy)) - (static_cast<double>(buyValue) * buyRate) - (static_cast<double>(sellValue) * sellRate);

        const int     bid  = _quote[leg].message.bid_levels[0].price;
        const int     ask  = _quote[leg].message.ask_levels[0].price;
        const int64_t net  = buyQuantity - sellQuantity;
        const int     mark = net > 0 ? bid : ask;
        if (net != 0 && mark > 0) {
            m2m += static_cast<double>(net) * (mark - (net > 0 ? avgBuy : avgSell));
        }
        cost += _ratios[leg] * ((bid * buyRate) + (ask * sellRate));
    }
    _spread._rlp      = static_cast<float>(rlp) / 100.0F;
    _spread._cutPL    = _spread._rlp;
    _spread._m2m      = static_cast<float>(m2m) / 100.0F;
    _spread._netPL    = _spread._rlp + _spread._m2m;
    _spread._cost     = static_cast<float>(cost) / 100.0F;
    _spread._bcmp     = MarketSpread(_longPack, nullptr, 0).value_or(0.0F) / 100.0F;
    _spread._scmp     = MarketSpread(_shortPack, nullptr, 0).value_or(0.0F) / 100.0F;
    _spread._bTrQ     = (_longPack._tradedPack);
    _spread._sTrQ     = (_shortPack._tradedPack);
    _spread._bATP     = static_cast<float>(_longPack._tradedSpread) / 100.0F;
    _spread._sATP     = static_cast<float>(_shortPack._tradedSpread) / 100.0F;
    _spread._status   = _status;
    _spread._flp      = static_cast<float>(_quote[BiddingLeg].message.ltp_) / 100.0F;
    _spread._reserved = _lastSent._reserved;

    if (_spread == _lastSent && nowNs_ - _lastSentNs < SpreadResendNs) {
        return std::nullopt;
    }
    _spread._reserved = static_cast<float>(std::exchange(_eventCount, 0));
    _lastSent         = _spread;
    _lastSentNs       = nowNs_;
    return _spread;
}

// ═══ Pack math ═══════════════════════════════════════════════════════════════

auto RatioLegStrategy::MarketSpread(const Pack& pack_, LegArrayT<int>* prices_, size_t index_) const -> std::optional<float> {
    long long raw = 0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const int price = Price(_quote[leg], pack_._oppositeSide[leg], index_);
        if (price <= 0) {
            return std::nullopt;
        }
        if (prices_ != nullptr) {
            (*prices_)[leg] = price;
        }
        raw += static_cast<long long>(price) * pack_._signedRatio[leg];
    }
    return static_cast<float>(AdjustGap(static_cast<double>(raw)));
}

// Leg 0 at price_ and the hedge legs at the opposite side's top: does the pack still make its target?
// A hedge leg with no price cannot be hedged, so the spread counts as gone.
auto RatioLegStrategy::SpreadPresent(const Pack& pack_, int price_) const -> bool {
    double raw = static_cast<double>(price_) * pack_._signedRatio[BiddingLeg];
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (leg != BiddingLeg) {
            const int price = Price(_quote[leg], pack_._oppositeSide[leg], 0);
            if (price <= 0) {
                return false;
            }
            raw += static_cast<double>(price) * pack_._signedRatio[leg];
        }
    }
    return AdjustGap(raw) >= pack_._targetSpread;
}

auto RatioLegStrategy::AdjustGap(double spread_) const -> double {
    if (!_hasStrikeGap) {
        return spread_;
    }
    return spread_ < 0 ? spread_ + _strikeGap : spread_ - _strikeGap;
}

auto RatioLegStrategy::LegSweep() const -> int {
    int worst = 0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (_quote[leg].header.product_id != 0) {  // no tick yet: stream_id is 0, not the leg's stream
            worst = std::max(worst, _hub->StreamSweep(_quote[leg].header.stream_id));
        }
    }
    return worst;
}

// price_ takes leg 0's opposite touch: a BUY at or above the ask, a SELL at or below the bid.
auto RatioLegStrategy::Crosses(const Pack& pack_, int price_) const -> bool {
    const int top = Price(_quote[BiddingLeg], pack_._oppositeSide[BiddingLeg], 0);
    return top > 0 && (pack_._sides[BiddingLeg] == BUY_SIDE ? price_ >= top : price_ <= top);
}

// First hedge leg whose touch (the side its hedge would take) is not to be trusted yet: its quantity dropped within
// _hedgeHoldNs (being eaten, whatever its size), or its price is newer than _hedgeHoldNs and shows less than
// _hedgeHoldCover times what leg 0's quantity_ needs on it. NoLeg when every touch has held.
auto RatioLegStrategy::FreshHedge(const Pack& pack_, int quantity_) const -> size_t {
    if (_hedgeHoldNs == 0) {
        return NoLeg;
    }
    const uint64_t now   = _hub->LatestExchangeNs();
    const int      packs = quantity_ / _lotSize / _ratios[BiddingLeg];
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const Touch& touch = _touch[leg][pack_._oppositeSide[leg] == BUY_SIDE ? 0 : 1];
        const bool   eaten = touch._shrunkNs + _hedgeHoldNs > now;
        const bool   young = touch._movedNs + _hedgeHoldNs > now && touch._qty < _hedgeHoldCover * packs * _ratios[leg] * _lotSize;
        if (leg != BiddingLeg && (eaten || young)) {
            return leg;
        }
    }
    return NoLeg;
}

auto RatioLegStrategy::FindLegIndex(int token_) const -> size_t {
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (_tokens[leg] == token_) {
            return leg;
        }
    }
    return NoLeg;
}

// ═══ Orders ══════════════════════════════════════════════════════════════════

// A rejected order is reset to idle by the vendor, so the next tick sent it again: millions of RMS -91000 rejects a day
// on sandbox10. Each order waits after a reject: an entry (bid or IOC) EntryRejectWaitNs, anything closing a position
// (hedge, reverse, cancel) RejectWaitNs, since an RMS reject can clear within the millisecond.
auto RatioLegStrategy::RejectedNs(Pack& pack_, const OrderObjectT& order_, size_t leg_) -> uint64_t& {
    return &order_ == pack_._reverse.get() ? pack_._reverseRejectedNs : pack_._rejectedNs[leg_];
}

auto RatioLegStrategy::RejectedWithin(uint64_t rejectedNs_, uint64_t waitNs_) -> bool {
    return rejectedNs_ != 0 && aef::infra::get_time_in_nanos() - rejectedNs_ < waitNs_;
}

// Entry orders only: the bid (and the IOC on the IOC branch).
void RatioLegStrategy::UpdateOrder(Pack& pack_, size_t leg_, int price_, int quantity_, std::string_view reason_) {
    if (RejectedWithin(pack_._rejectedNs[leg_], EntryRejectWaitNs)) {
        return;
    }
    Send(pack_, *pack_._orders[leg_], leg_, pack_._sides[leg_], price_, quantity_, reason_);
}

void RatioLegStrategy::Send(Pack& pack_, OrderObjectT& order_, size_t leg_, ORDER_SIDE side_, int price_, int quantity_, std::string_view reason_, bool hedge_) {
    if (_squaredOff || RejectedWithin(RejectedNs(pack_, order_, leg_), RejectWaitNs)) {
        return;
    }
    OrderOut out{&pack_, leg_, side_, OrderAction(order_), 0, order_.get_open_price(), price_, order_.get_open_qty(), quantity_, reason_};
    out._uid = _hub->UpdateOrder(order_, price_, quantity_, _clientUid, hedge_);
    RecordOrderOut(out);
}

void RatioLegStrategy::Pull(Pack& pack_, size_t leg_, std::string_view reason_) {
    OrderObjectT& order = *pack_._orders[leg_];
    if (!_hub->Throttled() && !RejectedWithin(pack_._rejectedNs[leg_], RejectWaitNs) && order.is_order_confirmed() && !order.is_response_pending() &&
        order.cancel_order()) {
        _hub->NoteRequest(order.get_uid());
        RecordOrderOut({&pack_, leg_, pack_._sides[leg_], "CANCEL", order.get_uid(), order.get_open_price(), order.get_open_price(), order.get_open_qty(), 0, reason_});
    }
}

// Keeps only what was really sent: UpdateOrder returns uid 0 for a refused or unchanged order.
void RatioLegStrategy::RecordOrderOut(const OrderOut& out_) {
    if (out_._uid == 0) {
        return;
    }
    if (_orderOutCount == _orderOuts.size()) {
        FlushOrderOuts();
    }
    _orderOuts[_orderOutCount++] = out_;
}

// Called at the end of every event that sends orders: formatting a line between two sends would delay the second.
void RatioLegStrategy::FlushOrderOuts() {
    for (const OrderOut& out : std::span{_orderOuts}.first(_orderOutCount)) {
        WriteLog("[ORDER-OUT] {} leg={} token={} {} {} uid={} price={}->{} qty={}->{} | {} lots=[{}] book=[{}]\n",
                 PackName(*out._pack), out._leg, _tokens[out._leg], SideName(out._side), out._action, out._uid, out._oldPrice, out._price, out._oldQty,
                 out._quantity, out._reason, LotsText(*out._pack), BookText());
    }
    _orderOutCount = 0;
    for (Pack* pack : {&_longPack, &_shortPack}) {
        if (pack->_holdLogDue) {
            LogHold(*pack);
        }
    }
}

// Leg 0 holds its place for hold_ (empty: free to move). A change is logged with the gate's inputs, at most once per
// GateLogNs per pack, with the changes skipped since; the line itself waits for FlushOrderOuts.
void RatioLegStrategy::Hold(Pack& pack_, std::string_view hold_, size_t leg_) {
    if (hold_ == pack_._hold) {
        return;
    }
    pack_._hold = hold_;
    ++pack_._holdChanges;
    if (const uint64_t now = _hub->LatestExchangeNs(); now >= pack_._holdLogNs + GateLogNs) {
        pack_._holdLogNs  = now;
        pack_._holdLogLeg = leg_;
        pack_._holdLogDue = true;
    }
}

void RatioLegStrategy::LogHold(Pack& pack_) {
    pack_._holdLogDue    = false;
    const int      changes = std::exchange(pack_._holdChanges, 0);
    const uint64_t now     = _hub->LatestExchangeNs();
    const size_t   leg     = pack_._holdLogLeg;
    if (pack_._hold.empty()) {
        WriteLog("[GATE] {} leg 0 free ({} gate changes since the last line)\n", PackName(pack_), changes);
    } else if (leg != NoLeg) {
        const Touch& touch = _touch[leg][pack_._oppositeSide[leg] == BUY_SIDE ? 0 : 1];
        WriteLog("[GATE] {} leg 0 holds: {} (leg {} touch {}x{}, new price {} us ago, last eaten {} us ago; {} gate changes since the last line) | book=[{}]\n",
                 PackName(pack_), pack_._hold, leg, touch._price, touch._qty, (now - touch._movedNs) / 1000, (now - touch._shrunkNs) / 1000, changes, BookText());
    } else {
        WriteLog("[GATE] {} leg 0 holds: {} (sweep {} msgs/ms, exchange round trip {} us; {} gate changes since the last line) | book=[{}]\n", PackName(pack_),
                 pack_._hold, _sweep, _hub->ExchangeLagNs() / 1000, changes, BookText());
    }
}

void RatioLegStrategy::TrackTouch(size_t leg_, const Quote& quote_) {
    const std::array<std::pair<int, int>, 2> tops{{{quote_.message.bid_levels[0].price, quote_.message.bid_levels[0].qty},
                                                   {quote_.message.ask_levels[0].price, quote_.message.ask_levels[0].qty}}};
    for (size_t side = 0; side < tops.size(); ++side) {
        Touch& touch = _touch[leg_][side];
        if (tops[side].first != touch._price) {
            touch._movedNs = quote_.header.exchange_timestamp;
        } else if (tops[side].second < touch._qty) {
            touch._shrunkNs = quote_.header.exchange_timestamp;
        }
        touch._price = tops[side].first;
        touch._qty   = tops[side].second;
    }
}

// ═══ Logs ════════════════════════════════════════════════════════════════════

// Logs when the gate changes. While the market meets the target but a hedge leg is thin, logs every time: a missed entry.

void RatioLegStrategy::LogResponse(const Pack& pack_, size_t leg_, const OrderSnapshot& order_, const oms_transaction& response_, bool isCut_) {
    const int32_t code = response_.hdr_.transaction_code;
    WriteLog("[ORDER-IN] {} leg={} token={} {}{} {}({}) uid={} price={} qty={} err={}/{}/{} -> state=0x{:x} open={}@{} filled={} liveUid={}\n",
             PackName(pack_), leg_, _tokens[leg_], SideName(isCut_ ? Opposite(pack_._sides[leg_]) : pack_._sides[leg_]), isCut_ ? " CUT" : "",
             TransactionName(code), code, uint32_t{response_.hdr_.uid_.id_}, int32_t{response_.packet_.price_}, int32_t{response_.packet_.quantity_},
             int32_t{response_.hdr_.error_code}, int32_t{response_.hdr_.reason_code}, int32_t{response_.packet_.exchange_error_code},
             order_._state, order_._openQty, order_._openPrice, order_._filledQty, order_._uid);
}

void RatioLegStrategy::WriteTimestamp() const {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    const long secondsOfDay = (now.tv_sec + _utcOffsetSeconds) % 86400;
    fmt::print(_logFile.get(), "{:02}:{:02}:{:02}.{:06} ", secondsOfDay / 3600, secondsOfDay / 60 % 60, secondsOfDay % 60, now.tv_nsec / 1000);
}

auto RatioLegStrategy::BookText() const -> std::string {
    std::string text;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        fmt::format_to(std::back_inserter(text), "{}{}/{}", leg == 0 ? "" : " ", int{_quote[leg].message.bid_levels[0].price}, int{_quote[leg].message.ask_levels[0].price});
    }
    return text;
}

auto RatioLegStrategy::LotsText(const Pack& pack_) const -> std::string {
    const int   biddingPacks = pack_._tradedLots[BiddingLeg] / _ratios[BiddingLeg];
    std::string text;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const int needed = leg == BiddingLeg ? pack_._totalAllowedPacks * _ratios[leg] : biddingPacks * _ratios[leg];
        fmt::format_to(std::back_inserter(text), "{}{}/{}", leg == 0 ? "" : " ", pack_._tradedLots[leg], needed);
    }
    return text;
}

// ═══ Book ════════════════════════════════════════════════════════════════════

auto RatioLegStrategy::Price(const Quote& quote_, ORDER_SIDE side_, size_t level_) -> int {
    return side_ == BUY_SIDE ? quote_.message.bid_levels[level_].price : quote_.message.ask_levels[level_].price;
}

auto RatioLegStrategy::HasLevels(const Quote& quote_, ORDER_SIDE side_, size_t depth_) -> bool {
    for (size_t level = 0; level < depth_; ++level) {
        if (Price(quote_, side_, level) <= 0) {
            return false;
        }
    }
    return true;
}

auto RatioLegStrategy::QuantityUpTo(const Quote& quote_, ORDER_SIDE side_, size_t depth_) -> int {
    int quantity = 0;
    for (size_t level = 0; level < depth_; ++level) {
        quantity += side_ == BUY_SIDE ? quote_.message.bid_levels[level].qty : quote_.message.ask_levels[level].qty;
    }
    return quantity;
}

auto RatioLegStrategy::OrderCount(const Quote& quote_, ORDER_SIDE side_) -> int {
    int orders = 0;
    for (size_t level = 0; level < aef::infra::quote::QUOTE_LEVELS; ++level) {
        orders += side_ == BUY_SIDE ? quote_.message.bid_levels[level].order_count_ : quote_.message.ask_levels[level].order_count_;
    }
    return orders;
}

auto RatioLegStrategy::DescribeDepth(const Quote& quote_, ORDER_SIDE side_, int minOrders_, size_t depth_, double minQuantity_) -> std::string {
    if (const int orders = OrderCount(quote_, side_); orders < minOrders_) {
        return fmt::format("{} orders in the book < {}", orders, minOrders_);
    }
    for (size_t level = 0; level < depth_; ++level) {
        if (Price(quote_, side_, level) <= 0) {
            return fmt::format("level {} has no price", level);
        }
    }
    if (const int quantity = QuantityUpTo(quote_, side_, depth_); quantity < minQuantity_) {
        return fmt::format("quantity {} over {} levels < required {}", quantity, depth_, minQuantity_);
    }
    return "ok";
}
