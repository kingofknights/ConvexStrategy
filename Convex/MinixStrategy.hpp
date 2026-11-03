/**
 * @file MinixStrategy.hpp
 * @brief Strategy hub: receives GUI strategy config, owns the live ratio strategies,
 *        and fans market-data / order callbacks to them.
 */
#pragma once

#include "ManualOrder.hpp"
#include "SquareOff.hpp"
#include "Utils.hpp"
#include "oms_api.hpp"
#include "order_instance.hpp"  // execution_strat::order_instance, client_uid

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

class RatioLegStrategy;

class MinixStrategy final : public AlgoBase {
    friend class RatioLegStrategy;
    friend class ManualOrder;
    friend class SquareOff;

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

    // Sweep score of a TBT stream: messages the exchange sent on it in the last 1 ms, from feed_status of the
    // sweep feed publisher. 0 once the stream has not ticked within 1 ms of the newest tick (it went quiet).
    [[nodiscard]] auto StreamSweep(int16_t streamId_) const -> int32_t;

    // Exchange round trip on the market clock (newest tick's exchange time), shared by every strategy: a request
    // is stamped when sent and measured when the exchange confirms it. ExchangeLagNs is the worst of the last
    // measured round trip and the age of any request still unanswered, so a stall shows before its confirm lands.
    void               NoteRequest(uint32_t uid_);
    [[nodiscard]] auto ExchangeLagNs() const -> uint64_t;
    [[nodiscard]] auto LatestExchangeNs() const -> uint64_t { return _latestExchangeNs; }

    // OMS throttle (-9996, nse_line_rate msgs/s on the line, shared by every strategy): after a throttle reject
    // nothing is sent for ThrottleBackoffNs. Resending on every tick kept the line throttled, so cancels never got out.
    [[nodiscard]] auto Throttled() const -> bool { return aef::infra::get_time_in_nanos() < _throttledUntilNs; }

  private:
    void NoteResponse(const oms_transaction& response_);
    void ApplyLegStrategyJson(int32_t interface_, const std::string& jsonText_);
    void HandleRatioLegStrategy(const nlohmann::json& root_, size_t legCount_, int32_t interface_, bool hasStrikeGap_);
    void RebuildTickRoutes();

    // Once per ~1s, echo spread and PnL of every strategy back to the GUI.
    void SendStrategySpreadsToUi(uint64_t nowNs_);
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
    std::unordered_map<int32_t, std::vector<RatioLegStrategy*>> _tickRoutes;  // token -> strategies with a leg on it
    uint64_t                                              _lastSpreadSendNs = 0;  // monotonic ns
    ManualOrder                                           _manualOrder{this};
    std::unique_ptr<SquareOff>                            _squareOff;  // built once the config is read

    // Timestamps of the last tick, stamped on orders for latency tracing.
    uint64_t _eventTimestamp   = 0;
    uint64_t _triggerTimestamp = 0;

    // Last sweep score seen per stream and the exchange time of that tick.
    struct StreamLoad {
        int32_t  _sweep      = 0;
        uint64_t _exchangeNs = 0;
    };
    static constexpr size_t   MaxStreams    = 64;
    static constexpr uint64_t SweepWindowNs = 1'000'000;
    std::array<StreamLoad, MaxStreams> _streamLoad{};
    uint64_t                           _latestExchangeNs = 0;

    // Requests awaiting the exchange. A request older than LagMemoryNs is taken as lost, not slow, and a
    // measured round trip counts for LagMemoryNs after it lands.
    struct PendingRequest {
        uint32_t _uid    = 0;
        uint64_t _sentNs = 0;
    };
    static constexpr size_t   MaxPending  = 64;
    static constexpr uint64_t LagMemoryNs = 1'000'000'000;
    std::array<PendingRequest, MaxPending> _pending{};
    uint64_t                               _lastLagNs   = 0;
    uint64_t                               _lastLagAtNs = 0;

    // The exchange throttle counts messages per second: a full second clears it. Cancels it held back are
    // retried from doWork at most once per CancelRetryNs, ticks or not.
    static constexpr uint64_t ThrottleBackoffNs  = 1'000'000'000;
    static constexpr uint64_t CancelRetryNs      = 1'000'000'000;
    uint64_t                  _throttledUntilNs  = 0;  // monotonic ns
    uint64_t                  _lastCancelRetryNs = 0;  // monotonic ns

    uint16_t _feedFlags = 0;                          // market-data event flags requested per token
    int32_t  _clientId = 0, _algoId = 0, _omsId = 0;  // ids from config (order routing)
};
