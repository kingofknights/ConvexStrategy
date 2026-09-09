# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.8.0] - 2026-09-08

### Changed
- Decoupled independent long and short side bidding evaluation in `RatioLegStrategy::OnTick`: unhedged state recovery on one side no longer blocks evaluation on the opposite side.
- Streamlined `EvaluateBidding` and `OrderBiddingLogic` by removing redundant unhedged checks already handled on the tick hot-path.
- Refactored trade tracer and slippage calculations in `RatioLegStrategy::CheckSlippageThreshold`:
  - Accurately compute `_qtyRemaining` from allowed total quantity minus traded lots.
  - Correctly evaluate and filter `_tracer._slippage` against `_allowedSlippage`.
  - Immediately invoke `Stop()` on slippage breach without deferred termination queue.
- Removed deferred termination queue (`_strategiesToTerminate`, `Registerfortermination`) in `MinixStrategy`.
- Updated build system to modern CMake:
  - Renamed target to `ConvexMoss` in `Convex/CMakeLists.txt`.
  - Replaced vendored `nlohmann` JSON with system `find_package(nlohmann_json REQUIRED)` and `find_package(fmt REQUIRED)`.
  - Enforced `CMAKE_CXX_STANDARD 20` and enabled `CMAKE_EXPORT_COMPILE_COMMANDS`.
- Added diagnostic failure logging in `OrderBiddingLogic` when order placement fails.
- Cleaned up formatting and member alignment in `vendor/minix/include/order_instance.hpp`.

## [1.7.0] - 2026-09-07

### Changed
- Moved all Minix platform sources and headers into `vendor/minix/` to consolidate all vendor code under a single directory:
  - Platform headers (`AlgoBase.hpp`, `oms_api.hpp`, `rms_api.hpp`, `ui_api.hpp`, `ProductInfo.hpp`, `Quote.hpp`, `TimeUtils.hpp`, `common.hpp`, `LoggerExports.hpp`, `LoggerFrontend.hpp`, `MboTypes.hpp`, `MbpTypes.hpp`, `order_instance.hpp`, `PortfolioOrderManager.hpp`) moved to `vendor/minix/include/`.
  - Framework sources (`order_instance.cpp`, `PortfolioOrderManager.cpp`) moved to `vendor/minix/src/`.
  - Removed top-level `include/` and standalone `minix/` directories.
- Updated `vendor/minix/CMakeLists.txt` to compile sources from `src/` and expose correct include paths.
- Updated `vendor/CMakeLists.txt` to `add_subdirectory(minix)`.
- Updated root `CMakeLists.txt`: `include_directories` now points to `vendor/minix/include` and `vendor/minix`; removed `add_subdirectory(minix)` (now handled inside `vendor`).
- Updated `Convex/CMakeLists.txt` include directories to `vendor/minix` and `vendor/minix/include`.
- Updated `GEMINI.md`, `.agents/AGENTS.md`, and `project_flow_documentation.md` to reflect new directory structure and file paths.

## [1.6.0] - 2026-09-07

### Added
- Created dedicated `minix/` directory with `minix/CMakeLists.txt` compiling internal Minix platform code (`order_instance.*`, `PortfolioOrderManager.*`) as a position-independent CMake `OBJECT` library.
- Created `vendor/CMakeLists.txt` exposing third-party libraries (`nlohmann/json.hpp`) as an interface library.
- Added `RoundOFF` template formula in `Convex/Utils.hpp` for price grid quantization.

### Changed
- Renamed strategy directory `sample_strat/` to `Convex/` via `git mv` to reflect project naming conventions.
- Separated code architecture by domain: `minix/` for internal platform components, `vendor/` for third-party libraries, `include/` for platform SDK headers, and `Convex/` for strategy implementation.
- Updated root `CMakeLists.txt` to register `vendor`, `minix`, and `Convex` via `add_subdirectory`.
- Updated `Convex/CMakeLists.txt` to link `minix` and `vendor` libraries.
- Applied HFT coding and mathematical standards to `Convex/`: non-abbreviated naming (`instrumentIndex`, `levelIndex`, `buyQuantity`, `sellQuantity`, `netQuantity`, `orderQuantity`, `_sliceQuantity`, `_hedgeTargetDepthQuantity`, `_thresholdQuantity`), eliminated shadowing, and unified symmetric BUY/SELL branching with directional sign multipliers.
- Updated documentation and agent guidance in `project_flow_documentation.md`, `GEMINI.md`, and `.agents/AGENTS.md`.

## [1.5.11] - 2026-09-04

### Added
- Implemented direct binary packed C POD struct protocol to send UI updates via `UIStruct::message` (`StrategySpreadUpdate` on code `100002` [64 bytes] and `StrategyStatusUpdate` on code `100001` [8 bytes]), matching the Trade Tracker (`100004`) pattern.
- Added 4-byte `StrategyStatus` enum (`StrategyStatus_INACTIVE = 0`, `StrategyStatus_ACTIVE = 1`, `StrategyStatus_APPLIED = 2`) and helper functions `StrategyStatusToString` and `StringToStrategyStatus` in `sample_strat/Utils.hpp`.
- Added `UI_BINARY_PROTOCOL_SPEC.md` documenting binary protocol wire layout, byte offset map, and UI parsing integration guide for the frontend team.

### Changed
- Replaced legacy chunked JSON dispatch (`sendJsonChunkedToUI`) in `MinixStrategy` with zero-allocation binary methods `sendStrategySpreadToUI` and `sendStrategyStatusToUI`.
- Removed `_active` boolean flag from `RatioLegStrategy` and transitioned execution gating to rely directly on `_status` (`IsActive` returns true for `StrategyStatus_APPLIED`, `IsStopped` returns true for `StrategyStatus_INACTIVE`).
- Optimized file logging in `RatioLegStrategy` using `fmt::print` with `std::FILE*`.
- Updated `project_flow_documentation.md` to document the 64-byte binary POD UI protocol.

### Fixed
- Fixed strategy status reporting `INACTIVE` upon receiving "Subscribe" action by supporting case-insensitive variants in `StringToStrategyStatus` and decoupling status lookup from the legacy `_active` flag.

## [1.5.10] - 2026-09-03

### Added
- Implemented `LegSideCache` in `RatioLegStrategy` to precompute opposite quote sides, signed ratios, target hedge depth quantities, order slice quantities, and gap-adjusted target spreads once during `ParamUpdate` and constructor.
- Added `_isUnhedged` flag to `MarketBidding` updated strictly on `OMS_TRADE` responses, replacing dynamic integer division loops on every tick with an $O(1)$ boolean check.
- Precomputed `_tradeGearPriceOffset`, `_minTickDiffThreshold`, `_buyCostCoeff`, and `_sellCostCoeff` in `RatioLegStrategy::RebuildCache`.

### Changed
- Enforced `const` qualifiers across member pointers, local variables, parameters, and references across `RatioLegStrategy`.

### Fixed
- Fixed `RatioLegStrategy::GetOrderCount` reading `.qty` instead of `order_count_` from `st_mbp_info`.

## [1.5.9] - 2026-09-03

### Changed
- Simplified log file naming in `RatioLegStrategy` constructor to `{numLegs}Ratio_{strategyId}_{HHMMSS}.log`, eliminating JSON parsing for strategy name and portfolio.
- Removed unused `_stratName` and `_portfolio` members from `RatioLegStrategy`.
- Updated UI status chunked response message code to `100001` in `MinixStrategy::applyLegStrategyJson`.

## [1.5.8] - 2026-09-02

### Added
- Implemented dedicated log file creation per strategy object instance formatted as `{StrategyName}_{portfolio}_{timeHHMMSS}.log` in `RatioLegStrategy` (`writeLog` writes to both log file and stdout).
- Implemented stored snapshot price placement for hedge legs upon first leg fill (`_windRate._price[leg]`) with fallback to calculated spread pricing in `RatioLegStrategy::ExecuteHedgeLeg()`.
- Implemented progressive per-retry tick step escalation in `RatioLegStrategy::ExecuteHedgeLeg()` (BUY side steps up by $+1\text{ tick}$, SELL side steps down by $-1\text{ tick}$ per retry from stored price).
- Implemented aggressive opposite side market touch execution (BUY at Ask, SELL at Bid) upon reaching max retries (`_marketOrderRetries`) to guarantee trade completion without stopping the strategy.
- Added unhedged position prioritization in `RatioLegStrategy::OnTick` to immediately cancel bidding quotes and execute hedge orders first when ratio imbalance is detected.

### Changed
- Replaced dynamic vector allocations in `WindRate` with fixed-size `std::array<int, MAX_LEGS>` to achieve zero heap allocations on the tick hot path.
- Converted `_hedgeRetryCount` to per-leg tracking vector `std::vector<size_t>` in `MarketBidding`, eliminating 1-second time-based variables.
- Refactored `RatioLegStrategy` into modular, single-responsibility functions (`HasUnhedgedLots`, `CheckHedgeLegsDepth`, `CheckBiddingLegDepth`, `EvaluateBidding`, `ComputeRawSpread`, `AdjustGap`, `CalculateTradedLots`, `ProcessTradeFill`, `CheckSlippageThreshold`, `ProcessLegResponse`, `ExecuteHedgeLeg`).
- Replaced `std::string` value parameters with `std::string_view` across order response and bidding logic to prevent string reallocations.
- Removed periodic `Print()` logging from heartbeat and disabled verbose spread mismatch logs in bidding logic.

### Fixed
- Fixed trade tracer UI update issue in `RatioLegStrategy::CheckSlippageThreshold()` by removing the legacy `_windRate._price` non-zero guard that prevented `sendTradeTracerToUI()` from firing when all legs finished trading.
- Corrected primary symbol initialization (`_tracer._symbol`) and side-aware slippage sign for trade tracer events.

## [1.5.7] - 2026-08-26

### Added
- Added tick event counter (`_eventCount`) to `RatioLegStrategy` to monitor tick frequency per strategy instance.
- Added LTP and LTQ market depth fields to `RatioLegStrategy::Print()` log output.

### Changed
- Supported `New` status alongside `Subscribed` for strategy initialization in `MinixStrategy::handleRatioLegStrategy`.
- Fixed UI chunked message dispatch in `MinixStrategy::sendJsonChunkedToUI` to use dynamic `interface_` ID instead of hardcoded `22`.
- Cleaned up unused global namespace and variables in `MinixStrategy.cpp`.

## [1.5.6] - 2026-08-18

### Added
- Implemented strategy stop and resume feature via GUI `Status` field (`Unsubscribed` status pauses strategy execution, cancels active leg orders, and retains strategy state; `Applied` status resumes strategy execution and updates parameters).
- Added `RatioLegStrategy::Stop()` method to safely halt strategy tick evaluation and cancel active orders.
- Added `RatioLegStrategy::IsActive()` and `RatioLegStrategy::IsStopped()` getter methods to query strategy operational status.
- Added `Registerfortermination()` mechanism in `MinixStrategy` to trigger strategy self-termination upon slippage breach and dispatch status update code 9621 (`Unsubscribed`) to GUI.

### Changed
- Updated transaction cost calculation in `PortfolioOrderManager` and `RatioLegStrategy::GetRLP()` to differentiate between option and future contracts.
- Renamed UI broadcast spread keys in `MinixStrategy::sendStrategySpreadsToUI` (`Net P/L` -> `NLP`, `Cut P/L` -> `CLP`).

## [1.5.5] - 2026-08-17

### Changed
- Deducted transaction costs from strategy-level realized P&L (`RatioLegStrategy::GetRLP()`) and portfolio token realized P&L (`PortfolioOrderManager::on_trade()`) using option-specific (`OptionBuyCost`/`OptionSellCost`) and future-specific (`FutureBuyCost`/`FutureSellCost`) multipliers based on actual traded value.

## [1.5.4] - 2026-08-13

### Changed
- Unified UI request routing for `Box` (4-leg) and `ConRev` (3-leg) strategies to use the unified `RatioLegStrategy`.
- Refactored `MinixStrategy` member variables (`_client`, `_algoid`, `_omsid`, `_flags`, `_lastTickTs`) to match member-naming conventions.
- Updated `ratioStrats_` and `strategyJson_` map keys to use `uint32_t` to match strategy ID types.

## [1.5.3] - 2026-08-07

### Added
- Added `TradeTracer` struct and `sendTradeTracerToUI` (message code 9956) in `MinixStrategy` to send trade execution details to UI.
- Integrated `TradeTracer` reporting inside `RatioLegStrategy` order response handler.

### Changed
- Fixed spread bidding logic comparison in `OrderBiddingLogic`.
- Added safety checks for `allowedSlippage` values.
- Muted verbose printing in `GetBCmp` and `GetSCmp`.

### Fixed
- Fixed division-by-zero bug leading to `-nan` average prices in `checkSlippage` by enforcing non-zero complete cycle validation.

## [1.5.2] - 2026-08-06

### Added
- Added log prints for parsed Params values in `RatioLegStrategy` config.

## [1.5.1] - 2026-08-06

### Changed
- Fixed `GetBATP()` and `GetSATP()` calculation in `RatioLegStrategy` to follow the same ratio-weighted and side-signed spread math as `BCmp`/`SCmp`.
- Integrated multiplier in `OrderBiddingLogic` for correct long/short passive bidding logic.
- Factored in leg ratios in transaction cost calculations.
- Cleaned up redundant spread UI fields in `MinixStrategy` and reduced UI update interval to 1s.

## [1.5.0] - 2026-08-04

### Changed
- Consolidated `Ratio2Leg`, `Ratio3Leg`, `Ratio4Leg`, `Ratio5Leg`, and `Ratio6Leg` strategies into a single universal `RatioLegStrategy` class.
- Parameterized the strategy by passing the dynamic number of legs in the constructor.
- Replaced separate ratio maps and handler functions in `MinixStrategy` with unified ones.
- Removed unused variable `hedgePacks` in `SecondOrderBidding` and simplified EOD clock setup in `MinixStrategy::OnTick`.

## [1.4.0] - 2026-08-04

### Added
- Supported per-leg ratio multipliers (`LegRatios` array nested under `Ratio` object in JSON) for all N-leg ratio strategies (`Ratio2Leg` to `Ratio6Leg`).
- Scaled spread calculations, target/slice quantities, total quantity limit checks, and traded lot counts using per-leg ratios.
- Adjusted slippage checks to normalize cycle completion checks and scale leg-specific slippage amounts by their corresponding leg ratios.

## [1.3.14] - 2026-07-31

### Changed
- Unified `BoxSpreadStrategy` implementation with `Ratio4LegStrategy` logic.
- Unified `ConversionReversalStrategy` implementation with `Ratio3LegStrategy` logic.

## [1.3.13] - 2026-07-30

### Changed
- Updated dynamic transaction cost logic in `Ratio3Leg`, `Ratio4Leg`, `Ratio5Leg`, and `Ratio6Leg` strategies to use option-specific (`OptionBuyCost` / `OptionSellCost`) and future-specific (`FutureBuyCost` / `FutureSellCost`) multipliers per leg, matching `Ratio2Leg`.

## [1.3.12] - 2026-07-29

### Changed
- Updated slippage check in all N-leg ratio strategies (`Ratio2Leg`, `Ratio3Leg`, `Ratio4Leg`, `Ratio5Leg`, and `Ratio6Leg`) to verify executed spread only when all legs have traded in equal quantities for the current cycle (`object_._cycleTradedLot[i] == object_._cycleTradedLot[j]`).
- Introduced cycle-specific accumulators `_cycleTradedLot` and `_cycleTradeValue` in all N-leg strategies to calculate slippage independently per cycle (preventing dilution over multiple successful cycles), resetting them back to 0 upon cycle completion.
- Simplified slippage calculation to compare `expectedHedgePrice` and `actualHedgePrice` based on each hedge leg's side (`actual - expected` for `BUY_SIDE`, and `expected - actual` for `SELL_SIDE`). Side arrays are now passed explicitly as parameters to `checkSlippage` and `processOrderResponse`.
- Implemented non-zero dynamic definitions for `GetFLP()` and `GetCost()` in 4-leg, 5-leg, and 6-leg strategies, calculating total strategy execution cost by summing up buy/sell cost parameters over all legs.

## [1.3.11] - 2026-07-29

### Changed
- Added `_tokensParam`, `_longSideParam`, and `_shortSideParam` to `Ratio3LegStrategy` class to match the parameter structure of `Ratio2LegStrategy`.
- Modified `Ratio3LegStrategy::ParamUpdate` to parse token and side parameters into the new `Param` arrays.
- Modified `Ratio3LegStrategy` constructor to map parameters directly to the active execution variables.

## [1.3.10] - 2026-07-29

### Fixed
- Added `-DFMT_HEADER_ONLY` globally via target compile definitions in `sample_strat/CMakeLists.txt` to prevent unresolved linker symbols for `fmt` in header inclusions, fixing the dynamic library load error.

## [1.3.9] - 2026-07-29

### Removed
- Removed file log writing (`_logFile`, `fopen`, and `fclose` calls) from all strategies (`Ratio2Leg`, `Ratio3Leg`, `Ratio4Leg`, `Ratio5Leg`, `Ratio6Leg`, `BoxSpread`, `Butterfly`, and `ConversionReversal`) and replaced it with direct stdout console output via `fmt::print`.

## [1.3.8] - 2026-07-29

### Removed
- Removed file log writing (`_logFile`, `fopen`, and `fclose` calls) from `Ratio2LegStrategy` and replaced it with direct stdout console output via simple `fmt::print`.

## [1.3.7] - 2026-07-29

### Changed
- Refactored `Ratio2LegStrategy` arrays (`_tokens`, `_longSide`, and `_shortSide`) to be pre-sorted during `ParamUpdate` such that index 0 is always the bidding leg and index 1 is always the hedge leg.
- Simplified and cleaned up `GetBCmp` and `GetSCmp` functions in `Ratio2LegStrategy.cpp` to use direct side-based price lookup, rendering the formulas easier to read and maintain.

## [1.3.6] - 2026-07-29

### Fixed
- Fixed quote indexing mismatch when `_biddingLeg` is 1 in `Ratio2LegStrategy.cpp` for functions `GetBCmp()`, `GetSCmp()`, and `GetCost()`, ensuring correct spread values (like `140.90` and `-141.55`) are generated dynamically based on actual leg side configuration (`B.S` and `S.B`).

## [1.3.5] - 2026-07-28

### Changed
- Renamed strategy functions `GetBuyTradedQuantity` to `GetLongTradedLots` and `GetSellTradedQuantity` to `GetShortTradedLots` across all spread strategies (Ratio2Leg, Ratio3Leg, Ratio4Leg, Ratio5Leg, Ratio6Leg, BoxSpread, Butterfly, ConversionReversal) and orchestrator (`MinixStrategy.cpp`) for clearer semantics since these quantities represent traded lots/packages of the spread, not the total quantities of the individual legs.
- Corrected realized PnL (`GetRLP()`) and Mark-to-Market (`GetM2M()`) calculations across all spread strategies to track traded quantities and sides individually per leg rather than assuming long orders are strictly BUY and short orders are strictly SELL. This ensures accurate PnL accounting for configurations with mixed buy/sell legs.

## [1.3.4] - 2026-07-28

### Changed
- Removed all `_ratio` logic and references from 2-leg, 3-leg, 4-leg, 5-leg, and 6-leg ratio strategy files ([Ratio2LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Projects/ConvexStrategy/sample_strat/Ratio2Leg/Ratio2LegStrategy.cpp), [Ratio3LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Projects/ConvexStrategy/sample_strat/Ratio3Leg/Ratio3LegStrategy.cpp), [Ratio4LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Projects/ConvexStrategy/sample_strat/Ratio4Leg/Ratio4LegStrategy.cpp), [Ratio5LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Projects/ConvexStrategy/sample_strat/Ratio5Leg/Ratio5LegStrategy.cpp), [Ratio6LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Projects/ConvexStrategy/sample_strat/Ratio6Leg/Ratio6LegStrategy.cpp)) as all leg ratios are fixed to 1:1:1:1.
- Updated `GetBCmp()` and `GetSCmp()` in all N-leg ratio strategies to compute the spread dynamically using the leg's side (Buy side is negative (`-GetPrice()`), Sell side is positive (`+GetPrice()`)). Other math functions (slippage, BATP, SATP) remain simple subtraction.

### Added
- Implemented per-strategy file logging producing `StrategyName_StrategyId.log` outputs via custom `writeLog` helper mapping to `fmt::print` in all strategy types.

### Fixed
- Fixed compilation errors in [Ratio2LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Projects/ConvexStrategy/sample_strat/Ratio2Leg/Ratio2LegStrategy.cpp) by replacing undeclared identifiers `hedgeLeg` and `_` with the correct private member variable `_hedgeLeg`.
- Fixed bidding/hedge leg index references (hardcoded to 0/1) and lot size scaling in `GetCost()` in [Ratio2LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Projects/ConvexStrategy/sample_strat/Ratio2Leg/Ratio2LegStrategy.cpp).

## [1.3.3] - 2026-07-27

### Added
- Integrated `BoxSpreadStrategy` calling and routing inside `MinixStrategy` (`applyLegStrategyJson`, `OnTick`, `onBcastData`, `OnOrderResponse`, and `sendStrategySpreadsToUI`).

## [1.3.2] - 2026-07-27

### Changed
- Refactored `OrderBiddingLogic` in `Ratio2LegStrategy`, `Ratio3LegStrategy`, `Ratio4LegStrategy`, `Ratio5LegStrategy`, `Ratio6LegStrategy`, and `ButterflyStrategy` to use early returns for tick-change check and quantity validations instead of nested conditionals.

## [1.3.1] - 2026-07-16

### Changed
- Box strategy gap calculation simplified to be based on the absolute difference between the strike prices of the first two legs.
- Enforced leg ratios to be exactly 1 for all legs (1:1:1:1) in the box strategy.
- Updated UI broadcast updates for Box Strategy to send the actual calculated `Gap` value instead of the `BCmp` (buy edge) value.

### Fixed
- Resolved compilation errors of `Ratio2LegStrategy` in `MinixStrategy.cpp` by correctly parsing and passing `entryRatios` and `exitRatios` from GUI JSON.

## [1.3.0] - 2026-07-16

### Added
- Dedicated `Ratio2LegStrategy` class implementing a 2-leg option ratio spread strategy with real-time spreads, execution modes (aggressive, bidding, all-leg bidding), and PnL calculations.
- Lifetime event handling, tick/broadcast/order event routing, and UI update channel integration for the new `Ratio 2 Leg` strategy inside `MinixStrategy`.
- Support for separate entry ratios and exit ratios per leg (e.g. `EntryRatio` / `ExitRatio` keys or arrays from JSON).
- Simultaneous two-sided bidding (parallel Entry and Exit slice state machines).
- OrderMap query helpers to safely check filled details on non-default-constructible order instances.

### Changed
- Updated `sample_strat/CMakeLists.txt` to compile and link `Ratio2LegStrategy.cpp` as part of `SampleAlgo` shared library.

## [1.2.0] - 2026-07-15

### Added
- Logging of product subscription and unsubscription details inside `MinixStrategy` and `BoxSpreadStrategy`.
- Logging of tick events inside `MinixStrategy::OnTick` and `BoxSpreadStrategy::onTick`.
- Comprehensive parser configuration and lifecycle event logging inside `MinixStrategy::handleBoxStrategy`.

### Changed
- Refactored `MinixStrategy::sendStrategySpreadsToUI` to format the broadcast payload to include `StrategyId` and `Status` directly instead of iterating and populating the `StrategyUpdates` array.

## [1.1.0] - 2026-07-15

### Added
- Dedicated box strategy parameters parsing and lifecycle management handler function `handleBoxStrategy` in `MinixStrategy`.

### Changed
- Refactored `MinixStrategy::applyLegStrategyJson` to route `Applied`, `Unsubscribed`, and `Cancelled` status events for "Box" strategy subtype to the dedicated handler.

## [1.0.0] - 2026-07-15

### Added
- Core orchestrator `MinixStrategy` implementing dynamic strategy loader hooks, UI JSON chunked reassembly layer, and broadcast/tick routing.
- `BoxSpreadStrategy` implementing a 4-leg option box spread strategy with real-time spread calculations, execution state machines, and EOD square-offs.
- OMS order lifecycle manager wrapper `order_instance` for placement, exchange confirmation, modification, and cancellation.
- lightweight position bookkeeper `PortfolioOrderManager` for tracking net token positions and real-time realized/unrealized PnL.
- Project architectural and flow documentation in `project_flow_documentation.md`.
- CMake build configuration (`CMakeLists.txt`) for compiling the strategy shared library.
- Initial `.gitignore` configuration to exclude build outputs, IDE configs, and cache files.
