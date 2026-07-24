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

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class MinixStrategy;

enum class StrategyType {
    CONVERSION = 1,
    REVERSION  = 2
};

class ConversionReversalStrategy {
    struct WindRate {
        int   _biddingPrice = 0;
        float _windRate     = 0.0f;
        bool  _valid        = false;
    };

    struct MarketBidding {
        OrderObjectPtrT _order[3];       // 0: CE, 1: PE, 2: FUT
        uint32_t        _uniqueID[3]   = {0, 0, 0};
        int32_t         _tradedLot[3]  = {0, 0, 0};
        uint64_t        _tradeValue[3] = {0, 0, 0};
        int32_t         _lastBiddingFillPrice = 0;
        size_t          _hedgeRetryCount = 0;
        WindRate        _windRate;
    };

  public:
    ConversionReversalStrategy(MinixStrategy* ms_, uint32_t strategyId_, const nlohmann::json& json_);

    void ParamUpdate(const nlohmann::json& json_);

    void OnTick(const Quote& event_, int64_t nowTs_);

    void OnBcast(const aef::infra::product::product_data& pd_, int64_t nowTs_);

    void OnOrderResponse(const oms_transaction& resp_);

    void OrderBiddingLogic(MarketBidding& object_, WindRate rate_, std::string name_);

    void SecondOrderBidding(MarketBidding& object_);

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

    // Pure spread calculations directly from con_rev_BIDDING.docx
    [[nodiscard]] auto CalculateConversionCallWindRate() const -> WindRate;
    [[nodiscard]] auto CalculateReversionCallWindRate() const -> WindRate;
    [[nodiscard]] auto CalculateConversionPutWindRate() const -> WindRate;
    [[nodiscard]] auto CalculateReversionPutWindRate() const -> WindRate;

  private:
    MinixStrategy* _ms;
    client_uid     _uid;

    uint32_t _strategyId;
    bool     _active     = false;
    int      _gap        = 0;
    int      _lotSize    = 0;
    int      _tickSize   = 0;
    int      _orderStrike = 0;

    StrategyType _stratType = StrategyType::CONVERSION;

    Quote _qoute[3]; // 0: CE, 1: PE, 2: FUT
    std::array<int, 3> _tokens = {0, 0, 0};

    MarketBidding _biddingOrders;

    int    _totalQuantity      = 0;
    int    _quantity           = 0;
    size_t _marketOrderRetries = 0;

    bool _isBidding = false;
};
