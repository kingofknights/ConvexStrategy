/**
 * @file rms_api.hpp
 * @brief Risk management structures and limit/reason codes.
 */
#pragma once
#include <stdint.h>
#include <string.h>
//!  namespace rms 
/*!
        This namespace is for RMS.
*/
namespace aef {
namespace oms {
namespace rms {

const int32_t PRO_CLIENT_IDENTIFIER = 1;

/** @brief Client details returned by RMS lookup. */
struct __attribute__((packed)) client_info {
    int32_t client_id{0};/*!< Mapped Client Id */
    int32_t reserved;/*!< Reserved Field */
    char client_name[16];/*!< Client Name */
    char pan[16];/*!< PAN No. */
    char settlor[16];/*!< Settlor. */

    client_info()
    {
        memset(client_name, ' ', 16);
        memset(pan, ' ', 16);
        memset(settlor, ' ', 16);
    }
};

/** @brief Dealer identifier and client mapping count. */
struct dealer_info {
    char dealer_id[16];
    int32_t mapped_clients_;
};

/** @brief Risk limit categories enforced by OMS/RMS. */
enum class RMS_LIMIT_TYPE : int32_t {
    SINGLE_ORDER_QUANTITY_LIMIT = 41111,/*!< Quantity Limit Set At Single Order Level */
    SINGLE_ORDER_VALUE_LIMIT = 41112,/*!< Value Limit Set At Single Order Level */
    MTM_LOSS_LIMIT = 41113,/*!< Mark To Market Limit Set */
    DPR_LIMIT = 41114,/*!< DPR Limit*/
    TER_LIMIT = 41115,/*!< TER Limit */
    MAX_LIVE_ORDER_COUNT_LIMIT = 41116,/*!< Max Live Order Count Limit */
    MAX_TURNOVER = 41117,/*!< Maximum Turnover Limit Set */
    MAX_TOTAL_BUY_QTY = 41118,/*!< Maximum Total Buy Qty Limit Set */
    MAX_TOTAL_BUY_VALUE = 41119,/*!< Maximum Total Buy Value Limit Set */
    MAX_TOTAL_SELL_QTY = 41120,/*!< Maximum Total Sell Qty Limit Set */
    MAX_TOTAL_SELL_VALUE = 41121,/*!< Maximum Total Sell Value Limit Set */
    PRICE_LIMIT = 41122,/*!< Price Limit */
    MAX_NET_QUANTITY = 41123,/*!< Max Net Quantity */
    MAX_NET_VALUE = 41124,/*!< Max Net Value */
    QTY_CHECK = 41125,/*!< Qty Lot Size Check */
    TICK_SIZE_CHECK = 41126,/*!< Tick Size Check */
    FREEZE_QTY_CHECK = 41127,
    MAX_NET_QTY_PERTOKEN = 41128,/*!< Maximum Total Buy Qty Limit Set */
    MAX_NET_VALUE_PERTOKEN = 41129,/*!< Maximum Total Buy Value Limit Set */
    MARGIN_CHECK = 41130,
    MAX_TOTAL_BUY_POSITION_CHECK = 41131,
    MAX_TOTAL_SELL_POSITION_CHECK = 41132,
    MAX_TOTAL_BUY_OPEN_VALUE_CHECK = 41133,
    MAX_TOTAL_SELL_OPEN_VALUE_CHECK = 41134,
    MAX_TOTAL_TOTAL_OPEN_VALUE_CHECK = 41135,
    MAX_TOTAL_BUY_OPEN_QTY_CHECK = 41136,
    MAX_TOTAL_SELL_OPEN_QTY_CHECK = 41137,
    MAX_TOTAL_TOTAL_OPEN_QTY_CHECK = 41138,
    MAX_TOTAL_NET_BUY_VALUE_CHECK = 41139,
    MAX_TOTAL_NET_SELL_VALUE_CHECK = 41140,
    MAX_TOTAL_BUY_EXPOSURE_CHECK = 41141,
    MAX_TOTAL_SELL_EXPOSURE_CHECK = 41142,
    MWPL_CHECK = 41143,
    MAX_BUY_POSITION_CHECK_PERTOKEN = 41144,
    MAX_SELL_POSITION_CHECK_PERTOKEN = 41145,
    AUTOEXEC_CHECK = 41146,    
    MAX_GROSS_EXPOSURE=41148,
    MAX_BUY_TURNOVER=41149,
    MAX_SELL_TURNOVER=41150,
    VELOCITY_TURNOVER_PER_SECOND=41151

};
 //!User should handle following RMS Reason Codes in their code as per their functionality
/** @brief Detailed RMS rejection reason codes. */
enum class RMS_REASON_CODE : int32_t {
    INVALID_REASON_CODE = -90999,/*!< Invalid Reason Code */
    CLI_SINGLE_ORDER_QUANTITY_LIMIT_FAILED = -91000,/*!< Single Order Quantity Limit Failed at Client Level */
    TERM_SINGLE_ORDER_QUANTITY_LIMIT_FAILED = -91001,/*!< Single Order Quantity Limit Failed at Terminal Level */
    CLI_SINGLE_ORDER_VALUE_LIMIT_FAILED = -91002,/*!< Single Order Value Limit Failed at Client Level */
    TERM_SINGLE_ORDER_VALUE_LIMIT_FAILED = -91003,/*!< Single Order Value Limit Failed at Terminal Level */
    CLI_MTM_LOSS_LIMIT_FAILED = -91004,/*!< MTM Loss Limit Failed at Client Level */
    TERM_MTM_LOSS_LIMIT_FAILED = -91005,/*!< MTM Loss Limit Failed at Terminal Level */
    DPR_LIMIT_FAILED = -91006,/*!< DPR Limit Failed */
    TER_LIMIT_FAILED = -91007,/*!< TER Limit Failed */
    FREEZE_QTY_EXCEEDED = -91008,/*!< Freeze Qty Exceeded */
    CLI_MAX_TURNOVER_EXCEEDED = -91009,/*!< Max Turnover Exceeded at Client Level */
    TERM_MAX_TURNOVER_EXCEEDED = -91010,/*!< Max Turnover Exceeded at Terminal Level */
    CLI_MAX_TOTAL_BUY_QTY_EXCEEDED = -91011,/*!< Max Total Buy Qty Exceeded at Client Level */
    TERM_MAX_TOTAL_BUY_QTY_EXCEEDED = -91012,/*!< Max Total Buy Qty Exceeded at Terminal Level */
    CLI_MAX_TOTAL_BUY_VALUE_EXCEEDED = -91013,/*!< Max Total Buy Value Exceeded at Client Level */
    TERM_MAX_TOTAL_BUY_VALUE_EXCEEDED = -91014,/*!< Max Total Buy Value Exceeded at Terminal Level */
    CLI_MAX_TOTAL_SELL_QTY_EXCEEDED = -91015,/*!< Max Total Sell Qty Exceeded at Client Level */
    TERM_MAX_TOTAL_SELL_QTY_EXCEEDED = -91016,/*!< Max Total Sell Qty Exceeded at Terminal Level */
    CLI_MAX_TOTAL_SELL_VALUE_EXCEEDED = -91017,/*!< Max Total Sell Value Exceeded at Client Level */
    TERM_MAX_TOTAL_SELL_VALUE_EXCEEDED = -91018,/*!< Max Total Sell Value Exceeded at Terminal Level */
    LIMIT_PRICE_RANGE_EXCEEDED = -91019,/*!< Limit Price Range Exceeded */
    CLI_MAX_NET_QUANTITY_EXCEEDED = -91020,/*!< Max Net Qty Exceeded at Client Level */
    TERM_MAX_NET_QUANTITY_EXCEEDED = -91021,/*!< Max Net Qty Exceeded at Terminal Level */
    CLI_MAX_NET_VALUE_EXCEEDED = -91022,/*!< Max Net Value Exceeded at Client Level */
    TERM_MAX_NET_VALUE_EXCEEDED = -91023,/*!< Max Net Value Exceeded at Terminal Level */
    CLI_MAX_NET_QTY_EXCEEDED_PERTOKEN = -91029,
    TERM_MAX_NET_QTY_EXCEEDED_PERTOKEN = -91032,
    CLI_MAX_NET_VALUE_EXCEEDED_PERTOKEN = -91033,
    TERM_MAX_NET_VALUE_EXCEEDED_PERTOKEN = -91034,
    INVALID_QTY = -91024,/*!< Invalid Qty */
    INVALID_PRICE = -91025,/*!< Invalid Price */
    INVALID_CLIENT_ID = -91026,/*!< Invalid Client Id */
    INVALID_ALGO_ID = -91027,/*!< Invalid Algo Id */
    INVALID_OMS_ID = -91028,/*!< Invalid Oms Id */
    CLI_MAX_MARGIN_EXCEEDED = -91035, 
    TERM_MAX_MARGIN_EXCEEDED = -91036,
    CLI_MAX_TOTAL_BUY_POSITION_EXCEEDED = -91040,
    CLI_MAX_TOTAL_SELL_POSITION_EXCEEDED = -91041,
    TERM_MAX_TOTAL_BUY_POSITION_EXCEEDED = -91042,
    TERM_MAX_TOTAL_SELL_POSITION_EXCEEDED = -91043,
    CLI_MAX_TOTAL_BUY_OPEN_VALUE_EXCEEDED = -91044,
    CLI_MAX_TOTAL_SELL_OPEN_VALUE_EXCEEDED = -91045,
    CLI_MAX_TOTAL_TOTAL_OPEN_VALUE_EXCEEDED = -91046,
    TERM_MAX_TOTAL_BUY_OPEN_VALUE_EXCEEDED = -91047,
    TERM_MAX_TOTAL_SELL_OPEN_VALUE_EXCEEDED = -91048,
    TERM_MAX_TOTAL_TOTAL_OPEN_VALUE_EXCEEDED = -91049,
    CLI_MAX_TOTAL_BUY_OPEN_QTY_EXCEEDED = -91050,
    CLI_MAX_TOTAL_SELL_OPEN_QTY_EXCEEDED = -91051,
    CLI_MAX_TOTAL_TOTAL_OPEN_QTY_EXCEEDED = -91052,
    TERM_MAX_TOTAL_BUY_OPEN_QTY_EXCEEDED = -91053,
    TERM_MAX_TOTAL_SELL_OPEN_QTY_EXCEEDED = -91054,
    TERM_MAX_TOTAL_TOTAL_OPEN_QTY_EXCEEDED = -91055,

    CLI_MAX_TOTAL_BUY_EXPOSURE_EXCEEDED = -91056,
    CLI_MAX_TOTAL_SELL_EXPOSURE_EXCEEDED = -91057,
    TERM_MAX_TOTAL_BUY_EXPOSURE_EXCEEDED = -91058,
    TERM_MAX_TOTAL_SELL_EXPOSURE_EXCEEDED = -91059,

    CLI_MAX_TOTAL_NET_BUY_VALUE_EXCEEDED = -91060,
    CLI_MAX_TOTAL_NET_SELL_VALUE_EXCEEDED = -91061,
    TERM_MAX_TOTAL_NET_BUY_VALUE_EXCEEDED = -91062,
    TERM_MAX_TOTAL_NET_SELL_VALUE_EXCEEDED = -91063,
    TERM_MWPL_EXCEEDED = -91064,
    CLI_MAX_BUY_POSITION_EXCEEDED = -91065,
    CLI_MAX_SELL_POSITION_EXCEEDED = -91066,
    TERM_MAX_BUY_POSITION_EXCEEDED = -91067,
    TERM_MAX_SELL_POSITION_EXCEEDED = -91068,
    AUTOEXEC_EXCEEDED = -91069,
    CLI_MAX_MARGINE_EXCEEDED = -91070,/*!< Max margine Value Exceeded at Client Level */
    TERM_MAX_MARGINE_EXCEEDED = -91071,/*!< Max margine Value Exceeded at Terminal Level */
    TERM_MAX_TOTAL_NET_EXPOSURE_EXCEEDED=-91072,
    CLI_MAX_TOTAL_NET_EXPOSURE_EXCEEDED=-91073

};

/** @brief Scope for applying RMS limits. */
enum class RMS_LIMIT_SCOPE : int32_t {
    RMS_GLOBAL_SCOPE = 2111,
    RMS_DEALER_SCOPE = 2112,
    RMS_CLIENT_SCOPE = 2113
};

} // namespace rms
} // namespace oms
} // namespace aef
