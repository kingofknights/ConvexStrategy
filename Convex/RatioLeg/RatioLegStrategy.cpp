#include "RatioLegStrategy.hpp"

#include "MinixStrategy.hpp"
#include "ProductInfo.hpp"
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

    constexpr auto Opposite(ORDER_SIDE side_) noexcept -> ORDER_SIDE { return side_ == BUY_SIDE ? SELL_SIDE : BUY_SIDE; }

    // GUI prices are rupees; round so 195.05 becomes 19505 paise, not 19504.999.
    auto ToPaise(double rupees_) noexcept -> float { return static_cast<float>(std::round(rupees_ * 100.0)); }

    constexpr auto SideName(ORDER_SIDE side_) noexcept -> std::string_view { return side_ == BUY_SIDE ? "BUY" : "SELL"; }
    constexpr auto BookSideName(ORDER_SIDE side_) noexcept -> std::string_view { return side_ == BUY_SIDE ? "bid" : "ask"; }

    auto TransactionName(int32_t code_) noexcept -> std::string_view {
        switch (code_) {
            case OMS_ORDER_PLACED:           return "PLACED";
            case OMS_ORDER_CONFIRMED:        return "CONFIRMED";
            case OMS_ORDER_MODIFY_PLACED:    return "MODIFY_PLACED";
            case OMS_ORDER_MODIFY_CONFIRMED: return "MODIFIED";
            case OMS_ORDER_CANCEL_ACCEPTED:  return "CANCEL_ACCEPTED";
            case OMS_ORDER_CANCELLED:        return "CANCELLED";
            case OMS_TRADE:                  return "TRADE";
            case OMS_REQ_REJ:                return "REJECT";
            default:                         return "OTHER";
        }
    }

    // What UpdateOrder will send for this order: a new order or a modify.
    auto OrderAction(const OrderObjectT& order_) noexcept -> std::string_view {
        return order_.get_current_state() == static_cast<uint32_t>(execution_strat::STRAT_ORDER_STATE::STRAT_INITIAL_STATE) ? "PLACE" : "MODIFY";
    }

}  // namespace

// ═══ Lifecycle ═══════════════════════════════════════════════════════════════

RatioLegStrategy::RatioLegStrategy(MinixStrategy* hub_, uint32_t strategyId_, int32_t interface_, const nlohmann::json& json_, size_t legCount_, bool hasStrikeGap_)
    : _hub(hub_), _strategyId(strategyId_), _interface(interface_), _legCount(legCount_), _hasStrikeGap(hasStrikeGap_) {
    if (_legCount < 2 || _legCount > MAX_LEGS) {
        throw std::runtime_error(fmt::format("strategy {}: {} legs, supported 2..{}", _strategyId, _legCount, MAX_LEGS));
    }

    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm           localTime{};
    localtime_r(&now, &localTime);
    _utcOffsetSeconds = localTime.tm_gmtoff;
    char dateText[16];
    char timeText[16];
    std::strftime(dateText, sizeof(dateText), "%Y%m%d", &localTime);
    std::strftime(timeText, sizeof(timeText), "%H%M%S", &localTime);

    const std::string logDir = fmt::format("log/{}", dateText);
    std::error_code   ignored;
    std::filesystem::create_directories(logDir, ignored);

    _logFileName = fmt::format("{}/{}Ratio_{}_{}.log", logDir, _legCount, _strategyId, timeText);
    _logFile.reset(std::fopen(_logFileName.c_str(), "a"));
    if (_logFile) {
        // Line-buffered: the last trades before a crash must reach disk. Logging is off the tick path.
        std::setvbuf(_logFile.get(), nullptr, _IOLBF, 0);
    } else {
        // Without this the strategy runs with every WriteLog silently dropped.
        std::cerr << "[RatioLeg] strat=" << _strategyId << " cannot open log '" << _logFileName << "': " << std::strerror(errno) << std::endl;
    }
    WriteLog(">>> [INIT] Opened log file: {}\n", _logFileName);

    _longPack._packSide  = BUY_SIDE;
    _shortPack._packSide = SELL_SIDE;
    _ratios.fill(1);

    ParseLegs(json_);
    LoadProductDetails();
    ParamUpdate(json_);

    _clientUid.composite_id_.client_id   = static_cast<uint32_t>(_hub->_clientId);
    _clientUid.composite_id_.strategy_id = _strategyId;

    for (Pack* pack : {&_longPack, &_shortPack}) {
        for (size_t leg = 0; leg < _legCount; ++leg) {
            pack->_orders[leg] = std::make_unique<OrderObjectT>(_tokens[leg], pack->_sides[leg], _lotSize, _hub->_clientId, _hub->_algoId, _hub->_omsId, ORDER_TYPE::LIMIT_ORDER_TYPE, _hub);
        }
    }

    // Subscribe last: a throw above must not leave feeds pointing at a strategy that never existed.
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const bool subscribed = _hub->subscribeProduct(_tokens[leg], _hub->_feedFlags);
        WriteLog("[FEED] leg={} token={} subscribe={}\n", leg, _tokens[leg], subscribed ? "ok" : "FAILED");
    }
}

RatioLegStrategy::~RatioLegStrategy() {
    WriteLog(">>> [DESTROY] Closing log file: {}\n", _logFileName);
}

// Tokens, sides and ratios are fixed at creation; later config updates only change params.
void RatioLegStrategy::ParseLegs(const nlohmann::json& json_) {
    const auto& legs = json_.at("Legs");
    for (size_t leg = 0; leg < _legCount && leg < legs.size(); ++leg) {
        _tokens[leg]           = legs[leg].value("Token", 0);
        _longPack._sides[leg]  = legs[leg].value("Side", "BUY") == "BUY" ? BUY_SIDE : SELL_SIDE;
        _shortPack._sides[leg] = Opposite(_longPack._sides[leg]);
    }

    if (json_.contains("Ratio") && json_["Ratio"].contains("LegRatios")) {
        const auto& legRatios = json_["Ratio"]["LegRatios"];
        for (size_t leg = 0; leg < _legCount && leg < legRatios.size(); ++leg) {
            _ratios[leg] = legRatios[leg].get<int>();
        }
    }
}

// Lot and tick size divide order quantities; trading without them would size orders from garbage.
void RatioLegStrategy::LoadProductDetails() {
    LegArray<ProductDetails> details{};
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (!_hub->getProductDetails(_tokens[leg], details[leg]) || details[leg].lot_size_ <= 0 || details[leg].tick_size_ <= 0) {
            WriteLog("[INIT] leg {} token {}: product details unavailable, strategy not created\n", leg, _tokens[leg]);
            throw std::runtime_error(fmt::format("strategy {}: leg {} token {} has no product details", _strategyId, leg, _tokens[leg]));
        }
        _isOption[leg] = details[leg].opt_type_ != aef::infra::product::OPTION_TYPE::FUTXX;
        WriteLog("[{}LegRatios] {}Leg Token: {} Symbol: {} strike = {}, lot = {} ticksize = {} ratio = {} longSide = {}\n",
                 _legCount, leg, _tokens[leg], details[leg].symbol, int(details[leg].strike_price_), int(details[leg].lot_size_), int(details[leg].tick_size_), _ratios[leg], SideName(_longPack._sides[leg]));
    }

    std::strncpy(_tradeTracer._symbol, details[BIDDING_LEG].symbol, sizeof(_tradeTracer._symbol) - 1);

    _strikeGap = std::abs(details[0].strike_price_ - details[1].strike_price_);
    _lotSize   = details[0].lot_size_;
    _tickSize  = details[0].tick_size_;
}

void RatioLegStrategy::ParamUpdate(const nlohmann::json& json_) {
    if (const auto found = json_.find("Params"); found != json_.end()) {
        const auto& params = *found;

        _longPack._params._slicePacks   = params.value("LongBuySoQ", 0);
        _longPack._params._totalPacks   = params.value("LongBuyQty", 0);
        _longPack._params._targetSpread = ToPaise(params.value("LongBuyPrice", 0.0));

        _shortPack._params._slicePacks   = params.value("ShortSellSoQ", 0);
        _shortPack._params._totalPacks   = params.value("ShortSellQty", 0);
        _shortPack._params._targetSpread = ToPaise(params.value("ShortSellPrice", 0.0));

        _repriceTicks       = params.value("TickSize", 0U);
        _orderDepth         = std::min<size_t>(params.value("OrderDepth", 1U), aef::infra::quote::QUOTE_LEVELS);
        _priceDepth         = std::min<size_t>(params.value("PriceDepth", 1U), aef::infra::quote::QUOTE_LEVELS);
        _allowedBidDepth    = std::min<size_t>(params.value("AllowedBidDepth", 1U), aef::infra::quote::QUOTE_LEVELS);
        _hedgeDepthPercent  = params.value("ThresholdQty", 100);
        _allowedSlippage    = params.value("AllowedSlippage", 0);
        _tradeGear          = params.value("TradeGear", 0);
        _marketOrderRetries = params.value("MarketRetries", 0U);

        WriteLog(
            "[RatioLeg] StrategyId: {} | Params parsed: "
            "\nLongBuySoQ: {}, \nLongBuyQty: {}, \nLongBuyPrice: {}; "
            "\nShortSellSoQ: {}, \nShortSellQty: {}, \nShortSellPrice: {}; "
            "\nTickSize: {}, \nOrderDepth: {}, \nPriceDepth: {}, \nAllowedBidDepth: {}, "
            "\nThresholdQty: {}, \nAllowedSlippage: {}, \nTradeGear: {}, \nMarketRetries: {}, \n_biddingLeg: {}\n",
            _strategyId,
            _longPack._params._slicePacks, _longPack._params._totalPacks, _longPack._params._targetSpread,
            _shortPack._params._slicePacks, _shortPack._params._totalPacks, _shortPack._params._targetSpread,
            _repriceTicks, _orderDepth, _priceDepth, _allowedBidDepth,
            _hedgeDepthPercent, _allowedSlippage, _tradeGear, _marketOrderRetries, BIDDING_LEG);
    }
    RebuildCache();
}

void RatioLegStrategy::RebuildCache() {
    _tradeGearOffset  = _tradeGear * _tickSize;
    _repriceThreshold = static_cast<int>(_repriceTicks) * _tickSize;

    const double hedgeDepthFraction = (_hedgeDepthPercent > 0 ? _hedgeDepthPercent : 100) / 100.0;

    for (Pack* pack : {&_longPack, &_shortPack}) {
        LegCache& cache = pack->_cache;
        for (size_t leg = 0; leg < _legCount; ++leg) {
            cache._takeSide[leg]           = Opposite(pack->_sides[leg]);
            cache._signedRatio[leg]        = (pack->_sides[leg] == BUY_SIDE ? -1 : 1) * _ratios[leg];
            cache._sliceQuantity[leg]      = pack->_params._slicePacks * _ratios[leg] * _lotSize;
            cache._requiredHedgeDepth[leg] = cache._sliceQuantity[leg] * hedgeDepthFraction;

            WriteLog("updateSideCache [.slice = {}, ._ratio = {}, ._index = {}, ._packSide = {}]\n",
                     cache._sliceQuantity[leg], _ratios[leg], leg, static_cast<int>(pack->_packSide));
        }
    }

    for (size_t leg = 0; leg < _legCount; ++leg) {
        _buyCostRate[leg]  = _ratios[leg] * (_isOption[leg] ? OptionBuyCost : FutureBuyCost);
        _sellCostRate[leg] = _ratios[leg] * (_isOption[leg] ? OptionSellCost : FutureSellCost);
    }
}

void RatioLegStrategy::SetStatus(StrategyStatus status_) {
    if (status_ == StrategyStatus_INACTIVE) {
        Stop("status INACTIVE requested");
        return;
    }
    _status             = status_;
    _longPack._bidGate  = BidGate::UNKNOWN;  // log the first gate under the new status
    _shortPack._bidGate = BidGate::UNKNOWN;
    std::cout << "[RatioLeg:SetStatus] strat=" << _strategyId << " _status=" << static_cast<int>(_status)
              << " (" << StrategyStatusToString(_status) << ")" << std::endl;
    WriteLog("[RatioLeg:SetStatus] strat={} _status={} ({})\n", _strategyId, static_cast<int>(_status), StrategyStatusToString(_status));
}

void RatioLegStrategy::Stop(std::string_view reason_) {
    _status = StrategyStatus_INACTIVE;
    WriteLog("[RatioLeg:Stop] strat={} _status=INACTIVE reason='{}' long_unhedged={} short_unhedged={} long_lots=[{}] short_lots=[{}]\n",
             _strategyId, reason_, _longPack._isUnhedged, _shortPack._isUnhedged, LotsText(_longPack), LotsText(_shortPack));

    // Stop new entries only. An unhedged pack keeps its hedge orders working until flat,
    // so stopping never leaves a naked leg. A cancel refused here is retried by ProcessPack.
    for (Pack* pack : {&_longPack, &_shortPack}) {
        for (size_t leg = 0; leg < _legCount; ++leg) {
            if (leg != BIDDING_LEG && pack->_isUnhedged) continue;
            PullOrder(*pack, leg, reason_);
            pack->_hedgeRetryCount[leg] = 0;
        }
        pack->_bidGate = BidGate::UNKNOWN;
    }
}

// ═══ Events ══════════════════════════════════════════════════════════════════

void RatioLegStrategy::OnTick(const Quote& quote_) {
    const size_t leg = FindLegIndex(quote_.header.product_id);
    if (leg == NO_LEG) return;

    // Until every leg has ticked the bid gate stays silent, so say when each feed goes live.
    if (_quote[leg].header.product_id == 0) {
        WriteLog("[FEED] leg={} token={} first tick bid={} ask={}\n", leg, _tokens[leg],
                 int{quote_.message.bid_levels[0].price}, int{quote_.message.ask_levels[0].price});
    }
    _quote[leg] = quote_;
    _eventCount += 1;
    ProcessPack(_longPack);
    ProcessPack(_shortPack);
}

void RatioLegStrategy::OnOrderResponse(const oms_transaction& response_) {
    const size_t leg  = FindLegIndex(response_.packet_.product_id_);
    const auto   side = static_cast<ORDER_SIDE>(response_.packet_.flags_.order_side);
    if (leg == NO_LEG || (side != BUY_SIDE && side != SELL_SIDE)) {
        WriteLog("[ORDER-IN] ignored: token={} side={} code={} uid={} (not a leg of this strategy)\n",
                 int{response_.packet_.product_id_}, static_cast<int>(side), int{response_.hdr_.transaction_code}, uint32_t{response_.hdr_.uid_.id_});
        return;
    }

    // Long and short packs trade opposite sides of every leg, so the side names the pack.
    Pack& pack = (side == _longPack._sides[leg]) ? _longPack : _shortPack;

    pack._orders[leg]->handle_confirmation(response_);
    LogResponse(pack, leg, response_);

    switch (response_.hdr_.transaction_code) {
        case OMS_TRADE:
            OnTrade(pack, leg, response_);
            break;
        case OMS_REQ_REJ:
            // Wait for the next tick, so a persistent reject (price band, margin) cannot resend at OMS speed.
            break;
        default:
            // An ack or cancel frees the order: act now instead of waiting for a tick.
            ProcessPack(pack);
            break;
    }
}

// Hedge first; quote only while APPLIED; otherwise keep the bid pulled (retries a cancel Stop() could not send).
void RatioLegStrategy::ProcessPack(Pack& pack_) {
    if (pack_._isUnhedged) {
        HedgePack(pack_);
    } else if (IsActive()) {
        EvaluateBidding(pack_);
    } else {
        PullOrder(pack_, BIDDING_LEG, "strategy not APPLIED");
    }
}

void RatioLegStrategy::OnTrade(Pack& pack_, size_t leg_, const oms_transaction& response_) {
    const int32_t  price    = response_.packet_.price_;
    const int32_t  quantity = response_.packet_.quantity_;
    const int32_t  lots     = quantity / _lotSize;
    const uint64_t value    = static_cast<uint64_t>(price) * static_cast<uint64_t>(quantity);

    const ORDER_SIDE side = pack_._sides[leg_];
    pack_._tradedLots[leg_] += lots;
    pack_._tradedValue[leg_] += value;
    pack_._unreportedLots[leg_] += lots;
    pack_._unreportedValue[leg_] += value;

    WriteLog("[TRADE EVENT] index: {}, token: {}, side: {} ({}), price: {}, quantity: {}, pack: {}, lots: [{}]\n",
             leg_, _tokens[leg_], SideName(side), static_cast<int>(side), price, quantity, PackName(pack_), LotsText(pack_));
    if (quantity % _lotSize != 0) {
        WriteLog("[TRADE EVENT] WARNING quantity {} is not a multiple of lot size {}; {} units not counted\n", quantity, _lotSize, quantity % _lotSize);
    }

    const bool wasUnhedged = pack_._isUnhedged;
    UpdateUnhedgedStatus(pack_);
    if (pack_._isUnhedged != wasUnhedged) {
        WriteLog("[PACK] {} {} | lots=[{}] completedPacks={}\n", PackName(pack_), pack_._isUnhedged ? "UNHEDGED" : "HEDGED", LotsText(pack_), CompletedPacks(pack_));
        pack_._bidGate = BidGate::UNKNOWN;
    }
    if (!pack_._isUnhedged) {
        pack_._hedgeRetryCount.fill(0);  // the next pack's hedges start from the snapshot price again
        pack_._tradedSpread = TradedSpread(pack_);
        CheckSlippageThreshold(pack_, response_);
    }
    ProcessPack(pack_);
}

// ═══ Entry: bidding leg ══════════════════════════════════════════════════════

void RatioLegStrategy::EvaluateBidding(Pack& pack_) {
    OrderObjectT& order         = *pack_._orders[BIDDING_LEG];
    const int     biddingRatio  = _ratios[BIDDING_LEG];
    const int     tradedLots    = pack_._tradedLots[BIDDING_LEG];
    const int     remainingLots = pack_._params._totalPacks * biddingRatio - tradedLots;

    size_t      thinLeg = NO_LEG;
    SpreadQuote spreadQuote{};
    const BidGate gate = [&] {
        if (remainingLots <= 0) return BidGate::DONE;
        thinLeg = FindThinHedgeLeg(pack_);
        if (thinLeg != NO_LEG) return BidGate::HEDGE_DEPTH;
        if (!CheckBiddingLegDepth(pack_)) return BidGate::BID_DEPTH;
        spreadQuote = QuoteSpread(pack_._cache);
        if (!spreadQuote._valid) return BidGate::NO_PRICE;
        return pack_._params._targetSpread > spreadQuote._spread ? BidGate::BELOW_TARGET : BidGate::OPEN;
    }();
    SetBidGate(pack_, gate, thinLeg);
    if (gate != BidGate::OPEN) {
        PullOrder(pack_, BIDDING_LEG, "bid gate closed");
        return;
    }

    // Finish a partly filled pack before starting a new slice, so the bidding leg only ever holds whole packs.
    const int packRemainder = tradedLots % biddingRatio;
    const int orderLots     = (packRemainder > 0) ? (biddingRatio - packRemainder) : (pack_._params._slicePacks * biddingRatio);
    const int quantity      = std::min(orderLots, remainingLots) * _lotSize;
    const int marketPrice   = GetPrice(_quote[BIDDING_LEG], pack_._sides[BIDDING_LEG], 0);

    if (std::abs(order.get_open_price() - marketPrice) < _repriceThreshold) {
        return;
    }
    const std::string_view action   = OrderAction(order);
    const int              oldPrice = order.get_open_price();
    const int              oldQty   = order.get_open_qty();
    if (const uint32_t uid = _hub->UpdateOrder(order, marketPrice, quantity, _clientUid); uid != 0) {
        WriteLog("[ORDER-OUT] {} leg={} token={} {} {} uid={} price={}->{} qty={}->{} | BID target={} market={} lots=[{}] book=[{}]\n",
                 PackName(pack_), BIDDING_LEG, _tokens[BIDDING_LEG], SideName(pack_._sides[BIDDING_LEG]), action, uid, oldPrice, marketPrice, oldQty, quantity,
                 pack_._params._targetSpread, spreadQuote._spread, LotsText(pack_), BookText());
        pack_._bidSnapshot = spreadQuote;
    }
}

auto RatioLegStrategy::FindThinHedgeLeg(const Pack& pack_) const -> size_t {
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (leg == BIDDING_LEG) continue;
        // A hedge crosses the book, so check liquidity on the side it will take (BUY hedge -> asks).
        const ORDER_SIDE takeSide = pack_._cache._takeSide[leg];
        if (!CheckOrderDepth(_quote[leg], _orderDepth, takeSide) ||
            !CheckPriceDepth(_quote[leg], _priceDepth, takeSide) ||
            GetAvailableQuantity(_quote[leg], _orderDepth, takeSide) < pack_._cache._requiredHedgeDepth[leg]) {
            return leg;
        }
    }
    return NO_LEG;
}

auto RatioLegStrategy::CheckBiddingLegDepth(const Pack& pack_) const -> bool {
    return CheckPriceDepth(_quote[BIDDING_LEG], _allowedBidDepth, pack_._sides[BIDDING_LEG]);
}

// ═══ Hedge legs ══════════════════════════════════════════════════════════════

void RatioLegStrategy::UpdateUnhedgedStatus(Pack& pack_) {
    const int biddingPacks = pack_._tradedLots[BIDDING_LEG] / _ratios[BIDDING_LEG];
    pack_._isUnhedged      = false;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (leg != BIDDING_LEG && pack_._tradedLots[leg] < biddingPacks * _ratios[leg]) {
            pack_._isUnhedged = true;
            return;
        }
    }
}

void RatioLegStrategy::HedgePack(Pack& pack_) {
    PullOrder(pack_, BIDDING_LEG, "pack unhedged");  // no new exposure while legs are uneven

    const int biddingPacks = pack_._tradedLots[BIDDING_LEG] / _ratios[BIDDING_LEG];
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (leg == BIDDING_LEG) continue;
        ExecuteHedgeLeg(pack_, leg, biddingPacks * _ratios[leg]);
    }
}

void RatioLegStrategy::ExecuteHedgeLeg(Pack& pack_, size_t leg_, int targetHedgeLots_) {
    const int missingLots = targetHedgeLots_ - pack_._tradedLots[leg_];
    if (missingLots <= 0) {
        pack_._hedgeRetryCount[leg_] = 0;
        return;
    }

    // Start at the price seen when the bid was sent and step one tick per retry; after
    // MarketRetries (or at once when it is 0) chase the opposite touch. TradeGear adds its ticks on top.
    const size_t retryCount    = pack_._hedgeRetryCount[leg_];
    const bool   isAggressive  = (_marketOrderRetries == 0 || retryCount >= _marketOrderRetries);
    const int    snapshotPrice = pack_._bidSnapshot._price[leg_];
    const int    basePrice     = (isAggressive || snapshotPrice <= 0) ? GetPrice(_quote[leg_], pack_._cache._takeSide[leg_], 0) : snapshotPrice;
    if (basePrice <= 0) {
        // Opposite side empty: wait for a real price instead of resting near zero.
        if (!pack_._hedgeWaiting[leg_]) {
            pack_._hedgeWaiting[leg_] = true;
            WriteLog("[HEDGE] {} leg={} token={} waiting: no {} price, {} lots missing | book=[{}]\n",
                     PackName(pack_), leg_, _tokens[leg_], BookSideName(pack_._cache._takeSide[leg_]), missingLots, BookText());
        }
        return;
    }
    pack_._hedgeWaiting[leg_] = false;

    const int retryOffset    = isAggressive ? 0 : static_cast<int>(retryCount) * _tickSize;
    const int sideMultiplier = (pack_._sides[leg_] == BUY_SIDE) ? 1 : -1;
    const int targetPrice    = std::max(basePrice + sideMultiplier * (_tradeGearOffset + retryOffset), _tickSize);
    const int quantity       = std::min(missingLots * _lotSize, pack_._cache._sliceQuantity[leg_]);

    OrderObjectT&          order    = *pack_._orders[leg_];
    const std::string_view action   = OrderAction(order);
    const int              oldPrice = order.get_open_price();
    const int              oldQty   = order.get_open_qty();
    if (const uint32_t uid = _hub->UpdateOrder(order, targetPrice, quantity, _clientUid); uid != 0) {
        WriteLog("[ORDER-OUT] {} leg={} token={} {} {} uid={} price={}->{} qty={}->{} | HEDGE mode={} retry={}/{} base={} missingLots={} lots=[{}] book=[{}]\n",
                 PackName(pack_), leg_, _tokens[leg_], SideName(pack_._sides[leg_]), action, uid, oldPrice, targetPrice, oldQty, quantity,
                 isAggressive ? "AGGRESSIVE_OPPOSITE" : "STORED_PLUS_TICKS", retryCount, _marketOrderRetries, basePrice, missingLots, LotsText(pack_), BookText());
        ++pack_._hedgeRetryCount[leg_];
    }
}

void RatioLegStrategy::CheckSlippageThreshold(Pack& pack_, const oms_transaction& response_) {
    int newPacks = pack_._unreportedLots[0] / _ratios[0];
    for (size_t leg = 1; leg < _legCount; ++leg) {
        newPacks = std::min(newPacks, pack_._unreportedLots[leg] / _ratios[leg]);
    }
    if (newPacks <= 0) {
        return;
    }

    double tradedSpread = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const double legAvgPrice = static_cast<double>(pack_._unreportedValue[leg]) / (static_cast<double>(pack_._unreportedLots[leg]) * _lotSize);
        WriteLog("[SLIPPAGE {}Leg] [strat = {}] [leg = {}] Val: {}, Lot: {}, LotSize: {}, AvgPrice: {}\n",
                 _legCount, _strategyId, leg, pack_._unreportedValue[leg], pack_._unreportedLots[leg], _lotSize, legAvgPrice);
        tradedSpread += (pack_._sides[leg] == BUY_SIDE ? -legAvgPrice : legAvgPrice) * _ratios[leg];
    }

    const auto  adjustedTradedSpread = static_cast<float>(AdjustGap(tradedSpread));
    const float slippage             = (pack_._params._targetSpread - adjustedTradedSpread) / 100.0F;

    _tradeTracer._orderId      = response_.packet_.exchange_order_id;
    _tradeTracer._time         = response_.hdr_.exchange_timestamp;
    _tradeTracer._qtyRemaining = pack_._params._totalPacks - CompletedPacks(pack_);
    _tradeTracer._strategyId   = _strategyId;
    _tradeTracer._side         = pack_._packSide;
    _tradeTracer._price        = pack_._params._targetSpread / 100.0F;
    _tradeTracer._ltp          = adjustedTradedSpread / 100.0F;
    _tradeTracer._ltq          = newPacks;
    _tradeTracer._slippage     = slippage >= static_cast<float>(_allowedSlippage) ? slippage : 0;

    WriteLog("Tracer [{}Leg] symbol {} stratId {} orderId {} time {} side {} price {} ltp {} ltq {} slippage {}\n",
             _legCount, _tradeTracer._symbol, _strategyId, _tradeTracer._orderId, _tradeTracer._time, _tradeTracer._side, _tradeTracer._price, _tradeTracer._ltp, _tradeTracer._ltq, _tradeTracer._slippage);

    _hub->SendToUi(UiMessageCode_TRADE_TRACER, _interface, _tradeTracer);

    // Carry lots beyond the reported packs into the next report at their average value.
    // newPacks is the minimum over legs, so every leg holds at least newPacks * ratio lots.
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const int32_t lots          = pack_._unreportedLots[leg];
        const int32_t leftLots      = lots - newPacks * _ratios[leg];
        pack_._unreportedValue[leg] = pack_._unreportedValue[leg] * static_cast<uint64_t>(leftLots) / static_cast<uint64_t>(lots);
        pack_._unreportedLots[leg]  = leftLots;
    }

    if (_allowedSlippage > 0 && slippage > static_cast<float>(_allowedSlippage)) {
        WriteLog("[SLIPPAGE {}Leg] Slippage {} > AllowedSlippage {}. Stopping strategy.\n", _legCount, slippage, _allowedSlippage);
        Stop("slippage above AllowedSlippage");
    }
}

// ═══ Pricing ═════════════════════════════════════════════════════════════════

auto RatioLegStrategy::QuoteSpread(const LegCache& cache_) const -> SpreadQuote {
    SpreadQuote spreadQuote{};
    double   spread = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        spreadQuote._price[leg] = GetPrice(_quote[leg], cache_._takeSide[leg], 0);
        if (spreadQuote._price[leg] <= 0) {
            return spreadQuote;  // a missing leg price means the spread is not tradable: reported as 0, invalid
        }
        spread += spreadQuote._price[leg] * cache_._signedRatio[leg];
    }
    spreadQuote._spread = static_cast<float>(AdjustGap(spread));
    spreadQuote._valid  = true;
    return spreadQuote;
}

auto RatioLegStrategy::AdjustGap(double spread_) const noexcept -> double {
    if (!_hasStrikeGap) {
        return spread_;
    }
    return (spread_ < 0) ? (spread_ + _strikeGap) : (spread_ - _strikeGap);
}

auto RatioLegStrategy::GetBCmp() const -> SpreadQuote { return QuoteSpread(_longPack._cache); }
auto RatioLegStrategy::GetSCmp() const -> SpreadQuote { return QuoteSpread(_shortPack._cache); }

// ═══ Positions / PnL (UI) ════════════════════════════════════════════════════

// A pack counts only once every leg has filled its ratio.
auto RatioLegStrategy::CompletedPacks(const Pack& pack_) const noexcept -> int {
    int totalPacks = pack_._tradedLots[0] / _ratios[0];
    for (size_t leg = 1; leg < _legCount; ++leg) {
        totalPacks = std::min(totalPacks, pack_._tradedLots[leg] / _ratios[leg]);
    }
    return totalPacks;
}

auto RatioLegStrategy::GetLongTradedPacks() const -> int { return CompletedPacks(_longPack); }
auto RatioLegStrategy::GetShortTradedPacks() const -> int { return CompletedPacks(_shortPack); }

auto RatioLegStrategy::TradedSpread(const Pack& pack_) const -> double {
    double spread = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (pack_._tradedLots[leg] == 0) {
            return 0.0;
        }
        const double avgPrice = static_cast<double>(pack_._tradedValue[leg]) / (static_cast<double>(pack_._tradedLots[leg]) * _lotSize);
        spread += (pack_._sides[leg] == BUY_SIDE ? -avgPrice : avgPrice) * _ratios[leg];
    }
    return AdjustGap(spread);
}

// ATP is cached on hedged fills so in-flight unhedged lots do not distort it.
auto RatioLegStrategy::GetBATP() const -> double { return _longPack._tradedSpread; }
auto RatioLegStrategy::GetSATP() const -> double { return _shortPack._tradedSpread; }

auto RatioLegStrategy::PositionOf(size_t leg_) const -> LegPosition {
    LegPosition position;
    for (const Pack* pack : {&_longPack, &_shortPack}) {
        const int64_t  quantity = static_cast<int64_t>(pack->_tradedLots[leg_]) * _lotSize;
        const uint64_t value    = pack->_tradedValue[leg_];
        if (pack->_sides[leg_] == BUY_SIDE) {
            position._buyQuantity += quantity;
            position._buyValue += value;
        } else {
            position._sellQuantity += quantity;
            position._sellValue += value;
        }
    }
    return position;
}

auto RatioLegStrategy::GetRLP() const -> double {
    double totalRLP  = 0.0;
    double totalCost = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const LegPosition position = PositionOf(leg);
        totalRLP += static_cast<double>(std::min(position._buyQuantity, position._sellQuantity)) * (position.AvgSell() - position.AvgBuy());

        // Transaction cost on actual traded value (paise).
        const double buyCostRate  = _isOption[leg] ? OptionBuyCost : FutureBuyCost;
        const double sellCostRate = _isOption[leg] ? OptionSellCost : FutureSellCost;
        totalCost += (static_cast<double>(position._buyValue) * buyCostRate) + (static_cast<double>(position._sellValue) * sellCostRate);
    }
    return totalRLP - totalCost;
}

auto RatioLegStrategy::GetCutPL() const -> double { return GetRLP(); }

auto RatioLegStrategy::GetM2M() const -> double {
    double totalM2M = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const LegPosition position    = PositionOf(leg);
        const int64_t     netQuantity = position._buyQuantity - position._sellQuantity;
        if (netQuantity == 0) continue;

        const double markPrice = (netQuantity > 0) ? _quote[leg].message.bid_levels[0].price : _quote[leg].message.ask_levels[0].price;
        if (markPrice <= 0) continue;  // empty book side: no mark, rather than a full-notional swing

        const double avgPrice = (netQuantity > 0) ? position.AvgBuy() : position.AvgSell();
        totalM2M += static_cast<double>(netQuantity) * (markPrice - avgPrice);
    }
    return totalM2M;
}

auto RatioLegStrategy::GetNetPL() const -> double { return GetRLP() + GetM2M(); }
auto RatioLegStrategy::GetFLP() const -> int { return _quote[BIDDING_LEG].message.ltp_; }

auto RatioLegStrategy::GetCost() const -> double {
    double totalCost = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        totalCost += (_quote[leg].message.bid_levels[0].price * _buyCostRate[leg]) +
                     (_quote[leg].message.ask_levels[0].price * _sellCostRate[leg]);
    }
    return totalCost;
}

auto RatioLegStrategy::GetStrategyID() const -> uint32_t { return _strategyId; }
auto RatioLegStrategy::GetInterface() const -> int32_t { return _interface; }
auto RatioLegStrategy::GetGap() const -> int { return _strikeGap / 100; }

// ═══ Status ══════════════════════════════════════════════════════════════════

auto RatioLegStrategy::IsActive() const -> bool { return _status == StrategyStatus_APPLIED; }
auto RatioLegStrategy::GetStatus() const -> StrategyStatus { return _status; }

// ═══ Diagnostics ═════════════════════════════════════════════════════════════

void RatioLegStrategy::WriteTimestamp() const {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    const long secondsOfDay = (now.tv_sec + _utcOffsetSeconds) % 86400;
    fmt::print(_logFile.get(), "{:02}:{:02}:{:02}.{:06} ", secondsOfDay / 3600, secondsOfDay / 60 % 60, secondsOfDay % 60, now.tv_nsec / 1000);
}

auto RatioLegStrategy::BookText() const -> std::string {
    std::string text;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        fmt::format_to(std::back_inserter(text), "{}{}/{}", leg == 0 ? "" : " ",
                       int{_quote[leg].message.bid_levels[0].price}, int{_quote[leg].message.ask_levels[0].price});
    }
    return text;
}

// Bidding leg: traded / total lots. Hedge legs: traded / lots the completed bidding packs need.
auto RatioLegStrategy::LotsText(const Pack& pack_) const -> std::string {
    const int   biddingPacks = pack_._tradedLots[BIDDING_LEG] / _ratios[BIDDING_LEG];
    std::string text;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const int needed = (leg == BIDDING_LEG) ? pack_._params._totalPacks * _ratios[leg] : biddingPacks * _ratios[leg];
        fmt::format_to(std::back_inserter(text), "{}{}/{}", leg == 0 ? "" : " ", pack_._tradedLots[leg], needed);
    }
    return text;
}

// Cancels the order and logs why. Runs on every tick, so it skips silently when the vendor
// would refuse (nothing live, unconfirmed, or a response pending); each refusal there logs.
void RatioLegStrategy::PullOrder(Pack& pack_, size_t leg_, std::string_view reason_) {
    OrderObjectT& order = *pack_._orders[leg_];
    if (!order.is_order_confirmed() || order.is_response_pending()) {
        return;
    }
    if (order.cancel_order()) {
        WriteLog("[ORDER-OUT] {} leg={} token={} {} CANCEL uid={} price={} qty={} | {}\n",
                 PackName(pack_), leg_, _tokens[leg_], SideName(pack_._sides[leg_]), order.get_uid(), order.get_open_price(), order.get_open_qty(), reason_);
    }
}

// One line per OMS response: what arrived, and the order state it left behind.
void RatioLegStrategy::LogResponse(const Pack& pack_, size_t leg_, const oms_transaction& response_) {
    const OrderObjectT& order         = *pack_._orders[leg_];
    const int32_t       code          = response_.hdr_.transaction_code;
    const uint32_t      uid           = response_.hdr_.uid_.id_;
    const int32_t       price         = response_.packet_.price_;
    const int32_t       quantity      = response_.packet_.quantity_;
    const int32_t       errorCode     = response_.hdr_.error_code;
    const int32_t       reasonCode    = response_.hdr_.reason_code;
    const int32_t       exchangeError = response_.packet_.exchange_error_code;
    WriteLog("[ORDER-IN] {} leg={} token={} {} {}({}) uid={} price={} qty={} err={}/{}/{} -> state=0x{:x} open={}@{} filled={} liveUid={}\n",
             PackName(pack_), leg_, _tokens[leg_], SideName(pack_._sides[leg_]), TransactionName(code), code, uid, price, quantity,
             errorCode, reasonCode, exchangeError, order.get_current_state(), order.get_open_qty(), order.get_open_price(), order.get_filled_qty(), order.get_uid());
}

// Logs only when the reason changes, so a quiet strategy still says why it is quiet.
void RatioLegStrategy::SetBidGate(Pack& pack_, BidGate gate_, size_t thinLeg_) {
    if (pack_._bidGate == gate_) {
        return;
    }
    static constexpr std::array<std::string_view, 7> GateNames{"UNKNOWN", "OPEN", "DONE", "HEDGE_DEPTH", "BID_DEPTH", "NO_PRICE", "BELOW_TARGET"};

    const SpreadQuote spreadQuote = QuoteSpread(pack_._cache);
    WriteLog("[BID-GATE] {} {} -> {} | target={} market={} valid={} lots=[{}] book=[{}]\n",
             PackName(pack_), GateNames[static_cast<size_t>(pack_._bidGate)], GateNames[static_cast<size_t>(gate_)],
             pack_._params._targetSpread, spreadQuote._spread, spreadQuote._valid, LotsText(pack_), BookText());
    if (thinLeg_ != NO_LEG) {
        const ORDER_SIDE takeSide = pack_._cache._takeSide[thinLeg_];
        WriteLog("[BID-GATE] {} thin hedge leg={} token={} side={} ordersOk={} pricesOk={} quantity={} required={} (OrderDepth={} PriceDepth={})\n",
                 PackName(pack_), thinLeg_, _tokens[thinLeg_], BookSideName(takeSide),
                 CheckOrderDepth(_quote[thinLeg_], _orderDepth, takeSide), CheckPriceDepth(_quote[thinLeg_], _priceDepth, takeSide),
                 GetAvailableQuantity(_quote[thinLeg_], _orderDepth, takeSide), pack_._cache._requiredHedgeDepth[thinLeg_], _orderDepth, _priceDepth);
    }
    pack_._bidGate = gate_;
}

// ═══ Order book helpers ══════════════════════════════════════════════════════

auto RatioLegStrategy::GetPrice(const Quote& quote_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int {
    return (side_ == BUY_SIDE) ? quote_.message.bid_levels[levelIndex_].price : quote_.message.ask_levels[levelIndex_].price;
}

auto RatioLegStrategy::GetQuantity(const Quote& quote_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int {
    return (side_ == BUY_SIDE) ? quote_.message.bid_levels[levelIndex_].qty : quote_.message.ask_levels[levelIndex_].qty;
}

auto RatioLegStrategy::GetOrderCount(const Quote& quote_, ORDER_SIDE side_, size_t levelIndex_) noexcept -> int {
    return (side_ == BUY_SIDE) ? quote_.message.bid_levels[levelIndex_].order_count_ : quote_.message.ask_levels[levelIndex_].order_count_;
}

auto RatioLegStrategy::GetAvailableQuantity(const Quote& quote_, size_t depth_, ORDER_SIDE side_) noexcept -> int {
    int totalQuantity = 0;
    for (size_t levelIndex = 0; levelIndex < depth_; ++levelIndex) {
        totalQuantity += GetQuantity(quote_, side_, levelIndex);
    }
    return totalQuantity;
}

auto RatioLegStrategy::CheckOrderDepth(const Quote& quote_, size_t depth_, ORDER_SIDE side_) noexcept -> bool {
    for (size_t levelIndex = 0; levelIndex < depth_; ++levelIndex) {
        if (GetOrderCount(quote_, side_, levelIndex) <= 0) {
            return false;
        }
    }
    return true;
}

auto RatioLegStrategy::CheckPriceDepth(const Quote& quote_, size_t depth_, ORDER_SIDE side_) noexcept -> bool {
    for (size_t levelIndex = 0; levelIndex < depth_; ++levelIndex) {
        if (GetPrice(quote_, side_, levelIndex) <= 0) {
            return false;
        }
    }
    return true;
}

auto RatioLegStrategy::GetEventCount() -> int {
    int count   = _eventCount;
    _eventCount = 0;
    return count;
}
