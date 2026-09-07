/**
 * @file PortfolioOrderManager.hpp
 * @brief Lightweight tracker for open orders and positions.
 */
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>
#include "../include/oms_api.hpp"

namespace execution_strat {

/** @brief Snapshot of an outstanding order used for UI/analytics. */
struct OrderSummary {
    uint32_t uid{0};
    int32_t token{0};
    ORDER_SIDE side{ORDER_SIDE::BUY_SIDE};
    int32_t open_qty{0};
    int32_t price{0};
};

/** @brief Position details for a token. */
struct PositionSnapshot {
    int32_t token{0};
    int64_t net_qty{0};
    double avg_price{0.0};
    double mark_price{0.0};
    double realized_pnl{0.0};
    double unrealized_pnl{0.0};
};

/**
 * @brief Maintains open orders and computes rolling PnL per token.
 *
 * The manager is intentionally minimal: it listens to OMS responses,
 * updates open order state, and converts fills into position changes
 * so the strategy can surface account state to a UI or logs.
 */
class PortfolioOrderManager {
  public:
    /** @brief Track a freshly placed order. */
    void on_order_placed(uint32_t uid, int32_t token, ORDER_SIDE side, int32_t price, int32_t qty);
    /** @brief Apply a modification confirmation to cached state. */
    void on_order_modify(uint32_t uid, int32_t new_price, int32_t new_qty);
    /** @brief Remove an order when cancellation succeeds. */
    void on_order_cancel(uint32_t uid);

    /** @brief Convert a fill into position and realized PnL updates. */
    void on_trade(int32_t token, ORDER_SIDE side, int32_t qty, int32_t price, bool is_option = false);
    /** @brief Route a raw OMS response to the appropriate handler. */
    void on_order_response(const oms_transaction& resp, bool is_option = false);

    /** @brief Update mark price used for unrealized PnL. */
    void update_mark_price(int32_t token, double mark_price);

    /** @brief Get a list of currently open orders. */
    std::vector<OrderSummary> get_open_orders() const;
    /** @brief Fetch the position snapshot for a token. */
    PositionSnapshot get_position(int32_t token) const;
    /** @brief Retrieve all positions across tokens. */
    std::vector<PositionSnapshot> get_all_positions() const;
    /** @brief Sum realized PnL across all instruments. */
    double get_total_realized_pnl() const;
    /** @brief Sum unrealized PnL across all instruments. */
    double get_total_unrealized_pnl() const;
    /** @brief Total open quantity across all active orders. */
    int64_t get_total_open_qty() const;

  private:
    struct OrderState {
        int32_t token{0};
        ORDER_SIDE side{ORDER_SIDE::BUY_SIDE};
        int32_t open_qty{0};
        int32_t price{0};
    };

    struct PositionState {
        int64_t net_qty{0};
        double avg_price{0.0};
        double mark_price{0.0};
        double realized_pnl{0.0};
    };

    /** @brief Update position state with an executed fill. */
    void apply_fill(PositionState& pos, ORDER_SIDE side, int32_t qty, int32_t price);
    /** @brief Compute unrealized PnL for a position snapshot. */
    double compute_unrealized(const PositionState& pos) const;

    std::unordered_map<uint32_t, OrderState> open_orders_;
    std::unordered_map<int32_t, PositionState> positions_;
};

}  // namespace execution_strat
