# Convex Flow — Design and Plan

> **Status 2026-10-19:** `RatioLegStrategy` was rewritten as one file (`Convex/RatioLeg/RatioLegStrategy.cpp`) that recomputes everything on every event. Since this document was written: the `'S'` synthetic-trade early hedge was removed (restored 2026-10-26 in `TryEarlyHedge` / `HedgeEarly`: leg 0 is chased at the opposite side's index 2 by `EvaluateHedgeMode`, and an early hedge left working after leg 0 goes away unfilled is pulled), the GUI spread dirty-flag cache was removed (values are recomputed on every `doWork` pass), and every reverse-cut fill now sends a Trade Tracer row (`HedgeFailed`, `ChaseFailed` or `PartialFill`). Sections below that describe those parts are out of date.

Branch `convex-flow`, cut from `manaul_order` (d18ad1d). Source spec: `Flow.md`.
This document is the agreed understanding of `Flow.md`, mapped onto `RatioLegStrategy`.

## Conventions

- Price levels are 0-indexed: index 0 is the top of the book. "Level N" in conversation is 1-based, so level 3 is index 2.
- A strategy has N legs (`token[]`, `ratio[]`, `side[]`) and runs two independent packs, LONG and SHORT. Each pack has its own single order lots, total lots and target spread (`user_spread`).
- Leg 0 is the bidding leg. Every other leg is a hedge leg.
- Spread: `BCMP = Σ signed(ratio_i × P_i)`. A BUY counts as negative, a SELL as positive. With a strike gap, the gap is applied by `AdjustGap`; this is the "constant" in the reverse formula.

## Flow

```
DoWork (every 60 s)
  check order_depth + price_depth on every hedge leg, on the side the hedge takes
  cache the result per pack (condition_1)

OnTick (per pack: LONG, SHORT)
  1. condition_1 false -> no bid
  2. back-solve leg 0 price P1 from user_spread, hedge prices and the strike gap
  3. too-deep guard on leg 0's own side, index 2:
       BUY  leg 0: bid[2] == 0 or P1 < bid[2] -> pull bid, stop bidding
       SELL leg 0: ask[2] == 0 or P1 > ask[2] -> pull bid, stop bidding
       bidding resumes as soon as the condition is false
  4. else place / modify one LIMIT on leg 0 at P1
  5. tick on leg 0 with update_type 'S' (Synthetic Trade):
       BUY leg 0: P1 > bid[0]   |   SELL leg 0: P1 < ask[0]
       -> our order is probably filled: hedge the full remaining pack now

OnOrderResponse
  leg 0 fill -> hedge the other legs by ratio at the opposite side's index 2
  hedges filled but leg 0 not filled (early hedge guessed wrong)
             -> chase leg 0 at the opposite side's index 1
  pack complete (lots_i = k × ratio_i)
             -> slippage check against AllowedSlippage
             -> trade tracker row
             -> lots-weighted average traded spread
```

## ASCII flow

### DoWork (hub timer, every 1 s)

```
+------------------------+
| DoWork (every 1 s)     |
+-----------+------------+
            |
            +-------------------------------+
            |                               |
            v                               v
  60 s since last depth check?     build GUI update from cached values
  (or first check, all feeds live)  (recompute quote part only if
     |yes            |no             _quoteDirty or position changed)
     v               v                      |
  FindThinHedgeLeg   skip                   v
  per pack                         any field changed? (_reserved ignored)
     |                                |yes             |no
     v                                |                v
  _hedgeDepthOk[pack]                 |      60 s since last send?
                                      |          |yes        |no
                                      v          v           v
                               +----------------------+   no send
                               | SEND STRATEGY_SPREAD |
                               | store as last sent   |
                               | GetEventCount reset  |
                               +----------------------+
```

### OnTick

```
+------------------+
| OnTick(quote)    |
+--------+---------+
         |
         v
  leg level-0 bid/ask price changed? --yes--> _quoteDirty = true
         |
         v
  _quote[leg] = quote
         |
         v
  leg == 0, update_type == 'S',                                  |
  update_side == leg 0 side? --no------------------------------->+
         |yes                                                    |
         v                                                       |
  for each pack: APPLIED, leg 0 order confirmed,                 |
  no response pending? --no------------------------------------->+
         |yes                                                    |
         v                                                       |
  working price beats book top?                                  |
  (BUY: P1 > bid[0] | SELL: P1 < ask[0]) --no------------------->+
         |yes                                                    |
         v                                                       |
  already fired for this leg 0 uid? --yes----------------------->+
         |no                                                     |
         v                                                       |
  +----------------------------------------------+               |
  | EARLY HEDGE: hedge full remaining pack       |               |
  |   lots_i = (traded0 + open0) / ratio0 x r_i  |               |
  |   price  = opposite side index 2             |               |
  | freeze leg 0 (no reprice, no cancel)         |               |
  +----------------------+-----------------------+               |
                         |                                       |
                         v                                       |
             ProcessPack(LONG), ProcessPack(SHORT) <-------------+
```

### ProcessPack (per pack, after every tick and response)

```
+--------------------+
| ProcessPack(pack)  |
+---------+----------+
          |
          v
  throttle pause or cut IOC in flight? --yes--> return
          |no
          v
  pack whole? --no--> in cut window? --yes--> CutPack: IOC reverse-cut
          |yes             |no                extra lots --> return
          |                v
          |       hedge legs behind leg 0? --yes--> HedgePack
          |                |no                      opposite index 2 --> return
          |                v
          |       leg 0 behind hedges?
          |                |yes
          |                v
          |       hedge order still working? --yes--> leg 0 frozen, wait --> return
          |                |no
          |                v
          |       CHASE leg 0: modify same order,
          |       opposite index 1, missing lots --> return
          v
  APPLIED? --no--> pull bid --> return
          |yes
          v
  EvaluateBidding(pack)
```

### EvaluateBidding (leg 0 bid gate)

```
+------------------------+
| EvaluateBidding(pack)  |
+-----------+------------+
            |
            v
  P1 = back-solved TargetBidPrice (always; crosses by itself
       when the market beats the target)
            |
            v
  remaining lots == 0 ---------------------------> DONE        -> pull bid
  any leg level-0 price missing -----------------> NO_PRICE    -> pull bid
  _hedgeDepthOk[pack] false (DoWork cache) ------> HEDGE_DEPTH -> pull bid
  AllowedBidDepth check fails -------------------> BID_DEPTH   -> pull bid
  P1 <= 0 ---------------------------------------> UNREACHABLE -> pull bid
  own side index 2 == 0, or P1 beyond index 2 ---> TOO_DEEP    -> pull bid
    (BUY: P1 < bid[2] | SELL: P1 > ask[2])
  throttle pause --------------------------------> pull bid
            |
            v  all pass (gate BID)
  +----------------------------------------------+
  | place / modify one LIMIT on leg 0 at P1      |
  | follows P1 on every change                   |
  +----------------------------------------------+
```

### OnOrderResponse

```
+--------------------------+
| OnOrderResponse(resp)    |
+------------+-------------+
             |
             v
  THROTTLE_LIMIT reject? --yes--> pause bids 1 s, no resend 5 ms
             |
             v
  cut IOC uid? --yes--> trade? --yes--> OnCutTrade: remove lots at avg,
             |no                        _unwindPnL += pnl,
             |                          position cache refresh --> ProcessPack
             v
  transaction code
     |
     +-- TRADE --> OnTrade: lots/value += fill
     |               position cache refresh
     |               pack hedged? --yes--> TradedSpread (lots-weighted)
     |                                     slippage vs AllowedSlippage
     |                                     tracer row on complete pack
     |               ProcessPack
     |
     +-- REJECT --> wait for next tick
     |
     +-- ACK / CANCEL --> ProcessPack

  Forget (square-off) --> drop lots, position cache refresh, Stop
```

## Changes

### 1. Hedge price at index 2

`ExecuteHedgeLeg` (`Convex/RatioLeg/RatioLegHedge.cpp`) places one LIMIT at the opposite side's index 2 (third level) price for exactly the ratio lots of leg 0's filled packs:

```
missingLots = filledPacks(leg 0) x ratio[leg] - tradedLots[leg]
quantity    = missingLots x lotSize
```

With ratio `[3, 1, 2]`: leg 0 fills 3 lots (1 pack) -> leg 1 one order of 1 lot, leg 2 one order of 2 lots. Leg 0 fills 1 of 3 lots (0 packs) -> no hedge; the part pack is completed or cut.

The order is placed once and never repriced. If index 2 has no price, the hedge waits. Lots still missing after the order is done go out in the next order; a pack left unhedged is cut by the cut windows. This replaces the index 1 pricing from d18ad1d.

Order rules from the old implementation that are gone (new idea, Flow.md only):

- `TradeGear` is not read: no tick offset on hedges or cuts. Cuts IOC at the held side's touch.
- `TickSize` (reprice threshold) and the past-target move rule are gone: leg 0 follows P1 on every change.
- Leg 0 always sends the back-solved P1, also when the market beats the target (the LIMIT then crosses and fills at the better touch).
- Hedges are not capped at the SoQ slice size: one order per leg for exactly its ratio lots (the old code sent `min(missing, max(slice, 1 pack))` per order, so several orders when missing lots exceeded a slice).

### 2. Depth check in DoWork every 60 s

- The hub `doWork` calls `strategy.DoWork()`. The hub only routes; the logic stays in the strategy.
- `DoWork` runs `FindThinHedgeLeg` every 60 s and caches the result per pack (`_hedgeDepthOk`). The cache is per pack because LONG and SHORT take opposite sides of the book.
- `EvaluateBidding` reads the cache instead of scanning depth on every tick.
- The first check runs as soon as every feed is live, not after 60 s. No bid before the first check.
- Up to 60 s of staleness is accepted.

### 3. Too-deep guard at index 2

New `BidGate::TOO_DEEP` in `EvaluateBidding`, checked on leg 0's own side on every tick:

- BUY leg 0: `bid[2] == 0` or `P1 < bid[2]` pulls the bid.
- SELL leg 0: `ask[2] == 0` or `P1 > ask[2]` pulls the bid.

The guard is keyed on leg 0's side, not the pack name; for the normal setup this matches the LONG / SHORT rule in `Flow.md`. It pauses bidding only while true. The gate log names the cause: no price at index 2, or P1 beyond index 2. `AllowedBidDepth` stays; it only adds a check when set above 3.

### 4. Early hedge on a synthetic trade

In `OnTick`, the early hedge fires when all of these hold:

- the tick is on leg 0, with `update_type == 'S'` and `update_side` equal to leg 0's own side (a synthetic trade on the other side cannot have hit our order);
- the strategy is APPLIED;
- the leg 0 order is confirmed with no response pending (an order or modify in flight is not in the book yet, so it would look filled);
- the working price (`get_open_price`) beats the book top: BUY price > bid[0], SELL price < ask[0].

Then:

- Our order should be at the top but is not, so it is probably filled.
- Hedge the full remaining pack: target lots per hedge leg = (leg 0 traded + leg 0 open lots) / ratio0 × ratio_i.
- Fire once per leg 0 uid, so repeated 'S' ticks do not resend.
- Freeze leg 0 (no reprice, no cancel) until the fill arrives or the chase starts, because it may already be filled.

### 5. Chase leg 0

New branch in `ProcessPack`. When hedge lots are beyond leg 0's lots, no hedge order is still working, and the leg 0 order is still open, modify the same leg 0 order to the opposite side's index 1 for the missing lots (no price at index 1: wait).

- While unfilled, the chase reprices to the opposite side's index 1 on every tick.
- Always a modify, never a new order: if the original fill is in flight the modify is rejected, while a new order could overfill.
- The never-naked cut windows stay. `ExtraLots` already counts hedge lots beyond completed packs, so a chase that does not fill is reverse-cut.
- If the early hedge is still unfilled when a cut window opens, the guess is dropped: hedges then follow only real leg 0 fills, so nothing is bought again for a fill that never happened.

### 6. Already in place

- Slippage check against `AllowedSlippage`.
- Lots-weighted average traded spread: `TradedSpread` (`Convex/RatioLeg/RatioLegReport.cpp`) averages each leg as value / lots.
- Trade tracer row on a complete pack.

### 7. Dry run

`tests/ratio_dry_run.cpp`, one scenario per change:

- hedge priced at index 2
- bid pulled when beyond index 2, and when index 2 is empty; bid resumes after
- 'S' early hedge, then the real fill arrives
- 'S' early hedge, no fill, chase, fill
- 'S' early hedge, no fill, chase, no fill, reverse cut
- no bid before the first depth check
- cached GUI values equal a fresh recompute after ticks, trades, a cut and `Forget`

### 8. GUI spread update: compute only on change, send only on change

`SendStrategySpreadsToUi` (`Convex/MinixStrategy.cpp`) runs every 1 s and today recomputes every value on every call.

Where each value changes:

| GUI field | Source | Changes when |
|---|---|---|
| `_strategyId`, `_interface` | set at creation | never |
| `_gap` | `_strikeGap` | never (set at creation) |
| `_status` | `_status` | `SetStatus`, `Stop` |
| `_bcmp` / `_scmp` | `QuoteSpread` | a leg's level-0 bid/ask price changes; `ParamUpdate` |
| `_cost` | level-0 bid/ask × cost rate | a leg's level-0 bid/ask price changes |
| `_flp` | leg 0 `ltp_` | leg 0 tick (plain read) |
| `_bTrQ` / `_sTrQ` | `CompletedPacks` | `OnTrade`, `OnCutTrade`, `Forget` |
| `_rlp`, `_cutPL` | `GetRLP` | `OnTrade`, `OnCutTrade`, `Forget` |
| `_m2m` | net position × level-0 mark | trade, cut trade, or a level-0 price change on a leg with a position |
| `_netPL` | `_rlp + _m2m` | derived |
| `_bATP` / `_sATP` | `_tradedSpread` (already cached) | hedged fill, `Forget` |
| `_reserved` | `GetEventCount` | every tick |

Waste today, per strategy per second: `GetRLP` runs 3 times (`_rlp`, `_cutPL`, `_netPL`) and `GetM2M` 2 times, each looping `PositionOf` over every leg and both packs, even when nothing changed.

Plan:

1. Position cache: recompute completed packs, RLP and per-leg net quantity / average price only at the end of `OnTrade`, `OnCutTrade` and in `Forget`.
2. Quote dirty flag: in `OnTick`, compare the new level-0 bid/ask price with the stored one before `_quote[leg]` is overwritten; set `_quoteDirty` only on a price change. `ParamUpdate` also sets it.
3. Recompute BCMP, SCMP, cost and M2M once, at send time, only when the quote is dirty or the position changed.
4. `_cutPL = _rlp`, `_netPL = _rlp + _m2m`; `_gap` computed once at creation.
5. Send only on change: each strategy keeps the last sent `StrategySpreadUpdate`. The update is built from cached values, compared field by field, and sent only if a field differs. The first update after creation or a param update is always sent.
6. `_reserved` (event count) is left out of the comparison. `GetEventCount` runs only when an update is sent, so the count accumulates across skipped seconds.
7. Full resend after 60 s: if a strategy has sent nothing for 60 s, the update is sent even though no value changed. The hub has no "GUI connected" event, so this is how a GUI that restarts during a flat market gets its values back. The 60 s counts from the last send (monotonic clock), so a quiet strategy sends once per 60 s and an active one is not sent twice.

Nothing new runs on the tick path except one price comparison per tick.

## Files

`Convex/RatioLeg/RatioLegStrategy.hpp`, `Convex/RatioLeg/RatioLegEntry.cpp`, `Convex/RatioLeg/RatioLegHedge.cpp`, `Convex/RatioLeg/RatioLegStrategy.cpp`, `Convex/RatioLeg/RatioLegReport.cpp`, `Convex/MinixStrategy.cpp`, `tests/ratio_dry_run.cpp`.
