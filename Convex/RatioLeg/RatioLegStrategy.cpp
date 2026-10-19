#include "RatioLegStrategy.hpp"

#include "MinixStrategy.hpp"
#include "ProductInfo.hpp"
#include "RatioLegHelpers.hpp"

#include <fmt/format.h>

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
    : _hub(hub_), _strategyId(strategyId_), _interface(interface_), _legCount(legCount_), _hasStrikeGap(hasStrikeGap_) {
    if (_legCount < 2 || _legCount > MAX_LEGS) {
        throw std::runtime_error(fmt::format("strategy {}: {} legs, supported 2..{}", _strategyId, _legCount, MAX_LEGS));
    }
    _logFile = OpenLogFile(json_.contains("Strategy") ? json_["Strategy"].value("UserId", 0U) : 0U, _strategyId);
    WriteLog(">>> [INIT] Opened log file\n");

    _longPack._packSide  = BUY_SIDE;
    _shortPack._packSide = SELL_SIDE;
    _ratios.fill(1);

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
    _strikeGap          = std::abs(details[0].strike_price_ - details[1].strike_price_);
    _lotSize            = details[0].lot_size_;
    _tickSize           = details[0].tick_size_;
    _spread._strategyId = _strategyId;
    _spread._gap        = static_cast<float>(_strikeGap / 100);

    ParamUpdate(json_);

    _clientUid.composite_id_.client_id   = static_cast<uint32_t>(_hub->_clientId);
    _clientUid.composite_id_.strategy_id = _strategyId;
    for (Pack* pack : {&_longPack, &_shortPack}) {
        for (size_t leg = 0; leg < _legCount; ++leg) {
            pack->_orders[leg]    = std::make_unique<OrderObjectT>(_tokens[leg], pack->_sides[leg], _lotSize, _hub->_clientId, _hub->_algoId, _hub->_omsId, ORDER_TYPE::LIMIT_ORDER_TYPE, _hub);
            pack->_cutOrders[leg] = std::make_unique<OrderObjectT>(_tokens[leg], Opposite(pack->_sides[leg]), _lotSize, _hub->_clientId, _hub->_algoId, _hub->_omsId, ORDER_TYPE::IOC_ORDER_TYPE, _hub);
        }
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

        _longPack._slicePacks    = params.value("LongBuySoQ", 0);
        _longPack._totalPacks    = params.value("LongBuyQty", 0);
        _longPack._targetSpread  = ToPaise(params.value("LongBuyPrice", 0.0));
        _shortPack._slicePacks   = params.value("ShortSellSoQ", 0);
        _shortPack._totalPacks   = params.value("ShortSellQty", 0);
        _shortPack._targetSpread = ToPaise(params.value("ShortSellPrice", 0.0));

        _orderDepth           = params.value("OrderDepth", 1);
        _priceDepth           = std::min<size_t>(params.value("PriceDepth", 1U), aef::infra::quote::QUOTE_LEVELS);
        _hedgeDepthPercent    = params.value("ThresholdQty", 100);
        _allowedSlippage      = params.value("AllowedSlippage", 0);
        _allowedBidDepth      = std::min<size_t>(params.value("AllowedBidDepth", 0U), aef::infra::quote::QUOTE_LEVELS);
        const int revertBidMs = params.value("TimeToRevertBidMs", 0);
        _revertBidMs          = revertBidMs > 0 ? revertBidMs : DEFAULT_REVERT_BID_MS;
        _passiveBidTicks      = std::max(0, params.value("PassiveBidTicks", DEFAULT_PASSIVE_BID_TICKS));

        WriteLog("[RatioLeg] StrategyId: {} | Params parsed: "
                 "\nLongBuySoQ: {}, \nLongBuyQty: {}, \nLongBuyPrice: {}; "
                 "\nShortSellSoQ: {}, \nShortSellQty: {}, \nShortSellPrice: {}; "
                 "\nOrderDepth: {}, \nPriceDepth: {}, "
                 "\nThresholdQty: {}, \nAllowedSlippage: {}, \nAllowedBidDepth: {}, \nTimeToRevertBidMs: {}, \nPassiveBidTicks: {}, \n_biddingLeg: {}\n",
                 _strategyId, _longPack._slicePacks, _longPack._totalPacks, _longPack._targetSpread,
                 _shortPack._slicePacks, _shortPack._totalPacks, _shortPack._targetSpread, _orderDepth, _priceDepth,
                 _hedgeDepthPercent, _allowedSlippage, _allowedBidDepth, _revertBidMs, _passiveBidTicks, BIDDING_LEG);
    }
    _depthCheckedNs = 0;
}

void RatioLegStrategy::SetStatus(StrategyStatus status_) {
    if (status_ == StrategyStatus_INACTIVE) {
        Stop("status INACTIVE requested");
        return;
    }
    _status             = status_;
    _deleted            = false;
    _longPack._bidGate  = BidGate::UNKNOWN;
    _shortPack._bidGate = BidGate::UNKNOWN;
    WriteLog("[RatioLeg:SetStatus] strat={} _status={} ({})\n", _strategyId, static_cast<int>(_status), StrategyStatusToString(_status));
}

void RatioLegStrategy::Stop(std::string_view reason_) {
    _status = StrategyStatus_ACTIVE;
    WriteLog("[RatioLeg:Stop] strat={} _status=ACTIVE reason='{}' long_unhedged={} short_unhedged={} long_lots=[{}] short_lots=[{}]\n",
             _strategyId, reason_, IsUnhedged(_longPack), IsUnhedged(_shortPack), LotsText(_longPack), LotsText(_shortPack));
    for (Pack* pack : {&_longPack, &_shortPack}) {
        const bool whole = IsWhole(*pack);
        for (size_t leg = 0; leg < _legCount; ++leg) {
            if (leg == BIDDING_LEG || whole) {
                Pull(*pack, leg, reason_);
            }
        }
        pack->_bidGate = BidGate::UNKNOWN;
    }
}

void RatioLegStrategy::Forget(std::string_view reason_) {
    WriteLog("[RatioLeg:Forget] strat={} reason='{}' dropping long_lots=[{}] short_lots=[{}]\n", _strategyId, reason_, LotsText(_longPack), LotsText(_shortPack));
    for (Pack* pack : {&_longPack, &_shortPack}) {
        pack->_tradedLots.fill(0);
        pack->_tradedValue.fill(0);
        pack->_unreportedLots.fill(0);
        pack->_unreportedValue.fill(0);
        pack->_hedgeWaiting.fill(false);
        pack->_tradedSpread      = 0.0;
        pack->_incompleteSinceNs = 0;
        pack->_cutReason         = {};
    }
    Stop(reason_);
}

auto RatioLegStrategy::IsIdle() const -> bool {
    for (const Pack* pack : {&_longPack, &_shortPack}) {
        if (!IsWhole(*pack)) {
            return false;
        }
        for (size_t leg = 0; leg < _legCount; ++leg) {
            if (pack->_orders[leg]->get_current_state() != IDLE || pack->_cutOrders[leg]->get_current_state() != IDLE) {
                return false;
            }
        }
    }
    return true;
}

// ═══ Events ══════════════════════════════════════════════════════════════════

void RatioLegStrategy::OnTick(const Quote& quote_) {
    const size_t leg = FindLegIndex(quote_.header.product_id);
    if (leg == NO_LEG) {
        return;
    }
    _quote[leg] = quote_;
    _eventCount += 1;

    Evaluate(_longPack);
    Evaluate(_shortPack);
}

void RatioLegStrategy::DoWork(uint64_t nowNs_) {
    if (nowNs_ - _depthCheckedNs >= DEPTH_CHECK_NS) {
        CheckHedgeDepth(nowNs_);
    }
}

void RatioLegStrategy::OnOrderResponse(const oms_transaction& response_) {
    const size_t leg  = FindLegIndex(response_.packet_.product_id_);
    const auto   side = static_cast<ORDER_SIDE>(response_.packet_.flags_.order_side);
    const auto   uid  = uint32_t{response_.hdr_.uid_.id_};
    const auto   code = int32_t{response_.hdr_.transaction_code};
    if (leg == NO_LEG || (side != BUY_SIDE && side != SELL_SIDE)) {
        WriteLog("[ORDER-IN] ignored: token={} side={} code={} uid={} (not a leg of this strategy)\n", int{response_.packet_.product_id_}, static_cast<int>(side), code, uid);
        return;
    }

    if (code == OMS_REQ_REJ && response_.hdr_.error_code == THROTTLE_LIMIT) {
        const uint64_t now = aef::infra::get_time_in_nanos();
        if (now >= _bidPausedUntilNs) {
            WriteLog("[THROTTLE] uid={} rejected by the message limit: new bids paused {} ms\n", uid, THROTTLE_PAUSE_NS / 1'000'000);
        }
        _bidPausedUntilNs  = now + THROTTLE_PAUSE_NS;
        _throttleRetryAtNs = now + THROTTLE_RETRY_NS;
    }

    for (Pack* pack : {&_longPack, &_shortPack}) {
        OrderObjectT& cut = *pack->_cutOrders[leg];
        if (cut.get_uid() == 0 || cut.get_uid() != uid) {
            continue;
        }
        cut.handle_confirmation(response_);
        LogResponse(*pack, leg, cut, response_, true);
        if (code == OMS_TRADE) {
            OnCutTrade(*pack, leg, response_);
        } else if (code != OMS_REQ_REJ) {
            Evaluate(*pack);
        }
        return;
    }

    Pack&         pack  = (side == _longPack._sides[leg]) ? _longPack : _shortPack;
    OrderObjectT& order = *pack._orders[leg];
    if (order.get_uid() == uid) {
        order.handle_confirmation(response_);
    }
    LogResponse(pack, leg, order, response_, false);
    if (code == OMS_TRADE) {
        OnTrade(pack, leg, response_);
    } else if (code != OMS_REQ_REJ) {
        Evaluate(pack);
    }
}

// ═══ Decision: everything is worked out again on every event ═════════════════

void RatioLegStrategy::Evaluate(Pack& pack_) {
    const uint64_t now = aef::infra::get_time_in_nanos();
    if (now < _throttleRetryAtNs) {
        return;
    }
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (pack_._cutOrders[leg]->get_current_state() != IDLE) {
            return;
        }
    }
    if (IsWhole(pack_)) {
        pack_._incompleteSinceNs = 0;
        pack_._cutReason         = {};
    } else {
        if (pack_._incompleteSinceNs == 0) {
            pack_._incompleteSinceNs = now;
        }
        const uint64_t window = static_cast<uint64_t>(_revertBidMs) * 1'000'000;
        if ((now - pack_._incompleteSinceNs) / window % 2 == 1) {
            const bool unhedged = IsUnhedged(pack_);
            if (unhedged && IsActive()) {
                Stop("hedge failed, reverse cut");
            }
            if (pack_._cutReason.empty()) {
                const int  biddingPacks = pack_._tradedLots[BIDDING_LEG] / _ratios[BIDDING_LEG];
                bool       hedgeAhead   = false;
                for (size_t leg = 1; leg < _legCount; ++leg) {
                    hedgeAhead = hedgeAhead || pack_._tradedLots[leg] > biddingPacks * _ratios[leg];
                }
                pack_._cutReason = unhedged ? "HedgeFailed" : hedgeAhead ? "ChaseFailed" : "PartialFill";
            }
            Cut(pack_);
            return;
        }
    }

    if (Complete(pack_)) {
        return;
    }
    if (IsActive()) {
        Bid(pack_);
    } else {
        Pull(pack_, BIDDING_LEG, "strategy not APPLIED");
    }
}

void RatioLegStrategy::Bid(Pack& pack_) {
    const int        ratio         = _ratios[BIDDING_LEG];
    const int        tradedLots    = pack_._tradedLots[BIDDING_LEG];
    const int        remainingLots = (pack_._totalPacks * ratio) - tradedLots;
    const ORDER_SIDE side          = pack_._sides[BIDDING_LEG];

    LegArray<int>              prices{};
    const std::optional<float> market = MarketSpread(pack_, &prices);
    double                     price  = 0.0;
    if (market) {
        const int signedRatio = SignedRatio(pack_, BIDDING_LEG);
        const int sign        = signedRatio > 0 ? 1 : -1;
        price                 = prices[BIDDING_LEG] - ((*market - pack_._targetSpread) / signedRatio) + (sign * _passiveBidTicks * _tickSize);
    }
    const int limit = Price(_quote[BIDDING_LEG], side, BID_LEVEL_LIMIT);

    BidGate gate = BidGate::BID;
    if (remainingLots <= 0) {
        gate = BidGate::DONE;
    } else if (!market) {
        gate = BidGate::NO_PRICE;
    } else {
        if (_depthCheckedNs == 0) {
            CheckHedgeDepth(aef::infra::get_time_in_nanos());
        }
        if (pack_._thinHedgeLeg != NO_LEG) {
            gate = BidGate::HEDGE_DEPTH;
        } else if (!HasLevels(_quote[BIDDING_LEG], side, _allowedBidDepth)) {
            gate = BidGate::BID_DEPTH;
        } else if (price <= 0) {
            gate = BidGate::UNREACHABLE;
        } else if (limit <= 0 || (side == BUY_SIDE ? price < limit : price > limit)) {
            gate = BidGate::TOO_DEEP;
        }
    }
    LogGate(pack_, gate, market.value_or(0.0F), market.has_value(), price);

    if (gate != BidGate::BID) {
        Pull(pack_, BIDDING_LEG, "bid gate closed");
        return;
    }
    if (aef::infra::get_time_in_nanos() < _bidPausedUntilNs) {
        Pull(pack_, BIDDING_LEG, "throttled");
        return;
    }
    const int partLots  = tradedLots % ratio;
    const int orderLots = partLots > 0 ? ratio - partLots : pack_._slicePacks * ratio;
    Send(pack_, BIDDING_LEG, *pack_._orders[BIDDING_LEG], price, std::min(orderLots, remainingLots), "BID");
}

// Every leg catches up to the leg furthest ahead. Hedge legs go first; leg 0 chases once no hedge is missing or out.
auto RatioLegStrategy::Complete(Pack& pack_) -> bool {
    int target = HedgeTarget(pack_);
    for (size_t leg = 1; leg < _legCount; ++leg) {
        target = std::max(target, (pack_._tradedLots[leg] + _ratios[leg] - 1) / _ratios[leg]);
    }

    bool behind  = false;
    bool working = false;
    for (size_t leg = 1; leg < _legCount; ++leg) {
        OrderObjectT& order = *pack_._orders[leg];
        const int     lots  = (target * _ratios[leg]) - pack_._tradedLots[leg];
        if (lots <= 0) {
            Pull(pack_, leg, "no hedge needed");
        } else {
            behind = true;
            if (order.get_current_state() == IDLE) {
                const ORDER_SIDE takeSide = Opposite(pack_._sides[leg]);
                const int        price    = Price(_quote[leg], takeSide, HEDGE_LEVEL);
                if (price > 0) {
                    pack_._hedgeWaiting[leg] = false;
                    Send(pack_, leg, order, price, lots, "HEDGE");
                } else if (!pack_._hedgeWaiting[leg]) {
                    pack_._hedgeWaiting[leg] = true;
                    WriteLog("[HEDGE] {} leg={} token={} waiting: no {} price at level {}, {} lots missing | book=[{}]\n",
                             PackName(pack_), leg, _tokens[leg], BookSideName(takeSide), HEDGE_LEVEL, lots, BookText());
                }
            }
        }
        working = working || order.get_current_state() != IDLE;
    }
    if (behind) {
        Pull(pack_, BIDDING_LEG, "pack unhedged");
        return true;
    }

    const int lots = (target * _ratios[BIDDING_LEG]) - pack_._tradedLots[BIDDING_LEG];
    if (lots <= 0) {
        return false;
    }
    if (!working) {
        Send(pack_, BIDDING_LEG, *pack_._orders[BIDDING_LEG], Price(_quote[BIDDING_LEG], Opposite(pack_._sides[BIDDING_LEG]), CHASE_LEVEL), lots, "CHASE");
    }
    return true;
}

// Every leg goes back down to the completed packs: pull the leg's own order, then IOC the extra lots at the touch.
void RatioLegStrategy::Cut(Pack& pack_) {
    const int packs = CompletedPacks(pack_);
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (pack_._orders[leg]->get_current_state() != IDLE) {
            Pull(pack_, leg, "reverse cut");
            continue;
        }
        if (const int lots = pack_._tradedLots[leg] - (packs * _ratios[leg]); lots > 0) {
            Send(pack_, leg, *pack_._cutOrders[leg], Price(_quote[leg], pack_._sides[leg], 0), lots, "REVERSE-CUT");
        }
    }
}

// ═══ Fills ═══════════════════════════════════════════════════════════════════

void RatioLegStrategy::OnTrade(Pack& pack_, size_t leg_, const oms_transaction& response_) {
    const int32_t  price    = response_.packet_.price_;
    const int32_t  quantity = response_.packet_.quantity_;
    const int32_t  lots     = quantity / _lotSize;
    const uint64_t value    = static_cast<uint64_t>(price) * static_cast<uint64_t>(quantity);
    const bool     wasUnhedged = IsUnhedged(pack_);

    pack_._tradedLots[leg_] += lots;
    pack_._tradedValue[leg_] += value;
    pack_._unreportedLots[leg_] += lots;
    pack_._unreportedValue[leg_] += value;

    const ORDER_SIDE side = pack_._sides[leg_];
    WriteLog("[TRADE EVENT] index: {}, token: {}, side: {} ({}), price: {}, quantity: {}, pack: {}, lots: [{}]\n",
             leg_, _tokens[leg_], SideName(side), static_cast<int>(side), price, quantity, PackName(pack_), LotsText(pack_));
    if (quantity % _lotSize != 0) {
        WriteLog("[TRADE EVENT] WARNING quantity {} is not a multiple of lot size {}; {} units not counted\n", quantity, _lotSize, quantity % _lotSize);
    }

    const bool isUnhedged = IsUnhedged(pack_);
    if (isUnhedged != wasUnhedged) {
        WriteLog("[PACK] {} {} | lots=[{}] completedPacks={}\n", PackName(pack_), isUnhedged ? "UNHEDGED" : "HEDGED", LotsText(pack_), CompletedPacks(pack_));
        pack_._bidGate = BidGate::UNKNOWN;
    }
    if (!isUnhedged) {
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
    Evaluate(pack_);
}

// Takes the cut lots off the leg at its average price and books the round trip in _unwindPnL.
void RatioLegStrategy::OnCutTrade(Pack& pack_, size_t leg_, const oms_transaction& response_) {
    const int32_t price    = response_.packet_.price_;
    const int32_t quantity = response_.packet_.quantity_;
    const int32_t lots     = std::min(quantity / _lotSize, pack_._tradedLots[leg_]);
    if (lots * _lotSize != quantity) {
        WriteLog("[CUT] {} leg={} WARNING trade qty={} but {} lots held: {} units not counted\n", PackName(pack_), leg_, quantity, pack_._tradedLots[leg_], quantity - (lots * _lotSize));
    }
    if (lots > 0) {
        const auto removeLots = [](int32_t& heldLots_, uint64_t& heldValue_, int32_t removeLots_) -> double {
            if (heldLots_ <= 0 || removeLots_ <= 0) {
                return 0.0;
            }
            const double removed = static_cast<double>(heldValue_) * removeLots_ / heldLots_;
            heldValue_ -= static_cast<uint64_t>(std::llround(removed));
            heldLots_ -= removeLots_;
            return removed;
        };
        const double entryValue = removeLots(pack_._tradedLots[leg_], pack_._tradedValue[leg_], lots);
        removeLots(pack_._unreportedLots[leg_], pack_._unreportedValue[leg_], std::min(lots, pack_._unreportedLots[leg_]));

        const double exitValue = static_cast<double>(price) * lots * _lotSize;
        const bool   soldFirst = pack_._sides[leg_] == SELL_SIDE;
        const double sellValue = soldFirst ? entryValue : exitValue;
        const double buyValue  = soldFirst ? exitValue : entryValue;
        const double pnl       = sellValue - buyValue - (sellValue * (_isOption[leg_] ? OptionSellCost : FutureSellCost)) - (buyValue * (_isOption[leg_] ? OptionBuyCost : FutureBuyCost));
        _unwindPnL += pnl;

        WriteLog("[CUT] {} leg={} {} {} lots @{} (entry avg {:.2f}) pnl={:.0f} total={:.0f} | lots=[{}] unhedged={}\n",
                 PackName(pack_), leg_, SideName(Opposite(pack_._sides[leg_])), lots, price, entryValue / (lots * _lotSize), pnl, _unwindPnL, LotsText(pack_), IsUnhedged(pack_));
        SendTracer(pack_, response_, static_cast<float>(entryValue / (lots * _lotSize) / 100.0), price / 100.0F, lots, 0, pack_._cutReason);
    }
    Evaluate(pack_);
}

// One Trade Tracer row per batch of packs completed since the last row, with the slippage against the target.
void RatioLegStrategy::ReportPacks(Pack& pack_, const oms_transaction& response_) {
    int packs = pack_._unreportedLots[0] / _ratios[0];
    for (size_t leg = 1; leg < _legCount; ++leg) {
        packs = std::min(packs, pack_._unreportedLots[leg] / _ratios[leg]);
    }
    if (packs <= 0) {
        return;
    }

    double spread = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const double average = static_cast<double>(pack_._unreportedValue[leg]) / (static_cast<double>(pack_._unreportedLots[leg]) * _lotSize);
        WriteLog("[SLIPPAGE {}Leg] [strat = {}] [leg = {}] Val: {}, Lot: {}, LotSize: {}, AvgPrice: {}\n",
                 _legCount, _strategyId, leg, pack_._unreportedValue[leg], pack_._unreportedLots[leg], _lotSize, average);
        spread += (pack_._sides[leg] == BUY_SIDE ? -average : average) * _ratios[leg];
    }
    const auto  traded   = static_cast<float>(AdjustGap(spread));
    const float slippage = pack_._targetSpread - traded;
    const bool  slipped  = std::round(slippage) > 0;
    SendTracer(pack_, response_, pack_._targetSpread / 100.0F, traded / 100.0F, packs, slipped ? slippage / 100.0F : 0, slipped ? "Slippage" : "");

    for (size_t leg = 0; leg < _legCount; ++leg) {
        const int32_t lots          = pack_._unreportedLots[leg];
        const int32_t leftLots      = lots - (packs * _ratios[leg]);
        pack_._unreportedValue[leg] = pack_._unreportedValue[leg] * static_cast<uint64_t>(leftLots) / static_cast<uint64_t>(lots);
        pack_._unreportedLots[leg]  = leftLots;
    }

    if (_allowedSlippage > 0 && slippage > static_cast<float>(_allowedSlippage)) {
        WriteLog("[SLIPPAGE {}Leg] Slippage {} paise > AllowedSlippage {} paise. Stopping strategy.\n", _legCount, slippage, _allowedSlippage);
        Stop("slippage above AllowedSlippage");
    }
}

void RatioLegStrategy::SendTracer(const Pack& pack_, const oms_transaction& response_, float price_, float ltp_, int ltq_, float slippage_, std::string_view message_) {
    _tradeTracer._orderId      = response_.packet_.exchange_order_id;
    _tradeTracer._time         = response_.hdr_.exchange_timestamp;
    _tradeTracer._qtyRemaining = pack_._totalPacks - CompletedPacks(pack_);
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
    _spread._bcmp     = MarketSpread(_longPack).value_or(0.0F) / 100.0F;
    _spread._scmp     = MarketSpread(_shortPack).value_or(0.0F) / 100.0F;
    _spread._bTrQ     = CompletedPacks(_longPack);
    _spread._sTrQ     = CompletedPacks(_shortPack);
    _spread._bATP     = static_cast<float>(_longPack._tradedSpread) / 100.0F;
    _spread._sATP     = static_cast<float>(_shortPack._tradedSpread) / 100.0F;
    _spread._status   = _status;
    _spread._flp      = static_cast<float>(_quote[BIDDING_LEG].message.ltp_) / 100.0F;
    _spread._reserved = _lastSent._reserved;

    if (_spread == _lastSent && nowNs_ - _lastSentNs < SPREAD_RESEND_NS) {
        return std::nullopt;
    }
    _spread._reserved = static_cast<float>(std::exchange(_eventCount, 0));
    _lastSent         = _spread;
    _lastSentNs       = nowNs_;
    return _spread;
}

// ═══ Pack math ═══════════════════════════════════════════════════════════════

// Each leg at level 0 of the side it would trade against (BUY at the ask, SELL at the bid), times its signed ratio.
auto RatioLegStrategy::MarketSpread(const Pack& pack_, LegArray<int>* prices_) const -> std::optional<float> {
    long long raw = 0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const int price = Price(_quote[leg], Opposite(pack_._sides[leg]), 0);
        if (price <= 0) {
            return std::nullopt;
        }
        if (prices_ != nullptr) {
            (*prices_)[leg] = price;
        }
        raw += static_cast<long long>(price) * SignedRatio(pack_, leg);
    }
    return static_cast<float>(AdjustGap(static_cast<double>(raw)));
}

auto RatioLegStrategy::AdjustGap(double spread_) const -> double {
    if (!_hasStrikeGap) {
        return spread_;
    }
    return spread_ < 0 ? spread_ + _strikeGap : spread_ - _strikeGap;
}

auto RatioLegStrategy::SignedRatio(const Pack& pack_, size_t leg_) const -> int {
    return (pack_._sides[leg_] == BUY_SIDE ? -1 : 1) * _ratios[leg_];
}

auto RatioLegStrategy::CompletedPacks(const Pack& pack_) const -> int {
    int packs = pack_._tradedLots[0] / _ratios[0];
    for (size_t leg = 1; leg < _legCount; ++leg) {
        packs = std::min(packs, pack_._tradedLots[leg] / _ratios[leg]);
    }
    return packs;
}

auto RatioLegStrategy::IsWhole(const Pack& pack_) const -> bool {
    const int packs = CompletedPacks(pack_);
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (pack_._tradedLots[leg] != packs * _ratios[leg]) {
            return false;
        }
    }
    return true;
}

// Packs the hedge legs must cover: leg 0's whole packs.
auto RatioLegStrategy::HedgeTarget(const Pack& pack_) const -> int {
    return pack_._tradedLots[BIDDING_LEG] / _ratios[BIDDING_LEG];
}

auto RatioLegStrategy::IsUnhedged(const Pack& pack_) const -> bool {
    const int target = HedgeTarget(pack_);
    for (size_t leg = 1; leg < _legCount; ++leg) {
        if (pack_._tradedLots[leg] < target * _ratios[leg]) {
            return true;
        }
    }
    return false;
}

void RatioLegStrategy::CheckHedgeDepth(uint64_t nowNs_) {
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (_quote[leg].header.product_id == 0) {
            return;
        }
    }
    _depthCheckedNs         = nowNs_;
    _longPack._thinHedgeLeg  = ThinHedgeLeg(_longPack);
    _shortPack._thinHedgeLeg = ThinHedgeLeg(_shortPack);
}

auto RatioLegStrategy::ThinHedgeLeg(const Pack& pack_) const -> size_t {
    for (size_t leg = 1; leg < _legCount; ++leg) {
        const ORDER_SIDE takeSide = Opposite(pack_._sides[leg]);
        if (OrderCount(_quote[leg], takeSide) < _orderDepth || !HasLevels(_quote[leg], takeSide, _priceDepth) ||
            QuantityUpTo(_quote[leg], takeSide, _priceDepth) < RequiredHedgeQuantity(pack_, leg)) {
            return leg;
        }
    }
    return NO_LEG;
}

auto RatioLegStrategy::RequiredHedgeQuantity(const Pack& pack_, size_t leg_) const -> double {
    const int percent = _hedgeDepthPercent > 0 ? _hedgeDepthPercent : 100;
    return static_cast<double>(pack_._slicePacks * _ratios[leg_] * _lotSize) * percent / 100.0;
}

auto RatioLegStrategy::FindLegIndex(int token_) const -> size_t {
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (_tokens[leg] == token_) {
            return leg;
        }
    }
    return NO_LEG;
}

// ═══ Orders ══════════════════════════════════════════════════════════════════

// Rounds to the tick away from the market (SELL up, BUY down). UpdateOrder skips a send that changes nothing.
void RatioLegStrategy::Send(Pack& pack_, size_t leg_, OrderObjectT& order_, double price_, int lots_, std::string_view reason_) {
    const bool             isCut    = &order_ == pack_._cutOrders[leg_].get();
    const ORDER_SIDE       side     = isCut ? Opposite(pack_._sides[leg_]) : pack_._sides[leg_];
    const double           ticks    = price_ / _tickSize;
    const int              price    = static_cast<int>(side == SELL_SIDE ? std::ceil(ticks) : std::floor(ticks)) * _tickSize;
    const std::string_view action   = OrderAction(order_);
    const int              oldPrice = order_.get_open_price();
    const int              oldQty   = order_.get_open_qty();
    if (const uint32_t uid = _hub->UpdateOrder(order_, price, lots_ * _lotSize, _clientUid); uid != 0) {
        WriteLog("[ORDER-OUT] {} leg={} token={} {} {}{} uid={} price={}->{} qty={}->{} | {} lots=[{}] book=[{}]\n",
                 PackName(pack_), leg_, _tokens[leg_], SideName(side), action, isCut ? " IOC" : "", uid, oldPrice, price, oldQty, lots_ * _lotSize, reason_, LotsText(pack_), BookText());
    }
}

void RatioLegStrategy::Pull(Pack& pack_, size_t leg_, std::string_view reason_) {
    OrderObjectT& order = *pack_._orders[leg_];
    if (!order.is_order_confirmed() || order.is_response_pending()) {
        return;
    }
    if (order.cancel_order()) {
        WriteLog("[ORDER-OUT] {} leg={} token={} {} CANCEL uid={} price={} qty={} | {}\n",
                 PackName(pack_), leg_, _tokens[leg_], SideName(pack_._sides[leg_]), order.get_uid(), order.get_open_price(), order.get_open_qty(), reason_);
    }
}

// ═══ Logs ════════════════════════════════════════════════════════════════════

// Logs when the gate changes. While the market meets the target but a hedge leg is thin, logs every time: a missed entry.
void RatioLegStrategy::LogGate(Pack& pack_, BidGate gate_, float market_, bool valid_, double price_) {
    const bool matched = valid_ && market_ >= pack_._targetSpread;
    if (pack_._bidGate == gate_ && !(matched && gate_ == BidGate::HEDGE_DEPTH)) {
        return;
    }
    static constexpr std::array<std::string_view, 8> GateNames{"UNKNOWN", "BID", "DONE", "HEDGE_DEPTH", "NO_PRICE", "UNREACHABLE", "BID_DEPTH", "TOO_DEEP"};
    WriteLog("[BID-GATE] {} {} -> {} | target={} market={} valid={} matched={} bid={:.2f} lots=[{}] book=[{}]\n",
             PackName(pack_), GateNames[static_cast<size_t>(pack_._bidGate)], GateNames[static_cast<size_t>(gate_)],
             pack_._targetSpread, market_, valid_, matched, price_, LotsText(pack_), BookText());

    const ORDER_SIDE ownSide = pack_._sides[BIDDING_LEG];
    switch (gate_) {
        case BidGate::NO_PRICE:
            for (size_t leg = 0; leg < _legCount; ++leg) {
                if (Price(_quote[leg], Opposite(pack_._sides[leg]), 0) <= 0) {
                    WriteLog("[BID-GATE] {} leg={} token={} no {} price at level 0\n", PackName(pack_), leg, _tokens[leg], BookSideName(Opposite(pack_._sides[leg])));
                    break;
                }
            }
            break;
        case BidGate::HEDGE_DEPTH: {
            const size_t     leg      = pack_._thinHedgeLeg;
            const ORDER_SIDE takeSide = Opposite(pack_._sides[leg]);
            WriteLog("[BID-GATE] {} thin hedge leg={} token={} side={}: {} (OrderDepth={} PriceDepth={})\n", PackName(pack_), leg, _tokens[leg], BookSideName(takeSide),
                     DescribeDepth(_quote[leg], takeSide, _orderDepth, _priceDepth, RequiredHedgeQuantity(pack_, leg)), _orderDepth, _priceDepth);
            break;
        }
        case BidGate::UNREACHABLE:
            WriteLog("[BID-GATE] {} target {} needs leg 0 price {:.2f} <= 0: no order\n", PackName(pack_), pack_._targetSpread, price_);
            break;
        case BidGate::BID_DEPTH:
            WriteLog("[BID-GATE] {} leg 0 token={} side={}: {} (AllowedBidDepth={}): no order\n", PackName(pack_), _tokens[BIDDING_LEG], BookSideName(ownSide),
                     DescribeDepth(_quote[BIDDING_LEG], ownSide, 0, _allowedBidDepth, 0), _allowedBidDepth);
            break;
        case BidGate::TOO_DEEP:
            WriteLog("[BID-GATE] {} leg 0 token={} {} level {} = {}, bid {:.2f}: no order\n", PackName(pack_), _tokens[BIDDING_LEG], BookSideName(ownSide),
                     BID_LEVEL_LIMIT, Price(_quote[BIDDING_LEG], ownSide, BID_LEVEL_LIMIT), price_);
            break;
        default:
            break;
    }
    pack_._bidGate = gate_;
}

void RatioLegStrategy::LogResponse(const Pack& pack_, size_t leg_, const OrderObjectT& order_, const oms_transaction& response_, bool isCut_) {
    const int32_t code = response_.hdr_.transaction_code;
    WriteLog("[ORDER-IN] {} leg={} token={} {}{} {}({}) uid={} price={} qty={} err={}/{}/{} -> state=0x{:x} open={}@{} filled={} liveUid={}\n",
             PackName(pack_), leg_, _tokens[leg_], SideName(isCut_ ? Opposite(pack_._sides[leg_]) : pack_._sides[leg_]), isCut_ ? " CUT" : "",
             TransactionName(code), code, uint32_t{response_.hdr_.uid_.id_}, int32_t{response_.packet_.price_}, int32_t{response_.packet_.quantity_},
             int32_t{response_.hdr_.error_code}, int32_t{response_.hdr_.reason_code}, int32_t{response_.packet_.exchange_error_code},
             order_.get_current_state(), order_.get_open_qty(), order_.get_open_price(), order_.get_filled_qty(), order_.get_uid());
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
    const int   biddingPacks = pack_._tradedLots[BIDDING_LEG] / _ratios[BIDDING_LEG];
    std::string text;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const int needed = leg == BIDDING_LEG ? pack_._totalPacks * _ratios[leg] : biddingPacks * _ratios[leg];
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
