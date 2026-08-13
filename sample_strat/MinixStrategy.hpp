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

class RatioLegStrategy;
class ButterflyStrategy;
class ConversionReversalStrategy;
class BoxSpreadStrategy;

/**
 * @brief Strategy entry point loaded by the engine as libSampleAlgo.so.
 */
class MinixStrategy : public AlgoBase {
    friend class RatioLegStrategy;

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
    /** @brief Place or modify an order entry in the supplied map. */
    int  update_order(OrderMap& order_handle_, int32_t token, int32_t price, int32_t qty);
    auto update_order(OrderObjectPtrT& order_, int32_t token_, int32_t price_, int32_t quantity_, client_uid& clientUid_) -> int;
    /** @brief Cancel an active order if present. */
    bool cancel_order(OrderMap& order_handle_, int32_t token);

    bool subscribeProduct(const int32_t product_id, const uint16_t flags);
    bool unSubscribeProduct(const int32_t product_id, const uint16_t flags);

    void sendOrderResponse(const oms_transaction& response_, std::string name_);

  private:
    void applyLegStrategyJson(const std::string& jsonText);
    void handleRatioLegStrategy(const nlohmann::json& root, const std::string& jsonText, size_t numLegs);
    void handleButterflyStrategy(const nlohmann::json& root, const std::string& jsonText);

    // Once per ~1s, echo strategy updates back to the GUI.
    void sendStrategySpreadsToUI();
    // GUI framing: one metadata packet {"packet_count":N,...} then N 1500-byte chunks.
    void sendJsonChunkedToUI(int32_t message_code, const std::string& payload);
    void sendTradeTracerToUI(const TradeTracer& tracer_);

    struct JsonReassembly {
        int         expected = 0;      // packet_count from the metadata header
        int         received = 0;      // data chunks appended so far
        bool        active   = false;  // metadata seen, collecting chunks
        std::string buf;               // concatenated JSON bytes
    };
    std::map<int32_t, JsonReassembly> jsonReassembly_;

    std::map<int32_t, std::pair<aef::infra::ui_cmd::StrategyDatafromui,
                                std::vector<aef::infra::ui_cmd::TokenDatafromui>>>
                                          legStrategies_;
    std::map<uint32_t, RatioLegStrategy*> ratioStrats_;
    std::map<uint32_t, std::string>       strategyJson_;
    long long                             lastSpreadSendMs_ = 0;  // last time BCmp/SCmp were pushed to the GUI

    // Order context shared with the box via update_order/cancel_order.
    execution_strat::PortfolioOrderManager portfolio_mgr_;
    client_uid                             clientUID;
    int                                    requestId        = 0;
    uint64_t                               event_timestamp_ = 0, trigger_timestamp_ = 0;

    uint16_t _flags  = 0;                           // market-data event flags requested per token
    int32_t  _client = 0, _algoid = 0, _omsid = 0;  // ids from config (order routing)
    int64_t  _lastTickTs = 0;                       // monotonic seconds-of-day*1e9 clock (max of OnTick + broadcast paths)
};
