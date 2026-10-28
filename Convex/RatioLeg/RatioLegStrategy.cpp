#include "RatioLegStrategy.hpp"

#include "MinixStrategy.hpp"
#include "ProductInfo.hpp"
#include "RatioLegHelpers.hpp"
#include "Utils.hpp"

#include <fmt/format.h>
#include <oms_api.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace {
    auto ToPaise(double rupees_) -> float { return static_cast<float>(std::round(rupees_ * 100.0)); }
}  // namespace

using namespace ratioleg;

// ═══ Lifecycle ═══════════════════════════════════════════════════════════════

RatioLegStrategy::RatioLegStrategy(MinixStrategy* hub_, uint32_t strategyId_, int32_t interface_, const nlohmann::json& json_, size_t legCount_, bool hasStrikeGap_)
    : _hub(hub_),
      _strategyId(strategyId_),
      _interface(interface_),
      _legCount(legCount_),
      _hasStrikeGap(hasStrikeGap_) {
    _logFile = OpenLogFile(json_.contains("Strategy") ? json_["Strategy"].value("UserId", 0U) : 0U, _strategyId);
    WriteLog(">>> [INIT] Opened log file strategyId={}\n", _strategyId);

    _longPack._packSide  = BUY_SIDE;
    _shortPack._packSide = SELL_SIDE;
    _ratios.fill(1);

    const auto& legs = json_.at("Legs");

    for (size_t leg = 0; leg < _legCount and leg < legs.size(); ++leg) {
        _tokens[leg]            = legs[leg].value("Token", 0);
        ORDER_SIDE side         = legs[leg].value("Side", "BUY") == "BUY" ? BUY_SIDE : SELL_SIDE;
        ORDER_SIDE oppositeSide = Opposite(side);

        _longPack._sides[leg]         = side;
        _longPack._oppositeSide[leg]  = oppositeSide;
        _shortPack._sides[leg]        = oppositeSide;
        _shortPack._oppositeSide[leg] = side;

        if (!_hub->getProductDetails(_tokens[leg], _product_data[leg])) {
            WriteLog("[INIT] leg {} token {}: product details unavailable, strategy not created\n", leg, _tokens[leg]);
            throw std::runtime_error(fmt::format("strategy {}: leg {} token {} has no product details", _strategyId, leg, _tokens[leg]));
        }
        _isOption[leg] = _product_data[leg].opt_type_ != aef::infra::product::OPTION_TYPE::FUTXX;
    }

    if (json_.contains("Ratio") && json_["Ratio"].contains("LegRatios")) {
        const auto& legRatios = json_["Ratio"]["LegRatios"];
        for (size_t leg = 0; leg < _legCount && leg < legRatios.size(); ++leg) {
            int sign                     = _longPack._sides[leg] == ORDER_SIDE::BUY_SIDE ? -1 : 1;
            _ratios[leg]                 = legRatios[leg].get<int>();
            _longPack._signedRatio[leg]  = _ratios[leg] * sign * 1;
            _shortPack._signedRatio[leg] = _ratios[leg] * sign * -1;
        }
    }

    for (size_t leg = 0; leg < _legCount; ++leg) {
        WriteLog("[{}LegRatios] {}Leg Token: {} Symbol: {} strike = {}, lot = {} ticksize = {} ratio = {} longSide = {}\n",
                 _legCount, leg, _tokens[leg], _product_data[leg].symbol, int(_product_data[leg].strike_price_), int(_product_data[leg].lot_size_), int(_product_data[leg].tick_size_), _ratios[leg], SideName(_longPack._sides[leg]));
    }

    std::strncpy(_tradeTracer._symbol, _product_data[BiddingLeg].symbol, sizeof(_tradeTracer._symbol) - 1);
    _strikeGap          = std::abs(_product_data[0].strike_price_ - _product_data[1].strike_price_);
    _lotSize            = _product_data[0].lot_size_;
    _tickSize           = _product_data[0].tick_size_;
    _spread._strategyId = _strategyId;
    _spread._gap        = static_cast<float>(_strikeGap / 100);

    ParamUpdate(json_);

    _clientUid.composite_id_.client_id   = static_cast<uint32_t>(_hub->_clientId);
    _clientUid.composite_id_.strategy_id = _strategyId;

    for (Pack* pack : {&_longPack, &_shortPack}) {
        for (size_t leg = 0; leg < _legCount; ++leg) {
            pack->_orders[leg] = std::make_unique<OrderObjectT>(_tokens[leg], pack->_sides[leg], _lotSize, _hub->_clientId, _hub->_algoId, _hub->_omsId, ORDER_TYPE::LIMIT_ORDER_TYPE, _hub);
        }
        pack->_reverse = std::make_unique<OrderObjectT>(_tokens[BiddingLeg], Opposite(pack->_sides[BiddingLeg]), _lotSize, _hub->_clientId, _hub->_algoId, _hub->_omsId, ORDER_TYPE::LIMIT_ORDER_TYPE, _hub);
    }

    for (size_t leg = 0; leg < _legCount; ++leg) {
        const bool subscribed = _hub->subscribeProduct(_tokens[leg], _hub->_feedFlags);
        WriteLog("[FEED] leg={} token={} subscribe={}\n", leg, _tokens[leg], subscribed ? "ok" : "FAILED");
        _hub->getLastQuote(_tokens[leg], _quote[leg]);
    }
}

RatioLegStrategy::~RatioLegStrategy() {
    WriteLog(">>> [DESTROY] Closing log file\n");
}

void RatioLegStrategy::ParamUpdate(const nlohmann::json& json_) {
    if (const auto found = json_.find("Params"); found != json_.end()) {
        const auto& params = *found;

        _longPack._singleOrderPacks  = params.value("LongBuySoQ", 0);
        _longPack._totalAllowedPacks = params.value("LongBuyQty", 0);
        _longPack._targetSpread      = ToPaise(params.value("LongBuyPrice", 0.0));

        _shortPack._singleOrderPacks  = params.value("ShortSellSoQ", 0);
        _shortPack._totalAllowedPacks = params.value("ShortSellQty", 0);
        _shortPack._targetSpread      = ToPaise(params.value("ShortSellPrice", 0.0));

        _orderDepth        = params.value("OrderDepth", 1);
        _priceDepth        = std::min<size_t>(params.value("PriceDepth", 1U), aef::infra::quote::QUOTE_LEVELS);
        _hedgeDepthPercent = params.value("ThresholdQty", 100);
        _allowedSlippage   = params.value("AllowedSlippage", 0);
        _allowedBidDepth   = std::min<size_t>(params.value("AllowedBidDepth", 0U), aef::infra::quote::QUOTE_LEVELS);
        _minChangeTick     = std::max(params.value("TickSize", 2), 2);

        WriteLog(
            "[RatioLeg] StrategyId: {} | Params parsed: "
            "\nLongBuySoQ: {}, \nLongBuyQty: {}, \nLongBuyPrice: {}; "
            "\nShortSellSoQ: {}, \nShortSellQty: {}, \nShortSellPrice: {}; "
            "\nOrderDepth: {}, \nPriceDepth: {}, "
            "\nThresholdQty: {}, \nAllowedSlippage: {}, \nAllowedBidDepth: {}, \nTickSize: {}, \n_biddingLeg: {}\n",
            _strategyId, _longPack._singleOrderPacks, _longPack._totalAllowedPacks, _longPack._targetSpread,
            _shortPack._singleOrderPacks, _shortPack._totalAllowedPacks, _shortPack._targetSpread, _orderDepth, _priceDepth,
            _hedgeDepthPercent, _allowedSlippage, _allowedBidDepth, _minChangeTick, BiddingLeg);
    }
    _depthCheckedNs = 0;
}

void RatioLegStrategy::SetStatus(StrategyStatus status_) {
    if (status_ == StrategyStatus_INACTIVE) {
        Stop("status INACTIVE requested");
        return;
    }
    _status  = status_;
    _deleted = false;
    WriteLog("[RatioLeg:SetStatus] strat={} _status={} ({})\n", _strategyId, static_cast<int>(_status), StrategyStatusToString(_status));
}

void RatioLegStrategy::Stop(std::string_view reason_) {
    _status = StrategyStatus_ACTIVE;
    WriteLog("[RatioLeg:Stop] strat={} _status=ACTIVE reason='{}' long_lots=[{}] short_lots=[{}]\n",
             _strategyId, reason_, LotsText(_longPack), LotsText(_shortPack));
    for (Pack* pack : {&_longPack, &_shortPack}) {
        Pull(*pack, BiddingLeg, reason_);
    }
}

void RatioLegStrategy::Forget(std::string_view reason_) {
    WriteLog("[RatioLeg:Forget] strat={} reason='{}' dropping long_lots=[{}] short_lots=[{}]\n", _strategyId, reason_, LotsText(_longPack), LotsText(_shortPack));
    for (Pack* pack : {&_longPack, &_shortPack}) {
        pack->_tradedLots.fill(0);
        pack->_tradedValue.fill(0);
        pack->_reportedLots.fill(0);
        pack->_reportedValue.fill(0);
        pack->_tradedSpread = 0.0;
        pack->_tradedPack   = 0;
        pack->_hedgeMode    = false;
        pack->_earlyHedgeUid = 0;
        pack->_reverseLots   = 0;
        pack->_reverseValue  = 0;
        if (pack->_reverse->is_order_confirmed() && !pack->_reverse->is_response_pending()) {
            pack->_reverse->cancel_order();
        }
    }
    Stop(reason_);
}

auto RatioLegStrategy::IsIdle() const -> bool {
    for (const Pack* pack : {&_longPack, &_shortPack}) {
        if (pack->_reverseLots != 0 || pack->_reverse->get_current_state() != IDLE) {
            return false;
        }
        for (size_t leg = 0; leg < _legCount; ++leg) {
            if (pack->_orders[leg]->get_current_state() != IDLE || pack->_tradedLots[leg] != pack->_tradedPack * _ratios[leg]) {
                return false;
            }
        }
    }
    return true;
}

// ═══ Events ══════════════════════════════════════════════════════════════════

void RatioLegStrategy::OnTick(const Quote& quote_) {
    const size_t leg = FindLegIndex(quote_.header.product_id);
    if (leg == NoLeg) {
        return;
    }
    _quote[leg] = quote_;
    _eventCount += 1;
    if (leg == BiddingLeg && quote_.message.updatetype_.update_type == 'S') {
        TryEarlyHedge(_longPack, quote_.message.updatetype_.update_side);
        TryEarlyHedge(_shortPack, quote_.message.updatetype_.update_side);
    }
    for (Pack* pack : {&_longPack, &_shortPack}) {
        if (pack->_reverseLots > 0) {
            Reverse(*pack);
        }
    }
    if (_longPack._hedgeMode) {
        EvaluateHedgeMode(_longPack);
    } else {
        EvaluateBidding(_longPack);
    }
    if (_shortPack._hedgeMode) {
        EvaluateHedgeMode(_shortPack);
    } else {
        EvaluateBidding(_shortPack);
    }
}

void RatioLegStrategy::OnOrderResponse(const oms_transaction& response_) {
    const size_t leg  = FindLegIndex(response_.packet_.product_id_);
    const auto   side = static_cast<ORDER_SIDE>(response_.packet_.flags_.order_side);
    const auto   uid  = uint32_t{response_.hdr_.uid_.id_};
    const auto   code = int32_t{response_.hdr_.transaction_code};
    if (leg == NoLeg || (side != BUY_SIDE && side != SELL_SIDE)) {
        WriteLog("[ORDER-IN] ignored: token={} side={} code={} uid={} (not a leg of this strategy)\n", int{response_.packet_.product_id_}, static_cast<int>(side), code, uid);
        return;
    }

    for (Pack* reversing : {&_longPack, &_shortPack}) {
        OrderObjectT& reverse = *reversing->_reverse;
        if (leg == BiddingLeg && reverse.get_uid() == uid) {
            reverse.handle_confirmation(response_);
            LogResponse(*reversing, leg, reverse, response_, true);
            if (code == OMS_TRADE) {
                OnReverseTrade(*reversing, response_);
            }
            return;
        }
    }

    Pack&         pack  = (side == _longPack._sides[leg]) ? _longPack : _shortPack;
    OrderObjectT& order = *pack._orders[leg];
    if (order.get_uid() == uid) {
        order.handle_confirmation(response_);
    }
    LogResponse(pack, leg, order, response_, false);
    if (code == OMS_TRADE) {
        OnTrade(pack, leg, response_);
    }
}

// ═══ Decision: everything is worked out again on every event ═════════════════

void RatioLegStrategy::EvaluateBidding(Pack& pack_) {
    if (pack_._reverseLots > 0) {
        Pull(pack_, BiddingLeg, "reversing leg 0");
        return;
    }
    // Leg 0 hedged early on an 'S' tick: it may be filled already, so no reprice or cancel. The early hedges follow the book until a fill decides.
    const OrderObjectT& bid = *pack_._orders[BiddingLeg];
    if (bid.get_uid() == pack_._earlyHedgeUid && bid.get_current_state() != IDLE) {
        HedgeEarly(pack_);
        return;
    }
    // Every leg is level here, so a hedge still working is an early hedge whose leg 0 went away unfilled.
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (leg != BiddingLeg) {
            Pull(pack_, leg, "early hedge without a leg 0 fill");
        }
    }
    if (not IsActive()) {
        Pull(pack_, BiddingLeg, "Strategy is not active");
        return;
    }
    LegArrayT<int>             prices{};
    LegArrayT<int>             deeper{};
    const std::optional<float> market = MarketSpread(pack_, &prices, 0);
    if (!market or !MarketSpread(pack_, &deeper, 2)) {
        Pull(pack_, BiddingLeg, "no price on a leg");
        return;
    }

    // Cushion: what the hedge legs lose two levels deeper, the level they are hedged at. The bidding leg's own depth is ours to set, so it is left out.
    double cushion = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (leg != BiddingLeg) {
            cushion += static_cast<double>(prices[leg] - deeper[leg]) * pack_._signedRatio[leg];
        }
    }

    // P1 = p0 + (target + cushion - market) / signedRatio0, rounded to the tick away from the market (BUY down, SELL up).
    const double p1         = prices[BiddingLeg] + ((pack_._targetSpread + cushion - *market) / pack_._signedRatio[BiddingLeg]);
    const double ticks      = p1 / _tickSize;
    const int    finalPrice = static_cast<int>(pack_._sides[BiddingLeg] == BUY_SIDE ? std::floor(ticks) : std::ceil(ticks)) * _tickSize;
    if (finalPrice <= 0) {
        Pull(pack_, BiddingLeg, "target needs a price <= 0");
        return;
    }

    int trade     = pack_._tradedLots[0];
    int tradePack = pack_._tradedPack;

    int quantity = std::min({
                       (pack_._totalAllowedPacks * _ratios[0]) - trade,               // lots left overall
                       ((tradePack + pack_._singleOrderPacks) * _ratios[0]) - trade,  // lots left in this slice
                   }) *
                   _lotSize;
    if (quantity <= 0) {
        Pull(pack_, BiddingLeg, "nothing left to bid");
        return;
    }
    const int price = finalPrice;
    // Never rest deeper than AllowedBidDepth levels on our own side; improving or crossing the touch is fine.
    // AllowedBidDepth 0, or that level empty in a thin book: no limit.
    if (_allowedBidDepth > 0) {
        const int deepest = Price(_quote[BiddingLeg], pack_._sides[BiddingLeg], _allowedBidDepth - 1);
        if (deepest > 0 && (pack_._sides[BiddingLeg] == BUY_SIDE ? price < deepest : price > deepest)) {
            Pull(pack_, BiddingLeg, "deeper than AllowedBidDepth levels");
            return;
        }
    }
    // A resting bid within _minChangeTick ticks stays only when it is more passive than the new price; one that overpays always moves.
    const OrderObjectT& order   = *pack_._orders[BiddingLeg];
    const int           open    = order.get_open_price();
    const bool          passive = pack_._sides[BiddingLeg] == BUY_SIDE ? open <= price : open >= price;
    if (order.is_order_confirmed() && order.get_open_qty() == quantity && passive && std::abs(price - open) < _minChangeTick * _tickSize) {
        return;
    }
    UpdateOrder(pack_, BiddingLeg, price, quantity, "BID");
}

void RatioLegStrategy::EvaluateHedgeMode(Pack& pack_) {
    LegArrayT<int> tradedPacks{};
    for (size_t leg = 0; leg < _legCount; ++leg) {
        tradedPacks[leg] = pack_._tradedLots[leg] / _ratios[leg];
    }
    const auto minMax = std::ranges::minmax(std::span{tradedPacks}.first(_legCount));
    if (minMax.min == minMax.max) {
        pack_._hedgeMode  = false;
        pack_._tradedPack = minMax.max;
        return;
    }
    pack_._hedgeMode = true;

    const int      target = std::min(minMax.max, pack_._totalAllowedPacks);
    LegArrayT<int> missingLots{};
    for (size_t leg = 0; leg < _legCount; ++leg) {
        missingLots[leg] = (target * _ratios[leg]) - pack_._tradedLots[leg];
    }
    SendHedges(pack_, missingLots, "HEDGE");
}

// A synthetic trade on leg 0's own side that leaves our bid better than that side's top has probably filled it:
// hedge the whole open bid now instead of waiting for the fill. Once per leg 0 uid.
void RatioLegStrategy::TryEarlyHedge(Pack& pack_, int updateSide_) {
    const OrderObjectT& order = *pack_._orders[BiddingLeg];
    const ORDER_SIDE    side  = pack_._sides[BiddingLeg];
    if (not IsActive() || pack_._hedgeMode || updateSide_ != side || !order.is_order_confirmed() || order.is_response_pending() || order.get_uid() == pack_._earlyHedgeUid) {
        return;
    }
    const int top  = Price(_quote[BiddingLeg], side, 0);
    const int open = order.get_open_price();
    if (top > 0 && (side == BUY_SIDE ? open <= top : open >= top)) {
        return;
    }
    if (!SpreadPresent(pack_, open)) {
        WriteLog("[EARLY-HEDGE] {} 'S' tick: leg 0 {} uid={} open {}@{} beats top {}, spread gone: no hedge, the fill decides\n",
                 PackName(pack_), SideName(side), order.get_uid(), order.get_open_qty(), open, top);
        return;
    }
    pack_._earlyHedgeUid = order.get_uid();
    WriteLog("[EARLY-HEDGE] {} 'S' tick: leg 0 {} uid={} open {}@{} beats top {}: hedging now\n",
             PackName(pack_), SideName(side), order.get_uid(), order.get_open_qty(), open, top);
    HedgeEarly(pack_);
}

// Hedges for leg 0's traded plus open lots, at the opposite side's index 2 like any hedge.
// Leg 0 itself is not chased here: EvaluateHedgeMode does that once a hedge fill puts the hedge legs ahead.
void RatioLegStrategy::HedgeEarly(Pack& pack_) {
    const int packs = (pack_._tradedLots[BiddingLeg] + (pack_._orders[BiddingLeg]->get_open_qty() / _lotSize)) / _ratios[BiddingLeg];
    LegArrayT<int> missingLots{};
    for (size_t leg = 0; leg < _legCount; ++leg) {
        missingLots[leg] = leg == BiddingLeg ? 0 : (packs * _ratios[leg]) - pack_._tradedLots[leg];
    }
    SendHedges(pack_, missingLots, "EARLY-HEDGE");
}

// Sends every leg with missing lots at the opposite side's index 2, highest price first.
void RatioLegStrategy::SendHedges(Pack& pack_, const LegArrayT<int>& missingLots_, std::string_view reason_) {
    LegArrayT<int>    prices{};
    LegArrayT<size_t> order{};
    for (size_t leg = 0; leg < _legCount; ++leg) {
        prices[leg] = Price(_quote[leg], pack_._oppositeSide[leg], 2);
        order[leg]  = leg;
    }
    const std::span legs = std::span{order}.first(_legCount);
    std::ranges::stable_sort(legs, std::ranges::greater{}, [&](size_t leg_) { return prices[leg_]; });
    for (const size_t leg : legs) {
        if (missingLots_[leg] > 0) {
            UpdateOrder(pack_, leg, prices[leg], missingLots_[leg] * _lotSize, reason_);
        }
    }
}

// Sends leg 0's unwanted lots back at its own side's index 2 (a BUY sells into the bids), repriced every tick until filled.
void RatioLegStrategy::Reverse(Pack& pack_) {
    OrderObjectT& order    = *pack_._reverse;
    const int     price    = Price(_quote[BiddingLeg], pack_._sides[BiddingLeg], 2);
    const int     oldPrice = order.get_open_price();
    if (const uint32_t uid = _hub->UpdateOrder(order, price, pack_._reverseLots * _lotSize, _clientUid); uid != 0) {
        WriteLog("[ORDER-OUT] {} leg={} token={} {} REVERSE uid={} price={}->{} qty={} | lots=[{}] book=[{}]\n",
                 PackName(pack_), BiddingLeg, _tokens[BiddingLeg], SideName(Opposite(pack_._sides[BiddingLeg])), uid, oldPrice, price, pack_._reverseLots * _lotSize, LotsText(pack_), BookText());
    }
}

void RatioLegStrategy::OnReverseTrade(Pack& pack_, const oms_transaction& response_) {
    const int32_t price = response_.packet_.price_;
    const int32_t lots  = std::min(response_.packet_.quantity_ / _lotSize, pack_._reverseLots);
    if (lots <= 0) {
        WriteLog("[REVERSE] {} fill {}@{} with no lots left to reverse: ignored\n", PackName(pack_), int32_t{response_.packet_.quantity_}, price);
        return;
    }
    const uint64_t entry  = pack_._reverseValue * static_cast<uint64_t>(lots) / static_cast<uint64_t>(pack_._reverseLots);
    const uint64_t exit   = static_cast<uint64_t>(price) * static_cast<uint64_t>(lots * _lotSize);
    const bool     bought = pack_._sides[BiddingLeg] == BUY_SIDE;
    const auto     buy    = static_cast<double>(bought ? entry : exit);
    const auto     sell   = static_cast<double>(bought ? exit : entry);
    _unwindPnL += sell - buy - (buy * (_isOption[BiddingLeg] ? OptionBuyCost : FutureBuyCost)) - (sell * (_isOption[BiddingLeg] ? OptionSellCost : FutureSellCost));

    pack_._reverseLots -= lots;
    pack_._reverseValue -= entry;
    WriteLog("[REVERSE] {} leg 0 {} lots back @{}, {} lots left to reverse, unwind P&L {}\n", PackName(pack_), lots, price, pack_._reverseLots, _unwindPnL);
    SendTracer(pack_, response_, pack_._targetSpread / 100.0F, static_cast<float>(price) / 100.0F, lots, 0, "Reverse");
}

void RatioLegStrategy::OnTrade(Pack& pack_, size_t leg_, const oms_transaction& response_) {
    const int32_t  price    = response_.packet_.price_;
    const int32_t  quantity = response_.packet_.quantity_;
    const int32_t  lots     = quantity / _lotSize;
    const uint64_t value    = static_cast<uint64_t>(price) * static_cast<uint64_t>(quantity);

    if (leg_ == BiddingLeg) {
        // Hedges already out for this fill (early on an 'S' tick, or leg 0 chased behind hedge fills): reversing would leave them naked.
        bool hedged = uint32_t{response_.hdr_.uid_.id_} == pack_._earlyHedgeUid;
        for (size_t leg = 0; leg < _legCount; ++leg) {
            hedged = hedged || pack_._tradedLots[leg] * _ratios[BiddingLeg] > pack_._tradedLots[BiddingLeg] * _ratios[leg];
        }
        if (!hedged && !SpreadPresent(pack_, price)) {
            pack_._reverseLots += lots;
            pack_._reverseValue += static_cast<uint64_t>(price) * static_cast<uint64_t>(lots * _lotSize);
            WriteLog("[TRADE EVENT] index: 0, token: {}, {} {}@{}: spread gone at the hedge legs' top, reversing {} lots, book=[{}]\n",
                     _tokens[BiddingLeg], PackName(pack_), quantity, price, pack_._reverseLots, BookText());
            Pull(pack_, BiddingLeg, "spread gone, reversing leg 0");
            Reverse(pack_);
            return;
        }
    }

    pack_._tradedLots[leg_] += lots;
    pack_._tradedValue[leg_] += value;

    const ORDER_SIDE side = pack_._sides[leg_];
    WriteLog("[TRADE EVENT] index: {}, token: {}, side: {} ({}), price: {}, quantity: {}, pack: {}, lots: [{}]\n",
             leg_, _tokens[leg_], SideName(side), static_cast<int>(side), price, quantity, PackName(pack_), LotsText(pack_));
    if (quantity % _lotSize != 0) {
        WriteLog("[TRADE EVENT] WARNING quantity {} is not a multiple of lot size {}; {} units not counted\n", quantity, _lotSize, quantity % _lotSize);
    }

    EvaluateHedgeMode(pack_);
    if (!pack_._hedgeMode) {
        double spread  = 0.0;
        bool   allLegs = true;
        for (size_t leg = 0; leg < _legCount; ++leg) {
            if (pack_._tradedLots[leg] == 0) {
                allLegs = false;
                break;
            }
            const double average = static_cast<double>(pack_._tradedValue[leg]) / (static_cast<double>(pack_._tradedLots[leg]) * _lotSize);
            spread += (pack_._sides[leg] == BUY_SIDE ? -average : average) * _ratios[leg];
        }
        pack_._tradedSpread = allLegs ? AdjustGap(spread) : 0.0;
        ReportPacks(pack_, response_);
    }
}

// One Trade Tracer row per batch of packs completed since the last row, with the slippage against the target.
void RatioLegStrategy::ReportPacks(Pack& pack_, const oms_transaction& response_) {
    const int newPacks = pack_._tradedPack - (pack_._reportedLots[BiddingLeg] / _ratios[BiddingLeg]);
    if (newPacks <= 0) {
        return;
    }

    double spread = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const int32_t  lots     = pack_._tradedLots[leg] - pack_._reportedLots[leg];
        const uint64_t value    = pack_._tradedValue[leg] - pack_._reportedValue[leg];
        const int32_t  newLots  = newPacks * _ratios[leg];
        const uint64_t newValue = value * static_cast<uint64_t>(newLots) / static_cast<uint64_t>(lots);
        const double   average  = static_cast<double>(newValue) / (static_cast<double>(newLots) * _lotSize);
        spread += (pack_._sides[leg] == BUY_SIDE ? -average : average) * _ratios[leg];

        pack_._reportedLots[leg] += newLots;
        pack_._reportedValue[leg] += newValue;
    }

    const auto  traded   = static_cast<float>(AdjustGap(spread));
    const float slippage = pack_._targetSpread - traded;
    const bool  slipped  = std::round(slippage) > static_cast<float>(_allowedSlippage);
    SendTracer(pack_, response_, pack_._targetSpread / 100.0F, traded / 100.0F, newPacks, slipped ? slippage / 100.0F : 0, slipped ? "Slippage" : "");

    if (slipped) {
        WriteLog("[SLIPPAGE] {} slippage {} paise > AllowedSlippage {} paise: stopping\n", PackName(pack_), slippage, _allowedSlippage);
        Stop("slippage above AllowedSlippage");
    }
}

void RatioLegStrategy::SendTracer(const Pack& pack_, const oms_transaction& response_, float price_, float ltp_, int ltq_, float slippage_, std::string_view message_) {
    _tradeTracer._orderId      = response_.packet_.exchange_order_id;
    _tradeTracer._time         = response_.hdr_.exchange_timestamp;
    _tradeTracer._qtyRemaining = pack_._totalAllowedPacks - pack_._tradedPack;
    _tradeTracer._strategyId   = _strategyId;
    _tradeTracer._side         = pack_._packSide;
    _tradeTracer._price        = price_;
    _tradeTracer._ltp          = ltp_;
    _tradeTracer._ltq          = ltq_;
    _tradeTracer._slippage     = slippage_;
    std::memset(_tradeTracer._message, 0, sizeof(_tradeTracer._message));
    std::memcpy(_tradeTracer._message, message_.data(), std::min(message_.size(), sizeof(_tradeTracer._message) - 1));

    WriteLog("Tracer [{}Leg] symbol {} stratId {} orderId {} time {} side {} price {} ltp {} ltq {} slippage {} message '{}'\n",
             _legCount, _tradeTracer._symbol, _strategyId, _tradeTracer._orderId, _tradeTracer._time, _tradeTracer._side, _tradeTracer._price, _tradeTracer._ltp,
             _tradeTracer._ltq, _tradeTracer._slippage, message_);
    _hub->SendToUi(UiMessageCode_TRADE_TRACER, _interface, _tradeTracer);
}

// ═══ UI ══════════════════════════════════════════════════════════════════════

auto RatioLegStrategy::SpreadUpdate(uint64_t nowNs_) -> std::optional<StrategySpreadUpdate> {
    double rlp  = _unwindPnL;
    double m2m  = 0.0;
    double cost = 0.0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        int64_t  buyQuantity  = 0;
        int64_t  sellQuantity = 0;
        uint64_t buyValue     = 0;
        uint64_t sellValue    = 0;
        for (const Pack* pack : {&_longPack, &_shortPack}) {
            const int64_t quantity = static_cast<int64_t>(pack->_tradedLots[leg]) * _lotSize;
            if (pack->_sides[leg] == BUY_SIDE) {
                buyQuantity += quantity;
                buyValue += pack->_tradedValue[leg];
            } else {
                sellQuantity += quantity;
                sellValue += pack->_tradedValue[leg];
            }
        }
        const double avgBuy   = buyQuantity > 0 ? static_cast<double>(buyValue) / buyQuantity : 0.0;
        const double avgSell  = sellQuantity > 0 ? static_cast<double>(sellValue) / sellQuantity : 0.0;
        const double buyRate  = _isOption[leg] ? OptionBuyCost : FutureBuyCost;
        const double sellRate = _isOption[leg] ? OptionSellCost : FutureSellCost;
        rlp += (static_cast<double>(std::min(buyQuantity, sellQuantity)) * (avgSell - avgBuy)) - (static_cast<double>(buyValue) * buyRate) - (static_cast<double>(sellValue) * sellRate);

        const int     bid  = _quote[leg].message.bid_levels[0].price;
        const int     ask  = _quote[leg].message.ask_levels[0].price;
        const int64_t net  = buyQuantity - sellQuantity;
        const int     mark = net > 0 ? bid : ask;
        if (net != 0 && mark > 0) {
            m2m += static_cast<double>(net) * (mark - (net > 0 ? avgBuy : avgSell));
        }
        cost += _ratios[leg] * ((bid * buyRate) + (ask * sellRate));
    }
    _spread._rlp      = static_cast<float>(rlp) / 100.0F;
    _spread._cutPL    = _spread._rlp;
    _spread._m2m      = static_cast<float>(m2m) / 100.0F;
    _spread._netPL    = _spread._rlp + _spread._m2m;
    _spread._cost     = static_cast<float>(cost) / 100.0F;
    _spread._bcmp     = MarketSpread(_longPack, nullptr, 0).value_or(0.0F) / 100.0F;
    _spread._scmp     = MarketSpread(_shortPack, nullptr, 0).value_or(0.0F) / 100.0F;
    _spread._bTrQ     = (_longPack._tradedPack);
    _spread._sTrQ     = (_shortPack._tradedPack);
    _spread._bATP     = static_cast<float>(_longPack._tradedSpread) / 100.0F;
    _spread._sATP     = static_cast<float>(_shortPack._tradedSpread) / 100.0F;
    _spread._status   = _status;
    _spread._flp      = static_cast<float>(_quote[BiddingLeg].message.ltp_) / 100.0F;
    _spread._reserved = _lastSent._reserved;

    if (_spread == _lastSent && nowNs_ - _lastSentNs < SpreadResendNs) {
        return std::nullopt;
    }
    _spread._reserved = static_cast<float>(std::exchange(_eventCount, 0));
    _lastSent         = _spread;
    _lastSentNs       = nowNs_;
    return _spread;
}

// ═══ Pack math ═══════════════════════════════════════════════════════════════

auto RatioLegStrategy::MarketSpread(const Pack& pack_, LegArrayT<int>* prices_, size_t index_) const -> std::optional<float> {
    long long raw = 0;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const int price = Price(_quote[leg], pack_._oppositeSide[leg], index_);
        if (price <= 0) {
            return std::nullopt;
        }
        if (prices_ != nullptr) {
            (*prices_)[leg] = price;
        }
        raw += static_cast<long long>(price) * pack_._signedRatio[leg];
    }
    return static_cast<float>(AdjustGap(static_cast<double>(raw)));
}

// Leg 0 at price_ and the hedge legs at the opposite side's top: does the pack still make its target?
// A hedge leg with no price cannot be hedged, so the spread counts as gone.
auto RatioLegStrategy::SpreadPresent(const Pack& pack_, int price_) const -> bool {
    double raw = static_cast<double>(price_) * pack_._signedRatio[BiddingLeg];
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (leg != BiddingLeg) {
            const int price = Price(_quote[leg], pack_._oppositeSide[leg], 0);
            if (price <= 0) {
                return false;
            }
            raw += static_cast<double>(price) * pack_._signedRatio[leg];
        }
    }
    return AdjustGap(raw) >= pack_._targetSpread;
}

auto RatioLegStrategy::AdjustGap(double spread_) const -> double {
    if (!_hasStrikeGap) {
        return spread_;
    }
    return spread_ < 0 ? spread_ + _strikeGap : spread_ - _strikeGap;
}

auto RatioLegStrategy::FindLegIndex(int token_) const -> size_t {
    for (size_t leg = 0; leg < _legCount; ++leg) {
        if (_tokens[leg] == token_) {
            return leg;
        }
    }
    return NoLeg;
}

// ═══ Orders ══════════════════════════════════════════════════════════════════

// Logs only what was really sent: UpdateOrder returns 0 for a refused or unchanged order.
void RatioLegStrategy::UpdateOrder(Pack& pack_, size_t leg_, int price_, int quantity_, std::string_view reason_) {
    OrderObjectT&          order    = *pack_._orders[leg_];
    const std::string_view action   = OrderAction(order);
    const int              oldPrice = order.get_open_price();
    const int              oldQty   = order.get_open_qty();
    if (const uint32_t uid = _hub->UpdateOrder(order, price_, quantity_, _clientUid); uid != 0) {
        WriteLog("[ORDER-OUT] {} leg={} token={} {} {} uid={} price={}->{} qty={}->{} | {} lots=[{}] book=[{}]\n",
                 PackName(pack_), leg_, _tokens[leg_], SideName(pack_._sides[leg_]), action, uid, oldPrice, price_, oldQty, quantity_, reason_, LotsText(pack_), BookText());
    }
}

void RatioLegStrategy::Pull(Pack& pack_, size_t leg_, std::string_view reason_) {
    OrderObjectT& order = *pack_._orders[leg_];
    if (!order.is_order_confirmed() || order.is_response_pending()) {
        return;
    }
    if (order.cancel_order()) {
        WriteLog("[ORDER-OUT] {} leg={} token={} {} CANCEL uid={} price={} qty={} | {}\n",
                 PackName(pack_), leg_, _tokens[leg_], SideName(pack_._sides[leg_]), order.get_uid(), order.get_open_price(), order.get_open_qty(), reason_);
    }
}

// ═══ Logs ════════════════════════════════════════════════════════════════════

// Logs when the gate changes. While the market meets the target but a hedge leg is thin, logs every time: a missed entry.

void RatioLegStrategy::LogResponse(const Pack& pack_, size_t leg_, const OrderObjectT& order_, const oms_transaction& response_, bool isCut_) {
    const int32_t code = response_.hdr_.transaction_code;
    WriteLog("[ORDER-IN] {} leg={} token={} {}{} {}({}) uid={} price={} qty={} err={}/{}/{} -> state=0x{:x} open={}@{} filled={} liveUid={}\n",
             PackName(pack_), leg_, _tokens[leg_], SideName(isCut_ ? Opposite(pack_._sides[leg_]) : pack_._sides[leg_]), isCut_ ? " CUT" : "",
             TransactionName(code), code, uint32_t{response_.hdr_.uid_.id_}, int32_t{response_.packet_.price_}, int32_t{response_.packet_.quantity_},
             int32_t{response_.hdr_.error_code}, int32_t{response_.hdr_.reason_code}, int32_t{response_.packet_.exchange_error_code},
             order_.get_current_state(), order_.get_open_qty(), order_.get_open_price(), order_.get_filled_qty(), order_.get_uid());
}

void RatioLegStrategy::WriteTimestamp() const {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    const long secondsOfDay = (now.tv_sec + _utcOffsetSeconds) % 86400;
    fmt::print(_logFile.get(), "{:02}:{:02}:{:02}.{:06} ", secondsOfDay / 3600, secondsOfDay / 60 % 60, secondsOfDay % 60, now.tv_nsec / 1000);
}

auto RatioLegStrategy::BookText() const -> std::string {
    std::string text;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        fmt::format_to(std::back_inserter(text), "{}{}/{}", leg == 0 ? "" : " ", int{_quote[leg].message.bid_levels[0].price}, int{_quote[leg].message.ask_levels[0].price});
    }
    return text;
}

auto RatioLegStrategy::LotsText(const Pack& pack_) const -> std::string {
    const int   biddingPacks = pack_._tradedLots[BiddingLeg] / _ratios[BiddingLeg];
    std::string text;
    for (size_t leg = 0; leg < _legCount; ++leg) {
        const int needed = leg == BiddingLeg ? pack_._totalAllowedPacks * _ratios[leg] : biddingPacks * _ratios[leg];
        fmt::format_to(std::back_inserter(text), "{}{}/{}", leg == 0 ? "" : " ", pack_._tradedLots[leg], needed);
    }
    return text;
}

// ═══ Book ════════════════════════════════════════════════════════════════════

auto RatioLegStrategy::Price(const Quote& quote_, ORDER_SIDE side_, size_t level_) -> int {
    return side_ == BUY_SIDE ? quote_.message.bid_levels[level_].price : quote_.message.ask_levels[level_].price;
}

auto RatioLegStrategy::HasLevels(const Quote& quote_, ORDER_SIDE side_, size_t depth_) -> bool {
    for (size_t level = 0; level < depth_; ++level) {
        if (Price(quote_, side_, level) <= 0) {
            return false;
        }
    }
    return true;
}

auto RatioLegStrategy::QuantityUpTo(const Quote& quote_, ORDER_SIDE side_, size_t depth_) -> int {
    int quantity = 0;
    for (size_t level = 0; level < depth_; ++level) {
        quantity += side_ == BUY_SIDE ? quote_.message.bid_levels[level].qty : quote_.message.ask_levels[level].qty;
    }
    return quantity;
}

auto RatioLegStrategy::OrderCount(const Quote& quote_, ORDER_SIDE side_) -> int {
    int orders = 0;
    for (size_t level = 0; level < aef::infra::quote::QUOTE_LEVELS; ++level) {
        orders += side_ == BUY_SIDE ? quote_.message.bid_levels[level].order_count_ : quote_.message.ask_levels[level].order_count_;
    }
    return orders;
}

auto RatioLegStrategy::DescribeDepth(const Quote& quote_, ORDER_SIDE side_, int minOrders_, size_t depth_, double minQuantity_) -> std::string {
    if (const int orders = OrderCount(quote_, side_); orders < minOrders_) {
        return fmt::format("{} orders in the book < {}", orders, minOrders_);
    }
    for (size_t level = 0; level < depth_; ++level) {
        if (Price(quote_, side_, level) <= 0) {
            return fmt::format("level {} has no price", level);
        }
    }
    if (const int quantity = QuantityUpTo(quote_, side_, depth_); quantity < minQuantity_) {
        return fmt::format("quantity {} over {} levels < required {}", quantity, depth_, minQuantity_);
    }
    return "ok";
}
