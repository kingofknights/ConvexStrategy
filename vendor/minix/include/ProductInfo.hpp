/**
 * @file ProductInfo.hpp
 * @brief Instrument metadata and subscription flag definitions.
 */
#pragma once
#include <stdint.h>
#include "MbpTypes.hpp"

namespace aef {
namespace infra {
namespace product {
//!  Product Attributes class. 
    
    
/// Snapshot flags \n 
/// to be used while subscribing to various events for specific instruments \n 
enum class SNAPSHOT_FLAGS : uint16_t {
    TER_UPDATE_EVENT = 0x0001, /*!< Subscribe to Trade Execution Range Events*/
    MBP_UPDATE_EVENT = 0x0002, /*!< Subscribe to Snapshot Data Events*/
    SECURITY_UPDATE_EVENT = 0x0004, /*!< Subscribe to Security Update Events like DPR */
    TBT_UPDATE_EVENT = 0x0008, /*!< Subscribe to TBT data Events*/
    OI_UPDATE_EVENT = 0x0010  /*!< Subscribe to OI Events*/// For Open Interest Changes
};

/// Instrument Types \n 
/// Different Instruments supported by platform \n 
enum class INSTRUMENT_TYPE : uint16_t {
    INVALID_INSTR = 0, /*!< Invalid Instrument Type*/
    NSE_EQ = 1, /*!< Equity Instrument Type*/
    NSE_FUTIDX = 2, /*!< Index Future Instrument Type*/
    NSE_FUTSTK = 3, /*!< Stock Future Instrument Type*/
    NSE_OPTIDX = 4, /*!< Index Option Instrument Type*/
    NSE_OPTSTK = 5, /*!< Stock Option Instrument Type*/
    BSE_EQ = 6, /*!< Equity Instrument Type*/
    BSE_FUTIDX = 7, /*!< Index Future Instrument Type*/
    BSE_FUTSTK = 8, /*!< Stock Future Instrument Type*/
    BSE_OPTIDX = 9, /*!< Index Option Instrument Type*/
    BSE_OPTSTK = 10, /*!< Stock Option Instrument Type*/
};

/// Option Types \n 
/// Different Option Types supported by platform \n
/// For Non-Derivatives, the type will be 0 i.e INVALID_OPT \n 
enum class OPTION_TYPE : uint16_t {
    INVALID_OPT = 0, /*!< Invalid Option Type*/
    FUTXX = 11, /*!< Future*/
    CE = 22, /*!< European Call*/
    PE = 33, /*!< European Put*/
    CA = 44, /*!< American Call*/
    PA = 55 /*!< American Put*/
};

/// Product data \n 
/// Sent whenever Product TER / DPR / Touchline / ltp , etc have changed
struct __attribute__((packed)) product_data {
    int32_t product_id_{0}; /*!< unique token number*/
    int32_t underline_product_id_{0}; /*!< unique underline productID */
    int32_t high_dpr_{0}; /*!< High DPR Limit*/
    int32_t low_dpr_{0}; /*!< Low DPR Limit*/
    int32_t freeze_qty_{0}; /*!< Freeze Quantity*/
    int32_t ltp_{0}; /*!< Last Traded Price*/
    int32_t ltq_{0}; /*!< Last Traded Qty*/
    int64_t volume_traded_today_{0};
    int32_t highExecBand_{0}; /*!< High TER Limit*/
    int32_t lowExecBand_{0}; /*!< Low TER Limit*/
    int32_t strike_price_{-1}; /*!< Strike Price*/
    int32_t expiry_date_{0}; /*!< Expiry Date of asset/instrument*/
    int16_t lot_size_{0}; /*!< Minimum lot size*/
    int16_t tick_size_{5}; /*!< Tick size*/
    int16_t enabled_{1}; /*!< Product Enabled / Disabled */
    uint16_t flags_{0}; /*!< Event Flags eg. TER_UPDATE_EVENT | MBP_UPDATE_EVENT*/
    INSTRUMENT_TYPE instru_type_; /*!< Instrument Type */
    OPTION_TYPE opt_type_; /*!< Option Type */
    aef::infra::mbp_book::st_mbp_info_cm bid_mbp[5]; /*!< BID Touchline */
    aef::infra::mbp_book::st_mbp_info_cm ask_mbp[5]; /*!< ASK Touchline */
    char symbol[11];/*!< Symbol*/
    char instrumentName[26];/*!< Complete Name*/
    int32_t open_interest{0}; /*!< Open Interest */
    char NetChangeIndicator;
    int NetPriceChangeFromClosingPrice{0};
    int AverageTradePrice{0};
    int LastTradeTime{0};
    int16_t TradingStatus{0};
    int OpenPrice{0};
    int ClosingPrice{0};
    int HighPrice{0};
    int LowPrice{0};
};


} // namespace product
} // namespace infra
} // namespace aef
