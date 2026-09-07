/**
 * @file order_instance.hpp
 * @brief Convenience wrapper around OMS order lifecycle.
 */
#pragma once
#include <vector>
#include <unordered_map>
#include <string.h>
#include "include/Quote.hpp"
#include <include/ProductInfo.hpp>
#include "../include/oms_api.hpp"
#include <include/rms_api.hpp>
#include <include/ui_api.hpp>
#include <include/AlgoBase.hpp>
#include <include/TimeUtils.hpp>
#include <inttypes.h>
#include <assert.h>
namespace execution_strat 
{

enum class STRAT_ORDER_STATE : uint32_t {
        STRAT_INITIAL_STATE = 0,/*!< Initial State - Order Not Placed Yet */
        STRAT_ORDER_PLACED = 1,/*!< Order Placed - Order is sent to OMS */
        STRAT_OMS_PLACED = 2,/*!< Order Placed Confirmation from OMS  */
        STRAT_EXCHG_CONF = 4,/*!< Exchange Confirmation  */
        STRAT_MODIFY_PLACED = 8,/*!< Modify Placed - Modification is sent to OMS */
        STRAT_OMS_MOD_PLACED = 16,/*!< Modification Placed Confirmation from OMS  */
        STRAT_CXL_PLACED = 32,/*!< Cancellation Placed - Cancellation is sent to OMS */
        STRAT_OMS_CXL_PLACED = 64/*!< Cancellation Placed Confirmation from OMS  */
};

/**
 * @brief Manages a single logical order and its state transitions.
 *
 * The wrapper tracks quantities, pushes requests via AlgoBase, and consumes
 * confirmations from the OMS/exchange to keep local state consistent.
 */
class order_instance {
  private:
    int32_t product_id_{0};
    int32_t open_qty_{0};
    int32_t notional_open_qty_{0};
    int64_t filled_qty_{0};
    uint32_t uid_{0};
    uint32_t client_id_;
    uint32_t algo_id_;
    int16_t omsid_;
    const int64_t max_qty_;
    ORDER_SIDE side_;
    uint32_t current_state_{static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_INITIAL_STATE)};
    AlgoBase* context_{nullptr};

    oms_transaction myOrder_;
    bool pendingCancel_{false};
    uint64_t timestamp;    
    uint64_t quote_event_timestamp;
    uint64_t quote_trigger_timestamp;
    uint64_t quote_received_timestamp;
    int32_t open_price_{0};
    int32_t filled_price_{0};
    ORDER_TYPE typeoforder;
  public:
        /**
         * @brief Construct an order wrapper for a token.
         * @param token Product identifier.
         * @param side Buy or sell side.
         * @param max_qty Maximum open quantity allowed.
         * @param client_id Client identifier.
         * @param algo_id Algorithm identifier.
         * @param omsid_ OMS identifier for routing.
         * @param odtype Order type used for requests.
         * @param context Pointer to AlgoBase for sending orders/logging.
         */
        order_instance(const int token, ORDER_SIDE side, const int64_t max_qty,const int32_t client_id,const int32_t algo_id,int16_t omsid_,ORDER_TYPE odtype,AlgoBase* context);

    /** @brief Whether an OMS response is awaited for the latest request. */
    bool is_response_pending() const;
    /** @brief True once the exchange confirms placement/modification. */
    bool is_order_confirmed() const;
    /** @brief True if cancel has been requested but not yet acknowledged. */
    bool is_order_to_be_cancelled() const;

    /**
     * @brief Send a new order using a supplied UID.
     * @return Valid UID on success, 0 otherwise.
     */
    uint32_t place_order(const int32_t price, const int32_t qty,const uint32_t uidno);
    /**
     * @brief Send a new order using an auto-generated UID.
     * @return Valid UID on success, 0 otherwise.
     */
    uint32_t place_order(const int32_t price, const int32_t qty);

    /**
     * @brief Modify price/quantity for an existing order.
     * @return Non-zero on success, false on failure.
     */
    bool update_order(const int32_t price, const int32_t qty);

    /** @brief Cancel the order if confirmed and no response is pending. */
    bool cancel_order();
        
    /** @brief Reset local state after terminal events. */
    void reset();
    /**
     * @brief Process a confirmation/acknowledgement from OMS.
     * @param response OMS response packet.
     */
    void handle_confirmation(const oms_transaction& response);
    /** @brief Current open quantity in the market. */
    int32_t get_open_qty() const;
    /** @brief Outstanding qty still awaiting confirmation. */
    int32_t get_notional_qty() const;
    /** @brief Bitmask of STRAT_ORDER_STATE describing lifecycle. */
    uint32_t get_current_state() const;
    /** @brief UID assigned to the latest live order. */
    uint32_t get_uid() const;
    /** @brief Quantity that was last sent to OMS. */
    int32_t get_existing_qty() const;
    /** @brief Price that was last sent to OMS. */
    int32_t get_existing_price() const;
    /** @brief Filled quantity accumulated so far. */
    int32_t get_filled_qty() const;
    /** @brief Last fill price received. */
    int32_t get_filled_price() const;
    /** @brief Price currently live at the exchange. */
    int32_t get_open_price() const;
        
        void set_time_stamps(const uint64_t& qt_event_timestamp,
        const uint64_t& qt_trigger_timestamp,
        const uint64_t qt_received_timestamp);
        /** @brief Set custom price type on outgoing packet. */
        void set_price_type() ;
        /** @brief Side helper for UI/strategy logs. */
        ORDER_SIDE get_side()  ;
        /** @brief Place a two-legged order (2L) for synthetics. */
        uint32_t place_order2L(const int32_t price, const int32_t qty);
        /** @brief Modify a previously placed two-legged order. */
        bool update_order2L(const int32_t price, const int32_t qty);
        /** @brief Cancel a two-legged order. */
        bool cancel_order2L();

        /** @brief Issue kill switch for a specific token. */
        bool kill_tokenOrder(const int32_t token);
        /** @brief Issue kill switch for all tokens/orders. */
        bool kill_allOrder();
        
};
    

} // namespace execution_strat
