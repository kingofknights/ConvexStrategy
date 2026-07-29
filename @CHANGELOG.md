# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
