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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

// Algo
#include "AlgoBase.hpp"

class MinixStrategy;

class RatioLegStrategy {
  public:
    constexpr static size_t MAX_LEGS = 6;

    // ponytail: fixed-size array avoids heap allocations on tick hot path
    struct WindRate {
        std::array<int, MAX_LEGS> _price{};
        float                     _spread = 0.0f;
        bool                      _valid  = false;  // false when any leg price is missing
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

    void OnTick(const Quote& event_);
    void OnOrderResponse(const oms_transaction& resp_);

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
    [[nodiscard]] auto GetM2M() const -> double;
    [[nodiscard]] auto GetStatus() const -> StrategyStatus;
    void               SetStatus(StrategyStatus status_);
    [[nodiscard]] auto IsActive() const -> bool;

  private:
    struct ParamLots {
        int   _totalQuantity = 0;  // packs
        int   _quantity      = 0;  // packs per slice (SoQ)
        float _spread        = 0;  // paise, net-credit convention
    };

    // Per-pack values precomputed from params so the tick path does no lookups.
    struct LegSideCache {
        std::array<ORDER_SIDE, MAX_LEGS> _oppQuoteSide{};  // side of the book a crossing order takes
        std::array<int, MAX_LEGS>        _signedRatio{};   // BUY -> -ratio, SELL -> +ratio
        std::array<double, MAX_LEGS>     _hedgeTargetDepthQuantity{};
        std::array<int, MAX_LEGS>        _sliceQuantity{};
    };

    // One direction of the strategy (long pack or short pack): leg sides, params, orders and fills.
    struct Pack {
        ORDER_SIDE                   _packSide = BUY_SIDE;  // BUY_SIDE for long pack, SELL_SIDE for short pack
        std::vector<ORDER_SIDE>      _sides;
        ParamLots                    _param;
        LegSideCache                 _cache;
        std::vector<OrderObjectPtrT> _order;
        std::vector<int32_t>         _tradedLot;
        std::vector<uint64_t>        _tradeValue;
        std::vector<int32_t>         _cycleTradedLot;
        std::vector<uint64_t>        _cycleTradeValue;
        std::vector<size_t>          _hedgeRetryCount;
        bool                         _isUnhedged = false;
        double                       _atp        = 0.0;  // traded spread, refreshed only while hedged
        WindRate                     _windRate;          // leg prices when the bidding order was last sent

        void resize(size_t n_) {
            _sides.resize(n_);
            _order.resize(n_);
            _tradedLot.assign(n_, 0);
            _tradeValue.assign(n_, 0);
            _cycleTradedLot.assign(n_, 0);
            _cycleTradeValue.assign(n_, 0);
            _hedgeRetryCount.assign(n_, 0);
        }
    };

    // Net buy/sell position of one leg across both packs.
    struct LegPosition {
        int64_t  _buyQuantity  = 0;
        int64_t  _sellQuantity = 0;
        uint64_t _buyValue     = 0;
        uint64_t _sellValue    = 0;

        [[nodiscard]] auto AvgBuy() const -> double { return _buyQuantity > 0 ? static_cast<double>(_buyValue) / _buyQuantity : 0.0; }
        [[nodiscard]] auto AvgSell() const -> double { return _sellQuantity > 0 ? static_cast<double>(_sellValue) / _sellQuantity : 0.0; }
    };

    static constexpr size_t NO_LEG      = std::numeric_limits<size_t>::max();
    static constexpr size_t _biddingLeg = 0;  // leg 0 always rests the bid; other legs hedge

    [[nodiscard]] static auto GetPrice(const Quote& event_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int;
    [[nodiscard]] static auto GetQuantity(const Quote& event_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int;
    [[nodiscard]] static auto GetOrderCount(const Quote& event_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int;
    [[nodiscard]] static auto GetAvailableQuantity(const Quote& event_, size_t depth_, ORDER_SIDE side_) noexcept -> int;
    [[nodiscard]] static auto CheckOrderDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) noexcept -> bool;
    [[nodiscard]] static auto CheckPriceDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) noexcept -> bool;

    [[nodiscard]] auto FindLegIndex(int token_) const noexcept -> size_t {
        for (size_t instrumentIndex = 0; instrumentIndex < _numLegs; ++instrumentIndex) {
            if (_tokens[instrumentIndex] == token_) {
                return instrumentIndex;
            }
        }
        return NO_LEG;
    }

    void ParseLegs(const nlohmann::json& json_);
    void RebuildCache();

    // ── Entry (bidding leg) ───────────────────────────────────────────────────
    void               ProcessPack(Pack& pack_, bool active_);
    void               EvaluateBidding(Pack& pack_);
    void               OrderBiddingLogic(Pack& pack_, const WindRate& rate_);
    [[nodiscard]] auto CheckHedgeLegsDepth(const Pack& pack_) const -> bool;
    [[nodiscard]] auto CheckBiddingLegDepth(const Pack& pack_) const -> bool;

    // ── Hedge legs ────────────────────────────────────────────────────────────
    void Rehedge(Pack& pack_);
    void HedgePack(Pack& pack_);
    void ExecuteHedgeLeg(Pack& pack_, size_t leg_, int targetHedgeLots_);
    void UpdateUnhedgedStatus(Pack& pack_) noexcept;
    void CheckSlippageThreshold(Pack& pack_, const oms_transaction& resp_);

    // ── Pricing / PnL ─────────────────────────────────────────────────────────
    [[nodiscard]] auto ComputeRate(const LegSideCache& cache_) const -> WindRate;
    [[nodiscard]] auto AdjustGap(double spread_) const noexcept -> double;
    [[nodiscard]] auto CalculateTradedLots(const Pack& pack_) const noexcept -> int;
    [[nodiscard]] auto ComputeATP(const Pack& pack_) const -> double;
    [[nodiscard]] auto PositionOf(size_t leg_) const -> LegPosition;

    MinixStrategy* const _ms;
    client_uid           _uid;

    int            _eventCount = 0;
    int            _printCount = 0;
    const uint32_t _strategyId;
    const int32_t  _interface;
    const size_t   _numLegs;
    const bool     _gapDiff;

    StrategyStatus _status = StrategyStatus_INACTIVE;

    int _gap      = 0;
    int _lotSize  = 0;
    int _tickSize = 0;

    // Leg definition, fixed at creation.
    std::vector<Quote> _qoute;
    std::vector<int>   _tokens;
    std::vector<int>   _ratios;
    std::vector<bool>  _isOption;

    Pack _long;
    Pack _short;

    // ── Params ────────────────────────────────────────────────────────────────
    size_t _minTickChange      = 0;
    size_t _orderDepth         = 0;
    size_t _priceDepth         = 0;
    size_t _allowedBidDepth    = 0;
    int    _thresholdQuantity  = 0;
    int    _allowedSlippage    = 0;
    int    _tradeGear          = 0;
    size_t _marketOrderRetries = 0;

    // ── Cached fast-path lookup ───────────────────────────────────────────────
    int                          _tradeGearPriceOffset = 0;
    int                          _minTickDiffThreshold = 0;
    std::array<double, MAX_LEGS> _buyCostCoeff{};
    std::array<double, MAX_LEGS> _sellCostCoeff{};

    TradeTracer _tracer;
    std::FILE*  _logFile = nullptr;
    std::string _logFileName;
};
