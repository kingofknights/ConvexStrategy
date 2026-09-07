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

#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class RatioLegStrategy;

class MinixStrategy final : public AlgoBase {
    friend class RatioLegStrategy;

  public:
    explicit MinixStrategy(AlgoBase::ContextHandle context_);
    ~MinixStrategy() override;

    void OnTick(const Quote& event_) override;
    void onBcastData(const aef::infra::product::product_data& product_details_) override;
    void OnOrderResponse(const oms_transaction& order_resp_) override;
    auto doWork() -> int override;
    void onUIRequest(const aef::infra::ui_cmd::UIStruct& ui_req_) override;

    auto UpdateOrder(OrderObjectPtrT& order_, int32_t token_, int32_t price_, int32_t quantity_, client_uid& clientUid_) -> uint32_t;

    auto subscribeProduct(int32_t product_id_, uint16_t flags_) -> bool;
    auto unSubscribeProduct(int32_t product_id_, uint16_t flags_) -> bool;

    void Registerfortermination(uint32_t strategyId_);

  private:
    void SendOrderResponse(const oms_transaction& response_, int32_t interface_);
    void ApplyLegStrategyJson(int32_t interface_, const std::string& jsonText_);
    void HandleRatioLegStrategy(const nlohmann::json& root_, const std::string& jsonText_, size_t numLegs_, int32_t interface_, bool gapDiff_);

    // Once per ~1s, echo strategy updates back to the GUI.
    void SendStrategySpreadsToUi();
    // ponytail: direct binary POD sending to UI matching trade tracer pattern
    void SendStrategySpreadToUi(const StrategySpreadUpdate& update_, int32_t interface_);
    void SendStrategyStatusToUi(uint32_t strategyId_, StrategyStatus status_, int32_t interface_);
    void SendTradeTracerToUi(const TradeTracer& tracer_, int32_t interface_);

    struct JsonReassembly {
        int         _expected = 0;
        int         _received = 0;
        bool        _active   = false;
        std::string _buf;
    };
    std::map<int32_t, JsonReassembly> _jsonReassembly;

    std::map<uint32_t, RatioLegStrategy*> _ratioStrats;
    std::map<uint32_t, std::string>       _strategyJson;
    long long                             _lastSpreadSendMs = 0;  // last time BCmp/SCmp were pushed to the GUI

    execution_strat::PortfolioOrderManager _portfolio_mgr;
    uint64_t                               _event_timestamp = 0, _trigger_timestamp = 0;

    uint16_t              _flags  = 0;                           // market-data event flags requested per token
    int32_t               _client = 0, _algoid = 0, _omsid = 0;  // ids from config (order routing)
    int64_t               _lastTickTs = 0;                       // monotonic seconds-of-day*1e9 clock (max of OnTick + broadcast paths)
    std::vector<uint32_t> _strategiesToTerminate;
};
