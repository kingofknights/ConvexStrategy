# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
