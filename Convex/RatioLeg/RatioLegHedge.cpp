// RatioLegStrategy: hedging, slippage report and reverse cut.
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

// ═══ Hedge legs ══════════════════════════════════════════════════════════════

void RatioLegStrategy::UpdateUnhedgedStatus(Pack& pack_) {
    const int biddingPacks = pack_._tradedLots[BIDDING_LEG] / _ratios[BIDDING_LEG];
    pack_._isUnhedged      = false;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (leg != BIDDING_LEG && pack_._tradedLots[leg] < biddingPacks * _ratios[leg]) {
            pack_._isUnhedged = true;
            return;
        }
    }
}

void RatioLegStrategy::HedgePack(Pack& pack_) {
    PullOrder(pack_, BIDDING_LEG, "pack unhedged");  // no new exposure while legs are uneven

    const int biddingPacks = pack_._tradedLots[BIDDING_LEG] / _ratios[BIDDING_LEG];
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (leg == BIDDING_LEG) continue;
        ExecuteHedgeLeg(pack_, leg, biddingPacks * _ratios[leg]);
    }
}

void RatioLegStrategy::ExecuteHedgeLeg(Pack& pack_, size_t leg_, int targetHedgeLots_) {
    const int missingLots = targetHedgeLots_ - pack_._tradedLots[leg_];
    if (missingLots <= 0) return;

    // Take the opposite touch at once, and follow it while unfilled; TradeGear adds its ticks on top. Stepping up from
    // the bid's prices cost a message per step under a ~30 per second throttle and reached the touch within 1 ms anyway.
    const int touch = GetPrice(_quote[leg_], pack_._cache._takeSide[leg_], 0);
    if (touch <= 0) {
        // Opposite side empty: wait for a real price instead of resting near zero.
        if (!pack_._hedgeWaiting[leg_]) {
            pack_._hedgeWaiting[leg_] = true;
            WriteLog("[HEDGE] {} leg={} token={} waiting: no {} price, {} lots missing | book=[{}]\n",
                     PackName(pack_), leg_, _tokens[leg_], BookSideName(pack_._cache._takeSide[leg_]), missingLots, BookText());
        }
        return;
    }
    pack_._hedgeWaiting[leg_] = false;

    const int sideMultiplier = (pack_._sides[leg_] == BUY_SIDE) ? 1 : -1;
    const int targetPrice    = std::max(touch + sideMultiplier * _tradeGearOffset, _tickSize);
    // Sliced like the bid, but never below one pack: SoQ 0 must not leave the hedge unsent.
    const int quantity = std::min(missingLots * _lotSize, std::max(pack_._cache._sliceQuantity[leg_], _ratios[leg_] * _lotSize));

    OrderObjectT&          order    = *pack_._orders[leg_];
    const std::string_view action   = OrderAction(order);
    const int              oldPrice = order.get_open_price();
    const int              oldQty   = order.get_open_qty();
    if (const uint32_t uid = _hub->UpdateOrder(order, targetPrice, quantity, _clientUid); uid != 0) {
        WriteLog("[ORDER-OUT] {} leg={} token={} {} {} uid={} price={}->{} qty={}->{} | HEDGE touch={} missingLots={} lots=[{}] book=[{}]\n",
                 PackName(pack_), leg_, _tokens[leg_], SideName(pack_._sides[leg_]), action, uid, oldPrice, targetPrice, oldQty, quantity,
                 touch, missingLots, LotsText(pack_), BookText());
    }
}

void RatioLegStrategy::CheckSlippageThreshold(Pack& pack_, const oms_transaction& response_) {
    int newPacks = pack_._unreportedLots[0] / _ratios[0];
    for (size_t leg = 1; leg < _legCount; ++leg) {
        newPacks = std::min(newPacks, pack_._unreportedLots[leg] / _ratios[leg]);
    }
    if (newPacks <= 0) {
        return;
    }

    double tradedSpread = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const double legAvgPrice = static_cast<double>(pack_._unreportedValue[leg]) / (static_cast<double>(pack_._unreportedLots[leg]) * _lotSize);
        WriteLog("[SLIPPAGE {}Leg] [strat = {}] [leg = {}] Val: {}, Lot: {}, LotSize: {}, AvgPrice: {}\n",
                 _legCount, _strategyId, leg, pack_._unreportedValue[leg], pack_._unreportedLots[leg], _lotSize, legAvgPrice);
        tradedSpread += (pack_._sides[leg] == BUY_SIDE ? -legAvgPrice : legAvgPrice) * _ratios[leg];
    }

    const auto  adjustedTradedSpread = static_cast<float>(AdjustGap(tradedSpread));
    const float slippagePaise        = pack_._params._targetSpread - adjustedTradedSpread;  // AllowedSlippage is in paise

    // Error column says Slippage only when the pack traded worse than the target by a whole paise; else it stays blank.
    const bool slipped = std::round(slippagePaise) > 0;
    SendTracer(pack_, response_, pack_._params._targetSpread / 100.0F, adjustedTradedSpread / 100.0F, newPacks, slipped ? slippagePaise / 100.0F : 0,
               slipped ? "Slippage" : "");

    // Carry lots beyond the reported packs into the next report at their average value.
    // newPacks is the minimum over legs, so every leg holds at least newPacks * ratio lots.
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const int32_t lots          = pack_._unreportedLots[leg];
        const int32_t leftLots      = lots - newPacks * _ratios[leg];
        pack_._unreportedValue[leg] = pack_._unreportedValue[leg] * static_cast<uint64_t>(leftLots) / static_cast<uint64_t>(lots);
        pack_._unreportedLots[leg]  = leftLots;
    }

    if (_allowedSlippage > 0 && slippagePaise > static_cast<float>(_allowedSlippage)) {
        WriteLog("[SLIPPAGE {}Leg] Slippage {} paise > AllowedSlippage {} paise. Stopping strategy.\n", _legCount, slippagePaise, _allowedSlippage);
        Stop("slippage above AllowedSlippage");
    }
}

void RatioLegStrategy::SendTracer(const Pack& pack_, const oms_transaction& response_, float price_, float ltp_, int ltq_, float slippage_, std::string_view message_) {
    _tradeTracer._orderId      = response_.packet_.exchange_order_id;
    _tradeTracer._time         = response_.hdr_.exchange_timestamp;
    _tradeTracer._qtyRemaining = pack_._params._totalPacks - CompletedPacks(pack_);
    _tradeTracer._strategyId   = _strategyId;
    _tradeTracer._side         = pack_._packSide;
    _tradeTracer._price        = price_;
    _tradeTracer._ltp          = ltp_;
    _tradeTracer._ltq          = ltq_;
    _tradeTracer._slippage     = slippage_;
    const size_t length        = std::min(message_.size(), sizeof(_tradeTracer._message) - 1);
    std::memset(_tradeTracer._message, 0, sizeof(_tradeTracer._message));
    std::memcpy(_tradeTracer._message, message_.data(), length);

    WriteLog("Tracer [{}Leg] symbol {} stratId {} orderId {} time {} side {} price {} ltp {} ltq {} slippage {} message '{}'\n",
             _legCount, _tradeTracer._symbol, _strategyId, _tradeTracer._orderId, _tradeTracer._time, _tradeTracer._side, _tradeTracer._price, _tradeTracer._ltp,
             _tradeTracer._ltq, _tradeTracer._slippage, message_);
    _hub->SendToUi(UiMessageCode_TRADE_TRACER, _interface, _tradeTracer);
}

// ═══ Reverse cut ═════════════════════════════════════════════════════════════

// Lots a leg holds beyond the completed packs: naked until the pack completes or they are cut.
auto RatioLegStrategy::ExtraLots(const Pack& pack_, size_t leg_) const noexcept -> int {
    return pack_._tradedLots[leg_] - CompletedPacks(pack_) * _ratios[leg_];
}

// One pass: ExtraLots per leg would recount the completed packs for every leg (legs x legs divisions per tick).
auto RatioLegStrategy::IsWhole(const Pack& pack_) const noexcept -> bool {
    const int completedPacks = CompletedPacks(pack_);
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (pack_._tradedLots[leg] != completedPacks * _ratios[leg]) return false;
    }
    return true;
}

// The clock starts when the pack stops being whole and is not reset until it is whole again. Windows of
// TimeToRevertBidMs alternate: even ones complete the pack, odd ones cut it.
// ponytail: checked on ticks and responses only; a silent feed delays the switch until the next event.
auto RatioLegStrategy::InCutWindow(Pack& pack_) -> bool {
    const uint64_t now = aef::infra::get_time_in_nanos();
    if (pack_._incompleteSinceNs == 0) {
        pack_._incompleteSinceNs = now;
    }
    return (now - pack_._incompleteSinceNs) / (static_cast<uint64_t>(_revertBidMs) * 1'000'000) % 2 == 1;
}

// Pull the pack's own order on a leg first (it would trade against the cut), then IOC the leg's extra lots
// across the touch. Repeats on every evaluation until no leg has extra lots.
void RatioLegStrategy::CutPack(Pack& pack_) {
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (pack_._orders[leg]->get_current_state() != IDLE) {
            PullOrder(pack_, leg, "reverse cut");
            continue;
        }
        const int lots = ExtraLots(pack_, leg);
        if (lots <= 0) continue;

        // The cut takes the book side the position was opened on (a SELL is bought back from the asks).
        const ORDER_SIDE heldSide = pack_._sides[leg];
        const int        touch    = GetPrice(_quote[leg], heldSide, 0);
        if (touch <= 0) continue;  // no price to cross
        const int price    = std::max(touch + (heldSide == SELL_SIDE ? _tradeGearOffset : -_tradeGearOffset), _tickSize);
        const int quantity = lots * _lotSize;
        if (const uint32_t uid = _hub->UpdateOrder(*pack_._cutOrders[leg], price, quantity, _clientUid); uid != 0) {
            WriteLog("[ORDER-OUT] {} leg={} token={} {} PLACE IOC uid={} price={} qty={} | REVERSE-CUT {} extra lots, lots=[{}] book=[{}]\n",
                     PackName(pack_), leg, _tokens[leg], SideName(Opposite(heldSide)), uid, price, quantity, lots, LotsText(pack_), BookText());
        }
    }
}

// Removes the cut lots from the leg at its average price; the round trip is booked in _unwindPnL.
void RatioLegStrategy::OnCutTrade(Pack& pack_, size_t leg_, const oms_transaction& response_) {
    const int32_t price    = response_.packet_.price_;
    const int32_t quantity = response_.packet_.quantity_;
    const int32_t lots     = std::min(quantity / _lotSize, pack_._tradedLots[leg_]);
    if (lots * _lotSize != quantity) {
        WriteLog("[CUT] {} leg={} WARNING trade qty={} but {} lots held: {} units not counted\n", PackName(pack_), leg_, quantity, pack_._tradedLots[leg_], quantity - lots * _lotSize);
    }
    if (lots > 0) {
        const auto removeLots = [](int32_t& heldLots_, uint64_t& heldValue_, int32_t removeLots_) -> double {
            if (heldLots_ <= 0 || removeLots_ <= 0) return 0.0;
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
        const double pnl       = sellValue - buyValue -
                                 sellValue * (_isOption[leg_] ? OptionSellCost : FutureSellCost) -
                                 buyValue * (_isOption[leg_] ? OptionBuyCost : FutureBuyCost);
        _unwindPnL += pnl;

        const bool wasUnhedged = pack_._isUnhedged;
        UpdateUnhedgedStatus(pack_);  // leg 0 or a hedge leg changed
        WriteLog("[CUT] {} leg={} {} {} lots @{} (entry avg {:.2f}) pnl={:.0f} total={:.0f} | lots=[{}] unhedged={}\n",
                 PackName(pack_), leg_, SideName(Opposite(pack_._sides[leg_])), lots, price, entryValue / (lots * _lotSize), pnl, _unwindPnL, LotsText(pack_), pack_._isUnhedged);

        // A reverted bid (part pack bought or sold back) gets its own Trade Tracer row. A failed hedge's cut does not.
        if (leg_ == BIDDING_LEG && !wasUnhedged) {
            SendTracer(pack_, response_, static_cast<float>(entryValue / (lots * _lotSize) / 100.0), price / 100.0F, lots, 0, "PartialFill");
        }
    }
    ProcessPack(pack_);
}
