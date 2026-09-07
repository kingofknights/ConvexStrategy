/**
 * @file PortfolioOrderManager.cpp
 * @brief Implementation for open-order and position tracking utilities.
 */
#include "PortfolioOrderManager.hpp"
#include <algorithm>
#include <cmath>

namespace execution_strat {

void PortfolioOrderManager::on_order_placed(uint32_t uid, int32_t token, ORDER_SIDE side, int32_t price, int32_t qty)
{
    if (uid == 0 || qty <= 0) {
        return;
    }
    open_orders_[uid] = OrderState{token, side, qty, price};
}

void PortfolioOrderManager::on_order_modify(uint32_t uid, int32_t new_price, int32_t new_qty)
{
    auto it = open_orders_.find(uid);
    if (it == open_orders_.end() || new_qty <= 0) {
        return;
    }
    it->second.price = new_price;
    it->second.open_qty = new_qty;
}

void PortfolioOrderManager::on_order_cancel(uint32_t uid)
{
    open_orders_.erase(uid);
}

void PortfolioOrderManager::apply_fill(PositionState& pos, ORDER_SIDE side, int32_t qty, int32_t price)
{
    if (qty <= 0) {
        return;
    }

    if (side == ORDER_SIDE::BUY_SIDE) {
        if (pos.net_qty < 0) {
            const int64_t cover_qty = std::min<int64_t>(qty, -pos.net_qty);
            pos.realized_pnl += (pos.avg_price - price) * static_cast<double>(cover_qty);
            pos.net_qty += cover_qty;
            qty -= static_cast<int32_t>(cover_qty);
            if (pos.net_qty == 0 && qty == 0) {
                pos.avg_price = 0.0;
            }
        }
        if (qty > 0) {
            const double total_notional = pos.avg_price * static_cast<double>(pos.net_qty) + price * static_cast<double>(qty);
            pos.net_qty += qty;
            pos.avg_price = pos.net_qty != 0 ? total_notional / static_cast<double>(pos.net_qty) : 0.0;
        }
    } else {
        if (pos.net_qty > 0) {
            const int64_t reduce_qty = std::min<int64_t>(qty, pos.net_qty);
            pos.realized_pnl += (price - pos.avg_price) * static_cast<double>(reduce_qty);
            pos.net_qty -= reduce_qty;
            qty -= static_cast<int32_t>(reduce_qty);
            if (pos.net_qty == 0 && qty == 0) {
                pos.avg_price = 0.0;
            }
        }
        if (qty > 0) {
            const double total_notional = pos.avg_price * std::abs(static_cast<double>(pos.net_qty)) + price * static_cast<double>(qty);
            pos.net_qty -= qty;
            pos.avg_price = pos.net_qty != 0 ? total_notional / std::abs(static_cast<double>(pos.net_qty)) : 0.0;
        }
    }
}

void PortfolioOrderManager::on_trade(int32_t token, ORDER_SIDE side, int32_t qty, int32_t price)
{
    auto& pos = positions_[token];
    pos.mark_price = pos.mark_price > 0.0 ? pos.mark_price : static_cast<double>(price);
    apply_fill(pos, side, qty, price);
}

void PortfolioOrderManager::on_order_response(const oms_transaction& resp)
{
    const uint32_t uid = resp.hdr_.uid_.composite_id_.request_id != 0
                             ? resp.hdr_.uid_.composite_id_.request_id
                             : resp.hdr_.uid_.id_;
    const auto token = resp.packet_.product_id_;
    const auto side = static_cast<ORDER_SIDE>(resp.packet_.flags_.order_side);
    const auto qty = resp.packet_.quantity_;
    const auto price = resp.packet_.price_;

    switch (resp.hdr_.transaction_code) {
        case OMS_API_TRANS_CODES::OMS_ORDER_PLACED:
        case OMS_API_TRANS_CODES::OMS_ORDER_CONFIRMED:
            on_order_placed(uid, token, side, price, qty);
            break;
        case OMS_API_TRANS_CODES::OMS_ORDER_MODIFY_CONFIRMED:
        case OMS_API_TRANS_CODES::OMS_ORDER_MODIFY_PLACED:
            on_order_modify(uid, price, qty);
            break;
        case OMS_API_TRANS_CODES::OMS_ORDER_CANCELLED:
            on_order_cancel(uid);
            break;
        case OMS_API_TRANS_CODES::OMS_REQ_REJ:
        case OMS_API_TRANS_CODES::OMS_REQ_REJ2L:
            on_order_cancel(uid);
            break;
        case OMS_API_TRANS_CODES::OMS_TRADE: {
            on_trade(token, side, qty, price);
            auto it = open_orders_.find(uid);
            if (it != open_orders_.end()) {
                it->second.open_qty = std::max<int32_t>(0, it->second.open_qty - qty);
                if (it->second.open_qty == 0) {
                    open_orders_.erase(it);
                }
            }
            break;
        }
        default:
            break;
    }
}

void PortfolioOrderManager::update_mark_price(int32_t token, double mark_price)
{
    if (mark_price <= 0.0) {
        return;
    }
    positions_[token].mark_price = mark_price;
}

double PortfolioOrderManager::compute_unrealized(const PositionState& pos) const
{
    return pos.net_qty * (pos.mark_price - pos.avg_price);
}

std::vector<OrderSummary> PortfolioOrderManager::get_open_orders() const
{
    std::vector<OrderSummary> out;
    out.reserve(open_orders_.size());
    for (const auto& [uid, state] : open_orders_) {
        out.push_back(OrderSummary{uid, state.token, state.side, state.open_qty, state.price});
    }
    return out;
}

PositionSnapshot PortfolioOrderManager::get_position(int32_t token) const
{
    PositionSnapshot snap;
    snap.token = token;
    const auto it = positions_.find(token);
    if (it != positions_.end()) {
        snap.net_qty = it->second.net_qty;
        snap.avg_price = it->second.avg_price;
        snap.mark_price = it->second.mark_price;
        snap.realized_pnl = it->second.realized_pnl;
        snap.unrealized_pnl = compute_unrealized(it->second);
    }
    return snap;
}

std::vector<PositionSnapshot> PortfolioOrderManager::get_all_positions() const
{
    std::vector<PositionSnapshot> snaps;
    snaps.reserve(positions_.size());
    for (const auto& [token, pos] : positions_) {
        snaps.push_back(PositionSnapshot{token, pos.net_qty, pos.avg_price, pos.mark_price, pos.realized_pnl, compute_unrealized(pos)});
    }
    return snaps;
}

double PortfolioOrderManager::get_total_realized_pnl() const
{
    double total = 0.0;
    for (const auto& [_, pos] : positions_) {
        total += pos.realized_pnl;
    }
    return total;
}

double PortfolioOrderManager::get_total_unrealized_pnl() const
{
    double total = 0.0;
    for (const auto& [_, pos] : positions_) {
        total += compute_unrealized(pos);
    }
    return total;
}

int64_t PortfolioOrderManager::get_total_open_qty() const
{
    int64_t total = 0;
    for (const auto& [_, state] : open_orders_) {
        total += state.open_qty;
    }
    return total;
}

}  // namespace execution_strat
