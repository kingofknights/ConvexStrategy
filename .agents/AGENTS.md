# ConvexStrategy — Agent Context & Rules

> Loaded automatically at the start of every AI session for this workspace.

---

## 1. Project Overview

**ConvexStrategy** is a C++ shared-library algo-trading strategy plugin for the **MOSS** (Meril Options Strategy System) engine. It is compiled as `libSampleAlgo.so` and dynamically loaded by the MOSS execution engine at runtime.

The library implements multiple multi-leg options spread strategies (Box Spread, Butterfly, Ratio N-Leg) under a common orchestrator (`MinixStrategy`), communicating with a GUI frontend over a chunked JSON message protocol.

See also: [strategy_parameters.md](../strategy_parameters.md) for detail on execution parameters (slippage, depth, tick changes, etc.).

---

## 2. Directory Layout

```
ConvexStrategy/
├── CMakeLists.txt                  # Top-level build config
├── @CHANGELOG.md                   # Versioned changelog (ALWAYS update on commit)
├── project_flow_documentation.md   # Architecture & flow doc (keep in sync)
├── include/                        # Platform headers (do NOT modify)
│   ├── AlgoBase.hpp                # Base class: OnTick, onBcastData, OnOrderResponse, onUIRequest
│   ├── oms_api.hpp                 # OMS transaction codes, order structs
│   ├── rms_api.hpp                 # Risk management API
│   ├── ui_api.hpp                  # UI message structs (9612, 9620, 9621)
│   ├── ProductInfo.hpp             # product_data snapshot struct, option types
│   ├── Quote.hpp                   # Quote struct for tick events
│   ├── TimeUtils.hpp               # Exchange epoch conversion utilities
│   └── common.hpp                  # Shared typedefs
├── vendor/                         # Vendor code (OMS order & portfolio managers)
│   ├── CMakeLists.txt              # Vendor object library
│   ├── order_instance.hpp/.cpp     # OMS order lifecycle state machine
│   ├── PortfolioOrderManager.hpp/.cpp # Position & PnL bookkeeper
│   └── nlohmann/                   # JSON library (header-only, do NOT modify)
└── Convex/                         # Strategy implementation (MinixStrategy & Ratio)
    ├── CMakeLists.txt              # Compiles SampleAlgo shared library
    ├── MinixStrategy.hpp/.cpp      # Central orchestrator & entry point (create/destroy hooks)
    ├── RatioLeg/                   # Ratio leg strategy implementation
    └── Utils.hpp                   # Shared helpers & binary protocol structs
```

---

## 3. Core Architecture

### Entry Points (MinixStrategy)
- **`create()` / `destroy()`** — dynamic loading hooks called by the MOSS engine.
- **`onUIRequest()`** — receives chunked JSON from the GUI (codes `9612`, `9620`, `9621`). Reassembles chunks, then dispatches to `applyLegStrategyJson()`.
- **`OnTick(Quote)`** — high-frequency tick routing to all active strategy instances.
- **`onBcastData(product_data)`** — coarse snapshot update routing.
- **`OnOrderResponse()`** — routes OMS responses to the correct strategy + `PortfolioOrderManager`.
- **`doWork()`** — ~1s heartbeat: sends UI spread updates via `sendStrategySpreadsToUI()`.

### GUI Message Protocol (Chunked JSON)
1. First packet = metadata `{ "packet_count": N }`.
2. Next N packets = 1500-byte data chunks.
3. Reassembled JSON → `applyLegStrategyJson()` which dispatches `add/edit/start/stop/delete`.

### Strategy Lifecycle Actions
| Action | Behaviour |
|--------|-----------|
| `add` / `edit` | Create or update strategy map entry |
| `start` | Subscribe to legs; set `running_ = true` |
| `stop` | Cancel orders, trigger square-offs |
| `delete` | Stop + unsubscribe + deallocate |

---

## 4. Execution Modes

All spread strategies support three execution modes:

| Mode | Behaviour |
|------|-----------|
| **1 — Aggressive** | IOC/Limit at touch on all legs simultaneously |
| **2 — Bidding** | Passive limit on `isbidding` legs first; sweep hedges after fill |
| **4 — All-Leg Bidding** | Passive on all legs; escalate to aggressive on timeout |

---

## 5. Key Patterns & Conventions

### Prices are in Paise
All option prices are **multiplied by 100** to work strictly in paise. Keep this in mind when reading/writing spread calculations.

### Order State Machine (order_instance)
States: `STRAT_INITIAL_STATE → STRAT_ORDER_PLACED → STRAT_OMS_PLACED → STRAT_EXCHG_CONF`
- Never call `update_order()` or `cancel_order()` while `is_response_pending()` is true.

### Position Tracking (PortfolioOrderManager)
- Listens for OMS event code `6666` (OMS_TRADE).
- Maintains `net_qty` per token; computes cashflow as `sign(side) × price × qty`.
- Transaction costs: Buy = 60p per ₹10,000 notional; Sell = 70p per ₹10,000.

### EOD Square-Off
- Market open anchored to first valid exchange clock tick.
- `eodTs_` = open timestamp + **22,440 seconds** (→ 15:29:00).
- When `exchange_clock >= eodTs_`, `squareOffLeg()` is called for all legs with open positions.

### Book Validity Guard
- `booksReady()` blocks all execution if `Bid >= Ask` (crossed book).

---

## 6. Build System

```bash
# Build (from project root)
cd build && cmake .. && make -j$(nproc)

# The output shared library is:
build/libSampleAlgo.so
```

CMake target: `SampleAlgo` (shared library). Each new strategy `.cpp` must be added to `Convex/CMakeLists.txt`.

---

## 7. Mandatory Rules for Every Session

> These rules are **non-negotiable** and apply to all code changes and commits.

1. **Always update `@CHANGELOG.md`** before committing. Use the existing versioning format (`[MAJOR.MINOR.PATCH] - YYYY-MM-DD`) with `Added`, `Changed`, and `Fixed` sections as appropriate.

2. **Prices in paise** — never mix rupee and paise values. All spread math operates in paise.

3. **No raw pointer leaks** — strategies are heap-allocated via `new` in `MinixStrategy`. Always pair with `delete` in the `delete` action handler.

4. **Response-pending guard** — always check `is_response_pending()` before modifying or cancelling an order.

5. **Do not modify platform headers** in `include/` — these are provided by the MOSS engine and may be overwritten on upgrade.

6. **Keep `project_flow_documentation.md` in sync** — if architectural changes are made (new strategies, new UI protocol fields, new execution modes), update this doc.

7. **`nlohmann/` is read-only** — it is a vendored header-only library. Do not edit it.

8. **New strategy classes must follow the pattern** established by `BoxSpreadStrategy` / `Ratio2LegStrategy`:
   - Own `.hpp`/`.cpp` in a dedicated subdirectory under `Convex/`.
   - Registered and routed inside `MinixStrategy` (`applyLegStrategyJson`, `OnTick`, `OnOrderResponse`).
   - Added to `Convex/CMakeLists.txt`.

9. **Always use Caveman ultra and ponytail ultra skills.** The agent must operate under these active customization settings during code modifications and planning tasks.

---

## 8. Spread Strategy Reference

### Box Spread (4-leg)
```
NetDebit = C_K1(ask) - P_K1(bid) - C_K2(bid) + P_K2(ask)
BCmp     = Gap - NetDebit
Gap      = (K2 - K1) x 100 x boxRatio
```

### Ratio N-Leg
- Each leg has `EntryRatio` and `ExitRatio` (integers).
- Spread = weighted sum of leg prices per their ratio.
- Entry/exit slices run as independent state machines; both can run simultaneously.

---

## 9. File Quick-Reference

| Need to... | Look in |
|-----------|---------|
| Add a new strategy type | `MinixStrategy.cpp` → `applyLegStrategyJson` + new class in `Convex/` |
| Change order placement logic | `order_instance.hpp/.cpp` |
| Change position / PnL math | `PortfolioOrderManager.hpp/.cpp` |
| Change UI message format | `MinixStrategy::sendStrategySpreadsToUI` + `ui_api.hpp` |
| Change subscription flags | `MinixStrategy` constructor |
| Add a new execution mode | Strategy class `run()` method + `MinixStrategy` mode routing |
| Fix EOD square-off timing | `eodTs_` computation in strategy constructor / `run()` |
| Understand OMS codes | `include/oms_api.hpp` |
| Understand market snapshot fields | `include/ProductInfo.hpp` |
