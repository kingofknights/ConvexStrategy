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
#include <string>

class MinixStrategy;

// One N-leg ratio spread (2..6 legs). Leg 0 rests a passive bid; when a whole pack of it fills,
// the other legs are hedged at once. The long and short directions run as independent packs.
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

    template <typename... Args>
    void WriteLog(fmt::format_string<Args...> format_, Args&&... args_) const {
        if (_logFile) {
            fmt::print(_logFile.get(), format_, std::forward<Args>(args_)...);
        }
    }

    void ParamUpdate(const nlohmann::json& json_);
    void SetStatus(StrategyStatus status_);
    void Stop();

    void OnTick(const Quote& quote_);
    void OnOrderResponse(const oms_transaction& response_);

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

  private:
    template <typename T>
    using LegArray = std::array<T, MAX_LEGS>;

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
        LegArray<size_t>          _hedgeRetryCount{};
        bool                      _isUnhedged   = false;
        double                    _tradedSpread = 0.0;  // average traded spread (paise), refreshed only while hedged
        SpreadQuote               _bidSnapshot;         // leg prices when the bidding order was last sent
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
    static constexpr size_t BIDDING_LEG = 0;  // leg 0 always rests the bid; other legs hedge

    [[nodiscard]] static auto GetPrice(const Quote& quote_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int;
    [[nodiscard]] static auto GetQuantity(const Quote& quote_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int;
    [[nodiscard]] static auto GetOrderCount(const Quote& quote_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int;
    [[nodiscard]] static auto GetAvailableQuantity(const Quote& quote_, size_t depth_, ORDER_SIDE side_) noexcept -> int;
    [[nodiscard]] static auto CheckOrderDepth(const Quote& quote_, size_t depth_, ORDER_SIDE side_) noexcept -> bool;
    [[nodiscard]] static auto CheckPriceDepth(const Quote& quote_, size_t depth_, ORDER_SIDE side_) noexcept -> bool;

    [[nodiscard]] auto FindLegIndex(int token_) const noexcept -> size_t {
        for (size_t leg = 0; leg < _legCount; ++leg) {
            if (_tokens[leg] == token_) {
                return leg;
            }
        }
        return NO_LEG;
    }

    void ParseLegs(const nlohmann::json& json_);
    void LoadProductDetails();
    void RebuildCache();

    // ── Decision: one call per event per pack ─────────────────────────────────
    void ProcessPack(Pack& pack_);
    void OnTrade(Pack& pack_, size_t leg_, const oms_transaction& response_);

    // ── Entry (bidding leg) ───────────────────────────────────────────────────
    void               EvaluateBidding(Pack& pack_);
    [[nodiscard]] auto CheckHedgeLegsDepth(const Pack& pack_) const -> bool;
    [[nodiscard]] auto CheckBiddingLegDepth(const Pack& pack_) const -> bool;

    // ── Hedge legs ────────────────────────────────────────────────────────────
    void HedgePack(Pack& pack_);
    void ExecuteHedgeLeg(Pack& pack_, size_t leg_, int targetHedgeLots_);
    void UpdateUnhedgedStatus(Pack& pack_) noexcept;
    void CheckSlippageThreshold(Pack& pack_, const oms_transaction& response_);

    // ── Pricing / PnL ─────────────────────────────────────────────────────────
    [[nodiscard]] auto QuoteSpread(const LegCache& cache_) const -> SpreadQuote;
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

    StrategyStatus _status = StrategyStatus_INACTIVE;

    int _strikeGap = 0;
    int _lotSize   = 0;
    int _tickSize  = 0;

    // Leg definition, fixed at creation.
    LegArray<Quote> _quote{};
    LegArray<int>   _tokens{};
    LegArray<int>   _ratios{};
    LegArray<bool>  _isOption{};

    Pack _longPack;
    Pack _shortPack;

    // ── Params ────────────────────────────────────────────────────────────────
    size_t _repriceTicks       = 0;
    size_t _orderDepth         = 0;
    size_t _priceDepth         = 0;
    size_t _allowedBidDepth    = 0;
    int    _hedgeDepthPercent  = 0;
    int    _allowedSlippage    = 0;
    int    _tradeGear          = 0;
    size_t _marketOrderRetries = 0;

    // ── Cached fast-path lookup ───────────────────────────────────────────────
    int              _tradeGearOffset  = 0;  // paise
    int              _repriceThreshold = 0;  // paise
    LegArray<double> _buyCostRate{};
    LegArray<double> _sellCostRate{};

    TradeTracer                            _tradeTracer{};
    std::string                            _logFileName;
    std::unique_ptr<std::FILE, FileCloser> _logFile;
};
