#pragma once
#include "Utils.hpp"
#include "nlohmann/json.hpp"

#include <fmt/format.h>
#include <oms_api.hpp>

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
    static constexpr size_t MaxLegs          = 6;
    static constexpr int    PassiveTickCount = 3;
    static constexpr size_t NoLeg            = std::numeric_limits<size_t>::max();

    template <typename T>
    using LegArrayT = std::array<T, MaxLegs>;

    struct Pack {
        LegArrayT<ORDER_SIDE> _sides{};
        LegArrayT<ORDER_SIDE> _oppositeSide{};
        LegArrayT<int>        _signedRatio{};

        LegArrayT<OrderObjectPtrT> _orders;
        LegArrayT<int32_t>         _tradedLots{};
        LegArrayT<uint64_t>        _tradedValue{};
        LegArrayT<int32_t>         _reportedLots{};
        LegArrayT<uint64_t>        _reportedValue{};

        int   _totalAllowedPacks = 0;
        int   _singleOrderPacks  = 0;
        float _targetSpread      = 0;

        double     _tradedSpread = 0.0;
        int        _tradedPack   = 0;
        bool       _hedgeMode    = false;
        ORDER_SIDE _packSide     = BUY_SIDE;
        uint32_t   _earlyHedgeUid = 0;  // leg 0 uid an 'S' tick hedged early; leg 0 is left alone while that order lives

        OrderObjectPtrT _reverse;           // leg 0 on the opposite side: sends back lots filled after the spread went away
        int32_t         _reverseLots  = 0;  // leg 0 lots still to send back; not in _tradedLots
        uint64_t        _reverseValue = 0;  // what those lots traded at, for the unwind P&L

        double   _cushion   = 0.0;  // hedge legs' level 0 to level 2 loss, refreshed every CushionRefreshNs
        uint64_t _cushionNs = 0;
    };

  public:
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
    [[nodiscard]] auto SpreadUpdate(uint64_t nowNs_) -> std::optional<StrategySpreadUpdate>;

    [[nodiscard]] auto GetTokens() const -> std::span<const int> { return {_tokens.data(), _legCount}; }
    [[nodiscard]] auto GetInterface() const -> int32_t { return _interface; }
    [[nodiscard]] auto GetStatus() const -> StrategyStatus { return _status; }
    [[nodiscard]] auto IsActive() const -> bool { return _status == StrategyStatus_APPLIED; }

  private:
    static constexpr size_t   BiddingLeg     = 0;
    static constexpr uint64_t SpreadResendNs   = 60'000'000'000;
    static constexpr uint64_t CushionRefreshNs = 1'000'000'000;

    void EvaluateBidding(Pack& pack_);
    void EvaluateHedgeMode(Pack& pack_);
    void TryEarlyHedge(Pack& pack_, int updateSide_);
    void HedgeEarly(Pack& pack_);
    void SendHedges(Pack& pack_, const LegArrayT<int>& missingLots_, std::string_view reason_);
    void Reverse(Pack& pack_);

    void OnTrade(Pack& pack_, size_t leg_, const oms_transaction& response_);
    void OnReverseTrade(Pack& pack_, const oms_transaction& response_);
    void ReportPacks(Pack& pack_, const oms_transaction& response_);
    void SendTracer(const Pack& pack_, const oms_transaction& response_, float price_, float ltp_, int ltq_, float slippage_, std::string_view message_);
    void Pull(Pack& pack_, size_t leg_, std::string_view reason_);
    void UpdateOrder(Pack& pack_, size_t leg_, int price_, int quantity_, std::string_view reason_);
    void LogResponse(const Pack& pack_, size_t leg_, const OrderObjectT& order_, const oms_transaction& response_, bool isCut_);

    [[nodiscard]] auto MarketSpread(const Pack& pack_, LegArrayT<int>* prices_ = nullptr, size_t index_ = 0) const -> std::optional<float>;
    [[nodiscard]] auto SpreadPresent(const Pack& pack_, int price_) const -> bool;
    [[nodiscard]] auto AdjustGap(double spread_) const -> double;
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

    int _strikeGap     = 0;
    int _lotSize       = 0;
    int _tickSize      = 0;
    int _minChangeTick = 2;  // GUI TickSize (min 2): a resting bid is repriced only when the new price is this many ticks away, or a fill would miss the target
    int _eventCount    = 0;

    LegArrayT<Quote> _quote{};
    LegArrayT<int>   _tokens{};
    LegArrayT<int>   _ratios{};
    LegArrayT<bool>  _isOption{};

    LegArrayT<aef::infra::product::product_data> _product_data;

    Pack _longPack;
    Pack _shortPack;

    int    _orderDepth        = 0;
    size_t _priceDepth        = 0;
    int    _hedgeDepthPercent = 0;
    int    _allowedSlippage   = 0;
    size_t _allowedBidDepth   = 0;

    double   _unwindPnL      = 0.0;
    uint64_t _depthCheckedNs = 0;

    StrategySpreadUpdate _spread{};
    StrategySpreadUpdate _lastSent{};
    uint64_t             _lastSentNs = 0;

    TradeTracer _tradeTracer{};
    LogFilePtrT _logFile;
};
