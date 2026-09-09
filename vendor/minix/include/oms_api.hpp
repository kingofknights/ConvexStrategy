/**
 * @file oms_api.hpp
 * @brief OMS request/response wire-format definitions and enums.
 */
#pragma once
#include <stdint.h>


/** @brief Identifier packing strategy id, client id, and request id. */
union client_uid {
    struct __attribute__((packed)) client_identifier {
        uint32_t strategy_id : 7;
        uint32_t client_id : 3;
        uint32_t request_id : 22;
    } composite_id_;
    uint32_t id_;
};

// struct __attribute__((packed)) client_identifier {
//     uint32_t strategy_id : 7;/*!< Strategy Id */
//     uint32_t client_id : 3;/*!< Strategy Id */
//     uint32_t request_id : 22;/*!< Request Id */
// //    uint32_t strategy_id : 6;/*!< Strategy Id */
// //    uint32_t request_id : 26;/*!< Request Id */
// };

/** @brief Compile-time power calculator used for id limits. */
template <int A, int B>
struct get_power {
    static const int value = A * get_power<A, B - 1>::value;
};

template <int A>
struct get_power<A, 0> {
    static const int value = 1;
};

const uint32_t MAX_REQUEST_ID = (get_power<2, 22>::value - 1);
const uint32_t MAX_STRAT_ID = (get_power<2, 7>::value - 1);

// union client_uid {
//     client_identifier composite_id_;/*!< client_identifier struct */
//     uint32_t id_;/*!< id_ */
// };

/** @brief Request/response header shared by OMS transactions. */
struct __attribute__((packed)) oms_api_header {
    int32_t transaction_code; /*!< Transaction Code */
    client_uid uid_; /*!< client_uid struct */
    int32_t error_code; /*!< Error Code */
    int32_t reason_code; /*!< Reason Code */
    uint64_t trigger_timestamp; /*!< Trigger TimeStamp */
    uint64_t event_timestamp; /*!< Event TimeStamp */
    uint64_t exchange_timestamp; /*!< Exchange TimeStamp */
};

enum OMS_API_TRANS_CODES {
    OMS_PLACE_REQ = 1111,/*!< Use this code while placing new order */
    OMS_REPLACE_REQ = 1112,/*!< Use this code while placing modification of order */
    OMS_CANCEL_REQ = 1113,/*!< Use this code while placing cancellation of order */
    OMS_ORDER_PLACED = 2222,/*!< This code is received when order is successfully placed at OMs  */
    OMS_ORDER_CONFIRMED = 2223,/*!< This code is received when order  gets confirmed at exchange  */
    OMS_ORDER_MODIFY_PLACED = 3333,/*!< This code is received when order is successfully mofified at OMS  */
    OMS_ORDER_MODIFY_CONFIRMED = 3334,/*!< This code is received when order modification gets confirmed at exchange  */
    OMS_ORDER_CANCEL_ACCEPTED = 4444,/*!< This code is received when order is successfully cancelled at OMS  */
    OMS_ORDER_CANCELLED = 5555,/*!< This code is received when order cancellation gets confirmed at exchange  */
    OMS_TRADE = 6666,/*!< This code is received when Trade Confirmation is recived from exchange  */
    OMS_REQ_REJ = 7777,/*!< This code is received when order request is reject either at OMS or at Exchange level.  */            
    OMS_PLACE_REQ2L = 8111,/*!< Use this code while placing new order2L */
    OMS_REPLACE_REQ2L = 8112,/*!< Use this code while placing modification of order2L */
    OMS_CANCEL_REQ2L = 8113,/*!< Use this code while placing cancellation of order2L */
    OMS_ORDER_PLACED2L = 8222,/*!< This code is received when order2L is successfully placed at OMs  */
    OMS_ORDER_CONFIRMED2L = 8223,/*!< This code is received when order2L  gets confirmed at exchange  */
    OMS_ORDER_MODIFY_PLACED2L = 8333,/*!< This code is received when order2L is successfully mofified at OMS  */
    OMS_ORDER_MODIFY_CONFIRMED2L = 8334,/*!< This code is received when order2L modification gets confirmed at exchange  */
    OMS_ORDER_CANCEL_ACCEPTED2L = 8444,/*!< This code is received when order2L is successfully cancelled at OMS  */
    OMS_ORDER_CANCELLED2L = 8555,/*!< This code is received when order2L cancellation gets confirmed at exchange  */
    OMS_TRADE2L = 8666,/*!< This code is received when Trade Confirmation is recived from exchange order2L  */
    OMS_REQ_REJ2L = 8777,/*!< This code is received when order request is reject either at OMS or at Exchange level. order2L */
    OMS_PLACE_REQ_NONTR = 8888,/*!< Use this code while placing new order non trimmed version*/
    OMS_REPLACE_REQ_NONTR = 8881,/*!< Use this code while placing modification of order non trimmed version*/
    OMS_CANCEL_REQ_NONTR = 8882,/*!< Use this code while placing cancellation of order non trimmed version*/
    
    OMS_PLACE_REQ_SL_NONTR = 8818,/*!< Use this code while placing new order non trimmed version*/
    OMS_KILL_SWITCH = 9999,
    OMS_KILL_SWITCH_TOKEN_WISE = 9991,
    OMS_KILL_SWITCH_ACCEPTED = 1233,
    OMS_KILL_SWITCH_REJ = 1234,
    OMS_TRADE_PREV_SESSION = 6667
   // OMS_SQUAROFF_SWITCH = 1331,
};

enum OMS_API_ERROR_CODES {
    INVALID_PRICE_MODIFY = -10000,
    CONNEX_FAILURE = -9999,/*!< Connection failure*/
    INVALID_PRICE = -9998,/*!< Invalid Price*/
    INVALID_QTY = -9997,/*!< Invalid Qty*/
    THROTTLE_LIMIT = -9996,/*!< No. Of Message requests to exchange more than the message limit specified*/
    EXCHG_ERROR = -9995,/*!< Error code received from exchange*/
    RMS_ERROR = -9994,/*!< Order Rejected from OMS due to RMS Limit failure*/
    INVALID_REQ_STATUS = -9993,/*!< Invalid Req status*/
    INVALID_PRODUCT = -9992,/*!< Invalid Product*/
    INVALID_CLIENT_ID = -9991,/*!< Invalid Client Id */
    INVALID_CLIENT_REQ_ID = -9990,/*!< Invalid Client Req Id */
    INVALID_TRANS_ID = -9989/*!< Invalid transaction Code */
};

enum class OMS_REQ_STATUS : uint8_t {
    INITIAL_STATUS = 0,
    OMS_ORDER_PLACED = 0b00000001,
    OMS_ORDER_CONFIRMED = 0b00000010,
    OMS_ORDER_MODIFY_PLACED = 0b00000100,
    OMS_ORDER_CANCEL_PLACED = 0b00001000,
    OMS_ORDER_CANCELLED = 0b00010000,
    OMS_ORDER_TRADED = 0b00100000,
    OMS_ORDER_KILL_SWITCH       = 0b01000000,
    OMS_ORDER_COMPLETELY_TRADED = 0b10000000
};

enum ORDER_TYPE { LIMIT_ORDER_TYPE = 0b0001, /*!< Order Type - LIMIT */
                  IOC_ORDER_TYPE = 0b0010    /*!< Order Type - IOC */};

enum ORDER_SIDE { BUY_SIDE = 0b01 ,  /*!< Order Side - BUY */ 
 	          SELL_SIDE = 0b10  /*!< Order Side - SELL */};

enum PRICE_TYPE { MARKET_PRICE = 0b001, /*!< Price Type - MARKET */
                 LIMIT_PRICE = 0b010    /*!< Price Type - LIMIT */};

/** @brief Encoded flags describing order type, side, and price type. */
struct __attribute__((packed)) order_flags {
    uint32_t order_type : 4;/*!< Order Type -LIMIT/IOC */
    uint32_t order_side : 2;/*!< Order Side -BUY/SELL */
    uint32_t price_type : 3;/*!< Price Type -LIMIT/MARKET */
    uint32_t pro_client : 2;/*!< Pro_Client -PRO/CLI */
    uint32_t reserved : 21;
};

/** @brief Payload for single-leg OMS requests/responses. */
struct __attribute__((packed)) oms_api_body {
    int32_t product_id_;/*!< Product ID */
    int32_t price_;/*!< Price */
    int32_t quantity_;/*!< Quantity */
    order_flags flags_;/*!< Order Flags */
    uint64_t exchange_order_id;/*!< Exchange Order Id */
    int32_t exchange_error_code;/*!< Exchange Error Code */
    int32_t exchange_response_code;/*!< Exchange Response Code */
    int32_t client_id;/*!< Client Id */
    int32_t oms_id;/*!< OMS Id */
    int32_t algo_id;/*!< Algo Id */
    int32_t exchange_fill_id;/*!< Exchange Fill Id */
};

/** @brief Combined header and body for single-leg OMS messages. */
struct __attribute__((packed)) oms_transaction {
    oms_api_header hdr_;
    oms_api_body packet_;
};


/** @brief Alternative client identifier layout for 3L packets. */
struct __attribute__((packed)) client_identifiernew {  
    uint32_t client_id ;/*!< Strategy Id */
    uint32_t request_id ;/*!< Request Id */
    char strategy_id ;/*!< Strategy Id */
};

/** @brief Order flags for 3-leg UI packets. */
struct __attribute__((packed)) order_flags3l {
    uint32_t order_type ;/*!< Order Type -LIMIT/IOC */
    uint32_t order_side ;/*!< Order Side -BUY/SELL */
    uint32_t price_type ;/*!< Price Type -LIMIT/MARKET */   
};

/** @brief Header used by UI-driven three-leg order packets. */
struct __attribute__((packed)) oms_api_header3l {
    int32_t transaction_code; /*!< Transaction Code */
    client_identifiernew uid_; /*!< client_uid struct */
    int32_t error_code; /*!< Error Code */
    int32_t reason_code; /*!< Reason Code */
    uint64_t trigger_timestamp; /*!< Trigger TimeStamp */
    uint64_t event_timestamp; /*!< Event TimeStamp */
    uint64_t exchange_timestamp; /*!< Exchange TimeStamp */
};
/** @brief Body used by UI-driven three-leg order packets. */
struct __attribute__((packed)) oms_api_body3l {
    uint64_t product_id_[3];/*!< Product ID */
    int32_t price_[3];/*!< Price */
    int32_t quantity_[3];/*!< Quantity */
    order_flags3l flags_[3];/*!< Order Flags */ 
    uint64_t exchange_order_id;/*!< Exchange Order Id */
    int32_t exchange_error_code;/*!< Exchange Error Code */
    int32_t exchange_response_code;/*!< Exchange Response Code */
    int32_t client_id;/*!< Client Id */
    int32_t oms_id;/*!< OMS Id */
    int32_t algo_id;/*!< Algo Id */
    int32_t exchange_fill_id;/*!< Exchange Fill Id */
    short token_status;
};
/** @brief UI header prepended to OMS three-leg messages. */
struct __attribute__((packed)) UIHeader3l {
    int32_t message_code;/*!< Unique Message Code  */
    int32_t component_id;/*!< Unique id of the component, to which message will be sent */
    int32_t message_length;/*!< Length of the message */
    int32_t timestamp;/*!< Time Stamp */
    int32_t interface_id;/*!< Id of the server */
};
/** @brief Full OMS packet for three-leg operations. */
struct __attribute__((packed)) oms_transaction3l {
    UIHeader3l uihdr;
    oms_api_header3l hdr_;
    oms_api_body3l packet_;
};
