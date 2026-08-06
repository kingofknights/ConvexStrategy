#pragma once
#include "ProductInfo.hpp"
#include "Quote.hpp"
#include "Utils.hpp"
#include "nlohmann/json.hpp"
#include "oms_api.hpp"
#include "order_instance.hpp"
#include "rms_api.hpp"
#include "ui_api.hpp"

#include <fmt/format.h>

#include <cstdio>
#include <string>
#include <vector>

// Algo
#include "AlgoBase.hpp"

class MinixStrategy;

class RatioLegStrategy {
    using OptionTypeT = aef::infra::product::OPTION_TYPE;

    struct TokenInfo {
        int        _token = 0;
        ORDER_SIDE _side  = BUY_SIDE;
        bool       _bid   = false;
    };

  public:
    struct WindRate {
        std::vector<int> _price;
        float            _spread = 0.0f;
    };

    struct ParamLots {
        int   _totalQuantity = 0;
        int   _quantity      = 0;
        float _spread        = 0;
    };

    struct MarketBidding {
        std::vector<OrderObjectPtrT> _order;
        std::vector<uint32_t>        _uniqueID;
        std::vector<int32_t>         _tradedLot;
        std::vector<uint64_t>        _tradeValue;
        std::vector<int32_t>         _cycleTradedLot;
        std::vector<uint64_t>        _cycleTradeValue;
        int32_t                      _lastBiddingFillPrice = 0;
        size_t                       _hedgeRetryCount      = 0;
        WindRate                     _windRate;

        void resize(size_t n) {
            _order.resize(n);
            _uniqueID.assign(n, 0);
            _tradedLot.assign(n, 0);
            _tradeValue.assign(n, 0);
            _cycleTradedLot.assign(n, 0);
            _cycleTradeValue.assign(n, 0);
            _windRate._price.assign(n, 0);
        }
    };

    RatioLegStrategy(MinixStrategy* ms_, uint32_t strategyId_, const nlohmann::json& json_, size_t numLegs_);
    ~RatioLegStrategy();

    template <typename... Args>
    void writeLog(fmt::format_string<Args...> fmt_str, Args&&... args) const {
        fmt::print(fmt_str, std::forward<Args>(args)...);
    }

    void ParamUpdate(const nlohmann::json& json_);

    void OnTick(const Quote& event_, int64_t nowTs_);

    void OnBcast(const aef::infra::product::product_data& pd_, int64_t nowTs_);

    void OnOrderResponse(const oms_transaction& resp_);

    void OrderBiddingLogic(MarketBidding& object_, ParamLots param_, WindRate rate_, int multiplier_, std::string name_);

    void SecondOrderBidding(MarketBidding& object_, ParamLots param_);

    [[nodiscard]] auto GetStrategyID() const -> uint32_t;
    [[nodiscard]] auto GetGap() const -> int;
    [[nodiscard]] auto GetCost() const -> double;
    [[nodiscard]] auto GetBCmp() const -> WindRate;
    [[nodiscard]] auto GetSCmp() const -> WindRate;
    [[nodiscard]] auto GetFLP() const -> int;
    [[nodiscard]] auto GetLongTradedLots() const -> int;
    [[nodiscard]] auto GetShortTradedLots() const -> int;
    [[nodiscard]] auto GetBATP() const -> double;
    [[nodiscard]] auto GetSATP() const -> double;
    [[nodiscard]] auto GetRLP() const -> double;
    [[nodiscard]] auto GetCutPL() const -> double;
    [[nodiscard]] auto GetNetPL() const -> double;
    [[nodiscard]] auto GetM2M() const -> int;

  protected:
    [[nodiscard]] auto GetPrice(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int;
    [[nodiscard]] auto GetQuantity(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int;
    [[nodiscard]] auto GetOrderCount(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int;
    [[nodiscard]] auto GetAvailableQuantity(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> int;

    [[nodiscard]] auto CheckOrderDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool;
    [[nodiscard]] auto CheckPriceDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool;

  private:
    MinixStrategy* _ms;
    client_uid     _uid;

    uint32_t _strategyId;
    size_t   _numLegs;
    bool     _active = false;

    int    _gap        = 0;
    int    _lotSize    = 0;
    int    _tickSize   = 0;
    size_t _biddingLeg = 0;

    std::vector<Quote>      _qoute;
    std::vector<int>        _tokens;
    std::vector<int>        _tokensParam;
    std::vector<int>        _ratios;
    std::vector<int>        _ratiosParam;
    std::vector<bool>       _isOption;
    std::vector<ORDER_SIDE> _longSide;
    std::vector<ORDER_SIDE> _shortSide;
    std::vector<ORDER_SIDE> _longSideParam;
    std::vector<ORDER_SIDE> _shortSideParam;

    MarketBidding _longOrders;
    MarketBidding _shortOrders;

    // ── Params ────────────────────────────────────────────────────────────────
    ParamLots _longParam;
    ParamLots _shortParam;

    size_t _minTickChange      = 0;
    size_t _orderDepth         = 0;
    size_t _priceDepth         = 0;
    size_t _allowedBidDepth    = 0;
    int    _thresholdQty       = 0;
    int    _allowedSlippage    = 0;
    int    _tradeGear          = 0;
    size_t _marketOrderRetries = 0;

    // ── Strategy meta ─────────────────────────────────────────────────────────
    bool _isBidding = false;
};
