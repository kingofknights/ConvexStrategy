#pragma once
#include "ProductInfo.hpp"
#include "Quote.hpp"
#include "Utils.hpp"
#include "nlohmann/json.hpp"
#include "oms_api.hpp"
#include "order_instance.hpp"
#include "rms_api.hpp"
#include "ui_api.hpp"

// Algo
#include "AlgoBase.hpp"

#include <cstddef>
#include <cstdint>
class MinixStrategy;

class Ratio2LegStrategy {
    using OptionTypeT = aef::infra::product::OPTION_TYPE;

    struct TokenInfo {
        int        _token = 0;
        ORDER_SIDE _side  = BUY_SIDE;
        bool       _bid   = false;
    };

    struct WindRate {
        int   _price[2];
        float _spread;
    };

    enum Mode {
        AGGRESSIVE     = 1,
        BIDDING        = 2,
        ALLLEG_BIDDING = 4,
    };

    struct ParamLots {
        int   _totalQuantity = 0;
        int   _quantity      = 0;
        float _spread        = 0;
    };

    struct MarketBidding {
        OrderObjectPtrT _order[2];
        uint32_t        _uniqueID[2]   = {0, 0};
        int32_t         _tradedLot[2]  = {0, 0};
        uint64_t        _tradeValue[2] = {0, 0};
        int32_t         _lastBiddingFillPrice = 0;
        size_t          _hedgeRetryCount = 0;
        WindRate        _windRate;
    };

  public:
    Ratio2LegStrategy(MinixStrategy* ms_, uint32_t strategyId_, const nlohmann::json& json_);

    void ParamUpdate(const nlohmann::json& json_);

    void OnTick(const Quote& event_, int64_t nowTs_);

    void OnBcast(const aef::infra::product::product_data& pd_, int64_t nowTs_);

    void OnOrderResponse(const oms_transaction& resp_);

    void OrderBiddingLogic(MarketBidding& object_, ParamLots param_, WindRate rate_, std::string name_);

    void SecondOrderBidding(MarketBidding& object_, ParamLots param_);

    [[nodiscard]] auto GetStrategyID() const -> uint32_t;
    [[nodiscard]] auto GetGap() const -> int;
    [[nodiscard]] auto GetCost() const -> double;
    [[nodiscard]] auto GetBCmp() const -> WindRate;
    [[nodiscard]] auto GetSCmp() const -> WindRate;
    [[nodiscard]] auto GetFLP() const -> int;
    [[nodiscard]] auto GetBuyTradedQuantity() const -> int;
    [[nodiscard]] auto GetSellTradedQuantity() const -> int;
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
    bool     _active     = false;
    int      _gap        = 0;
    int      _lotSize    = 0;
    int      _tickSize   = 0;
    size_t   _biddingLeg = 0;

    Quote                     _qoute[2];
    std::array<int, 2>        _tokens    = {0, 0};
    std::array<ORDER_SIDE, 2> _longSide  = {BUY_SIDE, SELL_SIDE};
    std::array<ORDER_SIDE, 2> _shortSide = {SELL_SIDE, BUY_SIDE};

    MarketBidding _longOrders;
    MarketBidding _shortOrders;

    // ── Params ────────────────────────────────────────────────────────────────
    ParamLots _longParam;
    ParamLots _shortParam;

    size_t _minTickChange = 0;
    size_t _orderDepth    = 0;
    size_t _priceDepth    = 0;
    size_t _allowedBidDepth = 0;
    int    _thresholdQty   = 0;
    int    _allowedSlippage = 0;
    int    _tradeGear     = 0;
    size_t _marketOrderRetries = 0;

    // ── Strategy meta ─────────────────────────────────────────────────────────
    bool _isBidding = false;
};
