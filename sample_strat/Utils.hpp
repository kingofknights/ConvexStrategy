#pragma once

#include "order_instance.hpp"  // execution_strat::order_instance, ORDER_SIDE, ORDER_TYPE

#include <cstdint>
using OrderObjectT    = execution_strat::order_instance;
using OrderObjectPtrT = std::unique_ptr<OrderObjectT>;
using PortfolioT      = uint32_t;
using UserIdT         = uint32_t;

constexpr static double OptionBuyCost  = 0.000450434;
constexpr static double OptionSellCost = 0.001920434;
constexpr static double FutureBuyCost  = 0.000042774;
constexpr static double FutureSellCost = 0.000522774;

#pragma pack(push, 1)
struct TradeTracer {
    int      _time;
    uint64_t _orderId;
    uint32_t _strategyId;
    int      _side;
    int      _ltq;
    int      _qtyRemaining;
    float    _price;
    float    _ltp;
    float    _slippage;
    char     _symbol[11];
};

struct UserPortfolio {
    UserIdT    _user;
    PortfolioT _portfolio;
};

using OrderIDT      = uint64_t;
using TimeT         = uint64_t;
using PriceT        = int32_t;
using QuantityT     = int32_t;
using IndexT        = size_t;
using TokenT        = uint32_t;
using ErrorMessageT = int32_t;
using CompositeIdT  = uint64_t;

constexpr static IndexT ClientCodeLength = 32;
constexpr static IndexT AlgoIDLength     = 32;
constexpr static IndexT PancardLength    = 10;
constexpr static IndexT ParentLength     = 16;
constexpr static IndexT Pad2Length       = 2;
constexpr static IndexT Pad3Length       = 3;

struct Ladder {
    PriceT    _price    = 0;  // 4
    QuantityT _quantity = 0;  // 4
};
enum LegExecution : uint8_t {
    LegExecution_MANUAL,
    LegExecution_STRATEGY_ENTRY,
    LegExecution_STRATEGY_EXIT,
};  // namespace Lancelot

enum OrderResponse : uint8_t {
    OrderResponse_NONE,
    OrderResponse_PENDING,
    OrderResponse_PLACED,
    OrderResponse_MODIFIED,
    OrderResponse_CANCELLED,
    OrderResponse_PARTIAL_FILLED,
    OrderResponse_FILLED,

    OrderResponse_NEW_REJECT,
    OrderResponse_MODIFY_REJECT,
    OrderResponse_CANCEL_REJECT,
};

static OrderResponse GetOrderResponsee(int code) {
    switch (code) {
        case 2223:
            return OrderResponse_PLACED;
        case 3334:
            return OrderResponse_MODIFIED;
        case 5555:
            return OrderResponse_CANCELLED;
        case 6666:
            return OrderResponse_FILLED;
        case 7777:
            return OrderResponse_NEW_REJECT;
    }
    return OrderResponse_NONE;
}
struct OrderResponseInfoT {
    TimeT    _timestamp = {};  // 8
    OrderIDT _orderId   = {};  // 8

    IndexT _uniqueId   = {};  // 8
    IndexT _fillNumber = {};  // 8

    Ladder _placed = {};  // 8
    Ladder _traded = {};  // 8

    QuantityT _remainingQuantity = {};  // 4
    TokenT    _token             = {};  // 4

    ErrorMessageT _errorCode       = {};  // 4
    OrderResponse _response        = {};  // 1
    char          _pad[Pad3Length] = {};  // 3 NOLINT
};

union CompositeId {
    UserPortfolio _fields;  // 4
    CompositeIdT  _index;   // 4
};

struct UserDetails {
    uint64_t    _locationId  = 0;   // 64
    uint32_t    _serverId    = 0;   // 32
    CompositeId _compositeID = {};  // 32

    char _clientCode[ClientCodeLength] = {'\0'};  // 32 NOLINT
    char _algoId[AlgoIDLength]         = {'\0'};  // 32 NOLINT
    char _pandcard[PancardLength]      = {'\0'};  // 10 NOLINT
    char _pad[Pad2Length]              = {'\0'};  // 2  NOLINT
};
enum Side : uint8_t {
    Side_BUY = 1,
    Side_SELL
};
enum OrderType : uint8_t {
    OrderType_LIMIT    = 1,
    OrderType_IOC      = 3,
    OrderType_STOPLOSS = 4,
};
struct Information {
    struct Fields {
        OrderType    _orderType    = OrderType_IOC;                // 1
        Side         _side         = Side_BUY;                     // 1
        LegExecution _legExecution = LegExecution_STRATEGY_ENTRY;  // 1
        uint8_t      _pad          = {};                           // 1
    };

    OrderIDT _orderId              = 0;   // 8
    Fields   _fields               = {};  // 4
    char     _parent[ParentLength] = {};  // 16 NOLINT
};

struct ExternalOrderResponse {
    OrderResponseInfoT _response        = {};
    UserDetails        _userDetails     = {};
    Information        _information     = {};
    uint16_t           _echo            = {};
    char               _pad[Pad2Length] = {};  // NOLINT
};

#pragma pack(pop)
