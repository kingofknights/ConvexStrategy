# ConvexStrategy — Parameter Configuration Details

Keys are exactly as the GUI sends them (checked against a real payload, 2026-10-03; `tests/ratio_dry_run.cpp` uses it verbatim). Read: `Legs[].Token`, `Legs[].Side`, `Ratio.LegRatios`, `Strategy.StrategyId/Status/SubType`, and the `Params` below plus `LongBuy*` / `ShortSell*` (`Qty` packs, `SoQ` packs per slice, `Price` rupees, net-credit convention). Sent but ignored by the engine: leg `OrderType` (all orders are LIMIT), `EnableBid`, `LegID`, `Lots`, `StrikePrice` (lot size and strike come from product details), `UnhedgedAction`, `TimeToRevertBidMs`, `ExecutionMode`, `BestBid`, `NormalBid`, `OrdersType`, `PriceExecutionRange`, `PN_*`, `AllowDuplicates`, `AllowDifferentLotsScripts`, `ShortFlag`, `B-*`/`S-*`.

This document outlines the usage and logic behind the key execution parameters defined under the `Params` section of strategy configurations (configured via the GUI frontend or test configuration JSON files).

---

### 1. Tick Size (`TickSize` / `_repriceTicks`)
* **Description:** The minimum price change step required to trigger a bidding order modification.
* **Details:** The exchange's minimum tick size is small (e.g., 5 paise in NSE-FNO). Setting our bidding adjustments to react to every 5 paise change would result in an excessive volume of order modifications, leading to communication line exhaustion. To prevent this, the user provides a larger minimum tick change threshold.
* **Engine Behavior:** Counted in exchange ticks. The bid is only repriced when the touch moves by at least `TickSize × tick size` paise (`_repriceThreshold`). `0` reprices on every move.

### 2. Order Depth (`OrderDepth` / `_orderDepth`)
* **Description:** The minimum count of active orders (bids/asks) that must be present in the book for the market leg (hedge leg) from the top level downwards.
* **Details:** For example, setting `OrderDepth = 4` means there must be at least 4 distinct orders active in the top levels of the order book for the market leg.
* **Engine Behavior:** Gated by `CheckOrderDepth` on the side each hedge will take (BUY hedge -> asks): each of the first `OrderDepth` levels must have at least one order. The same levels supply the quantity checked by `ThresholdQty`. If the condition fails, the bid is not placed (or is cancelled). Capped at 5.

### 3. Price Depth (`PriceDepth` / `_priceDepth`)
* **Description:** The minimum number of valid price levels (levels with a price > 0) that must be present in the market leg (hedge leg) depth.
* **Details:** For example, `PriceDepth = 2` means there must be active orders populated across at least 2 distinct price levels in the market leg's order book.
* **Engine Behavior:** Gated by `CheckPriceDepth` on the side each hedge will take. If the hedge leg depth is insufficient, bidding on the main leg will not start (or will be immediately cancelled). Capped at 5.

### 4. Allowed Bid Depth (`AllowedBidDepth` / `_allowedBidDepth`)
* **Description:** The minimum number of valid price levels that must be present in the order book of the primary **bidding leg** itself.
* **Engine Behavior:** Similar to `PriceDepth`, but checked on the bidding leg's own side of the book. It ensures the bidding leg has sufficient market depth before the strategy starts quoting. Capped at 5.

### 5. Threshold Qty (`ThresholdQty` / `_hedgeDepthPercent`)
* **Description:** An extra safety buffer quantity (expressed as a percentage) that must be available in the hedge leg's order book.
* **Details:** After meeting the `OrderDepth`, `PriceDepth`, and `AllowedBidDepth` checks, the order book must also have this percentage-based volume buffer to absorb the hedge order.
* **Engine Behavior:** Stored as `_hedgeDepthPercent`; `0` or less means 100%. Required quantity per hedge leg = slice quantity of that leg × percent / 100 (`_requiredHedgeDepth`).
* **Example:** For a Butterfly Strategy (e.g., 2-1-1 setup, requiring 2 lots for the bid leg), if `ThresholdQty = 200` (representing 200%), there must be at least $2 \times 200\% = 4$ lots available for execution in the market leg's order book before a bid is placed.

### 6. Allowed Slippage (`AllowedSlippage` / `_allowedSlippage`)
* **Description:** The maximum execution slippage tolerated after a trade executes.
* **Details:** Integer rupees per pack: slippage = (target spread − traded spread) / 100, evaluated each time a whole pack completes. `0` disables the guard.
* **Engine Behavior:** If the slippage of a completed pack exceeds `AllowedSlippage`, the strategy calls `Stop()`: no new entries. An unhedged pack still hedges until flat.

### 7. Market Retries (`MarketRetries` / `_marketOrderRetries`)
* **Description:** The maximum number of progressive tick-stepped limit modifications permitted for hedge legs before escalating to aggressive market touch execution.
* **Engine Behavior:** For each retry attempt $k < \text{MarketRetries}$, the hedge limit price is adjusted by 1 tick closer to the market, starting from the leg price captured when the bid was sent (`_bidSnapshot`). A retry is one sent modify, so steps advance at order round-trip speed. Counts reset when the pack becomes hedged. When retries reach `MarketRetries`, the order is aggressively placed at the opposite side market touch (BUY at Ask, SELL at Bid) to guarantee execution. The strategy is **never** stopped on max retries — stoppage is governed exclusively by `AllowedSlippage`.

### 8. Trade Gear (`TradeGear` / `_tradeGear`)
* **Description:** The number of aggressive ticks applied when placing orders on hedge legs.
* **Details:** Configured as an integer tick count. Converted internally to `_tradeGearOffset = _tradeGear * _tickSize`.
* **Engine Behavior:** When a fill occurs on the primary bidding leg and hedge legs must execute, the target limit price is offset by `_tradeGear` ticks in the aggressive direction (BUY: $\text{Ask} + \text{TradeGear} \times \text{TickSize}$; SELL: $\text{Bid} - \text{TradeGear} \times \text{TickSize}$). This accelerates fill latency and improves order queue priority. Each subsequent retry step adds one additional tick on top of this aggressive offset.

