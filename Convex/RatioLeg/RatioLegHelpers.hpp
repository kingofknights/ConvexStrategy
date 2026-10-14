#pragma once
// Small helpers shared by the RatioLegStrategy source files.
#include "Utils.hpp"

#include <string_view>

namespace ratioleg {

    constexpr auto Opposite(ORDER_SIDE side_) noexcept -> ORDER_SIDE { return side_ == BUY_SIDE ? SELL_SIDE : BUY_SIDE; }

    constexpr auto SideName(ORDER_SIDE side_) noexcept -> std::string_view { return side_ == BUY_SIDE ? "BUY" : "SELL"; }
    constexpr auto BookSideName(ORDER_SIDE side_) noexcept -> std::string_view { return side_ == BUY_SIDE ? "bid" : "ask"; }

    inline auto TransactionName(int32_t code_) noexcept -> std::string_view {
        switch (code_) {
            case OMS_ORDER_PLACED:
                return "PLACED";
            case OMS_ORDER_CONFIRMED:
                return "CONFIRMED";
            case OMS_ORDER_MODIFY_PLACED:
                return "MODIFY_PLACED";
            case OMS_ORDER_MODIFY_CONFIRMED:
                return "MODIFIED";
            case OMS_ORDER_CANCEL_ACCEPTED:
                return "CANCEL_ACCEPTED";
            case OMS_ORDER_CANCELLED:
                return "CANCELLED";
            case OMS_TRADE:
                return "TRADE";
            case OMS_REQ_REJ:
                return "REJECT";
            default:
                return "OTHER";
        }
    }

    inline constexpr auto IDLE = static_cast<uint32_t>(execution_strat::STRAT_ORDER_STATE::STRAT_INITIAL_STATE);

    // What UpdateOrder will send for this order: a new order or a modify.
    inline auto OrderAction(const OrderObjectT& order_) noexcept -> std::string_view { return order_.get_current_state() == IDLE ? "PLACE" : "MODIFY"; }

}  // namespace ratioleg
