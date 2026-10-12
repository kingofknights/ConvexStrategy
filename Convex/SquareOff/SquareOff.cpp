#include "SquareOff.hpp"

#include "MinixStrategy.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

    // Deployments write some config values as numbers and others as quoted strings; accept both.
    auto ConfigText(const nlohmann::json& config_, const char* key_) -> std::string {
        const auto value = config_.find(key_);
        if (value == config_.end()) return {};
        return value->is_string() ? value->get<std::string>() : value->dump();
    }

}  // namespace

SquareOff::SquareOff(MinixStrategy* hub_, const nlohmann::json& config_)
    : _hub{hub_} {
    _db._host     = ConfigText(config_, "host");
    _db._port     = ConfigText(config_, "port");
    _db._dbname   = ConfigText(config_, "dbname");
    _db._user     = ConfigText(config_, "user");
    _db._password = ConfigText(config_, "password");
    _db._table    = ConfigText(config_, "table_name");
    const std::string traderId = ConfigText(config_, "trader_id");
    _db._traderId              = traderId.empty() ? 0 : std::stoi(traderId);

    _clientUid.composite_id_.strategy_id = 0;
    // Strategy orders carry client_id = _clientId and manual orders _clientId + 1; + 2 keeps square-off uids apart from both.
    _clientUid.composite_id_.client_id = static_cast<uint32_t>(_hub->_clientId + 2) & 0b111U;
}

auto SquareOff::Start(int32_t interface_) -> bool {
    if (!_positions.empty()) {
        std::cout << "[SquareOff] already running for " << _positions.size() << " tokens -- ignored" << std::endl;
        return false;
    }

    _interface = interface_;
    std::map<int32_t, int32_t> netQuantities;
    try {
        // ponytail: blocks the strategy thread for the DB round trip, as the example does; move off-thread if it stalls ticks.
        netQuantities = LoadTodayPositions(_db);
    } catch (const std::exception& e) {
        std::cout << "[SquareOff] cannot load positions for trader " << _db._traderId << ": " << e.what() << std::endl;
        return false;
    }
    if (netQuantities.empty()) {
        std::cout << "[SquareOff] no open positions for trader " << _db._traderId << std::endl;
        return false;
    }

    for (const auto& [token, netQuantity] : netQuantities) {
        const ORDER_SIDE side = netQuantity > 0 ? SELL_SIDE : BUY_SIDE;
        _hub->subscribeProduct(token, _hub->_feedFlags);
        _positions[token] = Position{netQuantity, std::make_unique<OrderObjectT>(token, side, std::abs(netQuantity), _hub->_clientId, _hub->_algoId, _hub->_omsId,
                                                                                 ORDER_TYPE::LIMIT_ORDER_TYPE, _hub)};
        std::cout << "[SquareOff] token=" << token << " net=" << netQuantity << " -> " << (side == SELL_SIDE ? "SELL" : "BUY") << " " << std::abs(netQuantity) << std::endl;
    }
    return true;
}

void SquareOff::OnBcast(const aef::infra::product::product_data& product_) {
    const auto position = _positions.find(product_.product_id_);
    if (position == _positions.end()) {
        return;
    }
    // Third level, as the example does: deep enough to cross the touch, so the order trades on arrival.
    const int32_t netQuantity = position->second._netQuantity;
    const int32_t price       = netQuantity > 0 ? product_.bid_mbp[2].price : product_.ask_mbp[2].price;
    if (price > 0) {
        _hub->UpdateOrder(*position->second._order, price, std::abs(netQuantity), _clientUid);
    }
}

auto SquareOff::OnOrderResponse(const oms_transaction& response_) -> bool {
    const auto& uid = response_.hdr_.uid_.composite_id_;
    if (uid.strategy_id != _clientUid.composite_id_.strategy_id || uid.client_id != _clientUid.composite_id_.client_id) {
        return false;
    }
    _hub->_manualOrder.SendResponse(response_, _interface);

    const int32_t token    = response_.packet_.product_id_;
    const auto    position = _positions.find(token);
    if (position == _positions.end()) {
        return true;  // late response for a token already flat or dropped
    }
    position->second._order->handle_confirmation(response_);

    const int32_t code = response_.hdr_.transaction_code;
    if (code == OMS_TRADE) {
        const int32_t quantity = response_.packet_.quantity_;
        position->second._netQuantity += response_.packet_.flags_.order_side == BUY_SIDE ? quantity : -quantity;
        std::cout << "[SquareOff] token=" << token << " traded " << quantity << " @" << response_.packet_.price_ << " net now " << position->second._netQuantity << std::endl;
        if (position->second._netQuantity == 0) {
            _positions.erase(position);
        }
    } else if (code == OMS_REQ_REJ) {
        // Dropped, not retried: re-placing on every broadcast would repeat the reject. Press square-off again to retry.
        std::cout << "[SquareOff] token=" << token << " rejected error=" << response_.hdr_.error_code << " reason=" << response_.hdr_.reason_code
                  << " exchange=" << response_.packet_.exchange_error_code << " -- dropped, net " << position->second._netQuantity << " still open" << std::endl;
        _positions.erase(position);
    }
    if (_positions.empty()) {
        std::cout << "[SquareOff] done" << std::endl;
    }
    return true;
}
