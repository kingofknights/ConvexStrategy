# Ratio IOC entry: plan

Branch `ratio-ioc`, cut from `ratio-sweep-gate` (2f8e419). Worktree: `/home/vikramlodhi/Projects/pet/Worktree/ConvexStrategy-ratio-ioc`.

## Requirement

The GUI sends `Strategy.IsBidding`.

- `true`: today's bidding logic. Leg 0 rests one LIMIT order at the back-solved price P1 (target + cushion).
- `false`: IOC logic. Leg 0 never rests. It fires an IOC at its opposite touch when the market spread at the opposite top of book meets the target.

Everything after a leg 0 fill (spread check, reverse, hedges, slippage stop, Trade Tracer) is the same in both modes.

The per-leg `OrderType` ("BID"/"IOC") and `EnableBid` fields stay ignored. `IsBidding` is the only switch.

## Decisions (user approved 2026-11-03)

1. Fire rule: every leg at its opposite top of book (`MarketSpread(pack, &prices, 0)`). Fire when that market spread >= the target. No upper band: a market far better than the target still fires.
2. IOC price: leg 0's opposite touch (`prices[BiddingLeg]`). Only level 0 fills; any spread above the target is profit. No back-solve, no cushion.
3. Hedge depth check: fire only when every hedge leg's opposite top-of-book quantity covers the lots that leg needs for the IOC's packs. A thin hedge touch would make the hedge walk the book and slip.
4. Leg 0 goes first, alone. Hedges go out after the fill through the existing `OnTrade` / `EvaluateHedgeMode` path: LIMIT at the opposite side's index 2, repriced every tick until filled (never a naked position).
5. Re-fire: no cooldown. The next IOC goes out on the first tick after the last one is answered (filled or cancelled). `MarketRetries` stays unparsed. Add a cooldown only if `THROTTLE_LIMIT` (-9996) rejects show up in the logs.
6. Mode switch while live: if `IsBidding` changes while leg 0 has a live order, cancel the order. Swap the order object once leg 0 is idle.

## Design

### Order type

The order type is fixed when `OrderObjectT` is built (`order_instance::typeoforder`, written back on every `reset()`). One object cannot switch between LIMIT and IOC.

So the leg 0 object in `Pack::_orders[BiddingLeg]` is rebuilt with the other type when the mode changes. The IOC uses the same slot as the bid, so `OnOrderResponse`, `OnTrade`, `IsIdle`, `Pull`, `Stop` and the reverse path need no change. The reverse order (`Pack::_reverse`) and the hedge orders stay LIMIT.

### Changes, file by file

`Convex/RatioLeg/RatioLegStrategy.hpp`
- `bool _isBidding = true;` on the strategy.
- `ORDER_TYPE _entryType = LIMIT_ORDER_TYPE;` on `Pack`: the type leg 0's object was built with.

`Convex/RatioLeg/RatioLegStrategy.cpp`
- `ParamUpdate`: read `json_["Strategy"].value("IsBidding", true)` and log it with the other params. `ParamUpdate` already gets the root JSON, which has `Strategy`.
- Constructor: build leg 0's object with `IOC_ORDER_TYPE` when `_isBidding` is false, and set `_entryType`.
- `EvaluateBidding`, right after the stray-hedge pulls and before the active check:
  - If `pack_._entryType` does not match the mode: when leg 0 is idle, rebuild its object with the wanted type; otherwise `Pull` it ("order type change") and return.
- `EvaluateBidding`: move the slice quantity calculation up, before the cushion. It does not depend on the price.
- `EvaluateBidding`, after the market prices (index 0 and index 2) and the quantity: if `!_isBidding`, go to the IOC branch and return. The index 2 check stays shared: the hedges are sent at index 2, so a leg with no level 2 price cannot be hedged.
- IOC branch (a small `FireIoc(pack, prices, market, quantity)`):
  1. Leg 0 not idle (an IOC is in flight): return. This check is required: without it `UpdateOrder` would try to modify a live IOC.
  2. Sweep stop band (`_sweepBand == 2`): `Hold("sweep above SweepStopLevel")`, return.
  3. Exchange round trip above `MaxConfirmUs`: `Hold(...)`, return.
  4. Market spread below the target (target + `_sweepBuffer` in the sweep buffer band): `Hold("IOC: market spread below target")`, return.
  5. Hedge depth: for each hedge leg, `QuantityUpTo(_quote[leg], pack_._oppositeSide[leg], 1)` must be >= `packs * _ratios[leg] * _lotSize`, where `packs = quantity / _lotSize / _ratios[BiddingLeg]`. Short: `Hold("IOC: hedge touch thin", leg)`, return.
  6. A hedge touch younger than `HedgeHoldUs` (`FreshHedge`): `Hold(...)`, return.
  7. `Hold({})`, then `UpdateOrder(pack_, BiddingLeg, prices[BiddingLeg], quantity, "IOC")`.
  - TickSize band, `AllowedBidDepth` and the cushion do not apply: the IOC never rests.
- `TryEarlyHedge`: return at once when `!_isBidding`. The 'S' early hedge is for a resting bid.

No change in `MinixStrategy`, the GUI, or the wire protocol.

### Fill handling (unchanged code, IOC behavior)

- Full fill: `OnTrade` spread check at the hedge touch. Spread still there: `EvaluateHedgeMode` sends hedges. Spread gone: reverse leg 0, as today.
- Partial fill (leg 0 touch thinner than the slice): the exchange cancels the rest. Hedges go out only for whole packs: 2 of 3 leg 0 lots is no pack yet, so no hedge. The next tick fires the missing lots if the spread still holds; the hedge depth check counts the lots already traded.
- Part-pack reverse (`ReversePartPack`, user chose this over a leg 0 touch check): once the IOC is over, leg 0 lots short of a whole pack are moved into the reverse (`_reverseLots`) when the market spread is below target or the strategy is stopped. They are sent back at leg 0's own side index 2 like a reverse at the fill, with a "Reverse" Tracer row. After a square-off they are forgotten, not reversed: square-off owns them.
- No fill (touch moved before the IOC landed): exchange cancel, no position.

### Example (long pack, strike gap left out)

Leg 0 23500 SELL ratio 3, leg 1 23550 BUY ratio 1, leg 2 23450 BUY ratio 2, target `LongBuyPrice` 2.50, SoQ 1, lot 65.

- leg 0 bid 100.00 x 3 = +300.00; leg 1 ask 120.00 x 1 = -120.00; leg 2 ask 88.50 x 2 = -177.00.
- Market spread 3.00 >= 2.50. Hedge depth: leg 1 ask needs >= 65, leg 2 ask needs >= 130.
- IOC SELL 195 @ 100.00.

The short pack mirrors this: leg 0 at its ask, legs 1 and 2 at their bids, target `ShortSellPrice`.

## Dry run (`tests/ratio_dry_run.cpp`)

The simulator already cancels the unfilled rest of an IOC (`CancelUnfilledIoc`). `RatioConfig` needs an `IsBidding` argument. New scenarios:

1. IOC mode, market spread below target: no leg 0 order.
2. IOC mode, market meets target: one leg 0 IOC at its opposite touch, it fills, hedges go out at index 2, the pack completes.
3. IOC mode, market meets target but a hedge touch is thinner than its needed lots: no IOC.
3a. IOC part fill, then the spread goes: the part-pack lots are reversed; a full IOC once the spread is back. A part pack left at STOP is reversed.
4. IOC partial fill: rest cancelled, hedges for the filled lots only, next tick fires the remaining lots.
5. IOC miss (touch moves before the IOC lands): cancelled, no position, no hedges.
6. Mode switch with a resting bid (`IsBidding` true to false): the bid is cancelled, then the next order is an IOC. And false to true: the next order is a resting LIMIT.
7. Sweep stop band in IOC mode: no IOC fired.
8. IOC fill where the spread is gone at the hedge touch: leg 0 reversed, as in bidding mode.
9. GUI square-off with part-pack IOC lots open: the strategy stops and forgets them, square-off flattens them, no IOC or hedge after.

Existing bidding scenarios must pass unchanged.

Simulator change: an IOC trades on arrival against the touch, up to the touch quantity (`Book::bidQty/askQty`), then the rest is cancelled. Resting LIMIT orders match as before.

Status 2026-11-03: implemented, all scenarios pass.

## Not done

- Cooldown or retry cap on IOC misses (decision 5).
- Sweeping deeper leg 0 levels (back-solved IOC price).
- IOC hedges.
- Firing all legs at once.
