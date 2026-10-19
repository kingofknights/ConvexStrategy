#pragma once
#include "Utils.hpp"
#include "nlohmann/json.hpp"

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

class MinixStrategy;

class RatioLegStrategy {
  public:
    static constexpr size_t MAX_LEGS = 6;

    RatioLegStrategy(MinixStrategy* hub_, uint32_t strategyId_, int32_t interface_, const nlohmann::json& json_, size_t legCount_, bool hasStrikeGap_);
    ~RatioLegStrategy();

    RatioLegStrategy(const RatioLegStrategy&)            = delete;
    RatioLegStrategy& operator=(const RatioLegStrategy&) = delete;

    template <typename... Args>
    void WriteLog(fmt::format_string<Args...> format_, Args&&... args_) const {
        if (_logFile) {
            WriteTimestamp();
            fmt::print(_logFile.get(), format_, std::forward<Args>(args_)...);
        }
    }

    void FlushLog() const {
        if (_logFile) std::fflush(_logFile.get());
    }

    void ParamUpdate(const nlohmann::json& json_);
    void SetStatus(StrategyStatus status_);
    void Stop(std::string_view reason_);
    void Forget(std::string_view reason_);
    void MarkDeleted() { _deleted = true; }

    [[nodiscard]] auto IsDeleted() const -> bool { return _deleted; }
    [[nodiscard]] auto IsIdle() const -> bool;

    void               OnTick(const Quote& quote_);
    void               OnOrderResponse(const oms_transaction& response_);
    void               DoWork(uint64_t nowNs_);
    [[nodiscard]] auto SpreadUpdate(uint64_t nowNs_) -> std::optional<StrategySpreadUpdate>;

    [[nodiscard]] auto GetTokens() const -> std::span<const int> { return {_tokens.data(), _legCount}; }
    [[nodiscard]] auto GetInterface() const -> int32_t { return _interface; }
    [[nodiscard]] auto GetStatus() const -> StrategyStatus { return _status; }
    [[nodiscard]] auto IsActive() const -> bool { return _status == StrategyStatus_APPLIED; }

  private:
    template <typename T>
    using LegArray = std::array<T, MAX_LEGS>;

    static constexpr size_t NO_LEG = std::numeric_limits<size_t>::max();

    enum class BidGate : uint8_t { UNKNOWN, BID, DONE, HEDGE_DEPTH, NO_PRICE, UNREACHABLE, BID_DEPTH, TOO_DEEP };

    struct Pack {
        ORDER_SIDE                _packSide     = BUY_SIDE;
        LegArray<ORDER_SIDE>      _sides{};
        int                       _totalPacks   = 0;
        int                       _slicePacks   = 0;
        float                     _targetSpread = 0;
        LegArray<OrderObjectPtrT> _orders;
        LegArray<OrderObjectPtrT> _cutOrders;
        LegArray<int32_t>         _tradedLots{};
        LegArray<uint64_t>        _tradedValue{};
        LegArray<int32_t>         _unreportedLots{};
        LegArray<uint64_t>        _unreportedValue{};
        double                    _tradedSpread      = 0.0;
        uint64_t                  _incompleteSinceNs = 0;
        size_t                    _thinHedgeLeg      = NO_LEG;
        BidGate                   _bidGate           = BidGate::UNKNOWN;
        LegArray<bool>            _hedgeWaiting{};
        std::string_view          _cutReason;
    };

    static constexpr size_t   BIDDING_LEG               = 0;
    static constexpr size_t   HEDGE_LEVEL               = 2;
    static constexpr size_t   CHASE_LEVEL               = 1;
    static constexpr size_t   BID_LEVEL_LIMIT           = 2;
    static constexpr int      DEFAULT_REVERT_BID_MS     = 1000;
    static constexpr int      DEFAULT_PASSIVE_BID_TICKS = 5;
    static constexpr uint64_t THROTTLE_PAUSE_NS         = 1'000'000'000;
    static constexpr uint64_t THROTTLE_RETRY_NS         = 5'000'000;
    static constexpr uint64_t DEPTH_CHECK_NS            = 60'000'000'000;
    static constexpr uint64_t SPREAD_RESEND_NS          = 60'000'000'000;

    void Evaluate(Pack& pack_);
    void Bid(Pack& pack_);
    auto Complete(Pack& pack_) -> bool;
    void Cut(Pack& pack_);
    void OnTrade(Pack& pack_, size_t leg_, const oms_transaction& response_);
    void OnCutTrade(Pack& pack_, size_t leg_, const oms_transaction& response_);
    void ReportPacks(Pack& pack_, const oms_transaction& response_);
    void SendTracer(const Pack& pack_, const oms_transaction& response_, float price_, float ltp_, int ltq_, float slippage_, std::string_view message_);
    void CheckHedgeDepth(uint64_t nowNs_);
    void Send(Pack& pack_, size_t leg_, OrderObjectT& order_, double price_, int lots_, std::string_view reason_);
    void Pull(Pack& pack_, size_t leg_, std::string_view reason_);
    void LogGate(Pack& pack_, BidGate gate_, float market_, bool valid_, double price_);
    void LogResponse(const Pack& pack_, size_t leg_, const OrderObjectT& order_, const oms_transaction& response_, bool isCut_);

    [[nodiscard]] auto MarketSpread(const Pack& pack_, LegArray<int>* prices_ = nullptr) const -> std::optional<float>;
    [[nodiscard]] auto AdjustGap(double spread_) const -> double;
    [[nodiscard]] auto CompletedPacks(const Pack& pack_) const -> int;
    [[nodiscard]] auto IsWhole(const Pack& pack_) const -> bool;
    [[nodiscard]] auto HedgeTarget(const Pack& pack_) const -> int;
    [[nodiscard]] auto IsUnhedged(const Pack& pack_) const -> bool;
    [[nodiscard]] auto ThinHedgeLeg(const Pack& pack_) const -> size_t;
    [[nodiscard]] auto RequiredHedgeQuantity(const Pack& pack_, size_t leg_) const -> double;
    [[nodiscard]] auto SignedRatio(const Pack& pack_, size_t leg_) const -> int;
    [[nodiscard]] auto FindLegIndex(int token_) const -> size_t;
    [[nodiscard]] auto PackName(const Pack& pack_) const -> std::string_view { return &pack_ == &_longPack ? "LONG" : "SHORT"; }
    [[nodiscard]] auto BookText() const -> std::string;
    [[nodiscard]] auto LotsText(const Pack& pack_) const -> std::string;
    void               WriteTimestamp() const;

    [[nodiscard]] static auto Price(const Quote& quote_, ORDER_SIDE side_, size_t level_) -> int;
    [[nodiscard]] static auto HasLevels(const Quote& quote_, ORDER_SIDE side_, size_t depth_) -> bool;
    [[nodiscard]] static auto QuantityUpTo(const Quote& quote_, ORDER_SIDE side_, size_t depth_) -> int;
    [[nodiscard]] static auto OrderCount(const Quote& quote_, ORDER_SIDE side_) -> int;
    [[nodiscard]] static auto DescribeDepth(const Quote& quote_, ORDER_SIDE side_, int minOrders_, size_t depth_, double minQuantity_) -> std::string;

    MinixStrategy* const _hub;
    client_uid           _clientUid{};

    const uint32_t _strategyId;
    const int32_t  _interface;
    const size_t   _legCount;
    const bool     _hasStrikeGap;

    StrategyStatus _status           = StrategyStatus_INACTIVE;
    long           _utcOffsetSeconds = UtcOffsetSeconds();
    bool           _deleted          = false;

    int _strikeGap  = 0;
    int _lotSize    = 0;
    int _tickSize   = 0;
    int _eventCount = 0;

    LegArray<Quote> _quote{};
    LegArray<int>   _tokens{};
    LegArray<int>   _ratios{};
    LegArray<bool>  _isOption{};

    Pack _longPack;
    Pack _shortPack;

    int    _orderDepth        = 0;
    size_t _priceDepth        = 0;
    int    _hedgeDepthPercent = 0;
    int    _allowedSlippage   = 0;
    size_t _allowedBidDepth   = 0;
    int    _revertBidMs       = DEFAULT_REVERT_BID_MS;
    int    _passiveBidTicks   = DEFAULT_PASSIVE_BID_TICKS;

    double   _unwindPnL      = 0.0;
    uint64_t _depthCheckedNs = 0;

    StrategySpreadUpdate _spread{};
    StrategySpreadUpdate _lastSent{};
    uint64_t             _lastSentNs = 0;

    static inline uint64_t _bidPausedUntilNs  = 0;
    static inline uint64_t _throttleRetryAtNs = 0;

    TradeTracer _tradeTracer{};
    LogFilePtrT _logFile;
};
