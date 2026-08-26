# ConvexStrategy — Agent Context & Rules (GEMINI.md)

> This file is loaded automatically at the start of every Gemini AI session for this workspace.

---

## 1. Project Overview

**ConvexStrategy** is a C++ shared-library algo-trading strategy plugin for the **MOSS** (Meril Options Strategy System) engine. It compiles to `libSampleAlgo.so` and is dynamically loaded by the MOSS execution engine at runtime.

The library implements multiple multi-leg options spread strategies (Box Spread, Butterfly, Ratio N-Leg) under a common orchestrator (`MinixStrategy`), communicating with a GUI frontend over a chunked JSON message protocol.

See also: [strategy_parameters.md](strategy_parameters.md) for detail on execution parameters (slippage, depth, tick changes, etc.).

**Language:** C++17  
**Build System:** CMake  
**Output:** `build/libSampleAlgo.so`

---

## 2. Directory Layout

```
ConvexStrategy/
├── CMakeLists.txt                       # Top-level build config
├── @CHANGELOG.md                        # Versioned changelog — ALWAYS update on commit
├── project_flow_documentation.md        # Architecture & flow doc — keep in sync
├── include/                             # Platform headers — do NOT modify
│   ├── AlgoBase.hpp                     # Base class hooks: OnTick, onBcastData, OnOrderResponse, onUIRequest
│   ├── oms_api.hpp                      # OMS transaction codes & order structs
│   ├── rms_api.hpp                      # Risk management API
│   ├── ui_api.hpp                       # UI message structs (codes 9612, 9620, 9621)
│   ├── ProductInfo.hpp                  # product_data snapshot struct, option types
│   ├── Quote.hpp                        # Quote struct for tick events
│   ├── TimeUtils.hpp                    # Exchange epoch conversion utilities
│   └── common.hpp                       # Shared typedefs
└── sample_strat/                        # All strategy implementation lives here
    ├── CMakeLists.txt                   # Compiles all strategies into libSampleAlgo.so
    ├── MinixStrategy.hpp / .cpp         # Central orchestrator & dynamic entry point
    ├── PortfolioOrderManager.hpp / .cpp # Position & PnL bookkeeper
    ├── order_instance.hpp / .cpp        # OMS order lifecycle state machine
    ├── Utils.hpp                        # Shared helper utilities
    ├── BoxSpread/
    │   └── BoxSpreadStrategy.hpp / .cpp # 4-leg box spread strategy
    ├── Butterfly/
    │   └── ButterflyStrategy.hpp / .cpp # 3-leg butterfly spread strategy
    ├── ConversionReversal/              # Conversion/reversal strategy
    ├── Ratio2Leg/
    │   └── Ratio2LegStrategy.hpp / .cpp # 2-leg ratio spread strategy
    ├── Ratio3Leg/ … Ratio6Leg/          # Higher N-leg ratio strategies
    └── nlohmann/                        # Vendored JSON library — do NOT modify
```

---

## 3. Core Architecture

### MinixStrategy — Central Orchestrator
| Hook | Description |
|------|-------------|
| `create()` / `destroy()` | Dynamic loading hooks called by MOSS engine |
| `onUIRequest()` | Receives chunked JSON from GUI (codes 9612/9620/9621), reassembles, dispatches |
| `OnTick(Quote)` | High-frequency tick routing to all active strategy instances |
| `onBcastData(product_data)` | Coarse broadcast snapshot routing |
| `OnOrderResponse()` | Routes OMS responses to correct strategy + PortfolioOrderManager |
| `doWork()` | ~1s heartbeat — calls `sendStrategySpreadsToUI()` |

### GUI Chunked JSON Protocol
1. First packet = metadata: `{ "packet_count": N }`
2. Next N packets = 1500-byte data chunks
3. Fully reassembled JSON → `applyLegStrategyJson()` → action dispatch

### Strategy Lifecycle Actions
| Action | Behaviour |
|--------|-----------|
| `add` / `edit` | Create or update strategy map entry |
| `start` | Subscribe to leg tokens; set `running_ = true` |
| `stop` | Cancel outstanding orders; trigger square-offs |
| `delete` | Stop + unsubscribe + `delete` the strategy object |

---

## 4. Execution Modes

| Mode | Name | Behaviour |
|------|------|-----------|
| **1** | Aggressive | IOC/Limit at touch on all legs simultaneously |
| **2** | Bidding | Passive limit on `isbidding` legs first; sweep hedges after fill |
| **4** | All-Leg Bidding | Passive on all legs; escalate to aggressive on timeout |

---

## 5. Key Patterns & Conventions

### Prices Are Always in Paise
All option prices are **multiplied by 100** internally (paise). Never mix rupee and paise values.

### Order State Machine (`order_instance`)
```
STRAT_INITIAL_STATE → STRAT_ORDER_PLACED → STRAT_OMS_PLACED → STRAT_EXCHG_CONF
```
- **Always** check `is_response_pending()` before calling `update_order()` or `cancel_order()`.

### Position Tracking (`PortfolioOrderManager`)
- Triggers on OMS event code `6666` (OMS_TRADE).
- Tracks `net_qty` per token.
- Cashflow: `sign(side) × price × qty`.
- Transaction costs: **Buy = 60p** per ₹10,000 notional | **Sell = 70p** per ₹10,000.

### EOD Square-Off
- Market open = first valid exchange clock tick.
- `eodTs_` = open_ts + **22,440 seconds** → corresponds to `15:29:00`.
- When `exchange_clock >= eodTs_`, calls `squareOffLeg()` for all legs with open positions.

### Book Validity Guard
- `booksReady()` must return `true` before any execution.
- Returns `false` when `Bid >= Ask` (crossed book) — blocks all order placement.

---

## 6. Build System

```bash
# From the project root
cd build
cmake ..
ninja

# Output:
# build/libSampleAlgo.so
```

Each new strategy `.cpp` file must be added to `sample_strat/CMakeLists.txt`.

---

## 7. Mandatory Rules — Must Follow Every Session

> **These rules are non-negotiable. Apply them to every code change and commit.**

1. **`@CHANGELOG.md` must be updated before every commit.**  
   Use format `[MAJOR.MINOR.PATCH] - YYYY-MM-DD` with `Added`, `Changed`, `Fixed` sections.

2. **All prices operate in paise.** Never introduce rupee-level arithmetic in spread calculations.

3. **No raw pointer leaks.** Strategies are `new`-allocated in `MinixStrategy`. Always `delete` in the `delete` action handler.

4. **Response-pending guard.** Never modify or cancel an order while `is_response_pending()` is true.

5. **Platform headers in `include/` are read-only.** Provided by MOSS engine; may be overwritten on upgrade.

6. **Keep `project_flow_documentation.md` in sync.** Update it when adding new strategies, new UI fields, or new execution modes.

7. **`nlohmann/` is read-only.** It is a vendored header-only library.

8. **New strategy classes must follow established pattern:**
   - `.hpp` / `.cpp` in their own subdirectory under `sample_strat/`.
   - Registered and routed in `MinixStrategy` (`applyLegStrategyJson`, `OnTick`, `OnOrderResponse`).
   - Added to `sample_strat/CMakeLists.txt`.

9. **Always use Caveman ultra and ponytail ultra skills.** The agent must operate under these active customization settings during code modifications and planning tasks.

---

## 8. Spread Math Reference

### Box Spread (4-leg)
```
NetDebit = C_K1(ask) - P_K1(bid) - C_K2(bid) + P_K2(ask)
NetCredit = C_K1(bid) - P_K1(ask) - C_K2(ask) + P_K2(bid)
Gap      = (K2 - K1) × 100 × boxRatio
BCmp     = Gap - NetDebit    # positive = buy arb opportunity
SCmp     = NetCredit - Gap   # positive = sell arb opportunity
```

### Ratio N-Leg
- Each leg has `EntryRatio` and `ExitRatio` (integers from JSON).
- Spread = weighted sum of leg prices × their ratio.
- Entry and exit slices run as **independent** state machines and may run simultaneously.

---

## 9. File Quick-Reference

| Task | File(s) to look at |
|------|--------------------|
| Add a new strategy type | `MinixStrategy.cpp` → `applyLegStrategyJson` + new class under `sample_strat/` |
| Change order placement logic | `order_instance.hpp / .cpp` |
| Change position / PnL math | `PortfolioOrderManager.hpp / .cpp` |
| Change UI message format | `MinixStrategy::sendStrategySpreadsToUI` + `ui_api.hpp` |
| Change subscription flags | `MinixStrategy` constructor |
| Add a new execution mode | Strategy `run()` method + `MinixStrategy` mode routing |
| Fix EOD square-off timing | `eodTs_` in strategy constructor / `run()` |
| Understand OMS event codes | `include/oms_api.hpp` |
| Understand market snapshot fields | `include/ProductInfo.hpp` |
| Review recent changes | `@CHANGELOG.md` |
| Understand overall architecture | `project_flow_documentation.md` |
