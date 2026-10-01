# ConvexStrategy — Agent Context, Architecture & Mathematical Specification

> Loaded automatically at the start of every Gemini AI session for this workspace.

---

## 1. Project Overview

**ConvexStrategy** is a high-performance C++20 shared-library algorithmic trading strategy plugin for the **MOSS** (Meril Options Strategy System) execution engine. It compiles to `build/Convex/libConvexMoss.so` and is dynamically loaded by the MOSS engine at runtime.

The library implements multi-leg options spread strategies (Box Spread, Butterfly, Ratio N-Leg from 2 to 6 legs) under a common orchestrator (`MinixStrategy`), communicating with a GUI frontend over a chunked JSON control protocol and streaming high-frequency market quotes and binary UI telemetry.

* **Language**: C++20
* **Build System**: CMake + Ninja
* **Target Output**: `build/Convex/libConvexMoss.so`

---

## 2. Directory Layout

```
ConvexStrategy/
├── CMakeLists.txt                  # Top-level build config
├── @CHANGELOG.md                   # Versioned changelog (ALWAYS update before commit)
├── project_flow_documentation.md   # Architectural & workflow doc (keep in sync)
├── strategy_parameters.md          # Parameter reference (depth, slippage, tradeGear, etc.)
├── .agents/
│   └── AGENTS.md                   # Agent system rules, architecture & math specs
├── vendor/                         # Vendor libraries (nlohmann & minix platform) — READ ONLY
│   ├── CMakeLists.txt              # Vendor build config
│   ├── nlohmann/                   # Vendored JSON library (header-only, DO NOT MODIFY)
│   └── minix/                      # Minix platform framework & engine headers (DO NOT MODIFY)
│       ├── CMakeLists.txt          # Minix object library build config
│       ├── include/                # Platform headers (AlgoBase, oms_api, order_instance, etc.)
│       └── src/                    # Platform sources (order_instance.cpp, PortfolioOrderManager.cpp)
└── Convex/                         # Strategy implementation
    ├── CMakeLists.txt              # Compiles ConvexMoss shared library
    ├── MinixStrategy.hpp/.cpp      # Central orchestrator & entry points (create/destroy hooks)
    ├── RatioLeg/                   # Ratio leg strategy implementation (2-Leg to 6-Leg)
    │   ├── RatioLegStrategy.hpp
    │   └── RatioLegStrategy.cpp
    └── Utils.hpp                   # Shared helpers & binary protocol structs
```

---

## 3. Core Architecture & Workflow

### Dynamic Lifecycle Hooks (`MinixStrategy`)
* **`create()` / `destroy()`**: Dynamic entry points invoked by the MOSS engine on shared object load/unload.
* **`onUIRequest()`**: Receives chunked JSON from the GUI frontend (message codes `9612`, `9620`, `9621`). Reassembles 1500-byte chunks into complete JSON payloads and dispatches to `applyLegStrategyJson()`.
* **`OnTick(Quote)`**: High-frequency market quote dispatch to active strategies.
* **`onBcastData(product_data)`**: Broadcast market snapshot routing.
* **`OnOrderResponse(oms_transaction)`**: Routes OMS responses (placement, modification, cancellations, fills) to the target strategy and `PortfolioOrderManager`.
* **`doWork()`**: Heartbeat thread (~1 second interval): pushes binary spread updates to UI via `SendStrategySpreadsToUi()`. Hedging is event-driven (ticks + order responses), not polled here.

### Strategy Lifecycle States
| Action / Status | Behavior |
|-----------------|----------|
| `SUBSCRIBED` / `ACTIVE` | Creates or updates strategy instance, subscribes to market feeds, sets `_status = StrategyStatus_APPLIED`. |
| `APPLIED` / `APPLY` | Updates live parameters dynamically (`ParamUpdate`), rebuilds cache, and activates execution. |
| `UNSUBSCRIBED` / `STOP` | Sets `_status = StrategyStatus_INACTIVE`, cancels all open orders on all legs, resets retry counters. |
| `DELETED` / `DELETE` | Stops strategy, unregisters orders, unsubscribes products, and deallocates strategy memory (`delete`). |

---

## 4. Frontend Units, Sizing & Pack Mathematics

### The Fundamental Pack Principle
In Ratio Spread strategies (e.g., Ratio 3 : 1 : 2):
> **1 Lot from the Frontend = 1 Complete Ratio Pack**
> A single unit of the spread requires every leg to trade in its exact configured ratio multiplier ($\text{Ratio}_i$).

### Parameter Parsing & Unit Conversions
* **`LongBuyQty` / `ShortSellQty` (`param_._totalQuantity`)**: Total volume configured in **Packs**.
* **`LongBuySoQ` / `ShortSellSoQ` (`param_._quantity`)**: Slice Order Quantity configured in **Packs**.
* **Bidding Leg Total Lots**:
  $$\text{totalBiddingLots} = \text{param\_.\_totalQuantity} \times \text{\_ratios}[\text{\_biddingLeg}]$$
* **Remaining Bidding Lots**:
  $$\text{remainingLot} = \text{totalBiddingLots} - \text{object\_.\_tradedLot}[\text{\_biddingLeg}]$$
* **Traded Pack Calculation** (reported to GUI & slippage tracker):
  $$\text{totalPacks} = \min_{i=0}^{N-1} \left(\frac{\text{object\_.\_tradedLot}[i]}{\text{\_ratios}[i]}\right)$$
  Traded quantity is only considered $1$ when **all legs** have filled their respective ratio multipliers.

---

## 5. Partial Fill Management & Pack Remainder Clamping

### The Overfill Prevention Decision
When a partial fill occurs on the primary quoting leg (e.g., 2 lots fill out of a 3-lot slice in a 3:1:2 ratio spread):
* The quoting leg MUST NOT request a full new slice ($3$ lots), as that leads to overfilling ($2 + 3 = 5$ lots) and uneven ratio trades.
* The order quantity in `OrderBiddingLogic` is strictly clamped to the **exact remainder lots** needed to complete the in-flight pack:

$$\text{unhedgedRemainder} = \text{object\_.\_tradedLot}[\text{\_biddingLeg}] \pmod{\text{biddingRatio}}$$
$$\text{packRemainderLots} = (\text{unhedgedRemainder} > 0) \mathbin{?} (\text{biddingRatio} - \text{unhedgedRemainder}) : \left(\frac{\text{cache\_.\_sliceQuantity}[\text{\_biddingLeg}]}{\text{\_lotSize}}\right)$$
$$\text{quantity} = \min(\text{packRemainderLots}, \text{remainingLot}) \times \text{\_lotSize}$$

This mathematical guard guarantees that the quoting leg stops precisely at multiples of its ratio multiplier and never creates unhedgeable fractional pack inventory.

---

## 6. Unhedged State Machine & Hedging Mathematics

### Hedge Status Determination (`UpdateUnhedgedStatus`)
A strategy is unhedged if any secondary (hedge) leg has traded fewer lots than required by the completed bidding packs:
$$\text{biddingPacks} = \left\lfloor\frac{\text{object\_.\_tradedLot}[\text{\_biddingLeg}]}{\text{\_ratios}[\text{\_biddingLeg}]}\right\rfloor$$
$$\text{isUnhedged} = \exists \text{ leg} \neq \text{\_biddingLeg} \text{ such that } \text{object\_.\_tradedLot}[\text{leg}] < (\text{biddingPacks} \times \text{\_ratios}[\text{leg}])$$

* Implemented with zero heap allocations on the hot path (`O(1)` memory).

### Hedge Priority & Execution Flow
1. **Immediate Quoting Cancellation**: Whenever `_isUnhedged == true`, any open order on `_biddingLeg` is cancelled immediately to halt exposure accumulation.
2. **Target Hedge Lots**:
   $$\text{targetHedgeLots} = \text{biddingPacks} \times \text{\_ratios}[\text{leg}]$$
   $$\text{diff} = \text{targetHedgeLots} - \text{object\_.\_tradedLot}[\text{leg}]$$
   $$\text{quantity} = \min(\text{diff} \times \text{\_lotSize}, \text{cache\_.\_sliceQuantity}[\text{leg}])$$
3. **In-Flight Response Guard**:
   ```cpp
   if (order->is_response_pending()) return;
   ```
   Never modify an order or increment retry counters while an OMS/exchange confirmation is pending.
4. **Hedge Order Pricing & Escalation**:
   * Base Price: Captured snapshot price $\text{storedPrice} = \text{object\_.\_windRate.\_price}[\text{leg}]$, falling back to current opposite market touch if uninitialized or aggressive.
   * Directional Side Multiplier: $+1$ for `BUY_SIDE`, $-1$ for `SELL_SIDE`.
   * Aggressive Offset: $\text{\_tradeGearPriceOffset} = \text{\_tradeGear} \times \text{\_tickSize}$.
   * Tick Step Delta: $\text{tickDelta} = \text{isAggressive} \mathbin{?} 0 : (\text{retryCount} \times \text{\_tickSize})$.
   * Target Limit Price:
     $$\text{targetOrderPrice} = \text{basePrice} + \text{sideMultiplier} \times (\text{\_tradeGearPriceOffset} + \text{tickDelta})$$
   * Price Floor Guard: Clamped to $\ge \text{\_tickSize}$ (never $\le 0$).
5. **Aggressive Escalation Condition**:
   $$\text{isAggressive} = (\text{\_marketOrderRetries} == 0 \mathbin{\Vert} \text{retryCount} \ge \text{\_marketOrderRetries})$$
   * If `MarketOrderRetries == 0`: immediately placed aggressively at the opposite touch (BUY at Ask, SELL at Bid) without progressive tick decay.
   * If `MarketOrderRetries > 0`: steps aggressively closer to the market by 1 tick per retry until reaching `_marketOrderRetries`, then crosses the spread to opposite touch.

---

## 7. Real-Time Spread, PnL & Cost Mathematics (Paise)

### Internal Representation: Strictly Paise
All prices, values, and calculations operate strictly in **paise** ($\text{Rupees} \times 100$). Never perform rupee-level arithmetic in spread or risk formulas.

### Spread Formula
$$\text{RawSpread} = \sum_{i=0}^{N-1} \text{SideSign}_i \times \text{Price}_i \times \text{Ratio}_i$$
* $\text{SideSign}_i = -1$ if execution side is `BUY_SIDE`, $+1$ if `SELL_SIDE`.
* For buying a spread (`GetBCmp`): leg prices evaluated at opposite touch (Ask for BUY, Bid for SELL).
* For selling a spread (`GetSCmp`): leg prices evaluated at opposite touch (Bid for BUY, Ask for SELL).

### Gap Adjustment
If `_gapDiff` is active (Box, ConRev):
$$\text{AdjustGap}(\text{spread}) = (\text{spread} < 0) \mathbin{?} (\text{spread} + \text{gap}) : (\text{spread} - \text{gap})$$

### Realized PnL (RLP) & Transaction Costs
$$\text{RLP} = \sum_{i=0}^{N-1} \min(\text{buyQuantity}_i, \text{sellQuantity}_i) \times (\text{avgSellPrice}_i - \text{avgBuyPrice}_i) - \text{totalCost}_i$$
* Transaction costs:
  * Options: Buy = 60p per ₹10,000 notional | Sell = 70p per ₹10,000 notional.
  * Futures: Buy = 20p per ₹10,000 notional | Sell = 20p per ₹10,000 notional.

### Mark to Market (M2M) & Net PnL
$$\text{M2M} = \sum_{i=0}^{N-1} \text{netQuantity}_i \times (\text{markPrice}_i - \text{avgPrice}_i)$$
* $\text{markPrice} = \text{Bid}[0]$ if $\text{netQuantity} > 0$ (long), $\text{Ask}[0]$ if $\text{netQuantity} < 0$ (short).
* Net PnL: $\text{NetPL} = \text{RLP} + \text{M2M}$.

### Slippage Guard (`CheckSlippageThreshold`)
$$\text{tradedSpread} = \sum_{i=0}^{N-1} (\text{side}_i == \text{BUY} \mathbin{?} -\text{legAvgPrice}_i : \text{legAvgPrice}_i) \times \text{Ratio}_i$$
$$\text{slippage} = \frac{\text{param\_.\_spread} - \text{AdjustGap}(\text{tradedSpread})}{100.0}$$
* If `_allowedSlippage > 0` and $\text{slippage} > \text{\_allowedSlippage}$, the strategy immediately invokes `Stop()` to prevent adverse market damage.

---

## 8. Logging Architecture & Performance Rules

### Zero-Allocation, Bounded Logging
1. **Never log in the high-frequency tick path on benign or repeating conditions**:
   * NEVER log `diff <= 0` inside `ExecuteHedgeLeg` (causes multi-gigabyte log explosions).
   * NEVER log when `targetOrderPrice == currentPlacePrice` (order already placed).
   * NEVER log when `is_response_pending()` is true.
2. **Allowed Logging Points**:
   * Strategy initialization and termination (`INIT`, `DESTROY`, `STOP`).
   * Parameter reconfiguration (`ParamUpdate`).
   * Order placed / modified successfully (when UID > 0).
   * Trade executions (`[TRADE EVENT]`).
   * Cycle completion and slippage evaluation (`[SLIPPAGE]`, `Tracer`).
3. **Format Integrity**:
   * Every log call MUST terminate with a newline `\n`. Missing newlines concatenate entries and corrupt downstream ingestion.

---

## 9. Non-Negotiable Coding Standards

1. **Always Update `@CHANGELOG.md`** before committing using standard `[MAJOR.MINOR.PATCH] - YYYY-MM-DD` syntax.
2. **Prices in Paise**: Never mix rupee and paise values.
3. **Response-Pending Guard**: Always check `is_response_pending()` before calling `update_order()` or `cancel_order()`.
4. **Vendor Directory Is Read-Only**: Never edit `vendor/minix/` or `vendor/nlohmann/`.
5. **Explicit Non-Abbreviated Naming**:
   * NEVER use cryptic abbreviations (`idx` → `index`, `qty` → `quantity`, `px` → `price`).
   * Differentiate depth book levels from strategy parameters: use `levelPrice` / `levelQuantity` for book scans, and `orderQuantity` / `quantityAhead` for queue tracking.
6. **Price Grid Quantization**: All price roundings must use `RoundOFF` formula.
7. **Clean Memory Ownership**: Strategies allocated with `new` in `MinixStrategy` must be safely paired with `delete` on strategy deletion.
8. **Documentation Sync**: Keep `.agents/AGENTS.md`, `GEMINI.md`, and `project_flow_documentation.md` in sync whenever architecture, parameters, or execution rules change.
