#include "ManualOrder.hpp"

#include "MinixStrategy.hpp"

#include <cstring>
#include <iostream>
#include <iterator>

ManualOrder::ManualOrder(MinixStrategy* hub_)
    : _hub{hub_} {}

void ManualOrder::OnRequest(const aef::infra::ui_cmd::UIStruct& request_) {
    oms_transaction3l request;  // copied out: the packed struct is not aligned inside UIStruct::message
    std::memcpy(&request, request_.message, sizeof(request));
    const int32_t interface = request.uihdr.interface_id;

    client_uid uid{};
    uid.composite_id_.strategy_id = 0;  // manual orders always go out as strategy 0
    // Strategy orders carry client_id = _clientId; any other value keeps manual uids from ever matching theirs.
    uid.composite_id_.client_id  = static_cast<uint32_t>(_hub->_clientId + 1) & 0b111U;
    uid.composite_id_.request_id = request.hdr_.uid_.request_id;

    const auto makeOrder = [&](size_t leg_, int32_t transactionCode_) {
        oms_transaction order{};
        order.hdr_.transaction_code     = transactionCode_;
        order.hdr_.uid_                 = uid;
        order.hdr_.event_timestamp      = _hub->_eventTimestamp;
        order.hdr_.trigger_timestamp    = _hub->_triggerTimestamp;
        order.hdr_.exchange_timestamp   = aef::infra::get_realtime_in_nanos();
        order.packet_.product_id_       = static_cast<int32_t>(request.packet_.product_id_[leg_] & 0xFFFFFFFFU);
        order.packet_.price_            = request.packet_.price_[leg_];
        order.packet_.quantity_         = request.packet_.quantity_[leg_];
        order.packet_.flags_.order_type = request.packet_.flags_[leg_].order_type;
        order.packet_.flags_.order_side = request.packet_.flags_[leg_].order_side;
        order.packet_.client_id         = _hub->_clientId;
        order.packet_.algo_id           = _hub->_algoId;
        order.packet_.oms_id            = _hub->_omsId;
        order.packet_.exchange_order_id = aef::infra::get_realtime_in_nanos();
        return order;
    };
    const auto reject = [&](oms_transaction order_, int32_t errorCode_) {
        order_.hdr_.transaction_code       = OMS_REQ_REJ;
        order_.hdr_.error_code             = errorCode_;
        order_.hdr_.reason_code            = 1;
        order_.packet_.exchange_error_code = errorCode_;
        std::cout << "[ManualOrder] reject uid=" << uid.id_ << " token=" << order_.packet_.product_id_ << " error=" << errorCode_ << std::endl;
        SendResponse(order_, interface);
    };
    const auto send = [&](oms_transaction order_) {
        std::cout << "[ManualOrder] code=" << order_.hdr_.transaction_code << " uid=" << uid.id_ << " request_id=" << uint32_t{uid.composite_id_.request_id}
                  << " token=" << order_.packet_.product_id_ << " side=" << order_.packet_.flags_.order_side << " type=" << order_.packet_.flags_.order_type
                  << " price=" << order_.packet_.price_ << " qty=" << order_.packet_.quantity_ << " iface=" << interface << std::endl;
        if (!_hub->send_order(order_)) {
            std::cout << "[ManualOrder] send_order failed uid=" << uid.id_ << std::endl;
        }
    };

    // request_id is 22 bits in the OMS uid: a larger one would truncate onto another order's uid.
    if (request.hdr_.uid_.request_id > MAX_REQUEST_ID) {
        reject(makeOrder(0, request.hdr_.transaction_code), INVALID_CLIENT_REQ_ID);
        return;
    }

    switch (request.hdr_.transaction_code) {
        case OMS_PLACE_REQ: {
            // Legs fill product_id_ from index 0; every leg shares the request's uid, as the GUI tracks one request id.
            size_t legCount = 0;
            while (legCount < std::size(request.packet_.product_id_) && request.packet_.product_id_[legCount] != 0) {
                ++legCount;
            }
            for (size_t leg = 0; leg < legCount; ++leg) {
                const oms_transaction order = makeOrder(leg, OMS_PLACE_REQ);
                if (order.packet_.quantity_ <= 0 || order.packet_.price_ <= 0) {
                    reject(order, order.packet_.quantity_ <= 0 ? INVALID_QTY : INVALID_PRICE);
                    return;
                }
            }
            if (legCount == 0) {
                reject(makeOrder(0, OMS_PLACE_REQ), INVALID_PRODUCT);
                return;
            }
            _orders[uid.id_] = interface;
            for (size_t leg = 0; leg < legCount; ++leg) {
                send(makeOrder(leg, OMS_PLACE_REQ));
            }
            break;
        }
        case OMS_REPLACE_REQ:
        case OMS_CANCEL_REQ: {
            const oms_transaction order = makeOrder(0, request.hdr_.transaction_code);
            if (_orders.count(uid.id_) == 0) {
                reject(order, INVALID_CLIENT_REQ_ID);  // not a manual order this strategy placed
            } else if (order.hdr_.transaction_code == OMS_REPLACE_REQ && (order.packet_.quantity_ <= 0 || order.packet_.price_ <= 0)) {
                reject(order, order.packet_.quantity_ <= 0 ? INVALID_QTY : INVALID_PRICE);
            } else {
                send(order);
            }
            break;
        }
        default: {
            reject(makeOrder(0, request.hdr_.transaction_code), INVALID_TRANS_ID);
            break;
        }
    }
}

auto ManualOrder::OnOrderResponse(const oms_transaction& response_) -> bool {
    const auto order = _orders.find(response_.hdr_.uid_.id_);
    if (order == _orders.end()) {
        return false;
    }
    SendResponse(response_, order->second);
    return true;
}

void ManualOrder::SendResponse(const oms_transaction& response_, int32_t interface_) {
    oms_transaction3l response{};
    response.uihdr.message_code   = UiMessageCode_MANUAL_ORDER_RESPONSE;
    response.uihdr.component_id   = 1;
    response.uihdr.message_length = sizeof(response);
    response.uihdr.interface_id   = interface_;

    // The GUI shows any error code it receives, so only rejects and cancels carry one.
    const int32_t code    = response_.hdr_.transaction_code;
    const bool    isError = code == OMS_REQ_REJ || code == OMS_ORDER_CANCELLED;

    response.hdr_.transaction_code   = code;
    response.hdr_.error_code         = isError ? response_.hdr_.error_code : 0;
    response.hdr_.reason_code        = isError ? response_.hdr_.reason_code : 0;
    response.hdr_.uid_.client_id     = response_.hdr_.uid_.composite_id_.client_id;
    response.hdr_.uid_.request_id    = response_.hdr_.uid_.composite_id_.request_id;
    response.hdr_.uid_.strategy_id   = static_cast<char>(response_.hdr_.uid_.composite_id_.strategy_id);
    response.hdr_.event_timestamp    = response_.hdr_.event_timestamp;
    response.hdr_.trigger_timestamp  = response_.hdr_.trigger_timestamp;
    response.hdr_.exchange_timestamp = response_.hdr_.exchange_timestamp;

    response.packet_.exchange_error_code    = isError ? response_.packet_.exchange_error_code : 0;
    response.packet_.exchange_order_id      = response_.packet_.exchange_order_id;
    response.packet_.exchange_fill_id       = response_.packet_.exchange_fill_id;
    response.packet_.exchange_response_code = response_.packet_.exchange_response_code;
    response.packet_.client_id              = response_.packet_.client_id;
    response.packet_.product_id_[0]         = static_cast<uint32_t>(response_.packet_.product_id_);
    response.packet_.price_[0]              = response_.packet_.price_;
    response.packet_.quantity_[0]           = response_.packet_.quantity_;
    response.packet_.flags_[0].order_side   = response_.packet_.flags_.order_side;
    response.packet_.flags_[0].order_type   = response_.packet_.flags_.order_type;
    response.packet_.flags_[0].price_type   = response_.packet_.flags_.price_type;

    _hub->SendToUi(UiMessageCode_MANUAL_ORDER_RESPONSE, interface_, response);
}
