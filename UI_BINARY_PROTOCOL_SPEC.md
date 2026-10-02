# UI Binary Protocol Specification — ConvexStrategy

## 1. Overview

Strategy updates sent from **ConvexStrategy** to the GUI are migrated from the legacy chunked JSON protocol to **direct binary packed C POD structs** (matching the Trade Tracker / `TradeTracer` pattern).

### Key Changes
- **No JSON serialization / deserialization.**
- **No chunked framing** (no metadata packet with `packet_count`, no string chunk concatenation).
- **Fixed-size packed structs** copied directly into `UIStruct::message`.
- **Single network packet per update.**

---

## 2. Message Codes

| Message Code | Packet Type | Size | Description |
|:---|:---|:---|:---|
| **`100001`** | `StrategyStatusUpdate` | **8 bytes** | Strategy lifecycle state transitions (Applied, Active, Inactive) |
| **`100002`** | `StrategySpreadUpdate` | **64 bytes** | Periodic (~1s) spread, PnL, lot, and status snapshot |
| **`100003`** | `ExternalOrderResponse` | Existing | OMS order response (unchanged) |
| **`100004`** | `TradeTracer` | 55 bytes | Trade execution / slippage tracer (unchanged) |

---

## 3. Data Structures & Memory Layout

All structures are 1-byte packed using `#pragma pack(push, 1)`.

### 3.1 `StrategyStatus` (4-byte Enum)

```cpp
enum StrategyStatus : int32_t {
    StrategyStatus_INACTIVE = 0,
    StrategyStatus_ACTIVE   = 1,
    StrategyStatus_APPLIED  = 2
};
```

---

### 3.2 `StrategySpreadUpdate` (Message Code: `100002`)

Total Payload Size: **64 bytes**

```cpp
#pragma pack(push, 1)
struct StrategySpreadUpdate {
    uint32_t       _strategyId; // Unique strategy / portfolio ID
    StrategyStatus _status;     // 4-byte enum: INACTIVE (0), ACTIVE (1), APPLIED (2)
    float          _bcmp;       // Buy compared spread (paise / 100)
    float          _scmp;       // Sell compared spread (paise / 100)
    float          _cost;       // Transaction cost (paise / 100)
    float          _flp;        // Fair Leg Price / live parity (paise / 100)
    float          _gap;        // Spread strike gap
    int32_t        _bTrQ;       // Long traded lots
    int32_t        _sTrQ;       // Short traded lots
    float          _m2m;        // Mark-to-Market PnL (paise / 100)
    float          _netPL;      // Net PnL / NLP (paise / 100)
    float          _rlp;        // Realized PnL (paise / 100)
    float          _cutPL;      // Cut / square-off PnL / CLP (paise / 100)
    float          _reserved;   // Reserved, always 0 (was _trSpread)
    float          _bATP;       // Buy Average Traded Price (paise / 100)
    float          _sATP;       // Sell Average Traded Price (paise / 100)
};
#pragma pack(pop)
```

#### Byte Offset Table

| Offset | Field | Type | Size | Legacy JSON Key |
|:---|:---|:---|:---|:---|
| **`0`** | `_strategyId` | `uint32_t` | 4 bytes | `"StrategyId"` |
| **`4`** | `_status` | `StrategyStatus` | 4 bytes | `"Status"` |
| **`8`** | `_bcmp` | `float` | 4 bytes | `"BCmp"` |
| **`12`** | `_scmp` | `float` | 4 bytes | `"SCmp"` |
| **`16`** | `_cost` | `float` | 4 bytes | `"Cost"` |
| **`20`** | `_flp` | `float` | 4 bytes | `"FLP"` |
| **`24`** | `_gap` | `float` | 4 bytes | `"Gap"` |
| **`28`** | `_bTrQ` | `int32_t` | 4 bytes | `"B-TrQ"` |
| **`32`** | `_sTrQ` | `int32_t` | 4 bytes | `"S-TrQ"` |
| **`36`** | `_m2m` | `float` | 4 bytes | `"M2M"` |
| **`40`** | `_netPL` | `float` | 4 bytes | `"NLP"` / `"Net P/L"` |
| **`44`** | `_rlp` | `float` | 4 bytes | `"RLP"` / `"Realized P/L"` |
| **`48`** | `_cutPL` | `float` | 4 bytes | `"CLP"` / `"Cut P/L"` |
| **`52`** | `_reserved` | `float` | 4 bytes | — (was `"TrSpread"`, always 0) |
| **`56`** | `_bATP` | `float` | 4 bytes | `"B-ATP"` |
| **`60`** | `_sATP` | `float` | 4 bytes | `"S-ATP"` |

---

### 3.3 `StrategyStatusUpdate` (Message Code: `100001`)

Total Payload Size: **8 bytes**

```cpp
#pragma pack(push, 1)
struct StrategyStatusUpdate {
    uint32_t       _strategyId; // Unique strategy / portfolio ID
    StrategyStatus _status;     // 4-byte enum: INACTIVE (0), ACTIVE (1), APPLIED (2)
};
#pragma pack(pop)
```

#### Byte Offset Table

| Offset | Field | Type | Size | Legacy JSON Key |
|:---|:---|:---|:---|:---|
| **`0`** | `_strategyId` | `uint32_t` | 4 bytes | `"StrategyId"` |
| **`4`** | `_status` | `StrategyStatus` | 4 bytes | `"Status"` |

---

## 4. UI Implementation Guide (How UI Should Parse)

In the UI packet processor (e.g. `MessageBroker::Process`):

```cpp
void MessageBroker::Process(const char* buffer_, size_t size_) {
    const auto* uiStruct = reinterpret_cast<const UIStruct*>(buffer_);
    const auto& header   = uiStruct->header;

    switch (header.message_code) {
        case 100001: { // Lifecycle Status Update (8 bytes)
            const auto* statusUpdate = reinterpret_cast<const StrategyStatusUpdate*>(uiStruct->message);
            ProcessStrategyStatus(statusUpdate->_strategyId, statusUpdate->_status);
            break;
        }

        case 100002: { // Spread & PnL Update (64 bytes)
            const auto* update = reinterpret_cast<const StrategySpreadUpdate*>(uiStruct->message);
            ProcessStrategySpread(update);
            break;
        }

        case 100003: { // Order Response
            ProcessOrder(uiStruct->message);
            break;
        }

        case 100004: { // Trade Tracer
            ProcessStrategyTracer(uiStruct->message);
            break;
        }

        default:
            break;
    }
}
```

### Handler Example for `100002`:

```cpp
void MessageBroker::ProcessStrategySpread(const StrategySpreadUpdate* update_) {
    auto ptr = Utils::GetStrategyRow(update_->_strategyId);
    if (ptr.has_value() && !ptr->expired()) {
        const auto& strategy = ptr->lock();

        // Update status enum
        switch (update_->_status) {
            case StrategyStatus_ACTIVE:
                strategy->_status = StrategyStatus_ACTIVE;
                break;
            case StrategyStatus_APPLIED:
                strategy->_status = StrategyStatus_APPLIED;
                break;
            case StrategyStatus_INACTIVE:
            default:
                strategy->_status = StrategyStatus_INACTIVE;
                break;
        }

        // Direct field assignment without string parsing
        strategy->_bcmp  = update_->_bcmp;
        strategy->_scmp  = update_->_scmp;
        strategy->_cost  = update_->_cost;
        strategy->_flp   = update_->_flp;
        strategy->_gap   = update_->_gap;
        strategy->_bTrQ  = update_->_bTrQ;
        strategy->_sTrQ  = update_->_sTrQ;
        strategy->_m2m   = update_->_m2m;
        strategy->_netPL = update_->_netPL;
        strategy->_rlp   = update_->_rlp;
        strategy->_cutPL = update_->_cutPL;
        strategy->_bATP  = update_->_bATP;
        strategy->_sATP  = update_->_sATP;
    }
}
```
