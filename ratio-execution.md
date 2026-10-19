# Ratio Execution — Simple Order Flow

> **Status 2026-10-19:** `RatioLegStrategy` was rewritten as one file (`Convex/RatioLeg/RatioLegStrategy.cpp`) that recomputes everything on every event. Since this document was written: the `'S'` synthetic-trade early hedge was removed, the GUI spread dirty-flag cache was removed (values are recomputed on every `doWork` pass), and every reverse-cut fill now sends a Trade Tracer row (`HedgeFailed`, `ChaseFailed` or `PartialFill`). Sections below that describe those parts are out of date.

Branch `convex-flow`. Implemented; not committed.
Goal: keep today's behavior and make order placing easy to read. Each order decision should be one short path, with no copies of the same rule.
Decision: keep the reverse cut. Completing a pack and cutting a pack are the same loop with a different target.

## Principles

1. **One way to send.** Every order goes out through one function. It sends a new order or a modify, skips a send that changes nothing, and writes the one `[ORDER-OUT]` line. Callers only say leg, price, quantity and reason.
2. **One rule for every leg.** A pack has a target in packs. Each leg needs `target × ratio − traded` lots. This rule covers the bidding leg too.
3. **Compute once, then decide.** One call produces all the numbers for a decision. The decision code only compares and acts.
4. **Guards, then act.** Each decision function is a list of early returns, with the action on the last line.
5. **Count in packs.** The ratio is applied in one place only: where a pack target becomes a leg quantity.

## Where we are today

The pack balance logic is spread over eight functions with overlapping targets:

| Function | Target it chases |
|---|---|
| `HedgeTargetPacks` / `UpdateUnhedgedStatus` | hedge legs up to `floor(leg0 / r0)` or `_earlyHedgePacks` |
| `HedgePack` / `ExecuteHedgeLeg` | same, hedge legs only |
| `MissingBidLots` / `ChaseBiddingLeg` | leg 0 up to `max ceil(hedge_i / r_i)` |
| `ExtraLots` / `CutPack` | every leg down to `CompletedPacks` |

Each one has its own loop, quantity formula and `UpdateOrder` + `WriteLog` block. These are the same idea written four times.

The bid uses three calls with separate inputs: `QuoteSpread`, `TargetBidPrice` and `IsTooDeep`.

## Target shape

### Pack targets

```
packs done   = CompletedPacks(pack)                          // min over legs of floor(traded_i / r_i)
packs ahead  = max( floor(traded_0 / r_0),                   // leg 0 fills drive the hedges
                    max over hedge legs ceil(traded_i / r_i), // hedges ahead drive a leg 0 chase
                    _earlyHedgePacks )                        // synthetic trade
```

`floor` on leg 0 keeps today's rule: a part pack on leg 0 keeps bidding and is not hedged yet. `ceil` on the hedge legs keeps today's rule: a hedge ahead of leg 0 makes leg 0 chase.

### CompletePack and CutPack

Two small functions with the same shape: one target, one loop over the legs, `lots = target × r − traded`.

```
CompletePack(pack):                        // target = PacksAhead
    hedge legs: lots > 0 -> HedgeLeg (placed once at HEDGE_LEVEL, not repriced), else pull
    a hedge leg behind   -> pull the bid (not during an early hedge), return true
    leg 0 lots > 0       -> chase at CHASE_LEVEL once no hedge order is out, return true
    nothing missing      -> return false, leg 0 may bid

CutPack(pack):                             // target = CompletedPacks
    every leg: own order working -> pull it; lots beyond target -> IOC at own-side touch
```

The `TimeToRevertBidMs` windows from `InCutWindow` choose between them. Even windows complete, odd windows cut. A pack is never left naked.

A separate `LegPrice` function was not needed: each mode reads one price with one `GetPrice` call.

### Send

`Send` wraps `_hub->UpdateOrder` and the `[ORDER-OUT]` log line. It returns early when price and quantity are unchanged. `EvaluateBidding`, `CompletePack`, `HedgeLeg` and `CutPack` all use it. Four copies of the send + log block become one.

### BidRate

The reverse price calculation runs once per evaluation and returns one struct:

```
struct BidRate {
    SpreadQuote _quote;     // leg prices P1 is solved against, and the market spread
    int         _price;     // P1; <= 0 when no positive price reaches the target
    bool        _tooDeep;   // P1 behind leg 0's own side at BID_LEVEL_LIMIT, or that level empty
};
```

`RateBid(pack)` builds it. `QuoteSpread` stays, because the UI spread uses it too.

The math does not change:

```
hedge = Σ_{i≠0} signedRatio_i × price_i
raw   = target ± _strikeGap (sign of the raw market spread)     // inverse of AdjustGap
P1    = round_away((raw − hedge) / (signedRatio_0 × tick)) ± PassiveBidTicks
```

- `signedRatio_0` carries both the ratio and the side. Dividing by it turns a pack spread into a leg 0 price.
- `round_away` goes up for SELL and down for BUY, so the spread is never below the target.
- `PassiveBidTicks` (param, default `DEFAULT_PASSIVE_BID_TICKS = 5` when the GUI omits it) moves the price further from the market. The level 3 (index 2) rule checks the offset price.

P1 is solved from the target, so a separate "spread meets target" check is not needed. The guards that remain are `_price <= 0` (UNREACHABLE) and `_tooDeep` (TOO_DEEP). TOO_DEEP pulls the bid, as `Flow.md` says.

`EvaluateBidding` becomes: compute `BidRate`, run the gate guards, compute quantity, `Send`.

### ProcessPack

```
throttled retry window          -> return
cut IOC in flight               -> return
not whole and CUT window        -> CutPack, return
CompletePack                    -> return when it had work           // hedge or chase leg 0
APPLIED                         -> EvaluateBidding                   // whole, or leg 0 part pack
```

When `CompletePack` finds nothing missing, the pack is whole or leg 0 holds a part pack, so bidding is right in both cases. The current `hasPartLots` special case is no longer needed.

`_isUnhedged` stays as a derived flag (a hedge leg is behind `packs ahead`). It drives `Stop("hedge failed, reverse cut")`, the tracer and the slippage check. It no longer drives order flow.

## What was deleted

`MissingBidLots`, `ChaseBiddingLeg`, `HedgePack`, `ExecuteHedgeLeg` and `ExtraLots` were replaced by `PacksAhead`, `CompletePack` and `HedgeLeg`. `TargetBidPrice` and `IsTooDeep` were merged into `RateBid`. Four send + log blocks became `Send`. `CutPack` was rewritten in the same shape. These stay as they were: `HedgeTargetPacks` (it drives `_isUnhedged`), `UpdateUnhedgedStatus`, `IsWhole`, `CompletedPacks`, `InCutWindow`, `OnCutTrade`, `EarlyHedge`.

## What does not change

- Bid price math: passive offset, strike gap, rounding.
- Bid gate order and its logs.
- Early hedge on a `'S'` tick.
- Cut orders as separate IOC objects. A cut in flight pauses the pack.
- Throttle pause and retry.

## Verification (2026-10-16)

The dry run must be built with the g++ line in the `tests/ratio_dry_run.cpp` header. `ninja` does not rebuild it.

With the passive offset at 0, old and new code both print "all scenarios passed" with identical output. The `[ORDER-OUT]` sequence in every strategy log is identical: 150 order lines (48 BID, 66 HEDGE, 1 CHASE, 10 REVERSE-CUT, the rest CANCEL), same pack, leg, side, action, price and quantity.

With 5 ticks and the level 3 rule, leg 0 bids only when the market beats the target by 3 or more ticks, so most scenarios could no longer rest a bid. The offset became the `PassiveBidTicks` param instead. `GuiConfig` pins it to 0, so existing scenarios keep exact P1 prices. `PassiveBidsByDefault` drops the key: the bid rests at P1 525 + 5 ticks = 550 and is pulled when ask level 3 falls to 540. Setting the default to 0 makes that scenario fail (mutation check).

## Decided

- No `_slicePacks` cap outside the bid. Hedge, leg 0 chase and cut each send every missing lot in one order. The slice limits only how much leg 0 bids at a time, and each fill is hedged in full.
