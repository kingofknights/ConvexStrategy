#include "Ratio2LegStrategy.hpp"

#include "AlgoBase.hpp"
#include "MinixStrategy.hpp"
#include "Utils.hpp"
#include "oms_api.hpp"

#define FMT_HEADER_ONLY
#include <fmt/format.h>
#include <fmt/ostream.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

Ratio2LegStrategy::Ratio2LegStrategy(MinixStrategy* ms_, uint32_t strategyId_, const nlohmann::json& json_) : _ms(ms_), _strategyId(strategyId_) {
    ParamUpdate(json_);
    _uid.composite_id_.client_id   = static_cast<uint32_t>(_ms->client);
    _uid.composite_id_.strategy_id = strategyId_;

    for (int token : _tokens) {
        _ms->subscribeProduct(token, _ms->flags);
    }

    size_t hedgeLeg = _biddingLeg == 0 ? 1 : 0;

    _longSide[0]  = _longSideParam[_biddingLeg];
    _longSide[1]  = _longSideParam[hedgeLeg];
    _shortSide[0] = _shortSideParam[_biddingLeg];
    _shortSide[1] = _shortSideParam[hedgeLeg];

    _tokens[0] = _tokensParam[_biddingLeg];
    _tokens[1] = _tokensParam[hedgeLeg];

    ProductDetails details[2];
    ms_->getProductDetails(_tokens[0], details[0]);
    ms_->getProductDetails(_tokens[1], details[1]);

    _gap      = std::abs(details[0].strike_price_ - details[1].strike_price_) / 100;
    _lotSize  = details[0].lot_size_;
    _tickSize = details[0].tick_size_;

    _longOrders._order[0]  = std::make_unique<OrderObjectT>(_tokens[0], _longSide[0], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    _longOrders._order[1]  = std::make_unique<OrderObjectT>(_tokens[1], _longSide[1], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    _shortOrders._order[0] = std::make_unique<OrderObjectT>(_tokens[0], _shortSide[0], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);
    _shortOrders._order[1] = std::make_unique<OrderObjectT>(_tokens[1], _shortSide[1], _lotSize, _ms->client, _ms->algoid, _ms->omsid, ORDER_TYPE::LIMIT_ORDER_TYPE, _ms);

    writeLog("PD Token {} K {} BLQ {} Tick {}\n", static_cast<int>(details[0].product_id_), static_cast<int>(details[0].strike_price_), static_cast<int>(details[0].lot_size_), static_cast<int>(details[0].tick_size_));
    writeLog("PD Token {} K {} BLQ {} Tick {}\n", static_cast<int>(details[1].product_id_), static_cast<int>(details[1].strike_price_), static_cast<int>(details[1].lot_size_), static_cast<int>(details[1].tick_size_));
    writeLog("{} Gap {} Tick {} BLQ {}\n Long Side [{} {}]\n Short Side [{} {}]\n\n",
             __PRETTY_FUNCTION__, _gap, _tickSize, _lotSize,
             static_cast<int>(_longSide[0]), static_cast<int>(_longSide[1]),
             static_cast<int>(_shortSide[0]), static_cast<int>(_shortSide[1]));

    writeLog(" ---------------------------------\n _long [{} | {}]\n _short[{} | {}]\n\n",
             static_cast<int>(_longOrders._order[0]->get_side()), static_cast<int>(_longOrders._order[1]->get_side()),
             static_cast<int>(_shortOrders._order[0]->get_side()), static_cast<int>(_shortOrders._order[1]->get_side()));
    ;
}
Ratio2LegStrategy::~Ratio2LegStrategy() {}
void Ratio2LegStrategy::ParamUpdate(const nlohmann::json& json_) {
    // ── Legs ─────────────────────────────────────────────────────────────────
    auto legs = json_["Legs"];

    std::vector<TokenInfo> legsInfo;
    for (auto& item : legs) {
        int         token = item.value("Token", 0);
        std::string side  = item.value("Side", "BUY");
        size_t      legId = item.value("LegID", 0U);
        bool        bid   = item.value("EnableBid", false);

        if (bid && legId > 0) {
            _biddingLeg = legId - 1;
        }
        legsInfo.push_back(TokenInfo{
            ._token = token,
            ._side  = side == "BUY" ? BUY_SIDE : SELL_SIDE,
            ._bid   = bid,
        });
        writeLog(" ID {} Token {} Side {} bid {}\n", legId, token, side, bid);
    }
    _biddingLeg = std::min(_biddingLeg, static_cast<size_t>(1));

    for (size_t i = 0; i < 2; ++i) {
        TokenInfo info     = legsInfo[i];
        _tokensParam[i]    = info._token;
        _longSideParam[i]  = info._side;
        _shortSideParam[i] = info._side == SELL_SIDE ? BUY_SIDE : SELL_SIDE;
    }

    // ── Params ────────────────────────────────────────────────────────────────
    if (json_.contains("Params")) {
        const auto& parmas = json_["Params"];

        _longParam._quantity      = parmas.value("LongBuySoQ", 0);
        _longParam._totalQuantity = parmas.value("LongBuyQty", 0);
        _longParam._spread        = parmas.value("LongBuyPrice", 0.0F) * 100.0F;

        _shortParam._quantity      = parmas.value("ShortSellSoQ", 0);
        _shortParam._totalQuantity = parmas.value("ShortSellQty", 0);
        _shortParam._spread        = parmas.value("ShortSellPrice", 0.0F) * 100.0F;

        _minTickChange      = parmas.value("TickSize", 0U);
        _orderDepth         = parmas.value("OrderDepth", 1U);
        _priceDepth         = parmas.value("PriceDepth", 1U);
        _allowedBidDepth    = parmas.value("AllowedBidDepth", 1U);
        _thresholdQty       = parmas.value("ThresholdQty", 100);
        _allowedSlippage    = parmas.value("AllowedSlippage", 0) * 100;
        _tradeGear          = parmas.value("TradeGear", 0);
        _marketOrderRetries = parmas.value("MarketOrderRetries", 0U);

        _orderDepth      = std::min<size_t>(_orderDepth, 5U);
        _priceDepth      = std::min<size_t>(_priceDepth, 5U);
        _allowedBidDepth = std::min<size_t>(_allowedBidDepth, 5U);

        writeLog(" Params  _longParam._quantity        {}  _longParam._totalQuantity  {}  _longParam._spread         {}  _shortParam._quantity      {}  _shortParam._totalQuantity {}  _shortParam._spread        {}\n",
                 _longParam._quantity, _longParam._totalQuantity, _longParam._spread,
                 _shortParam._quantity, _shortParam._totalQuantity, _shortParam._spread);
    }

    // ── Strategy meta ─────────────────────────────────────────────────────────
    if (json_.contains("Strategy")) {
        const auto& strategy = json_["Strategy"];
        _isBidding           = strategy.value("IsBidding", false);
        std::string status   = strategy.value("Status", "None");
        _active              = status == "Applied";
        writeLog(" Strategy: IsBidding={}\n", _isBidding);
    }
}
void Ratio2LegStrategy::OnTick(const Quote& event_, int64_t nowTs_) {
    int  token  = event_.header.product_id;
    bool status = token == _tokens[0] or token == _tokens[1];
    if (not status) {
        return;
    }
    size_t index  = token == _tokens[0] ? 0 : 1;
    _qoute[index] = event_;
    if (not _active) {
        return;
    }

    auto evaluateBidding = [&](MarketBidding& orders_, ParamLots& param_, ORDER_SIDE mainSide_, ORDER_SIDE hedgeSide_, WindRate rate_, std::string name_) {
        bool hedgeLegOk   = CheckOrderDepth(_qoute[1], _orderDepth, hedgeSide_) &&
                            CheckPriceDepth(_qoute[1], _priceDepth, hedgeSide_);
        bool biddingLegOk = CheckPriceDepth(_qoute[0], _allowedBidDepth, mainSide_);

        double thresholdPct = _thresholdQty > 0 ? _thresholdQty : 100.0;
        double targetQty    = (param_._quantity * _lotSize) * (thresholdPct / 100.0);
        bool   qtyOk        = GetAvailableQuantity(_qoute[1], _orderDepth, hedgeSide_) >= targetQty;

        if (hedgeLegOk && biddingLegOk && qtyOk) {
            OrderBiddingLogic(orders_, param_, rate_, name_);
        } else {
            orders_._order[0]->cancel_order();
        }
    };

    evaluateBidding(_longOrders, _longParam, _longSide[0], _longSide[1], GetBCmp(), "Long");
    evaluateBidding(_shortOrders, _shortParam, _shortSide[0], _shortSide[1], GetSCmp(), "Short");
}

void Ratio2LegStrategy::OnBcast(const aef::infra::product::product_data& pd_, int64_t nowTs_) {
    int  token  = pd_.product_id_;
    bool status = token == _tokens[0] or token == _tokens[1];
    if (not status or not _active) {
        return;
    }
}
void Ratio2LegStrategy::OnOrderResponse(const oms_transaction& resp_) {
    writeLog("{} response id {} Side {} transaction_code {} price {} quantity {}\n",
             __FUNCTION__, resp_.hdr_.uid_.id_, static_cast<int>(resp_.packet_.flags_.order_side),
             resp_.hdr_.transaction_code, resp_.packet_.price_, resp_.packet_.quantity_);

    if (_strategyId != resp_.hdr_.uid_.composite_id_.strategy_id) {
        return;
    }
    _ms->sendOrderResponse(resp_, "2LegRatio");
    bool traded   = resp_.hdr_.transaction_code == OMS_TRADE;
    auto price    = resp_.packet_.price_;
    auto quantity = resp_.packet_.quantity_;
    auto lot      = quantity / _lotSize;
    auto value    = static_cast<uint64_t>(price * quantity);
    // Long Entry Orders
    writeLog(" id long {} {} {}\n", _longOrders._uniqueID[0], _longOrders._uniqueID[1], resp_.hdr_.uid_.id_);
    writeLog(" id short {} {} {}\n", _shortOrders._uniqueID[0], _shortOrders._uniqueID[1], resp_.hdr_.uid_.id_);

    auto handleTrade = [&](MarketBidding& object_, size_t index_) -> void {
        object_._order[index_]->handle_confirmation(resp_);
        object_._tradedLot[index_] += traded ? lot : 0;
        object_._tradeValue[index_] += traded ? value : 0;
    };

    auto checkSlippage = [&](MarketBidding& object_) {
        if (traded && object_._lastBiddingFillPrice > 0 && _allowedSlippage > 0) {
            double leg0FillPrice  = object_._lastBiddingFillPrice;
            double leg1FillPrice  = price;
            double executedSpread = leg0FillPrice - leg1FillPrice;
            double slippage       = 0.0;
            if (&object_ == &_longOrders) {
                slippage = executedSpread - _longParam._spread;
            } else {
                slippage = _shortParam._spread - executedSpread;
            }
            if (slippage > _allowedSlippage) {
                writeLog("[SLIPPAGE] Slippage {} > AllowedSlippage {}. Stopping strategy.\n", slippage, _allowedSlippage);
                _active = false;
                _longOrders._order[0]->cancel_order();
                _shortOrders._order[0]->cancel_order();
            }
        }
    };

    auto processOrderResponse = [&](MarketBidding& object_, size_t index_) -> bool {
        if (resp_.hdr_.uid_.id_ == object_._uniqueID[index_]) {
            handleTrade(object_, index_);
            if (0 == index_) {
                if (traded) {
                    object_._lastBiddingFillPrice = price;
                }
            } else {
                checkSlippage(object_);
            }
            return true;
        }
        return false;
    };

    if (processOrderResponse(_longOrders, 0)) {
    } else if (processOrderResponse(_longOrders, 1)) {
    } else if (processOrderResponse(_shortOrders, 0)) {
    } else if (processOrderResponse(_shortOrders, 1)) {
    }

    if (traded) {
        writeLog(" long traded {} {}\n", _longOrders._tradedLot[0], _longOrders._tradedLot[1]);
        writeLog(" short traded {} {}\n", _shortOrders._tradedLot[0], _shortOrders._tradedLot[1]);

        SecondOrderBidding(_longOrders, _longParam);
        SecondOrderBidding(_shortOrders, _shortParam);
    }
}

auto Ratio2LegStrategy::GetBCmp() const -> WindRate {
    int ask0 = GetPrice(_qoute[0], SELL_SIDE, 0);
    int bid0 = GetPrice(_qoute[0], BUY_SIDE, 0);
    int ask1 = GetPrice(_qoute[1], SELL_SIDE, 0);
    int bid1 = GetPrice(_qoute[1], BUY_SIDE, 0);

    double spread = 0.0;
    spread += (_longSide[0] == BUY_SIDE) ? -ask0 : bid0;
    spread += (_longSide[1] == BUY_SIDE) ? -ask1 : bid1;

    return WindRate{
        ._price = {
            (_longSide[0] == BUY_SIDE) ? ask0 : bid0,
            (_longSide[1] == BUY_SIDE) ? ask1 : bid1,
        },
        ._spread = static_cast<float>(spread),
    };
}
auto Ratio2LegStrategy::GetSCmp() const -> WindRate {
    int ask0 = GetPrice(_qoute[0], SELL_SIDE, 0);
    int bid0 = GetPrice(_qoute[0], BUY_SIDE, 0);
    int ask1 = GetPrice(_qoute[1], SELL_SIDE, 0);
    int bid1 = GetPrice(_qoute[1], BUY_SIDE, 0);

    double spread = 0.0;
    spread += (_shortSide[0] == BUY_SIDE) ? -ask0 : bid0;
    spread += (_shortSide[1] == BUY_SIDE) ? -ask1 : bid1;

    return WindRate{
        ._price = {
            (_shortSide[0] == BUY_SIDE) ? ask0 : bid0,
            (_shortSide[1] == BUY_SIDE) ? ask1 : bid1,
        },
        ._spread = static_cast<float>(spread),
    };
}

auto Ratio2LegStrategy::GetStrategyID() const -> uint32_t { return _strategyId; }

auto Ratio2LegStrategy::GetGap() const -> int { return _gap; }

auto Ratio2LegStrategy::GetLongTradedLots() const -> int {
    int leg0TradedPacks = _longOrders._tradedLot[0];
    int leg1TradedPacks = _longOrders._tradedLot[1];
    int totalPacks      = std::min(leg0TradedPacks, leg1TradedPacks);
    return totalPacks;
}

auto Ratio2LegStrategy::GetShortTradedLots() const -> int {
    int leg0TradedPacks = _shortOrders._tradedLot[0];
    int leg1TradedPacks = _shortOrders._tradedLot[1];
    int totalPacks      = std::min(leg0TradedPacks, leg1TradedPacks);
    return totalPacks;
}

auto Ratio2LegStrategy::GetBATP() const -> double {
    uint64_t leg0TradedValue = _longOrders._tradeValue[0];
    uint64_t leg1TradedValue = _longOrders._tradeValue[1];
    uint64_t leg0TradedLots  = static_cast<uint64_t>(_longOrders._tradedLot[0]);
    uint64_t leg1TradedLots  = static_cast<uint64_t>(_longOrders._tradedLot[1]);

    if (leg0TradedLots == 0 or leg1TradedLots == 0 or _lotSize == 0) {
        return 0.0;
    }
    double leg0AveragePrice = static_cast<double>(leg0TradedValue) / static_cast<double>(leg0TradedLots * _lotSize);
    double leg1AveragePrice = static_cast<double>(leg1TradedValue) / static_cast<double>(leg1TradedLots * _lotSize);
    return leg0AveragePrice - leg1AveragePrice;
}

auto Ratio2LegStrategy::GetSATP() const -> double {
    uint64_t leg0TradedValue = _shortOrders._tradeValue[0];
    uint64_t leg1TradedValue = _shortOrders._tradeValue[1];
    uint64_t leg0TradedLots  = static_cast<uint64_t>(_shortOrders._tradedLot[0]);
    uint64_t leg1TradedLots  = static_cast<uint64_t>(_shortOrders._tradedLot[1]);

    if (leg0TradedLots == 0 or leg1TradedLots == 0 or _lotSize == 0) {
        return 0.0;
    }
    double leg0AveragePrice = static_cast<double>(leg0TradedValue) / static_cast<double>(leg0TradedLots * _lotSize);
    double leg1AveragePrice = static_cast<double>(leg1TradedValue) / static_cast<double>(leg1TradedLots * _lotSize);
    return leg0AveragePrice - leg1AveragePrice;
}

auto Ratio2LegStrategy::GetRLP() const -> double {
    double totalRLP = 0.0;
    for (size_t i = 0; i < 2; ++i) {
        int64_t  buyQty  = 0;
        uint64_t buyVal  = 0;
        int64_t  sellQty = 0;
        uint64_t sellVal = 0;

        if (_longSide[i] == BUY_SIDE) {
            buyQty += static_cast<int64_t>(_longOrders._tradedLot[i]) * _lotSize;
            buyVal += _longOrders._tradeValue[i];
        } else {
            sellQty += static_cast<int64_t>(_longOrders._tradedLot[i]) * _lotSize;
            sellVal += _longOrders._tradeValue[i];
        }

        if (_shortSide[i] == BUY_SIDE) {
            buyQty += static_cast<int64_t>(_shortOrders._tradedLot[i]) * _lotSize;
            buyVal += _shortOrders._tradeValue[i];
        } else {
            sellQty += static_cast<int64_t>(_shortOrders._tradedLot[i]) * _lotSize;
            sellVal += _shortOrders._tradeValue[i];
        }

        double avgBuyPrice  = buyQty > 0 ? static_cast<double>(buyVal) / buyQty : 0.0;
        double avgSellPrice = sellQty > 0 ? static_cast<double>(sellVal) / sellQty : 0.0;

        if (buyQty > sellQty) {
            totalRLP += static_cast<double>(sellQty) * (avgSellPrice - avgBuyPrice);
        } else {
            totalRLP += static_cast<double>(buyQty) * (avgSellPrice - avgBuyPrice);
        }
    }
    return totalRLP;
}

auto Ratio2LegStrategy::GetCutPL() const -> double {
    return GetRLP();
}

auto Ratio2LegStrategy::GetM2M() const -> int {
    double totalM2M = 0.0;
    for (size_t i = 0; i < 2; ++i) {
        int64_t  buyQty  = 0;
        uint64_t buyVal  = 0;
        int64_t  sellQty = 0;
        uint64_t sellVal = 0;

        if (_longSide[i] == BUY_SIDE) {
            buyQty += static_cast<int64_t>(_longOrders._tradedLot[i]) * _lotSize;
            buyVal += _longOrders._tradeValue[i];
        } else {
            sellQty += static_cast<int64_t>(_longOrders._tradedLot[i]) * _lotSize;
            sellVal += _longOrders._tradeValue[i];
        }

        if (_shortSide[i] == BUY_SIDE) {
            buyQty += static_cast<int64_t>(_shortOrders._tradedLot[i]) * _lotSize;
            buyVal += _shortOrders._tradeValue[i];
        } else {
            sellQty += static_cast<int64_t>(_shortOrders._tradedLot[i]) * _lotSize;
            sellVal += _shortOrders._tradeValue[i];
        }

        double avgBuyPrice  = buyQty > 0 ? static_cast<double>(buyVal) / buyQty : 0.0;
        double avgSellPrice = sellQty > 0 ? static_cast<double>(sellVal) / sellQty : 0.0;

        int64_t netQty = buyQty - sellQty;
        if (netQty != 0) {
            double markPrice = 0.0;
            if (netQty > 0) {
                markPrice = _qoute[i].message.bid_levels[0].price;
            } else {
                markPrice = _qoute[i].message.ask_levels[0].price;
            }

            double avgPrice = netQty > 0 ? avgBuyPrice : avgSellPrice;
            totalM2M += static_cast<double>(netQty) * (markPrice - avgPrice);
        }
    }
    return static_cast<int>(totalM2M);
}

auto Ratio2LegStrategy::GetNetPL() const -> double {
    return GetRLP() + static_cast<double>(GetM2M());
}

auto Ratio2LegStrategy::GetFLP() const -> int {
    return _qoute[0].message.ltp_;
}
auto Ratio2LegStrategy::GetCost() const -> double {
    constexpr static double buyCostPercentage  = 0.002079734;
    constexpr static double sellCostPercentage = 0.000609734;

    std::array buyPrice = {
        _qoute[0].message.bid_levels[0].price,
        _qoute[1].message.bid_levels[0].price,
    };
    std::array sellPrice = {
        _qoute[0].message.ask_levels[0].price,
        _qoute[1].message.ask_levels[0].price,
    };

    double buyCost  = (static_cast<double>(buyPrice[0] + buyPrice[1]) * buyCostPercentage);
    double sellCost = (static_cast<double>(sellPrice[0] + sellPrice[1]) * sellCostPercentage);
    return (buyCost + sellCost);
}

void Ratio2LegStrategy::OrderBiddingLogic(MarketBidding& object_, ParamLots param_, WindRate rate_, std::string name_) {
    int firstTradedLots  = object_._tradedLot[0];
    int secondTradedLots = object_._tradedLot[1];
    if (firstTradedLots != secondTradedLots) {
        SecondOrderBidding(object_, param_);
        return;
    }
    if ((param_._spread < rate_._spread) or (object_._tradedLot[0]) >= param_._totalQuantity) {
        object_._order[0]->cancel_order();
        return;
        // Cancel;
    }

    OrderObjectPtrT& order = object_._order[0];

    int basePrice         = GetPrice(_qoute[0], order->get_side(), 0);
    int priceOffset       = _tradeGear * _tickSize;
    int marketPrice       = order->get_side() == BUY_SIDE ? (basePrice + priceOffset) : (basePrice - priceOffset);
    int currentPlacePrice = order->get_open_price();
    int diff              = std::abs(currentPlacePrice - marketPrice);
    int quantity          = param_._quantity * _lotSize;

    if (diff < (_minTickChange * _tickSize)) {
        writeLog("{} C M [{} {}] change {}  return\n", name_, currentPlacePrice, marketPrice, _minTickChange * _tickSize);
        return;
    }
    if (quantity <= 0) {
        writeLog("{} quantity {}  return\n", name_, quantity);
        return;
    }
    auto status = _ms->update_order(order, _tokens[0], marketPrice, quantity, _uid);
    if (status != 0) {
        writeLog("{} Order placed  _biddingLeg = {} price = {} quantity {} side {} uid {} client ID {} diff {} return {}\n",
                 __FUNCTION__, _biddingLeg, marketPrice, _lotSize * param_._quantity,
                 static_cast<int>(order->get_side()), _uid.id_, static_cast<uint32_t>(_uid.composite_id_.client_id), diff, status);
        object_._uniqueID[0] = _uid.id_;
        object_._windRate    = rate_;
    }
}
void Ratio2LegStrategy::SecondOrderBidding(MarketBidding& object_, ParamLots param_) {
    int firstTradedLots  = object_._tradedLot[0];
    int secondTradedLots = object_._tradedLot[1];
    int diff             = firstTradedLots - secondTradedLots;
    if (diff <= 0) {
        object_._hedgeRetryCount = 0;
        return;
    }

    if (_marketOrderRetries > 0 && object_._hedgeRetryCount >= _marketOrderRetries) {
        writeLog("[HEDGE RETRY EXHAUSTED] Retry count {} >= MarketOrderRetries {}. Terminating strategy & cancelling all orders.\n",
                 object_._hedgeRetryCount, _marketOrderRetries);
        _active = false;
        _longOrders._order[0]->cancel_order();
        _longOrders._order[1]->cancel_order();
        _shortOrders._order[0]->cancel_order();
        _shortOrders._order[1]->cancel_order();
        return;
    }

    writeLog("{} first {} second {} leg {} retryCount {}\n", __FUNCTION__, firstTradedLots, secondTradedLots, 1, object_._hedgeRetryCount);
    int              quantity          = std::min(diff, param_._quantity) * _lotSize;
    OrderObjectPtrT& order             = object_._order[1];
    int              currentPlacePrice = order->get_open_price();

    ORDER_SIDE hedgeMarketSide = order->get_side() == BUY_SIDE ? SELL_SIDE : BUY_SIDE;
    int        marketPrice     = GetPrice(_qoute[1], hedgeMarketSide, 0);

    if (marketPrice > 0 && marketPrice != currentPlacePrice) {
        auto status = _ms->update_order(order, _tokens[1], marketPrice, quantity, _uid);
        if (status != 0) {
            writeLog("{} placing hedge order side {} price {} quantity {} (retry {})\n",
                     __FUNCTION__, static_cast<int>(order->get_side()), marketPrice, quantity, object_._hedgeRetryCount + 1);
            object_._uniqueID[1] = _uid.id_;
            object_._hedgeRetryCount++;
        }
    }
}
[[nodiscard]] auto Ratio2LegStrategy::GetPrice(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].price : event_.message.ask_levels[index_].price;
}

[[nodiscard]] auto Ratio2LegStrategy::GetQuantity(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].qty : event_.message.ask_levels[index_].qty;
}

[[nodiscard]] auto Ratio2LegStrategy::GetOrderCount(const Quote& event_, ORDER_SIDE side_, size_t index_) const -> int {
    return side_ == BUY_SIDE ? event_.message.bid_levels[index_].order_count_ : event_.message.ask_levels[index_].order_count_;
}

[[nodiscard]] auto Ratio2LegStrategy::GetAvailableQuantity(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> int {
    int quantity = 0;
    for (size_t index = 0; index < depth_; ++index) {
        quantity += GetQuantity(event_, side_, index);
    }
    return quantity;
}

[[nodiscard]] auto Ratio2LegStrategy::CheckOrderDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool {
    int totalOrders = 0;
    for (size_t index = 0; index < 5; ++index) {
        totalOrders += GetOrderCount(event_, side_, index);
    }
    return static_cast<size_t>(totalOrders) >= depth_;
}

[[nodiscard]] auto Ratio2LegStrategy::CheckPriceDepth(const Quote& event_, size_t depth_, ORDER_SIDE side_) const -> bool {
    size_t validPriceLevels = 0;
    for (size_t index = 0; index < 5; ++index) {
        if (GetPrice(event_, side_, index) > 0) {
            validPriceLevels++;
        }
    }
    return validPriceLevels >= depth_;
}
