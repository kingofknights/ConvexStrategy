// RatioLegStrategy: leg 0 entry (bid pricing, bid gate).
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

using namespace ratioleg;

// ═══ Entry: bidding leg ══════════════════════════════════════════════════════

void RatioLegStrategy::EvaluateBidding(Pack& pack_) {
    OrderObjectT& bidOrder      = *pack_._orders[BIDDING_LEG];
    const int     biddingRatio  = _ratios[BIDDING_LEG];
    const int     tradedLots    = pack_._tradedLots[BIDDING_LEG];
    const int     remainingLots = pack_._params._totalPacks * biddingRatio - tradedLots;
    const float   targetSpread  = pack_._params._targetSpread;

    // Market spread prices every leg at the crossing side. When it meets the target, leg 0 takes its crossing touch
    // (at market). Else the bid rests at leg 0's price that gives exactly the target with the hedge legs at those prices.
    const SpreadQuote spreadQuote   = QuoteSpread(pack_._cache);
    const bool        spreadMatched = spreadQuote._valid && spreadQuote._spread >= targetSpread;
    const int         bidPrice      = !spreadQuote._valid ? 0 : spreadMatched ? spreadQuote._price[BIDDING_LEG] : TargetBidPrice(pack_, spreadQuote);

    size_t        thinLeg = NO_LEG;
    const BidGate gate    = [&] {
        if (remainingLots <= 0) return BidGate::DONE;
        if (!spreadQuote._valid) return BidGate::NO_PRICE;
        thinLeg = FindThinHedgeLeg(pack_);
        if (thinLeg != NO_LEG) return BidGate::HEDGE_DEPTH;
        if (bidPrice <= 0) return BidGate::UNREACHABLE;
        if (_allowedBidDepth > 0) {
            // A closed gate reopens a few ticks inside the limit: a bid jittering at the edge flapped TOO_FAR <-> BID
            // on every tick, a place and a cancel each time, and used up the message limit the hedges need.
            const int depth = (pack_._bidGate == BidGate::TOO_FAR) ? std::max(_allowedBidDepth - BID_GATE_REOPEN_TICKS, 0) : _allowedBidDepth;
            if (TicksBehindTouch(pack_, bidPrice) > depth) return BidGate::TOO_FAR;
        }
        return BidGate::BID;
    }();
    SetBidGate(pack_, gate, thinLeg, spreadQuote, spreadMatched, bidPrice);
    if (gate != BidGate::BID) {
        PullOrder(pack_, BIDDING_LEG, "bid gate closed");
        return;
    }
    // After a throttle reject the bid leaves the book until the pause ends: it cannot be repriced in time, so a resting
    // bid was filled past the target, and its messages are the hedges' share of the limit.
    if (aef::infra::get_time_in_nanos() < _bidPausedUntilNs) {
        PullOrder(pack_, BIDDING_LEG, "throttled");
        return;
    }

    // Finish a partly filled pack before starting a new slice, so the bidding leg only ever holds whole packs.
    const int packRemainder = tradedLots % biddingRatio;
    const int orderLots     = (packRemainder > 0) ? (biddingRatio - packRemainder) : (pack_._params._slicePacks * biddingRatio);
    const int quantity      = std::min(orderLots, remainingLots) * _lotSize;

    // A resting bid moves only when its price changes by TickSize ticks (GUI), each modify costs a message. A bid priced
    // past the target (SELL under, BUY over the bid price) within PAST_TARGET_MOVE_TICKS of its touch moves at once: one
    // leg 0 tick is ratio x tick of spread, so a fill there slipped 10-35 paise (2026-10-14 14:52 log). While the market
    // spread meets the target it follows the touch on every tick (UpdateOrder skips a modify to the same price).
    const int  oldPrice   = bidOrder.get_open_price();  // 0 when no order is working
    const bool pastTarget = (pack_._sides[BIDDING_LEG] == SELL_SIDE) ? oldPrice < bidPrice : oldPrice > bidPrice;
    const bool canFill    = TicksBehindTouch(pack_, oldPrice) <= PAST_TARGET_MOVE_TICKS;
    if (oldPrice != 0 && !spreadMatched && !(pastTarget && canFill) && std::abs(oldPrice - bidPrice) < _repriceThreshold) {
        return;
    }
    const std::string_view action = OrderAction(bidOrder);
    const int              oldQty = bidOrder.get_open_qty();
    if (const uint32_t uid = _hub->UpdateOrder(bidOrder, bidPrice, quantity, _clientUid); uid != 0) {
        WriteLog("[ORDER-OUT] {} leg={} token={} {} {} uid={} price={}->{} qty={}->{} | BID {} market={} target={} lots=[{}] book=[{}]\n",
                 PackName(pack_), BIDDING_LEG, _tokens[BIDDING_LEG], SideName(pack_._sides[BIDDING_LEG]), action, uid, oldPrice, bidPrice, oldQty, quantity,
                 spreadMatched ? "MARKET" : "REST", spreadQuote._spread, targetSpread, LotsText(pack_), BookText());
    }
}

// Solves signedRatio[0] * price + sum(hedge legs) >= target for leg 0, rounded to the tick away from the
// market (SELL up, BUY down) so the spread never lands under the target. With a strike gap the sign of the raw
// market spread picks the AdjustGap branch. Returns <= 0 when no positive price reaches the target.
auto RatioLegStrategy::TargetBidPrice(const Pack& pack_, const SpreadQuote& spreadQuote_) const -> int {
    const LegCache& cache       = pack_._cache;
    long long       rawMarket   = 0;
    long long       hedgeSpread = 0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const long long legSpread = static_cast<long long>(spreadQuote_._price[leg]) * cache._signedRatio[leg];
        rawMarket += legSpread;
        if (leg != BIDDING_LEG) hedgeSpread += legSpread;
    }
    long long rawTarget = std::llround(pack_._params._targetSpread);
    if (_hasStrikeGap) {
        rawTarget += (rawMarket < 0) ? -_strikeGap : _strikeGap;  // inverse of AdjustGap
    }
    const double ticks = static_cast<double>(rawTarget - hedgeSpread) / (static_cast<double>(cache._signedRatio[BIDDING_LEG]) * _tickSize);
    return static_cast<int>(cache._signedRatio[BIDDING_LEG] > 0 ? std::ceil(ticks) : std::floor(ticks)) * _tickSize;
}

// How far the bid would rest behind leg 0's own side (SELL above the best ask, BUY below the best bid).
auto RatioLegStrategy::TicksBehindTouch(const Pack& pack_, int bidPrice_) const -> int {
    const ORDER_SIDE side     = pack_._sides[BIDDING_LEG];
    const int        ownTouch = GetPrice(_quote[BIDDING_LEG], side, 0);
    if (ownTouch <= 0) return 0;
    return std::max(side == SELL_SIDE ? bidPrice_ - ownTouch : ownTouch - bidPrice_, 0) / _tickSize;
}

auto RatioLegStrategy::FindThinHedgeLeg(const Pack& pack_) const -> size_t {
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (leg == BIDDING_LEG) continue;
        // A hedge crosses the book, so check liquidity on the side it will take (BUY hedge -> asks).
        const ORDER_SIDE takeSide = pack_._cache._takeSide[leg];
        if (!CheckOrderDepth(_quote[leg], _orderDepth, takeSide) ||
            !CheckPriceDepth(_quote[leg], _priceDepth, takeSide) ||
            GetAvailableQuantity(_quote[leg], _orderDepth, takeSide) < pack_._cache._requiredHedgeDepth[leg]) {
            return leg;
        }
    }
    return NO_LEG;
}

// Logs when the reason changes, so a quiet strategy still says why it is quiet. While the market spread
// meets the target but a depth check blocks the order, logs on every evaluation: each one is a missed entry.
void RatioLegStrategy::SetBidGate(Pack& pack_, BidGate gate_, size_t thinLeg_, const SpreadQuote& spreadQuote_, bool spreadMatched_, int bidPrice_) {
    const bool missedEntry = spreadMatched_ && gate_ == BidGate::HEDGE_DEPTH;
    if (pack_._bidGate == gate_ && !missedEntry) {
        return;
    }
    static constexpr std::array<std::string_view, 7> GateNames{"UNKNOWN", "BID", "DONE", "HEDGE_DEPTH", "NO_PRICE", "UNREACHABLE", "TOO_FAR"};

    WriteLog("[BID-GATE] {} {} -> {} | target={} market={} valid={} matched={} bid={} lots=[{}] book=[{}]\n",
             PackName(pack_), GateNames[static_cast<size_t>(pack_._bidGate)], GateNames[static_cast<size_t>(gate_)],
             pack_._params._targetSpread, spreadQuote_._spread, spreadQuote_._valid, spreadMatched_, bidPrice_, LotsText(pack_), BookText());
    switch (gate_) {
        case BidGate::NO_PRICE:
            for (size_t leg = 0; leg < _legCount; ++leg) {
                if (spreadQuote_._price[leg] <= 0) {
                    WriteLog("[BID-GATE] {} leg={} token={} no {} price at level 0\n", PackName(pack_), leg, _tokens[leg], BookSideName(pack_._cache._takeSide[leg]));
                    break;
                }
            }
            break;
        case BidGate::HEDGE_DEPTH: {
            const ORDER_SIDE takeSide = pack_._cache._takeSide[thinLeg_];
            WriteLog("[BID-GATE] {} thin hedge leg={} token={} side={}: {} (OrderDepth={} PriceDepth={})\n",
                     PackName(pack_), thinLeg_, _tokens[thinLeg_], BookSideName(takeSide),
                     DescribeDepth(_quote[thinLeg_], _orderDepth, _priceDepth, takeSide, pack_._cache._requiredHedgeDepth[thinLeg_]), _orderDepth, _priceDepth);
            break;
        }
        case BidGate::UNREACHABLE:
            WriteLog("[BID-GATE] {} target {} needs leg 0 price {} <= 0: no order\n", PackName(pack_), pack_._params._targetSpread, bidPrice_);
            break;
        case BidGate::TOO_FAR:
            WriteLog("[BID-GATE] {} bid {} is {} ticks behind the touch > AllowedBidDepth {}: no order\n",
                     PackName(pack_), bidPrice_, TicksBehindTouch(pack_, bidPrice_), _allowedBidDepth);
            break;
        default:
            break;
    }
    pack_._bidGate = gate_;
}
