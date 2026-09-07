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

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
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
    constexpr static size_t MAX_LEGS = 6;

    // ponytail: fixed-size array avoids heap allocations on tick hot path
    struct WindRate {
        std::array<int, MAX_LEGS> _price{};
        float                     _spread = 0.0f;
    };

    struct ParamLots {
        int   _totalQuantity = 0;
        int   _quantity      = 0;
        float _spread        = 0;
    };

    struct LegSideCache {
        std::array<ORDER_SIDE, MAX_LEGS> _oppQuoteSide{};
        std::array<int, MAX_LEGS>        _signedRatio{};
        std::array<double, MAX_LEGS>     _hedgeTargetDepthQuantity{};
        std::array<int, MAX_LEGS>        _sliceQuantity{};
        double                           _targetRawSpread = 0.0;
    };

    struct MarketBidding {
        std::vector<OrderObjectPtrT> _order;
        std::vector<uint32_t>        _uniqueID;
        std::vector<int32_t>         _tradedLot;
        std::vector<uint64_t>        _tradeValue;
        std::vector<int32_t>         _cycleTradedLot;
        std::vector<uint64_t>        _cycleTradeValue;
        std::vector<size_t>          _hedgeRetryCount;
        int32_t                      _lastBiddingFillPrice = 0;
        bool                         _isUnhedged           = false;
        WindRate                     _windRate;

        void resize(size_t n_) {
            _order.resize(n_);
            _uniqueID.assign(n_, 0);
            _tradedLot.assign(n_, 0);
            _tradeValue.assign(n_, 0);
            _cycleTradedLot.assign(n_, 0);
            _cycleTradeValue.assign(n_, 0);
            _hedgeRetryCount.assign(n_, 0);
            _isUnhedged           = false;
            _lastBiddingFillPrice = 0;
            _windRate._price.fill(0);
        }
    };

    RatioLegStrategy(MinixStrategy* ms_, uint32_t strategyId_, int32_t interface_, const nlohmann::json& json_, size_t numLegs_, bool gapDiff_);
    ~RatioLegStrategy();

    // ponytail: use fmt::print directly to file and stdout, eliminating std::ofstream and intermediate string allocations
    template <typename... Args>
    void WriteLog(fmt::format_string<Args...> fmt_str_, Args&&... args_) const {
        if (_logFile) {
            fmt::print(_logFile, fmt_str_, std::forward<Args>(args_)...);
        }
    }

    void Print();
    void ParamUpdate(const nlohmann::json& json_);
    void Stop();

    void OnTick(const Quote& event_, int64_t nowTs_);
    void OnBcast(const aef::infra::product::product_data& pd_, int64_t nowTs_);
    void OnOrderResponse(const oms_transaction& resp_);

    void OrderBiddingLogic(MarketBidding& object_, const ParamLots& param_, const LegSideCache& cache_, const WindRate& rate_, int multiplier_, const char* name_);
    void SecondOrderBidding(MarketBidding& object_, const LegSideCache& cache_, const ParamLots& param_, int multiplier_);
    void CheckHedgeTimeout();

    [[nodiscard]] auto CalculateHedgePrice(const MarketBidding& object_, const LegSideCache& cache_, int multiplier_, size_t hedgeLeg_) const -> int;

    [[nodiscard]] auto GetInterface() const -> int32_t;
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
    [[nodiscard]] auto GetStatus() const -> StrategyStatus;
    void               SetStatus(StrategyStatus status_);
    [[nodiscard]] auto IsActive() const -> bool;
    [[nodiscard]] auto IsStopped() const -> bool;

  protected:
    [[nodiscard]] auto GetPrice(const Quote& event_, ORDER_SIDE side_, size_t levelIndex_) const -> int;
    [[nodiscard]] auto GetQuantity(const Quote& event_, ORDER_SIDE side_, size_t levelIndex_) const -> int;
    [[nodiscard]] auto GetOrderCount(const Quote& event_, ORDER_SIDE side_, size_t levelIndex_) const -> int;
    [[nodiscard]] auto GetAvailableQuantity(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> int;

    [[nodiscard]] auto CheckOrderDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool;
    [[nodiscard]] auto CheckPriceDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool;

  private:
    [[nodiscard]] inline auto FindLegIndex(int token_) const noexcept -> int {
        for (size_t instrumentIndex = 0; instrumentIndex < _numLegs; ++instrumentIndex) {
            if (_tokens[instrumentIndex] == token_) return static_cast<int>(instrumentIndex);
        }
        return -1;
    }

    [[nodiscard]] inline auto HasUnhedgedLots(const MarketBidding& object_) const noexcept -> bool {
        return object_._isUnhedged;
    }
    void UpdateUnhedgedStatus(MarketBidding& object_) noexcept;
    void RebuildCache();

    [[nodiscard]] auto CheckHedgeLegsDepth(const MarketBidding& object_, const LegSideCache& cache_) const -> bool;
    [[nodiscard]] auto CheckBiddingLegDepth(const MarketBidding& object_) const -> bool;
    void               EvaluateBidding(MarketBidding& object_, const ParamLots& param_, const LegSideCache& cache_, const WindRate& rate_, int multiplier_, const char* name_);

    [[nodiscard]] auto ComputeRawSpread(const LegSideCache& cache_, std::array<int, MAX_LEGS>& prices_) const -> double;
    [[nodiscard]] auto AdjustGap(double spread_) const noexcept -> double;
    [[nodiscard]] auto CalculateTradedLots(const MarketBidding& object_) const noexcept -> int;

    void CheckSlippageThreshold(MarketBidding& object_, const std::vector<ORDER_SIDE>& sides_, const ParamLots& param_, int sideOfPack_, const oms_transaction& resp_);
    void ExecuteHedgeLeg(MarketBidding& object_, const LegSideCache& cache_, const ParamLots& param_, int multiplier_, size_t leg_, int targetHedgeLots_);

    MinixStrategy* const _ms;
    client_uid           _uid;

    int            _eventCount = 0;
    const uint32_t _strategyId;
    const int32_t  _interface;
    const size_t   _numLegs;
    const bool     _gapDiff;
    std::string    _name;

    StrategyStatus _status = StrategyStatus_INACTIVE;

    int             _gap        = 0;
    int             _lotSize    = 0;
    int             _tickSize   = 0;
    volatile size_t _biddingLeg = 0;

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
    int    _thresholdQuantity  = 0;
    int    _allowedSlippage    = 0;
    int    _tradeGear          = 0;
    size_t _marketOrderRetries = 0;

    // ── Strategy meta ─────────────────────────────────────────────────────────
    bool _isBidding = false;

    // ── Cached fast-path lookup ───────────────────────────────────────────────
    LegSideCache                 _longCache;
    LegSideCache                 _shortCache;
    int                          _tradeGearPriceOffset = 0;
    int                          _minTickDiffThreshold = 0;
    std::array<double, MAX_LEGS> _buyCostCoeff{};
    std::array<double, MAX_LEGS> _sellCostCoeff{};

    TradeTracer _tracer;
    std::FILE*  _logFile = nullptr;
    std::string _logFileName;
};
