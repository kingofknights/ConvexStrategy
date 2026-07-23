# Ratio2Leg Strategy Documentation

## Overview
`Ratio2LegStrategy` is a 2-leg option/futures spread trading strategy with configurable ratios (e.g., 1:1, 1:2). It runs dual-side passive order bidding on a primary leg (`_biddingLeg`) and places immediate aggressive hedge orders on the secondary leg upon fills.

---

## Strategy Mechanics

### 1. Structure & Setup
- **Legs**: 2 instruments configured in JSON `Legs`.
- **Ratios**: Configured in JSON `Ratio.LegRatios` (e.g., `[1, 2]`).
- **Depth Slots**: Maintains up to 5 depth levels (`_longOrders[5]` and `_shortOrders[5]`).
- **Packs (Order Pairs)**: Each depth slot tracks:
  - Bidding Order (`_order[_biddingLeg]`): Passive Maker order.
  - Hedge Order (`_order[hedgeLeg]`): Aggressive Taker order.

### 2. Spread Calculations
- **Long Market Spread (`BCmp`)**:
  - `(Bid[1] * ratio[1]) - (Ask[0] * ratio[0])`
- **Short Market Spread (`SCmp`)**:
  - `(Bid[0] * ratio[0]) - (Ask[1] * ratio[1])`

### 3. Execution Flow
1. **OnTick**: Evaluates depth slots `0.._orderDepth-1`. If market spread meets target threshold (`param._spread`), updates/places limit order on `_biddingLeg`.
2. **OnOrderResponse**: Fill confirmations (`OMS_TRADE`) on `_biddingLeg` trigger `SecondOrderBidding`.
3. **SecondOrderBidding**: Calculates traded lot mismatch between leg 0 and leg 1. Immediately sends/modifies hedge order on secondary leg to complete the pack.

---

## Telemetry Field Formulas & Editing Guide

All values are computed in `Ratio2LegStrategy` in **paise** and converted to **Rupees** (`value / 100.0`) in `MinixStrategy::sendStrategySpreadsToUI()`.

---

#### 1. B-TrQ (Buy Traded Quantity)
- **GUI Field**: `B-TrQ`
- **C++ Method**: `GetBuyTradedQuantity()`
- **File & Line**: [Ratio2LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Downloads/MOSS_STRAT/sample_strat/Ratio2Leg/Ratio2LegStrategy.cpp#L283)
- **Description**: Total completed packs (both legs filled according to ratio) traded on the Long side.
- **Formula**:
  ```text
  B-TrQ = Sum of min(tradedLot[0] / ratio[0], tradedLot[1] / ratio[1]) across all 5 _longOrders slots
  ```

---

#### 2. S-TrQ (Sell Traded Quantity)
- **GUI Field**: `S-TrQ`
- **C++ Method**: `GetSellTradedQuantity()`
- **File & Line**: [Ratio2LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Downloads/MOSS_STRAT/sample_strat/Ratio2Leg/Ratio2LegStrategy.cpp#L293)
- **Description**: Total completed packs traded on the Short side.
- **Formula**:
  ```text
  S-TrQ = Sum of min(tradedLot[0] / ratio[0], tradedLot[1] / ratio[1]) across all 5 _shortOrders slots
  ```

---

#### 3. B-ATP (Buy Average Traded Price / Spread)
- **GUI Field**: `B-ATP`
- **C++ Method**: `GetBATP()`
- **File & Line**: [Ratio2LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Downloads/MOSS_STRAT/sample_strat/Ratio2Leg/Ratio2LegStrategy.cpp#L303)
- **Description**: Weighted average executed spread achieved across all filled Long packs.
- **Formula**:
  ```text
  Leg0_AvgPrice = Total_Long_Leg0_TradeValue / (Total_Long_Leg0_TradedLot * LotSize)
  Leg1_AvgPrice = Total_Long_Leg1_TradeValue / (Total_Long_Leg1_TradedLot * LotSize)

  B-ATP = (Leg1_AvgPrice) - (Leg0_AvgPrice )
  ```

---

#### 4. S-ATP (Sell Average Traded Price / Spread)
- **GUI Field**: `S-ATP`
- **C++ Method**: `GetSATP()`
- **File & Line**: [Ratio2LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Downloads/MOSS_STRAT/sample_strat/Ratio2Leg/Ratio2LegStrategy.cpp#L320)
- **Description**: Weighted average executed spread achieved across all filled Short packs.
- **Formula**:
  ```text
  Leg0_AvgPrice = Total_Short_Leg0_TradeValue / (Total_Short_Leg0_TradedLot * LotSize)
  Leg1_AvgPrice = Total_Short_Leg1_TradeValue / (Total_Short_Leg1_TradedLot * LotSize)

  S-ATP = (Leg0_AvgPrice ) - (Leg1_AvgPrice )
  ```

---

#### 5. RLP (Realized PnL)
- **GUI Field**: `RLP`
- **C++ Method**: `GetRLP()`
- **File & Line**: [Ratio2LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Downloads/MOSS_STRAT/sample_strat/Ratio2Leg/Ratio2LegStrategy.cpp#L337)
- **Description**: Realized PnL from matched Long and Short packs (closed positions).
- **Formula**:
  ```text
  MatchedPacks = min(B-TrQ, S-TrQ)
  RLP = (S-ATP - B-ATP) * MatchedPacks * LotSize
  ```

---

#### 6. Cut P/L (Closed Position PnL)
- **GUI Field**: `Cut P/L`
- **C++ Method**: `GetCutPL()`
- **File & Line**: [Ratio2LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Downloads/MOSS_STRAT/sample_strat/Ratio2Leg/Ratio2LegStrategy.cpp#L347)
- **Description**: Locked Realized PnL when positions are squared off/closed.
- **Formula**:
  ```text
  Cut P/L = RLP
  ```

---

#### 7. M2M (Mark-to-Market / Unrealized PnL)
- **GUI Field**: `M2M`
- **C++ Method**: `GetM2M()`
- **File & Line**: [Ratio2LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Downloads/MOSS_STRAT/sample_strat/Ratio2Leg/Ratio2LegStrategy.cpp#L351)
- **Description**: Floating PnL of open unhedged packs evaluated against current market spreads (`BCmp` / `SCmp`).
- **Formula**:
  ```text
  OpenLongPacks  = max(0, B-TrQ - S-TrQ)
  OpenShortPacks = max(0, S-TrQ - B-TrQ)

  Long_M2M  = (Current_BCmp - B-ATP) * OpenLongPacks * LotSize
  Short_M2M = (S-ATP - Current_SCmp) * OpenShortPacks * LotSize

  M2M = Long_M2M + Short_M2M
  ```

---

#### 8. Net P/L (Total PnL)
- **GUI Field**: `Net P/L`
- **C++ Method**: `GetNetPL()`
- **File & Line**: [Ratio2LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Downloads/MOSS_STRAT/sample_strat/Ratio2Leg/Ratio2LegStrategy.cpp#L370)
- **Description**: Total overall strategy PnL (Realized PnL + Mark-to-Market PnL).
- **Formula**:
  ```text
  Net P/L = RLP + M2M
  ```

---

## How to Manually Edit a Telemetry Formula

1. Open [Ratio2LegStrategy.cpp](file:///home/vikram.lodhi@corp.merillife.com/Downloads/MOSS_STRAT/sample_strat/Ratio2Leg/Ratio2LegStrategy.cpp).
2. Go to the C++ method listed above (e.g., `GetBATP()`, `GetM2M()`).
3. Modify the math return calculation.
4. Rebuild the shared library:
   ```bash
   cmake --build ./build/Debug/
   ```
