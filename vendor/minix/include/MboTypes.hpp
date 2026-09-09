/**
 * @file MboTypes.hpp
 * @brief Market-by-order primitives and side enums.
 */
#pragma once
#include <cstdint>

namespace aef {
namespace infra {
namespace mbo_book {

/** @brief Level-1 order tick with order id, price, and qty. */
struct __attribute__((packed)) tick_type {
    uint64_t order_id{0};
    int32_t price{-1};
    int32_t qty{-1};

    tick_type() = default;

    tick_type(uint64_t orderId, int32_t ticks, int32_t lots)
      : order_id{orderId}
      , price{ticks}
      , qty{lots}
    {
    }
};

/** @brief Trade record linking the resting and incoming order ids. */
struct __attribute__((packed)) trade {
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    int32_t price;
    int32_t qty;
};

/// Buy/sell indicator used inside MBO ticks.
enum OrderSide : char { BUY = 'B', SELL = 'S' };

} // namespace mbo_book
} // namespace infra
} // namespace aef
