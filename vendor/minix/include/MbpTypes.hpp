/**
 * @file MbpTypes.hpp
 * @brief Market-by-price book level definitions.
 */
#pragma once
#include <cstdint>

namespace aef {
namespace infra {
namespace mbp_book {

/// Maximum bid/ask depth carried in snapshot packets.
const int32_t BCAST_MBP_SIDE_COUNT = 5;

/** @brief Tick-level depth entry used for cash/derivatives feeds. */
struct __attribute__((packed)) st_mbp_info {
    int32_t qty{0};/*!< Qty at one level*/
    int32_t price{0};/*!< Price at one level */
    uint16_t order_count_{0};/*!< Order Count at one level */
    uint64_t timestamp{0};
};


/** @brief Depth entry variant with 64-bit qty for commodity feeds. */
struct __attribute__((packed)) st_mbp_info_cm {
    int64_t qty{0};/*!< Qty at one level*/
    int32_t price{0};/*!< Price at one level */
    uint16_t order_count_{0};/*!< Order Count at one level */
    uint64_t timestamp{0};
};

} // namespace mbp_book

} // namespace infra
} // namespace aef
