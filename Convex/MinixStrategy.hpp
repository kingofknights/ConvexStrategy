/**
 * @file MinixStrategy.hpp
 * @brief Strategy hub: receives GUI strategy config, owns the live ratio strategies,
 *        and fans market-data / order callbacks to them.
 */
#pragma once

#include "Utils.hpp"
#include "oms_api.hpp"
#include "order_instance.hpp"  // execution_strat::order_instance, client_uid

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <type_traits>

class RatioLegStrategy;

class MinixStrategy final : public AlgoBase {
    friend class RatioLegStrategy;

  public:
    explicit MinixStrategy(AlgoBase::ContextHandle context_);
    ~MinixStrategy() override;

    void OnTick(const Quote& quote_) override;
    void onBcastData(const aef::infra::product::product_data& product_details_) override;
    void OnOrderResponse(const oms_transaction& response_) override;
    auto doWork() -> int override;
    void onUIRequest(const aef::infra::ui_cmd::UIStruct& request_) override;

    // Places or modifies order_ so it rests quantity_ open at price_.
    // Returns the order uid when a request was sent, 0 when nothing was sent.
    auto UpdateOrder(OrderObjectT& order_, int32_t price_, int32_t quantity_, client_uid& clientUid_) -> uint32_t;

    auto subscribeProduct(int32_t product_id_, uint16_t flags_) -> bool;

  private:
    void ApplyLegStrategyJson(int32_t interface_, const std::string& jsonText_);
    void HandleRatioLegStrategy(const nlohmann::json& root_, size_t legCount_, int32_t interface_, bool hasStrikeGap_);

    // Once per ~1s, echo spread and PnL of every strategy back to the GUI.
    void SendStrategySpreadsToUi();
    void SendOrderResponse(const oms_transaction& response_, int32_t interface_);

    // Copies a packed POD straight into one UI packet (UI_BINARY_PROTOCOL_SPEC.md).
    template <typename Payload>
    void SendToUi(UiMessageCode messageCode_, int32_t interface_, const Payload& payload_) {
        static_assert(std::is_trivially_copyable_v<Payload>);
        static_assert(sizeof(Payload) <= sizeof(aef::infra::ui_cmd::UIStruct::message));

        aef::infra::ui_cmd::UIStruct packet{};
        packet.header.message_code   = messageCode_;
        packet.header.interface_id   = interface_;
        packet.header.message_length = sizeof(packet);
        packet.header.component_id   = 1;
        packet.header.timestamp      = 0;
        std::memcpy(packet.message, &payload_, sizeof(payload_));
        sentoUI(packet);
    }

    struct JsonReassembly {
        int         _expected = 0;
        int         _received = 0;
        bool        _active   = false;
        std::string _buffer;
    };
    JsonReassembly _jsonReassembly;

    std::map<uint32_t, std::unique_ptr<RatioLegStrategy>> _strategies;
    std::chrono::steady_clock::time_point                 _lastSpreadSend{};

    // Timestamps of the last tick, stamped on orders for latency tracing.
    uint64_t _eventTimestamp   = 0;
    uint64_t _triggerTimestamp = 0;

    uint16_t _feedFlags = 0;                          // market-data event flags requested per token
    int32_t  _clientId = 0, _algoId = 0, _omsId = 0;  // ids from config (order routing)
    int32_t  _eventCount = 0;
};
