/**
 * @file MinixStrategy.hpp
 * @brief Strategy hub: receives GUI strategy config, owns the live box instances,
 *        and fans market-data / order callbacks to them.
 */
#pragma once

#include "PortfolioOrderManager.hpp"  // execution_strat::PortfolioOrderManager
#include "Utils.hpp"
#include "oms_api.hpp"
#include "order_instance.hpp"  // execution_strat::order_instance, client_uid, OrderMap types

#include <nlohmann/json.hpp>

#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class BoxSpreadStrategy;  // 4-leg box strategy (the only strategy in this library)
class Ratio2LegStrategy;
class Ratio3LegStrategy;
class Ratio4LegStrategy;

/**
 * @brief Strategy entry point loaded by the engine as libSampleAlgo.so.
 *
 * MinixStrategy is a thin hub: it loads client/algo/oms ids + subscriptions from
 * config, parses the GUI's chunked strategy-config JSON into BoxSpreadStrategy
 * instances (one per strategynumber), and forwards OnTick / onBcastData /
 * OnOrderResponse to every live box. Once per second it echoes each box's live
 * BCmp/SCmp/Cost back to the GUI. The box reaches back (friend) for update_order,
 * (un)subscribeProduct and getProductDetails, using MinixStrategy as order context.
 */
class MinixStrategy : public AlgoBase {
    friend class BoxSpreadStrategy;
    friend class Ratio2LegStrategy;
    friend class Ratio3LegStrategy;
    friend class Ratio4LegStrategy;

  public:
    MinixStrategy(AlgoBase::ContextHandle context);
    ~MinixStrategy();

    // ---- AlgoBase callbacks ------------------------------------------------
    void OnTick(const Quote& event) override;                                              // TBT market data
    void onBcastData(const aef::infra::product::product_data& product_details_) override;  // broadcast/snapshot
    void OnOrderResponse(const oms_transaction& order_resp) override;                      // OMS/exchange order responses
    int  doWork() override;                                                                // periodic engine hook
    void onUIRequest(const aef::infra::ui_cmd::UIStruct& ui_req) override;                 // chunked GUI strategy-config JSON

    using OrderMap = std::unordered_map<int32_t, execution_strat::order_instance>;
    /** @brief Place or modify an order entry in the supplied map (box order context). */
    int  update_order(OrderMap& order_handle_, int32_t token, int32_t price, int32_t qty);
    auto update_order(OrderObjectPtrT& order_, int32_t token_, int32_t price_, int32_t quantity_, client_uid& clientUid_) -> int;
    /** @brief Cancel an active order if present. */
    bool cancel_order(OrderMap& order_handle_, int32_t token);

    bool subscribeProduct(const int32_t product_id, const uint16_t flags);
    bool unSubscribeProduct(const int32_t product_id, const uint16_t flags);

    void sendOrderResponse(const oms_transaction& response_, std::string name_);

  private:
    // Parse one GUI strategy-config JSON doc -> create/edit/start/stop/delete a box.
    void applyLegStrategyJson(const std::string& jsonText);
    void handleBoxStrategy(const nlohmann::json& root, const std::string& jsonText);
    void handleRatio2LegStrategy(const nlohmann::json& root, const std::string& jsonText);

    // Once per ~1s, echo each box's original GUI JSON back with live BCmp/SCmp/Cost.
    void sendStrategySpreadsToUI();
    // GUI framing: one metadata packet {"packet_count":N,...} then N 1500-byte chunks.
    void sendJsonChunkedToUI(int32_t message_code, const std::string& payload);

    // Reassembly state for the GUI's chunked JSON UI messages. The connector
    // (sendStrategyConfig) sends a METADATA packet first — {"packet_count":N,...}
    // zero-padded to 1500B — then N raw 1500B UTF-8 slices, all on one message_code.
    struct JsonReassembly {
        int         expected = 0;      // packet_count from the metadata header
        int         received = 0;      // data chunks appended so far
        bool        active   = false;  // metadata seen, collecting chunks
        std::string buf;               // concatenated JSON bytes
    };
    std::map<int32_t, JsonReassembly> jsonReassembly_;

    // Canonical wire structs parsed from the GUI JSON, kept by strategynumber.
    std::map<int32_t, std::pair<aef::infra::ui_cmd::StrategyDatafromui,
                                std::vector<aef::infra::ui_cmd::TokenDatafromui>>>
        legStrategies_;
    // Live box instances + the original GUI JSON to echo back filled.
    std::map<int32_t, BoxSpreadStrategy*> boxStrats_;
    std::map<int32_t, Ratio2LegStrategy*> ratio2Strats_;
    std::map<int32_t, std::string>        strategyJson_;
    long long                             lastSpreadSendMs_ = 0;  // last time BCmp/SCmp were pushed to the GUI

    // Order context shared with the box via update_order/cancel_order.
    execution_strat::PortfolioOrderManager portfolio_mgr_;
    client_uid                             clientUID;
    int                                    requestId        = 0;
    uint64_t                               event_timestamp_ = 0, trigger_timestamp_ = 0;

    uint16_t flags  = 0;                         // market-data event flags requested per token
    int32_t  client = 0, algoid = 0, omsid = 0;  // ids from config (order routing)
    // Authoritative timer clock for the boxes: a clean synthetic ns clock derived in
    // OnTick (market-hours filtered); onBcastData forwards the same cached value so the
    // two feed paths never mix epochs. 0 until the first OnTick (the box ignores <=0).
    int64_t lastTickTs_ = 0;  // monotonic seconds-of-day*1e9 clock (max of OnTick + broadcast paths)
};
