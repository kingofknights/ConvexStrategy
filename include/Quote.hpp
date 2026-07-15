/**
 * @file Quote.hpp
 * @brief Market-depth quote message definitions (header + MBP body).
 */
#pragma once

#include "MboTypes.hpp"
#include "MbpTypes.hpp"

namespace aef {
namespace infra {
namespace quote {

/// Depth of book stored in quote structures.
constexpr int32_t QUOTE_LEVELS = 5;

/** @brief Common header prepended to all quote updates. */
struct __attribute__((packed)) QuoteHeader {
    int32_t product_id;/*!< Product Id */   
    int16_t stream_id;/*!< Stream Id */
    int16_t segment_id;/*!< Segment Id */
    int32_t feed_status;/*!< Feed Status */
    int32_t sequence_no;/*!< Seq. No. */
    int16_t bids;/*!< No. Of Bid Levels */
    int16_t asks;/*!< No. Of Ask Levels. */
    uint64_t event_timestamp;/*!< Event TimeStamp. */
    uint64_t trigger_timestamp;/*!< Trigger TimeStamp */
    uint64_t exchange_timestamp;/*!< Exchange TimeStamp */
};

enum QUOTE_SIDE { BUY_SIDE = 0b01, SELL_SIDE = 0b10 };

/** @brief Encodes how a level changed and on which side/level. */
struct __attribute__((packed)) UpdateType {
    uint8_t update_type; /*!< 'N'- NEW Order, 'M' - MODFIY Order, 'X' - CANCEL Order, 'T' - TRADE, 'S' - Synthetic Trade*/
    uint8_t update_side : 2;  /*!< 1 - BUY, 2 - SELL */
    int8_t  update_level : 6; /*!< Lowest Level at which update happened */   
};

/**
 * @brief Body of a quote update carrying last trade and top-of-book data.
 * @tparam BookType Book representation for each level.
 */
template <class BookType>
struct __attribute__((packed)) QuoteBody {
    UpdateType updatetype_;
    int32_t vol_traded_today_{0};/*!< Total volume till now */
    int32_t ltp_{0};/*!< Last Traded Price */
    int32_t ltq_{0};/*!< Last Traded Qty */
    int32_t vltp_{0};/*!< Last Traded Price */
    int32_t vltq_{0};/*!< Last Traded Qty */
    int32_t bit_{0};/*!< Buyer Initiated Trades */
    int32_t sit_{0};/*!< Seller Initiated Trades */
    int32_t bid_count_{0};/*!< No Of Buy Orders Till 5th Level */
    int32_t ask_count_{0};/*!< No Of Sell Orders Till 5th Level */
    int64_t vwap_{0};/*!< Volume Weighted Average Price */
    int32_t update_level_old{0};
    int32_t price{0};/*!< price of Tick  */
    int32_t qty{0};/*!< qty of Tick*/
    int32_t lastprice{0};/*!< price of Prev Tick  */
    int32_t lastqty{0};/*!< qty of Prev Tick */
    int64_t buy_orderid{0};/*!< buy_orderid  */
    int64_t sell_orderid{0};/*!< sell_orderid */
    BookType bid_levels[QUOTE_LEVELS];/*!< Top 5 Bid Levels(Qty,Price,No.Of Orders) */
    BookType ask_levels[QUOTE_LEVELS];/*!< Top 5 ask Levels(Qty,Price,No.Of Orders) */
};

/** @brief Complete quote message combining header and MBP body. */
struct __attribute__((packed)) Quote {
    QuoteHeader header;
    QuoteBody<aef::infra::mbp_book::st_mbp_info> message; /*!< n Levels Quote Body */
};

} // namespace quote

} // namespace infra
} // namespace aef
