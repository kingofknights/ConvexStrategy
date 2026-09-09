# ConvexStrategy — Parameter Configuration Details

This document outlines the usage and logic behind the key execution parameters defined under the `Params` section of strategy configurations (configured via the GUI frontend or test configuration JSON files).

---

### 1. Tick Size (`TickSize` / `_minTickChange`)
* **Description:** The minimum price change step required to trigger a bidding order modification.
* **Details:** The exchange's minimum tick size is small (e.g., 5 paise in NSE-FNO). Setting our bidding adjustments to react to every 5 paise change would result in an excessive volume of order modifications, leading to communication line exhaustion. To prevent this, the user provides a larger minimum tick change threshold.
* **Engine Behavior:** Bids are only modified if the target bid price changes by at least the specified `TickSize`.

### 2. Order Depth (`OrderDepth` / `_orderDepth`)
* **Description:** The minimum count of active orders (bids/asks) that must be present in the book for the market leg (hedge leg) from the top level downwards.
* **Details:** For example, setting `OrderDepth = 4` means there must be at least 4 distinct orders active in the top levels of the order book for the market leg.
* **Engine Behavior:** Gated by `CheckOrderDepth`. If this depth condition is not met, the main leg will not place a bid.

### 3. Price Depth (`PriceDepth` / `_priceDepth`)
* **Description:** The minimum number of valid price levels (levels with a price > 0) that must be present in the market leg (hedge leg) depth.
* **Details:** For example, `PriceDepth = 2` means there must be active orders populated across at least 2 distinct price levels in the market leg's order book.
* **Engine Behavior:** Gated by `CheckPriceDepth`. If the hedge leg depth is insufficient, bidding on the main leg will not start (or will be immediately cancelled).

### 4. Allowed Bid Depth (`AllowedBidDepth` / `_allowedBidDepth`)
* **Description:** The minimum number of valid price levels that must be present in the order book of the primary **bidding leg** itself.
* **Engine Behavior:** Similar to `PriceDepth`, but checked against the bidding leg instead of the hedge leg. It ensures the bidding leg has sufficient market depth before the strategy starts quoting.

### 5. Threshold Qty (`ThresholdQty` / `_thresholdQty`)
* **Description:** An extra safety buffer quantity (expressed as a percentage) that must be available in the hedge leg's order book.
* **Details:** After meeting the `OrderDepth`, `PriceDepth`, and `AllowedBidDepth` checks, the order book must also have this percentage-based volume buffer to absorb the hedge order.
* **Example:** For a Butterfly Strategy (e.g., 2-1-1 setup, requiring 2 lots for the bid leg), if `ThresholdQty = 200` (representing 200%), there must be at least $2 \times 200\% = 4$ lots available for execution in the market leg's order book before a bid is placed.

### 6. Allowed Slippage (`AllowedSlippage` / `_allowedSlippage`)
* **Description:** The maximum execution slippage tolerated after a trade executes.
* **Details:** Calculated in paise (input multiplied by 100).
* **Engine Behavior:** If a trade executes and the resulting slippage exceeds the `AllowedSlippage`, the strategy immediately halts bidding for that position and pauses itself to protect against adverse market movements.

### 7. Market Order Retries (`MarketOrderRetries` / `_marketOrderRetries`)
* **Description:** The maximum number of progressive tick-stepped limit modifications permitted for hedge legs before escalating to aggressive market touch execution.
* **Engine Behavior:** For each retry attempt $k < \text{MarketOrderRetries}$, the hedge limit price is adjusted by 1 tick closer to the market. When retries reach `MarketOrderRetries`, the order is aggressively placed at the opposite side market touch (BUY at Ask, SELL at Bid) to guarantee execution. The strategy is **never** stopped on max retries — stoppage is governed exclusively by `AllowedSlippage`.

### 8. Trade Gear (`TradeGear` / `_tradeGear`)
* **Description:** The number of aggressive ticks applied when placing orders on hedge legs.
* **Details:** Configured as an integer tick count. Converted internally to `_tradeGearPriceOffset = _tradeGear * _tickSize`.
* **Engine Behavior:** When a fill occurs on the primary bidding leg and hedge legs must execute, the target limit price is offset by `_tradeGear` ticks in the aggressive direction (BUY: $\text{Ask} + \text{TradeGear} \times \text{TickSize}$; SELL: $\text{Bid} - \text{TradeGear} \times \text{TickSize}$). This accelerates fill latency and improves order queue priority. Each subsequent retry step adds one additional tick on top of this aggressive offset.

