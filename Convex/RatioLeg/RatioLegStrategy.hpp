#pragma once
#include "Utils.hpp"  // pulls order_instance.hpp, which includes the platform headers in the order AlgoBase.hpp needs
#include "nlohmann/json.hpp"

#include <fmt/format.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>

class MinixStrategy;

// One N-leg ratio spread (2..6 legs). Leg 0 bids with one LIMIT order at the price that makes the spread
// exactly the target with the other legs at their crossing touch: it crosses when the market beats the
// target and rests when it misses. When a whole pack fills, the other legs are hedged at once.
// A pack is never left naked: while any leg holds lots beyond the completed packs, the strategy alternates every
// TimeToRevertBidMs between completing the pack (hedge, or bid the rest of leg 0) and reverse-cutting the extra
// lots of every leg with IOCs, until the pack is whole again. This runs in any status.
// The long and short directions run as independent packs.
class RatioLegStrategy {
  public:
    static constexpr size_t MAX_LEGS = 6;

    // Leg prices on the crossing side of the book and the net spread they give.
    struct SpreadQuote {
        std::array<int, MAX_LEGS> _price{};
        float                     _spread = 0.0F;
        bool                      _valid  = false;  // false when any leg price is missing
    };

    // Throws std::runtime_error when the legs cannot be resolved; nothing is subscribed in that case.
    RatioLegStrategy(MinixStrategy* hub_, uint32_t strategyId_, int32_t interface_, const nlohmann::json& json_, size_t legCount_, bool hasStrikeGap_);
    ~RatioLegStrategy();

    RatioLegStrategy(const RatioLegStrategy&)            = delete;
    RatioLegStrategy& operator=(const RatioLegStrategy&) = delete;

    // Every line starts with local wall time "HH:MM:SS.uuuuuu".
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
    void Forget(std::string_view reason_);  // after a square-off flattened the account: stop and drop every position
    void MarkDeleted() { _deleted = true; }  // erased by the hub once IsIdle(); a later SetStatus revives it

    [[nodiscard]] auto IsDeleted() const -> bool { return _deleted; }
    [[nodiscard]] auto IsIdle() const -> bool;  // every pack whole and no order working or in flight

    void OnTick(const Quote& quote_);
    void OnOrderResponse(const oms_transaction& response_);

    [[nodiscard]] auto GetTokens() const -> std::span<const int> { return {_tokens.data(), _legCount}; }
    [[nodiscard]] auto GetInterface() const -> int32_t;
    [[nodiscard]] auto GetStrategyID() const -> uint32_t;
    [[nodiscard]] auto GetGap() const -> int;
    [[nodiscard]] auto GetCost() const -> double;
    [[nodiscard]] auto GetBCmp() const -> SpreadQuote;
    [[nodiscard]] auto GetSCmp() const -> SpreadQuote;
    [[nodiscard]] auto GetFLP() const -> int;
    [[nodiscard]] auto GetLongTradedPacks() const -> int;
    [[nodiscard]] auto GetShortTradedPacks() const -> int;
    [[nodiscard]] auto GetBATP() const -> double;
    [[nodiscard]] auto GetSATP() const -> double;
    [[nodiscard]] auto GetRLP() const -> double;
    [[nodiscard]] auto GetCutPL() const -> double;
    [[nodiscard]] auto GetNetPL() const -> double;
    [[nodiscard]] auto GetM2M() const -> double;
    [[nodiscard]] auto GetStatus() const -> StrategyStatus;
    [[nodiscard]] auto IsActive() const -> bool;
    [[nodiscard]] auto GetEventCount() -> int;

  private:
    template <typename T>
    using LegArray = std::array<T, MAX_LEGS>;

    // Why the bidding leg is (not) quoting. Logged only when it changes, so a silent strategy explains itself.
    // BID: leg 0 order works at the target price. UNREACHABLE: the target needs a leg 0 price <= 0.
    // BID_DEPTH: leg 0's own side (where the bid rests) has fewer than AllowedBidDepth price levels.
    enum class BidGate : uint8_t { UNKNOWN,
                                   BID,
                                   DONE,
                                   HEDGE_DEPTH,
                                   NO_PRICE,
                                   UNREACHABLE,
                                   BID_DEPTH };

    struct PackParams {
        int   _totalPacks   = 0;  // packs to trade in total
        int   _slicePacks   = 0;  // packs per order slice (SoQ)
        float _targetSpread = 0;  // paise, net-credit convention
    };

    // Per-leg values precomputed from params so the tick path does no lookups.
    struct LegCache {
        LegArray<ORDER_SIDE> _takeSide{};            // side of the book a crossing order takes
        LegArray<int>        _signedRatio{};         // BUY -> -ratio, SELL -> +ratio
        LegArray<double>     _requiredHedgeDepth{};  // book quantity a hedge needs before we bid
        LegArray<int>        _sliceQuantity{};       // order quantity of one slice
    };

    // One direction of the strategy (long pack or short pack): leg sides, params, orders and fills.
    struct Pack {
        ORDER_SIDE                _packSide = BUY_SIDE;  // BUY_SIDE for long pack, SELL_SIDE for short pack
        LegArray<ORDER_SIDE>      _sides{};
        PackParams                _params;
        LegCache                  _cache;
        LegArray<OrderObjectPtrT> _orders;
        LegArray<int32_t>         _tradedLots{};
        LegArray<uint64_t>        _tradedValue{};
        LegArray<int32_t>         _unreportedLots{};  // fills not yet reported as whole packs to the tracer
        LegArray<uint64_t>        _unreportedValue{};
        bool                      _isUnhedged = false;
        BidGate                   _bidGate    = BidGate::UNKNOWN;
        LegArray<bool>            _hedgeWaiting{};      // hedge leg has no opposite price; logged once per wait
        double                    _tradedSpread = 0.0;  // average traded spread (paise), refreshed only while hedged
        LegArray<OrderObjectPtrT> _cutOrders;           // IOC on the opposite side of each leg: reverse-cuts extra lots
        uint64_t                  _incompleteSinceNs = 0;  // monotonic ns when the pack stopped being whole; 0 when whole
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

    struct FileCloser {
        void operator()(std::FILE* file_) const noexcept { std::fclose(file_); }
    };

    static constexpr size_t NO_LEG      = std::numeric_limits<size_t>::max();
    static constexpr size_t BIDDING_LEG = 0;  // leg 0 always bids; other legs hedge
    static constexpr int    DEFAULT_REVERT_BID_MS = 1000;  // TimeToRevertBidMs 0 would leave a pack naked forever
    static constexpr size_t LOG_BUFFER_BYTES      = 1 << 16;
    static constexpr uint64_t THROTTLE_PAUSE_NS   = 1'000'000'000;  // exchange message limits count per second
    static constexpr uint64_t THROTTLE_RETRY_NS   = 5'000'000;      // after a THROTTLE_LIMIT reject nothing is resent sooner (was every tick)
    static constexpr int    PAST_TARGET_MOVE_TICKS = 3;    // a bid past the target this close to its touch moves at once

    [[nodiscard]] static auto GetPrice(const Quote& quote_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int;
    [[nodiscard]] static auto GetQuantity(const Quote& quote_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int;
    [[nodiscard]] static auto GetOrderCount(const Quote& quote_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int;
    [[nodiscard]] static auto GetAvailableQuantity(const Quote& quote_, size_t depth_, ORDER_SIDE side_) noexcept -> int;
    [[nodiscard]] static auto CountOrders(const Quote& quote_, ORDER_SIDE side_) noexcept -> int;  // over every quote level
    [[nodiscard]] static auto CheckPriceDepth(const Quote& quote_, size_t depth_, ORDER_SIDE side_) noexcept -> bool;
    [[nodiscard]] static auto DescribeDepth(const Quote& quote_, int minOrders_, size_t priceDepth_, ORDER_SIDE side_, double requiredQuantity_) -> std::string;

    [[nodiscard]] auto FindLegIndex(int token_) const noexcept -> size_t {
        for (size_t leg = 0; leg < _legCount; ++leg) {
            if (_tokens[leg] == token_) {
                return leg;
            }
        }
        return NO_LEG;
    }

    void               WriteTimestamp() const;
    [[nodiscard]] auto PackName(const Pack& pack_) const noexcept -> std::string_view { return &pack_ == &_longPack ? "LONG" : "SHORT"; }
    [[nodiscard]] auto BookText() const -> std::string;                   // touch of every leg: "bid/ask bid/ask ..."
    [[nodiscard]] auto LotsText(const Pack& pack_) const -> std::string;  // per leg "traded/needed" lots

    // ── Diagnostics: every request sent, every response, every state change ───
    void PullOrder(Pack& pack_, size_t leg_, std::string_view reason_);
    void LogResponse(const Pack& pack_, size_t leg_, const OrderObjectT& order_, const oms_transaction& response_, bool isCut_);
    void SetBidGate(Pack& pack_, BidGate gate_, size_t thinLeg_, const SpreadQuote& spreadQuote_, bool spreadMatched_, int bidPrice_);

    void ParseLegs(const nlohmann::json& json_);
    void LoadProductDetails();
    void RebuildCache();

    // ── Decision: one call per event per pack ─────────────────────────────────
    void ProcessPack(Pack& pack_);
    void OnTrade(Pack& pack_, size_t leg_, const oms_transaction& response_);

    // ── Entry (bidding leg) ───────────────────────────────────────────────────
    void               EvaluateBidding(Pack& pack_);
    [[nodiscard]] auto FindThinHedgeLeg(const Pack& pack_) const -> size_t;  // NO_LEG when every hedge leg has depth
    [[nodiscard]] auto TargetBidPrice(const Pack& pack_, const SpreadQuote& spreadQuote_) const -> int;
    [[nodiscard]] auto TicksBehindTouch(const Pack& pack_, int bidPrice_) const -> int;  // 0 when crossing or no touch

    // ── Hedge legs ────────────────────────────────────────────────────────────
    void HedgePack(Pack& pack_);
    void ExecuteHedgeLeg(Pack& pack_, size_t leg_, int targetHedgeLots_);
    void UpdateUnhedgedStatus(Pack& pack_);
    void CheckSlippageThreshold(Pack& pack_, const oms_transaction& response_);
    // One Trade Tracer row (GUI 100004); message_ is the Error column ("Slippage", "PartialFill" or empty).
    void SendTracer(const Pack& pack_, const oms_transaction& response_, float price_, float ltp_, int ltq_, float slippage_, std::string_view message_);

    // ── Reverse cut: lots beyond the completed packs ──────────────────────────
    [[nodiscard]] auto ExtraLots(const Pack& pack_, size_t leg_) const noexcept -> int;
    [[nodiscard]] auto IsWhole(const Pack& pack_) const noexcept -> bool;
    [[nodiscard]] auto InCutWindow(Pack& pack_) -> bool;
    void               CutPack(Pack& pack_);
    void               OnCutTrade(Pack& pack_, size_t leg_, const oms_transaction& response_);

    // ── Pricing / PnL ─────────────────────────────────────────────────────────
    [[nodiscard]] auto QuoteSpread(const LegCache& cache_) const -> SpreadQuote;
    [[nodiscard]] auto PriceSpread(const LegCache& cache_, const std::array<int, MAX_LEGS>& prices_) const -> float;
    [[nodiscard]] auto AdjustGap(double spread_) const noexcept -> double;
    [[nodiscard]] auto CompletedPacks(const Pack& pack_) const noexcept -> int;
    [[nodiscard]] auto TradedSpread(const Pack& pack_) const -> double;
    [[nodiscard]] auto PositionOf(size_t leg_) const -> LegPosition;

    MinixStrategy* const _hub;
    client_uid           _clientUid{};  // zeroed: request_id must start at 0, not stack garbage

    const uint32_t _strategyId;
    const int32_t  _interface;
    const size_t   _legCount;
    const bool     _hasStrikeGap;

    StrategyStatus _status           = StrategyStatus_INACTIVE;
    long           _utcOffsetSeconds = 0;  // ponytail: taken once at creation; fine for IST (no DST)

    int _strikeGap  = 0;
    int _lotSize    = 0;
    int _tickSize   = 0;
    int _eventCount = 0;

    // Leg definition, fixed at creation.
    LegArray<Quote> _quote{};
    LegArray<int>   _tokens{};
    LegArray<int>   _ratios{};
    LegArray<bool>  _isOption{};

    Pack _longPack;
    Pack _shortPack;

    // ── Params ────────────────────────────────────────────────────────────────
    size_t _repriceTicks       = 0;
    int    _orderDepth         = 0;  // orders a hedge leg needs on the side it takes, summed over every quote level
    size_t _priceDepth         = 0;  // price levels a hedge leg needs on the side it takes; also ThresholdQty's window
    int    _hedgeDepthPercent  = 0;  // ThresholdQty: % of a hedge leg's slice quantity its first PriceDepth levels must hold
    int    _allowedSlippage    = 0;  // paise
    int    _tradeGear          = 0;
    size_t _allowedBidDepth    = 0;  // price levels leg 0 needs on its own side (where the bid rests); 0 = no check
    int    _revertBidMs        = DEFAULT_REVERT_BID_MS;  // length of each complete / reverse-cut window

    // ── Cached fast-path lookup ───────────────────────────────────────────────
    int              _tradeGearOffset  = 0;  // paise
    int              _repriceThreshold = 0;  // paise
    LegArray<double> _buyCostRate{};
    LegArray<double> _sellCostRate{};

    double _unwindPnL = 0.0;  // realized on reverse-cut lots, after cost (paise x quantity, like GetRLP)
    bool   _deleted   = false;

    // Set by a THROTTLE_LIMIT reject. Shared by every strategy: the limit counts the whole OMS session's messages.
    // Monotonic nanoseconds (aef::infra::get_time_in_nanos, CLOCK_MONOTONIC).
    static inline uint64_t _bidPausedUntilNs  = 0;
    static inline uint64_t _throttleRetryAtNs = 0;

    TradeTracer                            _tradeTracer{};
    std::string                            _logFileName;
    std::unique_ptr<std::FILE, FileCloser> _logFile;
};
