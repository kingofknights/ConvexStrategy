// RatioLegStrategy: spread pricing, PnL for the UI, logging and order book helpers.
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

// ═══ Pricing ═════════════════════════════════════════════════════════════════

auto RatioLegStrategy::QuoteSpread(const LegCache& cache_) const -> SpreadQuote {
    SpreadQuote spreadQuote{};
    for (size_t leg = 0; leg < _legCount; ++leg) {
        spreadQuote._price[leg] = GetPrice(_quote[leg], cache_._takeSide[leg], 0);
        if (spreadQuote._price[leg] <= 0) {
            return spreadQuote;  // a missing leg price means the spread is not tradable: reported as 0, invalid
        }
    }
    spreadQuote._spread = PriceSpread(cache_, spreadQuote._price);
    spreadQuote._valid  = true;
    return spreadQuote;
}

auto RatioLegStrategy::PriceSpread(const LegCache& cache_, const std::array<int, MAX_LEGS>& prices_) const -> float {
    double spread = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        spread += prices_[leg] * cache_._signedRatio[leg];
    }
    return static_cast<float>(AdjustGap(spread));
}

auto RatioLegStrategy::AdjustGap(double spread_) const noexcept -> double {
    if (!_hasStrikeGap) {
        return spread_;
    }
    return (spread_ < 0) ? (spread_ + _strikeGap) : (spread_ - _strikeGap);
}

auto RatioLegStrategy::GetBCmp() const -> SpreadQuote { return QuoteSpread(_longPack._cache); }
auto RatioLegStrategy::GetSCmp() const -> SpreadQuote { return QuoteSpread(_shortPack._cache); }

// ═══ Positions / PnL (UI) ════════════════════════════════════════════════════

// A pack counts only once every leg has filled its ratio.
auto RatioLegStrategy::CompletedPacks(const Pack& pack_) const noexcept -> int {
    int totalPacks = pack_._tradedLots[0] / _ratios[0];
    for (size_t leg = 1; leg < _legCount; ++leg) {
        totalPacks = std::min(totalPacks, pack_._tradedLots[leg] / _ratios[leg]);
    }
    return totalPacks;
}

auto RatioLegStrategy::GetLongTradedPacks() const -> int { return CompletedPacks(_longPack); }
auto RatioLegStrategy::GetShortTradedPacks() const -> int { return CompletedPacks(_shortPack); }

auto RatioLegStrategy::TradedSpread(const Pack& pack_) const -> double {
    double spread = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (pack_._tradedLots[leg] == 0) {
            return 0.0;
        }
        const double avgPrice = static_cast<double>(pack_._tradedValue[leg]) / (static_cast<double>(pack_._tradedLots[leg]) * _lotSize);
        spread += (pack_._sides[leg] == BUY_SIDE ? -avgPrice : avgPrice) * _ratios[leg];
    }
    return AdjustGap(spread);
}

// ATP is cached on hedged fills so in-flight unhedged lots do not distort it.
auto RatioLegStrategy::GetBATP() const -> double { return _longPack._tradedSpread; }
auto RatioLegStrategy::GetSATP() const -> double { return _shortPack._tradedSpread; }

auto RatioLegStrategy::PositionOf(size_t leg_) const -> LegPosition {
    LegPosition position;
    for (const Pack* pack : {&_longPack, &_shortPack}) {
        const int64_t  quantity = static_cast<int64_t>(pack->_tradedLots[leg_]) * _lotSize;
        const uint64_t value    = pack->_tradedValue[leg_];
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
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const LegPosition position = PositionOf(leg);
        totalRLP += static_cast<double>(std::min(position._buyQuantity, position._sellQuantity)) * (position.AvgSell() - position.AvgBuy());

        // Transaction cost on actual traded value (paise).
        const double buyCostRate  = _isOption[leg] ? OptionBuyCost : FutureBuyCost;
        const double sellCostRate = _isOption[leg] ? OptionSellCost : FutureSellCost;
        totalCost += (static_cast<double>(position._buyValue) * buyCostRate) + (static_cast<double>(position._sellValue) * sellCostRate);
    }
    return totalRLP - totalCost + _unwindPnL;
}

auto RatioLegStrategy::GetCutPL() const -> double { return GetRLP(); }

auto RatioLegStrategy::GetM2M() const -> double {
    double totalM2M = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const LegPosition position    = PositionOf(leg);
        const int64_t     netQuantity = position._buyQuantity - position._sellQuantity;
        if (netQuantity == 0) continue;

        const double markPrice = (netQuantity > 0) ? _quote[leg].message.bid_levels[0].price : _quote[leg].message.ask_levels[0].price;
        if (markPrice <= 0) continue;  // empty book side: no mark, rather than a full-notional swing

        const double avgPrice = (netQuantity > 0) ? position.AvgBuy() : position.AvgSell();
        totalM2M += static_cast<double>(netQuantity) * (markPrice - avgPrice);
    }
    return totalM2M;
}

auto RatioLegStrategy::GetNetPL() const -> double { return GetRLP() + GetM2M(); }
auto RatioLegStrategy::GetFLP() const -> int { return _quote[BIDDING_LEG].message.ltp_; }

auto RatioLegStrategy::GetCost() const -> double {
    double totalCost = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        totalCost += (_quote[leg].message.bid_levels[0].price * _buyCostRate[leg]) +
                     (_quote[leg].message.ask_levels[0].price * _sellCostRate[leg]);
    }
    return totalCost;
}

auto RatioLegStrategy::GetStrategyID() const -> uint32_t { return _strategyId; }
auto RatioLegStrategy::GetInterface() const -> int32_t { return _interface; }
auto RatioLegStrategy::GetGap() const -> int { return _strikeGap / 100; }

// ═══ Diagnostics ═════════════════════════════════════════════════════════════

void RatioLegStrategy::WriteTimestamp() const {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    const long secondsOfDay = (now.tv_sec + _utcOffsetSeconds) % 86400;
    fmt::print(_logFile.get(), "{:02}:{:02}:{:02}.{:06} ", secondsOfDay / 3600, secondsOfDay / 60 % 60, secondsOfDay % 60, now.tv_nsec / 1000);
}

auto RatioLegStrategy::BookText() const -> std::string {
    std::string text;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        fmt::format_to(std::back_inserter(text), "{}{}/{}", leg == 0 ? "" : " ",
                       int{_quote[leg].message.bid_levels[0].price}, int{_quote[leg].message.ask_levels[0].price});
    }
    return text;
}

// Bidding leg: traded / total lots. Hedge legs: traded / lots the completed bidding packs need.
auto RatioLegStrategy::LotsText(const Pack& pack_) const -> std::string {
    const int   biddingPacks = pack_._tradedLots[BIDDING_LEG] / _ratios[BIDDING_LEG];
    std::string text;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const int needed = (leg == BIDDING_LEG) ? pack_._params._totalPacks * _ratios[leg] : biddingPacks * _ratios[leg];
        fmt::format_to(std::back_inserter(text), "{}{}/{}", leg == 0 ? "" : " ", pack_._tradedLots[leg], needed);
    }
    return text;
}

// Cancels the order and logs why. Runs on every tick, so it skips silently when the vendor
// would refuse (nothing live, unconfirmed, or a response pending); each refusal there logs.
void RatioLegStrategy::PullOrder(Pack& pack_, size_t leg_, std::string_view reason_) {
    OrderObjectT& order = *pack_._orders[leg_];
    if (!order.is_order_confirmed() || order.is_response_pending()) {
        return;
    }
    if (order.cancel_order()) {
        WriteLog("[ORDER-OUT] {} leg={} token={} {} CANCEL uid={} price={} qty={} | {}\n",
                 PackName(pack_), leg_, _tokens[leg_], SideName(pack_._sides[leg_]), order.get_uid(), order.get_open_price(), order.get_open_qty(), reason_);
    }
}

// One line per OMS response: what arrived, and the order state it left behind.
void RatioLegStrategy::LogResponse(const Pack& pack_, size_t leg_, const OrderObjectT& order_, const oms_transaction& response_, bool isCut_) {
    const OrderObjectT& order         = order_;
    const int32_t       code          = response_.hdr_.transaction_code;
    const uint32_t      uid           = response_.hdr_.uid_.id_;
    const int32_t       price         = response_.packet_.price_;
    const int32_t       quantity      = response_.packet_.quantity_;
    const int32_t       errorCode     = response_.hdr_.error_code;
    const int32_t       reasonCode    = response_.hdr_.reason_code;
    const int32_t       exchangeError = response_.packet_.exchange_error_code;
    WriteLog("[ORDER-IN] {} leg={} token={} {}{} {}({}) uid={} price={} qty={} err={}/{}/{} -> state=0x{:x} open={}@{} filled={} liveUid={}\n",
             PackName(pack_), leg_, _tokens[leg_], SideName(isCut_ ? Opposite(pack_._sides[leg_]) : pack_._sides[leg_]), isCut_ ? " CUT" : "", TransactionName(code), code, uid, price, quantity,
             errorCode, reasonCode, exchangeError, order.get_current_state(), order.get_open_qty(), order.get_open_price(), order.get_filled_qty(), order.get_uid());
}

// ═══ Order book helpers ══════════════════════════════════════════════════════

auto RatioLegStrategy::GetPrice(const Quote& quote_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int {
    return (side_ == BUY_SIDE) ? quote_.message.bid_levels[levelIndex_].price : quote_.message.ask_levels[levelIndex_].price;
}

auto RatioLegStrategy::GetQuantity(const Quote& quote_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int {
    return (side_ == BUY_SIDE) ? quote_.message.bid_levels[levelIndex_].qty : quote_.message.ask_levels[levelIndex_].qty;
}

auto RatioLegStrategy::GetOrderCount(const Quote& quote_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int {
    return (side_ == BUY_SIDE) ? quote_.message.bid_levels[levelIndex_].order_count_ : quote_.message.ask_levels[levelIndex_].order_count_;
}

auto RatioLegStrategy::GetAvailableQuantity(const Quote& quote_, size_t depth_, ORDER_SIDE side_) noexcept -> int {
    int totalQuantity = 0;
    for (size_t levelIndex = 0; levelIndex < depth_; ++levelIndex) {
        totalQuantity += GetQuantity(quote_, side_, levelIndex);
    }
    return totalQuantity;
}

auto RatioLegStrategy::CheckOrderDepth(const Quote& quote_, size_t depth_, ORDER_SIDE side_) noexcept -> bool {
    for (size_t levelIndex = 0; levelIndex < depth_; ++levelIndex) {
        if (GetOrderCount(quote_, side_, levelIndex) <= 0) {
            return false;
        }
    }
    return true;
}

auto RatioLegStrategy::CheckPriceDepth(const Quote& quote_, size_t depth_, ORDER_SIDE side_) noexcept -> bool {
    for (size_t levelIndex = 0; levelIndex < depth_; ++levelIndex) {
        if (GetPrice(quote_, side_, levelIndex) <= 0) {
            return false;
        }
    }
    return true;
}

// First level that fails a depth check, and why; "ok" when none does. Only for logs, off the hot path.
auto RatioLegStrategy::DescribeDepth(const Quote& quote_, size_t orderDepth_, size_t priceDepth_, ORDER_SIDE side_, double requiredQuantity_) -> std::string {
    for (size_t levelIndex = 0; levelIndex < std::max(orderDepth_, priceDepth_); ++levelIndex) {
        if (levelIndex < priceDepth_ && GetPrice(quote_, side_, levelIndex) <= 0) {
            return fmt::format("level {} has no price", levelIndex);
        }
        if (levelIndex < orderDepth_ && GetOrderCount(quote_, side_, levelIndex) <= 0) {
            return fmt::format("level {} has no orders (price {})", levelIndex, GetPrice(quote_, side_, levelIndex));
        }
    }
    const int available = GetAvailableQuantity(quote_, orderDepth_, side_);
    if (available < requiredQuantity_) {
        return fmt::format("quantity {} over {} levels < required {}", available, orderDepth_, requiredQuantity_);
    }
    return "ok";
}

auto RatioLegStrategy::GetEventCount() -> int {
    int count   = _eventCount;
    _eventCount = 0;
    return count;
}
