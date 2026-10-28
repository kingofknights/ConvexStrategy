// Dry run of the ratio strategy against a simulated platform and exchange.
//
// Links the real MinixStrategy, RatioLegStrategy and vendor order_instance against a fake
// AlgoBase (no libAlgoBase.so) and a fake trade DB (no Postgres), feeds GUI config through the chunked
// UI protocol, ticks the book, and fills orders the way the exchange would. Every scenario asserts on what the
// exchange saw, not on strategy internals.
//
// Build and run from the repo root (one line):
//   g++ -std=c++20 -O1 -g -Wall -Wextra -DFMT_HEADER_ONLY -I Convex -I Convex/RatioLeg -I Convex/ManualOrder -I Convex/SquareOff
//       -I vendor/minix/include -I vendor/minix -I vendor tests/ratio_dry_run.cpp Convex/MinixStrategy.cpp Convex/RatioLeg/RatioLeg*.cpp
//       Convex/ManualOrder/ManualOrder.cpp Convex/SquareOff/SquareOff.cpp vendor/minix/src/order_instance.cpp
//       -o build/ratio_dry_run && (cd build && ./ratio_dry_run)

#include "MinixStrategy.hpp"

#include <cassert>
#include <cstdio>
#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            std::fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            std::exit(1);                                                             \
        }                                                                             \
    } while (0)

// ═══ Simulated exchange ══════════════════════════════════════════════════════

namespace exchange {

    constexpr int TICK = 5;

    struct Book {
        int bid = 0, ask = 0;
    };

    struct LiveOrder {
        oms_transaction request;  // last request, carries uid / side / token
        int             price    = 0;
        int             openQty  = 0;
        int             filled   = 0;
        int             strategy = 0;
    };

    std::map<int, ProductDetails>  products;
    std::map<int, Book>            books;
    std::map<uint32_t, LiveOrder>  live;      // uid -> resting order
    std::deque<oms_transaction>    outbox;    // requests the strategy sent, not yet processed
    std::vector<oms_transaction>   sent;      // every request the strategy sent
    std::map<std::pair<int, int>, long> position;  // (strategy, token) -> signed quantity
    std::vector<aef::infra::ui_cmd::UIStruct> uiPackets;
    std::set<int>                  subscribed;
    int                            vendorRefusalLogs = 0;
    std::map<int32_t, int32_t>     dbPositions;  // what the fake trade DB returns to square-off
    MinixStrategy*                 hub               = nullptr;

    // New orders on rejectToken are acked, then rejected with rejectCode: once, or every time while rejectForever.
    // While rejectChanges, modifies and cancels on rejectToken are rejected too (no ack first).
    int  rejectToken   = 0;
    int  rejectCode    = EXCHG_ERROR;
    bool rejectForever = false;
    int  rejectCount   = 0;
    bool rejectChanges = false;

    auto Respond(const oms_transaction& request_, int code_, int price_, int quantity_) -> void {
        oms_transaction response    = request_;
        response.hdr_.transaction_code = code_;
        response.packet_.price_     = price_;
        response.packet_.quantity_  = quantity_;
        hub->OnOrderResponse(response);
    }

    auto Reject(const oms_transaction& request_) -> void {
        oms_transaction reject = request_;
        reject.hdr_.error_code = EXCHG_ERROR;
        Respond(reject, OMS_REQ_REJ, request_.packet_.price_, request_.packet_.quantity_);
    }

    auto Trade(uint32_t uid_, int price_, int quantity_) -> void {
        LiveOrder order = live.at(uid_);
        const int sign  = order.request.packet_.flags_.order_side == BUY_SIDE ? 1 : -1;
        const int token = order.request.packet_.product_id_;
        position[{order.strategy, token}] += sign * quantity_;
        live[uid_].openQty -= quantity_;
        live[uid_].filled += quantity_;
        if (live[uid_].openQty == 0) live.erase(uid_);
        Respond(order.request, OMS_TRADE, price_, quantity_);
    }

    // A resting order that crosses the touch trades in full at the touch.
    auto MatchCrossing() -> void {
        for (bool traded = true; traded;) {
            traded = false;
            for (auto& [uid, order] : live) {
                const Book& book  = books[order.request.packet_.product_id_];
                const bool  isBuy = order.request.packet_.flags_.order_side == BUY_SIDE;
                if (isBuy ? (book.ask > 0 && order.price >= book.ask) : (book.bid > 0 && order.price <= book.bid)) {
                    Trade(uid, isBuy ? book.ask : book.bid, order.openQty);
                    traded = true;
                    break;  // map changed
                }
            }
        }
    }

    // An IOC that did not trade in full on arrival is cancelled for the rest.
    auto CancelUnfilledIoc() -> void {
        for (auto order = live.begin(); order != live.end();) {
            if (order->second.request.packet_.flags_.order_type == IOC_ORDER_TYPE) {
                const oms_transaction request = order->second.request;
                const int             openQty = order->second.openQty;
                order                         = live.erase(order);
                Respond(request, OMS_ORDER_CANCELLED, request.packet_.price_, openQty);
                return;  // the response may have changed the map
            }
            ++order;
        }
    }

    // Process every request the strategy sent, including the ones its reactions send.
    auto Pump() -> void {
        while (!outbox.empty()) {
            const oms_transaction request = outbox.front();
            outbox.pop_front();
            const uint32_t uid = request.hdr_.uid_.id_;
            if (rejectChanges && request.packet_.product_id_ == rejectToken && request.hdr_.transaction_code != OMS_PLACE_REQ) {
                oms_transaction reject = request;
                reject.hdr_.error_code = rejectCode;
                ++rejectCount;
                Respond(reject, OMS_REQ_REJ, request.packet_.price_, request.packet_.quantity_);
                continue;
            }
            switch (request.hdr_.transaction_code) {
                case OMS_PLACE_REQ:
                    if (request.packet_.product_id_ == rejectToken) {
                        Respond(request, OMS_ORDER_PLACED, request.packet_.price_, request.packet_.quantity_);
                        oms_transaction reject = request;
                        reject.hdr_.error_code = rejectCode;
                        ++rejectCount;
                        if (!rejectForever) rejectToken = 0;
                        Respond(reject, OMS_REQ_REJ, request.packet_.price_, request.packet_.quantity_);
                        break;
                    }
                    live[uid] = LiveOrder{request, request.packet_.price_, request.packet_.quantity_, 0, static_cast<int>(request.hdr_.uid_.composite_id_.strategy_id)};
                    Respond(request, OMS_ORDER_PLACED, request.packet_.price_, request.packet_.quantity_);
                    Respond(request, OMS_ORDER_CONFIRMED, request.packet_.price_, request.packet_.quantity_);
                    break;
                case OMS_REPLACE_REQ: {
                    if (!live.count(uid)) {  // filled while the modify was in flight
                        Reject(request);
                        break;
                    }
                    LiveOrder& order = live.at(uid);  // packet quantity is open + filled
                    order.request    = request;
                    order.price      = request.packet_.price_;
                    order.openQty    = request.packet_.quantity_ - order.filled;
                    Respond(request, OMS_ORDER_MODIFY_PLACED, request.packet_.price_, request.packet_.quantity_);
                    Respond(request, OMS_ORDER_MODIFY_CONFIRMED, request.packet_.price_, request.packet_.quantity_);
                    break;
                }
                case OMS_CANCEL_REQ:
                    if (!live.count(uid)) {  // filled while the cancel was in flight
                        Reject(request);
                        break;
                    }
                    live.erase(uid);
                    Respond(request, OMS_ORDER_CANCEL_ACCEPTED, request.packet_.price_, request.packet_.quantity_);
                    Respond(request, OMS_ORDER_CANCELLED, request.packet_.price_, request.packet_.quantity_);
                    break;
                default:
                    CHECK(false && "unexpected request");
            }
            MatchCrossing();
            CancelUnfilledIoc();
        }
    }

    // pump_ = false leaves the strategy's requests unanswered, as if the exchange were slow.
    // bidLevels_ / askLevels_ price levels, each with 3 orders and levelQty_; level n is n ticks away from the touch.
    auto Publish(const Quote& quote_, bool pump_) -> void {
        books[quote_.header.product_id] = Book{quote_.message.bid_levels[0].price, quote_.message.ask_levels[0].price};
        hub->OnTick(quote_);
        if (pump_) {
            MatchCrossing();
            Pump();
        }
    }

    auto BookQuote(int token_, int bid_, int ask_, int levelQty_ = 300,
                   int bidLevels_ = aef::infra::quote::QUOTE_LEVELS, int askLevels_ = aef::infra::quote::QUOTE_LEVELS) -> Quote {
        Quote quote{};
        quote.header.product_id = token_;
        for (int level = 0; level < aef::infra::quote::QUOTE_LEVELS; ++level) {
            if (bid_ > 0 && level < bidLevels_) quote.message.bid_levels[level] = {levelQty_, bid_ - level * TICK, 3, 0};
            if (ask_ > 0 && level < askLevels_) quote.message.ask_levels[level] = {levelQty_, ask_ + level * TICK, 3, 0};
        }
        return quote;
    }

    auto Tick(int token_, int bid_, int ask_, bool pump_ = true, int levelQty_ = 300,
              int bidLevels_ = aef::infra::quote::QUOTE_LEVELS, int askLevels_ = aef::infra::quote::QUOTE_LEVELS) -> void {
        Publish(BookQuote(token_, bid_, ask_, levelQty_, bidLevels_, askLevels_), pump_);
    }

    // The market trades through our passive order: fill quantity_ of it at its own price.
    auto FillPassive(int strategy_, int token_, int quantity_) -> void {
        for (auto& [uid, order] : live) {
            if (order.strategy == strategy_ && order.request.packet_.product_id_ == token_) {
                Trade(uid, order.price, quantity_);  // before any queued request, as a fill racing it would
                Pump();
                return;
            }
        }
        CHECK(false && "no resting order to fill");
    }

    auto LiveOrdersOf(int strategy_) -> int {
        int count = 0;
        for (const auto& [uid, order] : live) count += order.strategy == strategy_;
        return count;
    }

}  // namespace exchange

// ═══ Fake platform (stands in for libAlgoBase.so) ════════════════════════════

AlgoBase::AlgoBase(ContextHandle) {}
AlgoBase::~AlgoBase() = default;
std::string AlgoBase::get_strategy_config_file() { return "dry_run_config.json"; }
bool        AlgoBase::subscribeProduct(const int32_t product_id_, const uint16_t) { return exchange::subscribed.insert(product_id_), true; }
bool        AlgoBase::getProductDetails(const int32_t product_id_, ProductDetails& value_) {
    const auto found = exchange::products.find(product_id_);
    if (found == exchange::products.end()) return false;
    value_ = found->second;
    return true;
}
bool AlgoBase::send_order(oms_transaction& request_) {
    exchange::sent.push_back(request_);
    exchange::outbox.push_back(request_);
    return true;
}
bool AlgoBase::getLastQuote(const int32_t, Quote&) { return false; }  // no snapshot: a new strategy waits for ticks
bool AlgoBase::sentoUI(const aef::infra::ui_cmd::UIStruct& packet_) { return exchange::uiPackets.push_back(packet_), true; }
void AlgoBase::log_debug(const char*) { ++exchange::vendorRefusalLogs; }
void AlgoBase::log_info(const char*) { ++exchange::vendorRefusalLogs; }
void AlgoBase::log_error(const char*) {}
void AlgoBase::OnTick(const Quote&) {}
void AlgoBase::OnOrderResponse(const oms_transaction&) {}
void AlgoBase::onStreamStatusChange(const int16_t, const int16_t, const int32_t) {}
void AlgoBase::onBcastData(const aef::infra::product::product_data&) {}
void AlgoBase::onTERData(const int32_t, const int32_t, const int32_t) {}
void AlgoBase::handleSecurityUpdate(const aef::infra::product::product_data&) {}
void AlgoBase::onUIRequest(const aef::infra::ui_cmd::UIStruct&) {}
void AlgoBase::handleTimerCalculations(const aef::infra::ui_cmd::UIStruct&) {}
int  AlgoBase::doWork() { return 0; }
uint32_t AlgoBase::get_next_order_id() { return 0; }  // only the auto-uid overload uses it; the strategy passes its own uid
void AlgoBase::onOIdata(const aef::infra::product::product_data&) {}

// Stands in for SquareOff/TradeDb.cpp (Postgres).
auto LoadTodayPositions(const SquareOff::DbConfig&) -> std::map<int32_t, int32_t> { return exchange::dbPositions; }

// ═══ Scenario helpers ════════════════════════════════════════════════════════

constexpr int LEG0 = 40745, LEG1 = 40741, LEG2 = 40751, MISSING_TOKEN = 99999;
constexpr int LOT  = 65;

// A real "Subscribed" payload captured from the GUI (2026-10-03), used verbatim as the base of every config.
// Long pack: SELL 3 x 23200, BUY 1 x 23100, BUY 2 x 23300. Short pack: the opposite sides.
constexpr std::string_view GUI_PAYLOAD = R"({"B-Buy":0.0,"B-Sell":0.0,"Legs":[{"EnableBid":true,"LegID":1,"Lots":65,"OrderType":"BID","Side":"SELL","StrikePrice":23200.0,"Token":40745},{"EnableBid":false,"LegID":2,"Lots":65,"OrderType":"IOC","Side":"BUY","StrikePrice":23100.0,"Token":40741},{"EnableBid":false,"LegID":3,"Lots":65,"OrderType":"IOC","Side":"BUY","StrikePrice":23300.0,"Token":40751}],"Params":{"AllowDifferentLotsScripts":false,"AllowDuplicates":false,"AllowedBidDepth":10,"AllowedSlippage":10,"BestBid":false,"ExecutionMode":"Normal","LongBuyPrice":100.0,"LongBuyQty":0,"LongBuySoQ":1,"MarketRetries":5,"NormalBid":true,"OrderDepth":4,"OrdersType":"Limit","PN_Add":0,"PN_Add_Enabled":false,"PN_Mult":1,"PN_Mult_Enabled":false,"PriceDepth":4,"PriceExecutionRange":"LeaveAsIs","ShortFlag":0,"ShortSellPrice":-0.05000000074505806,"ShortSellQty":5,"ShortSellSoQ":1,"ThresholdQty":0,"TickSize":1,"TimeToRevertBidMs":0,"TradeGear":0,"UnhedgedAction":"Ratio"},"Ratio":{"LegRatios":[3,1,2]},"S-Buy":0.0,"S-Sell":0.0,"Strategy":{"AllowDeliveryScripts":false,"IsBidding":true,"Name":"3LegRatio","Status":"Subscribed","StrategyId":3,"SubType":"3LegRatio","UserId":139}})";

// Sends the JSON the way the GUI does: a metadata packet, then 1500 B slices.
void SendConfig(const nlohmann::json& json_, bool pump_ = true) {
    const std::string text  = json_.dump();
    const size_t      slice = sizeof(aef::infra::ui_cmd::UIStruct::message);
    const size_t      count = (text.size() + slice - 1) / slice;

    aef::infra::ui_cmd::UIStruct packet{};
    packet.header.message_code = UiMessageCode_STRATEGY_CONFIG;
    const std::string meta     = nlohmann::json{{"packet_count", count}, {"timestamp", 0}}.dump();
    std::memcpy(packet.message, meta.data(), meta.size());
    exchange::hub->onUIRequest(packet);

    for (size_t offset = 0; offset < text.size(); offset += slice) {
        packet = {};
        packet.header.message_code = UiMessageCode_STRATEGY_CONFIG;
        std::memcpy(packet.message, text.data() + offset, std::min(slice, text.size() - offset));
        exchange::hub->onUIRequest(packet);
    }
    if (pump_) exchange::Pump();
}

// The GUI payload as sent, with id and status replaced. Leg 0 bids at P1.
auto GuiConfig(int strategyId_, const std::string& status_) -> nlohmann::json {
    nlohmann::json config            = nlohmann::json::parse(GUI_PAYLOAD);
    config["Strategy"]["StrategyId"] = strategyId_;
    config["Strategy"]["Status"]     = status_;
    return config;
}

// The GUI payload trading the long pack instead: 2 packs at a credit of at least 1.40, short side off.
auto RatioConfig(int strategyId_, const std::string& status_, int legToken0_ = LEG0) -> nlohmann::json {
    nlohmann::json config               = GuiConfig(strategyId_, status_);
    config["Legs"][0]["Token"]          = legToken0_;
    config["Params"]["LongBuyQty"]      = 2;
    config["Params"]["LongBuyPrice"]    = 1.40;
    config["Params"]["ShortSellQty"]    = 0;
    return config;
}

// Price of the nth (1-based) new order a strategy sent on a token.
auto NthPlacePrice(int strategy_, int token_, int nth_) -> int {
    int count = 0;
    for (const auto& request : exchange::sent) {
        if (request.hdr_.uid_.composite_id_.strategy_id == static_cast<uint32_t>(strategy_) && request.packet_.product_id_ == token_ &&
            request.hdr_.transaction_code == OMS_PLACE_REQ && ++count == nth_) {
            return request.packet_.price_;
        }
    }
    return 0;
}

// The nth (1-based) new order a strategy sent on a token.
auto NthPlace(int strategy_, int token_, int nth_) -> oms_transaction {
    int count = 0;
    for (const auto& request : exchange::sent) {
        if (request.hdr_.uid_.composite_id_.strategy_id == static_cast<uint32_t>(strategy_) && request.packet_.product_id_ == token_ &&
            request.hdr_.transaction_code == OMS_PLACE_REQ && ++count == nth_) {
            return request;
        }
    }
    CHECK(false && "no such order");
    return {};
}

auto LastStatusEcho() -> StrategyStatusUpdate {
    for (auto packet = exchange::uiPackets.rbegin(); packet != exchange::uiPackets.rend(); ++packet) {
        if (packet->header.message_code == UiMessageCode_STRATEGY_STATUS) {
            StrategyStatusUpdate update{};
            std::memcpy(&update, packet->message, sizeof(update));
            return update;
        }
    }
    CHECK(false && "no status echo");
    return {};
}

// Trade Tracer rows sent for one strategy with this message ("" for slippage reports).
auto TracerRows(int strategy_, std::string_view message_) -> std::vector<TradeTracer> {
    std::vector<TradeTracer> rows;
    for (const auto& packet : exchange::uiPackets) {
        if (packet.header.message_code != UiMessageCode_TRADE_TRACER) continue;
        TradeTracer tracer{};
        std::memcpy(&tracer, packet.message, sizeof(tracer));
        if (static_cast<int>(tracer._strategyId) == strategy_ && std::string_view(tracer._message) == message_) rows.push_back(tracer);
    }
    return rows;
}

void TickAllLegs() {
    exchange::Tick(LEG0, 525, 535);
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
}

// Leg2 thins to 2 asks before the exchange answers: the spread still holds at its top, but there is no index 2 to hedge at.
void ThinLeg2() { exchange::Tick(LEG2, 315, 325, false, 300, aef::infra::quote::QUOTE_LEVELS, 2); }

auto Position(int strategy_, int token_) -> long { return exchange::position[{strategy_, token_}]; }

// Price of a strategy's working order on a token, 0 when none.
auto LivePrice(int strategy_, int token_) -> int {
    for (const auto& [uid, order] : exchange::live) {
        if (order.strategy == strategy_ && order.request.packet_.product_id_ == token_) return order.price;
    }
    return 0;
}

// New orders a strategy sent on one side of a token.
auto PlacesOf(int strategy_, int token_, ORDER_SIDE side_) -> int {
    int count = 0;
    for (const auto& request : exchange::sent) {
        count += request.hdr_.uid_.composite_id_.strategy_id == static_cast<uint32_t>(strategy_) && request.packet_.product_id_ == token_ &&
                 request.hdr_.transaction_code == OMS_PLACE_REQ && request.packet_.flags_.order_side == side_;
    }
    return count;
}

// Requests of one transaction code a strategy sent on a token.
auto RequestsOf(int strategy_, int token_, int code_) -> int {
    int count = 0;
    for (const auto& request : exchange::sent) {
        count += request.hdr_.uid_.composite_id_.strategy_id == static_cast<uint32_t>(strategy_) && request.packet_.product_id_ == token_ &&
                 request.hdr_.transaction_code == code_;
    }
    return count;
}

// ═══ Scenarios ═══════════════════════════════════════════════════════════════

// A resting bid that overpays moves on any tick; one that is more passive than the new price moves only 2 ticks or more
// (20261022 log: one-tick flicker sent 4M modifies; 20261023 log: a bid left overpaying filled with slippage).
void BidHoldsOnlyWhenPassive() {
    constexpr int  ID           = 90;
    nlohmann::json config       = RatioConfig(ID, "Applied");
    config["Params"]["TickSize"] = 1;  // clamped to the minimum of 2
    SendConfig(config);
    TickAllLegs();
    int price = LivePrice(ID, LEG0);
    CHECK(price > 0);
    int modifies = 0;
    int oneTicks = 0;
    // Hedge leg ask rises: the SELL bid must rise with it, every tick it can.
    for (int step = 1; step <= 12; ++step) {
        exchange::Tick(LEG1, 770 + step * exchange::TICK, 780 + step * exchange::TICK);
        const int now = LivePrice(ID, LEG0);
        CHECK(now >= price);
        if (now != price) {
            oneTicks += now - price == exchange::TICK;
            ++modifies;
            price = now;
        }
    }
    CHECK(oneTicks > 0);
    // Hedge leg ask falls back: the SELL bid is now more passive than needed, so it waits for 2 ticks.
    for (int step = 11; step >= 0; --step) {
        exchange::Tick(LEG1, 770 + step * exchange::TICK, 780 + step * exchange::TICK);
        const int now = LivePrice(ID, LEG0);
        CHECK(now <= price);
        if (now != price) {
            CHECK(price - now >= 2 * exchange::TICK);
            ++modifies;
            price = now;
        }
    }
    CHECK(modifies > 0 && RequestsOf(ID, LEG0, OMS_REPLACE_REQ) == modifies);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  bid move: %d modifies, %d one-tick moves while overpaying, passive moves all >= 2 ticks\n", modifies, oneTicks);
}

// The market beats the target: the bid moves toward the market by the spare edge and fills at once
// (std::abs on the back-solve used to mirror it away from the market).
void BidCrossesWhenMarketBeatsTarget() {
    constexpr int ID = 91;
    SendConfig(RatioConfig(ID, "Applied"));
    TickAllLegs();
    // market 3*545 - 780 - 2*325 = 205 > 140, cushion (790-780) + 2*(335-325) = 30: P1 = 545 + (170 - 205) / 3 = 533.33 -> 535
    exchange::Tick(LEG0, 545, 555);
    CHECK(NthPlace(ID, LEG0, 1).packet_.price_ == 535);
    CHECK(Position(ID, LEG0) == -3 * LOT);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  spare edge: SELL bid at 535 under the 545 bid, filled at once\n");
}

// The bid never rests deeper than AllowedBidDepth levels on its own side: not placed, pulled once the book moves away.
void BidNotDeeperThanAllowedDepth() {
    constexpr int  ID                   = 92;
    nlohmann::json config               = RatioConfig(ID, "Applied");
    config["Params"]["AllowedBidDepth"] = 2;
    SendConfig(config);
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 505, 515);  // P1 535, asks 515/520: level 2 is 520, 535 is deeper, nothing sent
    CHECK(exchange::LiveOrdersOf(ID) == 0 && NthPlacePrice(ID, LEG0, 1) == 0);
    exchange::Tick(LEG0, 520, 530);  // asks 530/535: 535 is level 2, SELL rests at 535
    CHECK(LivePrice(ID, LEG0) == 535);
    exchange::Tick(LEG0, 500, 510);  // asks 510/515: 535 is deeper again, pulled
    CHECK(exchange::LiveOrdersOf(ID) == 0 && RequestsOf(ID, LEG0, OMS_CANCEL_REQ) == 1);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  bid depth: AllowedBidDepth 2, SELL 535 sent only while 535 is within the best 2 asks, pulled after\n");
}

// Full life: subscribe, apply, bid rests at the target price while the market misses, fills once the bid reaches it, done.
void FullCycle() {
    constexpr int ID = 10;
    SendConfig(RatioConfig(ID, "Subscribed"));
    CHECK(LastStatusEcho()._status == StrategyStatus_ACTIVE);
    TickAllLegs();
    CHECK(exchange::LiveOrdersOf(ID) == 0);  // ACTIVE only watches

    SendConfig(RatioConfig(ID, "Applied"));
    CHECK(LastStatusEcho()._status == StrategyStatus_APPLIED);
    // market 3*505 - 780 - 2*325 = 85 < 140, cushion (790-780) + 2*(335-325) = 30: P1 = 505 + (170 - 85) / 3 = 533.33 -> SELL rests at 535
    exchange::Tick(LEG0, 505, 515);
    const oms_transaction bid = NthPlace(ID, LEG0, 1);
    CHECK(bid.packet_.flags_.order_type == LIMIT_ORDER_TYPE && bid.packet_.price_ == 535 && bid.packet_.quantity_ == 3 * LOT);
    CHECK(bid.hdr_.uid_.composite_id_.request_id == 1);  // _clientUid starts at 0, first order is 1
    CHECK(exchange::LiveOrdersOf(ID) == 1 && Position(ID, LEG0) == 0);

    exchange::Tick(LEG0, 535, 545);  // bid reaches 535: the resting SELL fills, hedges complete pack 1 at 3*535 - 1430 = 175
    CHECK(Position(ID, LEG0) == -3 * LOT && Position(ID, LEG1) == LOT && Position(ID, LEG2) == 2 * LOT);
    exchange::Tick(LEG0, 535, 545);  // next tick: P1 = 535 + (170 - 175) / 3 = 533.33 -> 535 against the 535 bid: fills at once
    CHECK(Position(ID, LEG0) == -6 * LOT && Position(ID, LEG1) == 2 * LOT && Position(ID, LEG2) == 4 * LOT);
    CHECK(TracerRows(ID, "Slippage").empty());
    CHECK(TracerRows(ID, "").size() == 2);  // traded at 175, better than the 140 target: Error column blank
    CHECK(exchange::LiveOrdersOf(ID) == 0);  // total quantity reached: no more orders

    TickAllLegs();
    CHECK(exchange::LiveOrdersOf(ID) == 0);

    exchange::hub->doWork();
    StrategySpreadUpdate spread{};
    for (const auto& packet : exchange::uiPackets) {
        if (packet.header.message_code == UiMessageCode_STRATEGY_SPREAD) {
            std::memcpy(&spread, packet.message, sizeof(spread));
            if (spread._strategyId == ID) break;
        }
    }
    CHECK(spread._strategyId == ID && spread._bTrQ == 2 && spread._sTrQ == 0);
    std::printf("  full cycle: rests @535 at market 85, 2 packs filled @535, legs %ld/%+ld/%+ld, B-TrQ=%d B-ATP=%.2f RLP=%.2f\n",
                Position(ID, LEG0), Position(ID, LEG1), Position(ID, LEG2), spread._bTrQ, spread._bATP, spread._rlp);
}

// STOP while a crossing bid awaits the exchange: the bid still fills, and its hedges complete after STOP.
void StopRacesPendingBid() {
    constexpr int ID = 11;
    SendConfig(RatioConfig(ID, "Applied"));
    exchange::Tick(LEG1, 770, 780);         // hedge legs first: no leg0 price yet, so no bid
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 540, 550, false);  // market 190: P1 523.33 -> 525, SELL 540 sent at the 540 bid, not yet answered
    CHECK(exchange::outbox.size() == 1);

    SendConfig(RatioConfig(ID, "Stop"), false);  // STOP arrives before the exchange answers
    CHECK(LastStatusEcho()._status == StrategyStatus_ACTIVE);  // Stop() keeps the strategy ACTIVE: feed and UI on, no entries

    exchange::Pump();  // bid fills -> hedges cross
    CHECK(Position(ID, LEG0) == -3 * LOT && Position(ID, LEG1) == LOT && Position(ID, LEG2) == 2 * LOT);
    CHECK(exchange::LiveOrdersOf(ID) == 0);
    TickAllLegs();
    CHECK(exchange::LiveOrdersOf(ID) == 0);  // stopped: no new bid
    std::printf("  stop race: in-flight bid filled, hedged after STOP, flat\n");
}

// The bid fills while leg2 has no asks; then STOP. Hedging must continue after STOP until flat.
void StopKeepsHedging() {
    constexpr int ID = 13;
    SendConfig(RatioConfig(ID, "Applied"));
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 540, 550, false);  // SELL 540 sent at the 325 leg2 snapshot
    ThinLeg2();
    exchange::Pump();                       // bid fills: leg1 hedges, leg2 waits for a third ask
    CHECK(Position(ID, LEG0) == -3 * LOT && Position(ID, LEG1) == LOT && Position(ID, LEG2) == 0);

    SendConfig(RatioConfig(ID, "Stop"));
    CHECK(LastStatusEcho()._status == StrategyStatus_ACTIVE);
    exchange::Tick(LEG2, 315, 330);  // asks deepen again
    CHECK(Position(ID, LEG2) == 2 * LOT);
    CHECK(NthPlacePrice(ID, LEG2, 1) == 340);  // level 3 once it is there
    CHECK(exchange::LiveOrdersOf(ID) == 0);
    TickAllLegs();
    CHECK(exchange::LiveOrdersOf(ID) == 0);  // stopped: no new bid
    std::printf("  stop keeps hedging: leg2 hedged after STOP once a third ask returned, flat\n");
}

// A leg without product details: no strategy, no subscription, UI told INACTIVE.
void RejectsUnknownToken() {
    constexpr int ID         = 12;
    const size_t  subscribed = exchange::subscribed.size();
    SendConfig(RatioConfig(ID, "Applied", MISSING_TOKEN));
    CHECK(LastStatusEcho()._strategyId == ID && LastStatusEcho()._status == StrategyStatus_INACTIVE);
    CHECK(exchange::subscribed.size() == subscribed && !exchange::subscribed.count(MISSING_TOKEN));
    TickAllLegs();
    CHECK(exchange::LiveOrdersOf(ID) == 0);
    std::printf("  unknown token: creation refused, nothing subscribed\n");
}

// A hedge goes straight to level 3 (index 2): leg2's ask moved 325 -> 335 while the bid was in flight, so one order at 345,
// no steps up from the 325 the bid was priced on (each step cost a message under the ~30 per second throttle).
void HedgeTakesLevel3() {
    constexpr int ID = 14;
    SendConfig(RatioConfig(ID, "Applied"));
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 540, 550, false);  // SELL 540 crosses the 540 bid, not answered yet
    exchange::Tick(LEG2, 325, 335, false);  // leg2 ask moves before the fill
    exchange::Pump();
    CHECK(Position(ID, LEG2) == 2 * LOT && NthPlacePrice(ID, LEG2, 1) == 345);
    int leg2Modifies = 0;
    for (const auto& request : exchange::sent) {
        leg2Modifies += request.hdr_.uid_.composite_id_.strategy_id == ID && request.packet_.product_id_ == LEG2 && request.hdr_.transaction_code == OMS_REPLACE_REQ;
    }
    CHECK(leg2Modifies == 0);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  hedge takes level 3: leg2 placed @345, bought @335 in one order, no modifies\n");
}

// Hedges go out highest price first: leg2 (ask 780) is placed before leg1 (ask 325), against leg order.
void HedgesHighestPriceFirst() {
    constexpr int ID = 16;
    SendConfig(RatioConfig(ID, "Applied"));
    exchange::Tick(LEG1, 315, 325);
    exchange::Tick(LEG2, 770, 780);
    exchange::Tick(LEG0, 700, 710);  // 3 x 700 - 335 - 2 x 790 = 185 beats the 140 target: the SELL crosses and fills
    size_t leg1At = 0, leg2At = 0;
    for (size_t index = 0; index < exchange::sent.size(); ++index) {
        const auto& request = exchange::sent[index];
        if (request.hdr_.uid_.composite_id_.strategy_id != ID || request.hdr_.transaction_code != OMS_PLACE_REQ) continue;
        if (request.packet_.product_id_ == LEG1 && leg1At == 0) leg1At = index;
        if (request.packet_.product_id_ == LEG2 && leg2At == 0) leg2At = index;
    }
    CHECK(leg1At > 0 && leg2At > 0 && leg2At < leg1At);
    CHECK(Position(ID, LEG1) == LOT && Position(ID, LEG2) == 2 * LOT);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  hedges highest price first: leg2 @790 placed before leg1 @335\n");
}

// A hedge rests at ask level 3 and is repriced to the new level 3 on every tick until it fills.
void HedgeRepricedEveryTick() {
    constexpr int  ID              = 48;
    nlohmann::json config          = RatioConfig(ID, "Applied");
    config["Params"]["LongBuyQty"] = 1;
    SendConfig(config);
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 540, 550, false);  // SELL 540 crosses the 540 bid, not answered yet
    exchange::books[LEG2].ask = 400;        // leg2 asks run away on the exchange before the hedge arrives
    exchange::Pump();                       // SELL filled, leg1 hedged, leg2 BUY 335 rests
    CHECK(Position(ID, LEG0) == -3 * LOT && Position(ID, LEG1) == LOT && LivePrice(ID, LEG2) == 335);
    exchange::Tick(LEG2, 390, 400);  // repriced to 410, takes the 400 ask
    CHECK(Position(ID, LEG2) == 2 * LOT && RequestsOf(ID, LEG2, OMS_REPLACE_REQ) == 1 && exchange::LiveOrdersOf(ID) == 0);
    exchange::Tick(LEG2, 315, 325);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  hedge repriced: leg2 rests @335 while asks move to 400, repriced to 410 on the next tick, filled\n");
}

// The GUI payload verbatim: short pack, 5 packs at a credit of at least -0.05.
// Short credit = bid(23100) + 2 x bid(23300) - 3 x ask(23200).
void ShortPackFromGuiPayload() {
    constexpr int ID = 15;
    SendConfig(GuiConfig(ID, "Applied"));
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    // credit 770 + 2*315 - 3*480 = -40 < -5, cushion (770-760) + 2*(315-305) = 30: P1 = 480 - (-5 + 30 + 40) / 3 = 458.33 -> BUY rests at 455
    exchange::Tick(LEG0, 470, 480);
    const oms_transaction bid = NthPlace(ID, LEG0, 1);
    CHECK(bid.packet_.flags_.order_type == LIMIT_ORDER_TYPE && bid.packet_.flags_.order_side == BUY_SIDE && bid.packet_.price_ == 455);
    CHECK(exchange::LiveOrdersOf(ID) == 1 && LivePrice(ID, LEG0) == 455 && Position(ID, LEG0) == 0);
    exchange::Tick(LEG0, 470, 475);  // credit -25 < -5: P1 = 475 - 50 / 3 = 458.33 -> still 455, not modified
    CHECK(RequestsOf(ID, LEG0, OMS_REPLACE_REQ) == 0 && LivePrice(ID, LEG0) == 455);

    for (int pack = 0; pack < 5; ++pack) exchange::Tick(LEG0, 450, 455);  // ask reaches 455: one pack fills per tick
    CHECK(Position(ID, LEG0) == 15 * LOT && Position(ID, LEG1) == -5 * LOT && Position(ID, LEG2) == -10 * LOT);
    CHECK(exchange::LiveOrdersOf(ID) == 0);
    std::printf("  GUI payload verbatim: BUY rests @455 at P1, 5 packs filled, hedged SELL 23100 + 2 x 23300\n");
}

// AllowedSlippage is in paise. Leg2 hedges at 355 instead of 325: traded 1620 - 780 - 710 = 130, 10 paise under target 140.
// AllowedSlippage 9 stops before the second pack; 10 lets it trade.
auto SlippedPacks(int strategyId_, int allowedSlippagePaise_) -> long {
    nlohmann::json config               = RatioConfig(strategyId_, "Applied");
    config["Params"]["AllowedSlippage"] = allowedSlippagePaise_;
    SendConfig(config);
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 540, 550, false);  // SELL 540 sent at the 325 leg2 snapshot
    ThinLeg2();
    exchange::Pump();                       // bid fills, leg2 hedge waits for a third ask
    exchange::Tick(LEG2, 345, 355);         // hedge takes 355: pack done, 10 paise slipped
    CHECK(Position(strategyId_, LEG2) == 2 * LOT);
    exchange::Tick(LEG2, 315, 325);         // SELL 540 crosses the 540 bid again: second pack unless stopped
    SendConfig(RatioConfig(strategyId_, "Stop"));
    return -Position(strategyId_, LEG0) / (3 * LOT);
}

void SlippageInPaise() {
    CHECK(SlippedPacks(18, 9) == 1);
    CHECK(SlippedPacks(19, 10) == 2);
    CHECK(TracerRows(18, "Slippage").size() == 1 && TracerRows(19, "Slippage").empty());  // 10 paise is allowed at 10
    std::printf("  slippage in paise: 10 paise slip stops at AllowedSlippage 9, trades on at 10\n");
}

// The resting bid follows P1: P1 = (140 + cushion 30 + 780 + 2 x leg2 ask) / 3, SELL rounded up.
void BidFollowsP1() {
    constexpr int ID = 20;
    SendConfig(RatioConfig(ID, "Applied"));
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 510, 520);  // P1 (170 + 780 + 2*325) / 3 = 533.33 -> SELL rests at 535
    CHECK(LivePrice(ID, LEG0) == 535);
    exchange::Tick(LEG2, 300, 310);  // P1 523.33 -> 525: passive by 2 ticks, moves
    CHECK(LivePrice(ID, LEG0) == 525);
    exchange::Tick(LEG2, 320, 330);  // P1 536.67 -> 540: overpaying, moves
    CHECK(LivePrice(ID, LEG0) == 540);
    exchange::Tick(LEG2, 290, 300);  // P1 516.67 -> 520: passive by 4 ticks, moves
    CHECK(LivePrice(ID, LEG0) == 520 && RequestsOf(ID, LEG0, OMS_REPLACE_REQ) == 3);
    exchange::Tick(LEG0, 520, 530);  // bid reaches 520: pack 1 fills, traded 1560 - 780 - 600 = 180 >= 140
    exchange::Tick(LEG0, 520, 530);  // pack 2: P1 = 520 + (170 - 180) / 3 = 516.67 -> 520 crosses at once
    CHECK(Position(ID, LEG0) == -6 * LOT && Position(ID, LEG1) == 2 * LOT && Position(ID, LEG2) == 4 * LOT);
    std::printf("  bid follows P1: 535 -> 525 -> 540 -> 520 on each hedge move, filled\n");
}

// A target no positive leg 0 price can reach sends nothing.
void UnreachableTargetSendsNothing() {
    constexpr int  ID                  = 22;
    nlohmann::json config              = GuiConfig(ID, "Applied");
    config["Params"]["ShortSellPrice"] = 14.00;  // BUY price (770 + 2*315 - 1400) / 3 = 0
    SendConfig(config);
    TickAllLegs();
    CHECK(exchange::LiveOrdersOf(ID) == 0 && NthPlacePrice(ID, LEG0, 1) == 0);
    SendConfig(GuiConfig(ID, "Stop"));
    std::printf("  unreachable target: BUY price 0, no order\n");
}

// ConRev subtracts the 100 rupee strike gap from a raw spread >= 0, so target -98.60 needs raw 1.40: same bid as no gap.
void StrikeGapBackSolve() {
    constexpr int  ID                  = 23;
    nlohmann::json config              = RatioConfig(ID, "Applied");
    config["Strategy"]["SubType"]      = "ConRev";
    config["Params"]["LongBuyPrice"]   = -98.60;
    SendConfig(config);
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 505, 515);  // raw target -9860 + 10000 = 140, cushion 30: P1 533.33, SELL rests at 535
    CHECK(NthPlacePrice(ID, LEG0, 1) == 535 && Position(ID, LEG0) == 0);
    config["Strategy"]["Status"] = "Stop";
    SendConfig(config);
    std::printf("  strike gap: ConRev target -98.60 rests at 535\n");
}

// A hedge rejected with a code the vendor had no branch for (INVALID_PRICE) stayed in flight forever. Now it is retried.
void UnhandledRejectRetries() {
    constexpr int ID = 25;
    SendConfig(RatioConfig(ID, "Applied"));
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::rejectToken = LEG2;
    exchange::rejectCode  = INVALID_PRICE;
    exchange::Tick(LEG0, 540, 550, false);  // one pack: the bid crosses, leg2's first hedge is rejected
    SendConfig(RatioConfig(ID, "Stop"));
    exchange::Tick(LEG2, 315, 325);
    CHECK(Position(ID, LEG0) == -3 * LOT && Position(ID, LEG1) == LOT && Position(ID, LEG2) == 2 * LOT);
    CHECK(PlacesOf(ID, LEG2, BUY_SIDE) == 2 && exchange::LiveOrdersOf(ID) == 0);
    exchange::rejectCode = EXCHG_ERROR;
    std::printf("  unhandled reject: INVALID_PRICE on the leg2 hedge, retried on the next tick, pack whole\n");
}

// DELETE while the bid is in flight: the strategy lives on until its pack is hedged, then the hub erases it.
void DeleteWaitsUntilWhole() {
    constexpr int ID = 29;
    SendConfig(RatioConfig(ID, "Applied"));
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 540, 550, false);
    SendConfig(RatioConfig(ID, "Delete"), false);
    exchange::hub->doWork();  // bid in flight: not idle, kept
    CHECK(LastStatusEcho()._strategyId == ID && LastStatusEcho()._status == StrategyStatus_ACTIVE);

    exchange::Pump();  // bid fills, hedges cross
    CHECK(Position(ID, LEG0) == -3 * LOT && Position(ID, LEG1) == LOT && Position(ID, LEG2) == 2 * LOT);
    exchange::hub->doWork();  // whole and idle: erased
    CHECK(LastStatusEcho()._strategyId == ID && LastStatusEcho()._status == StrategyStatus_INACTIVE);
    TickAllLegs();
    CHECK(exchange::LiveOrdersOf(ID) == 0);
    std::printf("  delete: bid in flight filled and hedged, erased once idle, INACTIVE echoed\n");
}

// A synthetic-trade tick: the feed saw a trade on side_ of token_ before the exchange told us. Requests stay unanswered.
void SyntheticTick(int token_, int bid_, int ask_, ORDER_SIDE side_) {
    Quote quote                           = exchange::BookQuote(token_, bid_, ask_);
    quote.message.updatetype_.update_type = 'S';
    quote.message.updatetype_.update_side = side_;
    exchange::Publish(quote, false);
}

// Leg 1 and 2 at 770/780 and 315/325, then leg 0 at 505/515: the SELL bid rests at 535.
void RestBidAt535(int strategyId_) {
    SendConfig(RatioConfig(strategyId_, "Applied"));
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 505, 515);
    CHECK(LivePrice(strategyId_, LEG0) == 535);
}

// An 'S' tick on the ask side leaves our SELL 535 better than the 540 ask: hedged before the fill arrives, the fill completes the pack.
void EarlyHedgeThenFill() {
    constexpr int ID = 60;
    RestBidAt535(ID);
    SyntheticTick(LEG0, 505, 540, SELL_SIDE);
    CHECK(PlacesOf(ID, LEG1, BUY_SIDE) == 1 && PlacesOf(ID, LEG2, BUY_SIDE) == 1 && Position(ID, LEG0) == 0);
    exchange::FillPassive(ID, LEG0, 3 * LOT);  // the real fill, then the early hedges cross
    CHECK(Position(ID, LEG0) == -3 * LOT && Position(ID, LEG1) == LOT && Position(ID, LEG2) == 2 * LOT);
    CHECK(PlacesOf(ID, LEG1, BUY_SIDE) == 1 && PlacesOf(ID, LEG2, BUY_SIDE) == 1 && RequestsOf(ID, LEG0, OMS_CANCEL_REQ) == 0);
    SyntheticTick(LEG0, 505, 540, SELL_SIDE);  // a repeat 'S': only the pack 2 bid goes out, still unconfirmed, so no hedge
    CHECK(PlacesOf(ID, LEG1, BUY_SIDE) == 1 && PlacesOf(ID, LEG0, SELL_SIDE) == 2);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  'S' early hedge: SELL 535 beats the 540 ask, hedges sent before the fill, pack whole once it arrives\n");
}

// The 'S' guess was wrong: the hedges fill, leg 0 did not, so leg 0 is chased by a modify of the same order.
void EarlyHedgeChasesLeg0() {
    constexpr int ID = 61;
    RestBidAt535(ID);
    SyntheticTick(LEG0, 505, 540, SELL_SIDE);
    exchange::Pump();  // hedges fill; leg 0 modified to the bid's index 2 (495), trades at the 505 bid
    CHECK(Position(ID, LEG0) == -3 * LOT && Position(ID, LEG1) == LOT && Position(ID, LEG2) == 2 * LOT);
    CHECK(PlacesOf(ID, LEG0, SELL_SIDE) == 1 && RequestsOf(ID, LEG0, OMS_REPLACE_REQ) == 1 && exchange::LiveOrdersOf(ID) == 0);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  'S' guessed wrong: hedges filled, leg 0 chased by a modify to 495, filled at 505\n");
}

// A wrong 'S' guess with hedges that do not fill: leg 0 is not repriced or pulled while they work; STOP cancels it, then the early hedges are pulled.
void EarlyHedgePulledWithoutFill() {
    constexpr int ID = 62;
    RestBidAt535(ID);
    SyntheticTick(LEG0, 505, 540, SELL_SIDE);
    exchange::books[LEG1].ask = 900;  // hedge asks run away before the hedges arrive
    exchange::books[LEG2].ask = 900;
    exchange::Pump();
    CHECK(exchange::LiveOrdersOf(ID) == 3 && Position(ID, LEG1) == 0 && Position(ID, LEG2) == 0);
    exchange::Tick(LEG0, 400, 410);  // P1 far below: leg 0 would be repriced or pulled, but is left alone
    CHECK(LivePrice(ID, LEG0) == 535 && RequestsOf(ID, LEG0, OMS_REPLACE_REQ) == 0 && RequestsOf(ID, LEG0, OMS_CANCEL_REQ) == 0);
    SendConfig(RatioConfig(ID, "Stop"));  // leg 0 cancelled
    exchange::Tick(LEG1, 770, 900);       // leg 0 gone unfilled: early hedges pulled
    CHECK(exchange::LiveOrdersOf(ID) == 0 && Position(ID, LEG0) == 0 && Position(ID, LEG1) == 0 && Position(ID, LEG2) == 0);
    TickAllLegs();
    std::printf("  'S' guessed wrong, hedges unfilled: leg 0 held at 535, STOP cancels it, early hedges pulled, flat\n");
}

// Leg 0 fills after leg1's asks jumped to 820: 3 x 535 - 820 - 2 x 325 = 135 misses the 140 target, so no hedge.
// Leg 0 is bought back at the ask's index 2, a "Reverse" row goes to the Trade Tracer, and bidding goes on.
void ReverseWhenSpreadGone() {
    constexpr int ID = 63;
    RestBidAt535(ID);
    exchange::Tick(LEG1, 800, 820, false);     // P1 550 is past 5 ask levels: the bid's cancel is still in flight
    exchange::FillPassive(ID, LEG0, 3 * LOT);  // the SELL 535 fills first; BUY 525 takes the 515 ask
    CHECK(Position(ID, LEG0) == 0 && Position(ID, LEG1) == 0 && Position(ID, LEG2) == 0);
    CHECK(PlacesOf(ID, LEG0, BUY_SIDE) == 1 && PlacesOf(ID, LEG1, BUY_SIDE) == 0 && PlacesOf(ID, LEG2, BUY_SIDE) == 0);
    const std::vector<TradeTracer> rows = TracerRows(ID, "Reverse");
    CHECK(rows.size() == 1 && rows[0]._ltq == 3 && std::abs(rows[0]._ltp - 5.15F) < 0.001F && rows[0]._slippage == 0);
    CHECK(TracerRows(ID, "").empty());
    exchange::Tick(LEG1, 770, 780);  // bids again at 535
    CHECK(PlacesOf(ID, LEG0, SELL_SIDE) == 2 && LivePrice(ID, LEG0) == 535);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  spread gone at the fill: no hedge, SELL 535 bought back @515, Reverse row sent, bid again @535\n");
}

// One lot of 3 fills after the spread went: that lot is bought back, the other 2 are cancelled before the next bid.
void ReversePartialCancelsRest() {
    constexpr int ID = 64;
    RestBidAt535(ID);
    exchange::Tick(LEG0, 520, 530);         // asks 530..550: room for P1 550
    exchange::Tick(LEG1, 800, 820, false);  // the bid's modify to 550 is still in flight
    exchange::books[LEG0].ask = 0;          // no sellers: the buy-back rests
    exchange::FillPassive(ID, LEG0, LOT);   // 1 lot @535; the modify lands on the other 2
    CHECK(Position(ID, LEG0) == -LOT && exchange::LiveOrdersOf(ID) == 2 && PlacesOf(ID, LEG0, BUY_SIDE) == 1);
    exchange::Tick(LEG0, 520, 530);  // the rest of the bid is cancelled, the BUY 540 buy-back takes the 530 ask
    CHECK(Position(ID, LEG0) == 0 && Position(ID, LEG1) == 0 && Position(ID, LEG2) == 0);
    CHECK(RequestsOf(ID, LEG0, OMS_CANCEL_REQ) == 1 && exchange::LiveOrdersOf(ID) == 0);
    CHECK(TracerRows(ID, "Reverse").size() == 1 && TracerRows(ID, "Reverse")[0]._ltq == 1);
    exchange::Tick(LEG0, 520, 530);  // reversed: bids again at 550
    CHECK(PlacesOf(ID, LEG0, SELL_SIDE) == 2);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  partial fill after the spread went: 1 lot bought back, 2 lots cancelled, bid again\n");
}

// An 'S' tick while the bid is stuck at 535 (its modify refused) and leg1's asks are at 820: the spread is gone, so no early hedge.
// The fill then decides: it is reversed.
void EarlyHedgeSkippedWhenSpreadGone() {
    constexpr int ID = 65;
    RestBidAt535(ID);
    exchange::rejectToken   = LEG0;
    exchange::rejectChanges = true;
    exchange::Tick(LEG1, 800, 820);  // modify to 550 refused: SELL 535 stays
    exchange::rejectChanges = false;
    exchange::rejectToken   = 0;
    CHECK(LivePrice(ID, LEG0) == 535);
    SyntheticTick(LEG0, 505, 540, SELL_SIDE);
    CHECK(PlacesOf(ID, LEG1, BUY_SIDE) == 0 && PlacesOf(ID, LEG2, BUY_SIDE) == 0);
    exchange::FillPassive(ID, LEG0, 3 * LOT);  // fills @535, bought back at the 540 ask
    CHECK(Position(ID, LEG0) == 0 && Position(ID, LEG1) == 0 && Position(ID, LEG2) == 0);
    CHECK(TracerRows(ID, "Reverse").size() == 1);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  'S' with the spread gone: no early hedge, the fill is bought back\n");
}

// SoQ 0 used to size the hedge at 0 lots, so it was never sent.
void HedgeIgnoresZeroSlice() {
    constexpr int  ID                 = 31;
    nlohmann::json config             = RatioConfig(ID, "Applied");
    SendConfig(config);
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 540, 550, false);
    ThinLeg2();
    exchange::Pump();  // leg2 waits for a third ask, no order
    config["Params"]["LongBuySoQ"] = 0;  // Applied: STOP does not read params. SoQ 0 also means no new bid
    SendConfig(config);
    exchange::Tick(LEG2, 315, 325);
    CHECK(Position(ID, LEG0) == -3 * LOT && Position(ID, LEG1) == LOT && Position(ID, LEG2) == 2 * LOT);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  SoQ 0 while unhedged: leg2 hedge still sent for the whole pack\n");
}

auto SpreadRows(int strategy_) -> std::vector<StrategySpreadUpdate> {
    std::vector<StrategySpreadUpdate> rows;
    for (const auto& packet : exchange::uiPackets) {
        if (packet.header.message_code != UiMessageCode_STRATEGY_SPREAD) continue;
        StrategySpreadUpdate row{};
        std::memcpy(&row, packet.message, sizeof(row));
        if (static_cast<int>(row._strategyId) == strategy_) rows.push_back(row);
    }
    return rows;
}

// The GUI row goes out only when a value in it changed; the tick count alone is no change and adds up meanwhile.
void SpreadSentOnlyOnChange() {
    constexpr int ID = 46;
    SendConfig(RatioConfig(ID, "Subscribed"));  // ACTIVE: watches, no orders
    TickAllLegs();
    const auto sendRound = [] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1010));  // the hub sends rows once a second
        exchange::hub->doWork();
    };
    sendRound();
    CHECK(SpreadRows(ID).size() == 1);
    TickAllLegs();  // same prices
    sendRound();
    CHECK(SpreadRows(ID).size() == 1);
    exchange::Tick(LEG1, 775, 785);  // leg1 moved: BCMP and cost change
    sendRound();
    const std::vector<StrategySpreadUpdate> rows = SpreadRows(ID);
    CHECK(rows.size() == 2 && rows[1]._bcmp != rows[0]._bcmp && rows[1]._cost != rows[0]._cost && rows[1]._reserved == 4);
    exchange::Tick(LEG1, 770, 780);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  spread row: sent once, skipped while unchanged, sent on a leg1 move with 4 ticks counted\n");
}

auto StrategyLogBytes() -> uintmax_t {
    exchange::hub->doWork();  // strategy logs are buffered until the hub flushes them
    uintmax_t bytes = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator("log")) {
        if (entry.is_regular_file()) bytes += entry.file_size();
    }
    return bytes;
}

// Idle ticks must not log: neither the vendor's refusal lines nor our own diagnostics.
void QuietTicks() {
    TickAllLegs();  // settle: earlier scenarios may leave a book change to react to
    const int       vendorBefore = exchange::vendorRefusalLogs;
    const uintmax_t bytesBefore  = StrategyLogBytes();
    for (int i = 0; i < 1000; ++i) TickAllLegs();
    std::printf("  3000 idle ticks: %d vendor log lines, %ju strategy log bytes\n", exchange::vendorRefusalLogs - vendorBefore, StrategyLogBytes() - bytesBefore);
    CHECK(exchange::vendorRefusalLogs - vendorBefore == 0);
    CHECK(StrategyLogBytes() == bytesBefore);
}

// ═══ Manual orders (GUI 9911 in, 5001 out) ═══════════════════════════════════

struct ManualLeg {
    int token, side, price, quantity;
};

// Sends a manual order request the way the GUI does: oms_transaction3l on code 9911 from interface 7.
void SendManual(int code_, uint32_t requestId_, std::initializer_list<ManualLeg> legs_) {
    oms_transaction3l request{};
    request.uihdr.interface_id    = 7;
    request.hdr_.transaction_code = code_;
    request.hdr_.uid_.strategy_id = 0;  // the GUI sends manual orders as strategy 0
    request.hdr_.uid_.request_id  = requestId_;
    size_t leg = 0;
    for (const ManualLeg& order : legs_) {
        request.packet_.product_id_[leg]       = order.token;
        request.packet_.price_[leg]            = order.price;
        request.packet_.quantity_[leg]         = order.quantity;
        request.packet_.flags_[leg].order_side = order.side;
        request.packet_.flags_[leg].order_type = LIMIT_ORDER_TYPE;
        ++leg;
    }
    aef::infra::ui_cmd::UIStruct packet{};
    packet.header.message_code = UiMessageCode_MANUAL_ORDER;
    std::memcpy(packet.message, &request, sizeof(request));
    exchange::hub->onUIRequest(packet);
    exchange::Pump();
}

// Manual order echoes sent to the GUI since uiPackets index from_.
auto ManualEchoes(size_t from_) -> std::vector<oms_transaction3l> {
    std::vector<oms_transaction3l> echoes;
    for (size_t i = from_; i < exchange::uiPackets.size(); ++i) {
        if (exchange::uiPackets[i].header.message_code != UiMessageCode_MANUAL_ORDER_RESPONSE) continue;
        CHECK(exchange::uiPackets[i].header.interface_id == 7);
        echoes.emplace_back();
        std::memcpy(&echoes.back(), exchange::uiPackets[i].message, sizeof(oms_transaction3l));
    }
    return echoes;
}

// Order updates (ORDER_RESPONSE) sent to the GUI from packet from_ on, as strategy 0 on interface 7.
auto OrderUpdates(size_t from_) -> std::vector<ExternalOrderResponse> {
    std::vector<ExternalOrderResponse> updates;
    for (size_t i = from_; i < exchange::uiPackets.size(); ++i) {
        if (exchange::uiPackets[i].header.message_code != UiMessageCode_ORDER_RESPONSE) continue;
        CHECK(exchange::uiPackets[i].header.interface_id == 7);
        updates.emplace_back();
        std::memcpy(&updates.back(), exchange::uiPackets[i].message, sizeof(ExternalOrderResponse));
        CHECK(updates.back()._userDetails._compositeID._fields._portfolio == 0);
    }
    return updates;
}

void ManualOrders() {
    // Place, modify, cancel one leg: each request reaches the exchange and every response echoes with the GUI's request id.
    size_t sent = exchange::sent.size(), ui = exchange::uiPackets.size();
    SendManual(OMS_PLACE_REQ, 1, {{LEG1, BUY_SIDE, 100, LOT}});
    CHECK(exchange::sent.size() == sent + 1);
    const oms_transaction placed = exchange::sent.back();
    CHECK(placed.packet_.product_id_ == LEG1 && placed.packet_.price_ == 100 && placed.packet_.quantity_ == LOT);
    CHECK(placed.hdr_.uid_.composite_id_.strategy_id == 0 && placed.hdr_.uid_.composite_id_.request_id == 1);
    CHECK(placed.hdr_.uid_.composite_id_.client_id != 1);  // ratio orders carry client 1
    auto echoes = ManualEchoes(ui);
    CHECK(echoes.size() == 2 && echoes[0].hdr_.transaction_code == OMS_ORDER_PLACED && echoes[1].hdr_.transaction_code == OMS_ORDER_CONFIRMED);
    CHECK(echoes[1].hdr_.uid_.request_id == 1 && echoes[1].hdr_.uid_.strategy_id == 0);
    CHECK(echoes[1].packet_.product_id_[0] == LEG1 && echoes[1].hdr_.error_code == 0);
    auto updates = OrderUpdates(ui);
    CHECK(updates.size() == 1 && updates[0]._response._response == OrderResponse_PLACED && updates[0]._response._token == LEG1);

    SendManual(OMS_REPLACE_REQ, 1, {{LEG1, BUY_SIDE, 105, LOT}});
    CHECK(exchange::live.at(placed.hdr_.uid_.id_).price == 105);
    SendManual(OMS_CANCEL_REQ, 1, {{LEG1, BUY_SIDE, 105, LOT}});
    CHECK(!exchange::live.count(placed.hdr_.uid_.id_));
    CHECK(ManualEchoes(ui).back().hdr_.transaction_code == OMS_ORDER_CANCELLED);
    updates = OrderUpdates(ui);
    CHECK(updates.size() == 3 && updates[1]._response._response == OrderResponse_MODIFIED && updates[2]._response._response == OrderResponse_CANCELLED);

    // Bad input is rejected to the GUI and never reaches the exchange.
    sent = exchange::sent.size();
    ui   = exchange::uiPackets.size();
    SendManual(OMS_CANCEL_REQ, 99, {{LEG1, BUY_SIDE, 105, LOT}});  // never placed
    SendManual(OMS_PLACE_REQ, 3, {{LEG1, BUY_SIDE, 100, 0}});
    SendManual(OMS_PLACE_REQ, 4, {{LEG1, BUY_SIDE, 100, LOT}, {LEG2, SELL_SIDE, 0, LOT}});  // one bad leg: nothing sent
    CHECK(exchange::sent.size() == sent);
    echoes = ManualEchoes(ui);
    CHECK(echoes.size() == 3);
    CHECK(echoes[0].hdr_.transaction_code == OMS_REQ_REJ && echoes[0].hdr_.error_code == INVALID_CLIENT_REQ_ID && echoes[0].hdr_.uid_.request_id == 99);
    CHECK(echoes[1].hdr_.error_code == INVALID_QTY && echoes[2].hdr_.error_code == INVALID_PRICE);
    updates = OrderUpdates(ui);
    CHECK(updates.size() == 3 && updates[0]._response._response == OrderResponse_NEW_REJECT && updates[0]._response._errorCode == INVALID_CLIENT_REQ_ID);
    CHECK(updates[1]._response._errorCode == INVALID_QTY && updates[2]._response._errorCode == INVALID_PRICE);

    // Two legs: each order goes out on its own leg's token.
    SendManual(OMS_PLACE_REQ, 2, {{LEG1, BUY_SIDE, 100, LOT}, {LEG2, SELL_SIDE, 900, LOT}});
    CHECK(exchange::sent.size() == sent + 2);
    CHECK(exchange::sent[sent].packet_.product_id_ == LEG1 && exchange::sent[sent + 1].packet_.product_id_ == LEG2);
    CHECK(exchange::sent[sent + 1].packet_.flags_.order_side == SELL_SIDE && exchange::sent[sent + 1].packet_.price_ == 900);
    std::printf("  manual orders: place / modify / cancel echoed on 5001 and as order updates, bad requests rejected, 2 legs on their own tokens\n");
}

// ═══ Square-off (GUI 1331 all, 1332 one token) ═════════════════════════════════════

// Broadcast with only the third level set: square-off prices there.
void Bcast(int token_, int thirdBid_, int thirdAsk_) {
    aef::infra::product::product_data product{};
    product.product_id_       = token_;
    product.bid_mbp[2].price  = thirdBid_;
    product.ask_mbp[2].price  = thirdAsk_;
    exchange::hub->onBcastData(product);
    exchange::Pump();
}

void SquareOffFlattens() {
    exchange::dbPositions = {{LEG1, 2 * LOT}, {LEG2, -LOT}};  // long 2 lots of LEG1, short 1 lot of LEG2
    const size_t sent     = exchange::sent.size();
    const size_t ui       = exchange::uiPackets.size();

    aef::infra::ui_cmd::UIStruct packet{};
    packet.header.message_code = UiMessageCode_SQUARE_OFF;
    packet.header.interface_id = 7;  // ManualEchoes expects responses on this interface
    exchange::hub->onUIRequest(packet);
    CHECK(exchange::sent.size() == sent);  // nothing goes out before a broadcast prices it
    CHECK(exchange::subscribed.count(LEG1) && exchange::subscribed.count(LEG2));

    exchange::hub->onUIRequest(packet);  // pressed again while running: ignored
    Bcast(LEG1, 760, 790);               // book 770 / 780: SELL @760 crosses the 770 bid
    Bcast(LEG2, 305, 335);               // book 315 / 325: BUY @335 crosses the 325 ask
    CHECK(exchange::sent.size() == sent + 2);

    const oms_transaction& sell = exchange::sent[sent];
    const oms_transaction& buy  = exchange::sent[sent + 1];
    CHECK(sell.packet_.product_id_ == LEG1 && sell.packet_.flags_.order_side == SELL_SIDE && sell.packet_.price_ == 760 && sell.packet_.quantity_ == 2 * LOT);
    CHECK(buy.packet_.product_id_ == LEG2 && buy.packet_.flags_.order_side == BUY_SIDE && buy.packet_.price_ == 335 && buy.packet_.quantity_ == LOT);
    CHECK(sell.hdr_.uid_.composite_id_.strategy_id == 0 && sell.hdr_.uid_.composite_id_.client_id == 3);  // client 1 + 2: apart from ratio and manual
    CHECK(!exchange::live.count(sell.hdr_.uid_.id_) && !exchange::live.count(buy.hdr_.uid_.id_));          // both traded

    // Every response echoes to the GUI on 5001 as strategy 0, ending in a trade per token.
    const auto echoes = ManualEchoes(ui);
    CHECK(!echoes.empty() && echoes.back().hdr_.transaction_code == OMS_TRADE && echoes.back().hdr_.uid_.strategy_id == 0);
    CHECK(std::count_if(echoes.begin(), echoes.end(), [](const oms_transaction3l& echo_) { return echo_.hdr_.transaction_code == OMS_TRADE; }) == 2);
    const auto updates = OrderUpdates(ui);  // and as order updates: the GUI order book sees both fills
    CHECK(std::count_if(updates.begin(), updates.end(), [](const ExternalOrderResponse& update_) { return update_._response._response == OrderResponse_FILLED; }) == 2);

    Bcast(LEG1, 760, 790);  // flat now: nothing more
    CHECK(exchange::sent.size() == sent + 2);
    std::printf("  square-off: long 130 sold @760, short 65 bought @335, echoed on 5001 and as order updates, flat, repeat press and later broadcasts ignored\n");
}

// Square-off while a pack waits for its leg2 hedge: strategies stop and drop their positions, so the hedge
// never re-opens leg2 once the account is flat.
void SquareOffStopsStrategies() {
    constexpr int ID = 32;
    SendConfig(RatioConfig(ID, "Applied"));
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 540, 550, false);
    ThinLeg2();
    exchange::Pump();  // leg2 hedge waits for a third ask
    CHECK(Position(ID, LEG0) == -3 * LOT && Position(ID, LEG1) == LOT && Position(ID, LEG2) == 0);

    std::map<int, long> squareOffBefore;  // strategy 0 also holds earlier square-off and manual fills
    for (const int token : {LEG0, LEG1, LEG2}) squareOffBefore[token] = Position(0, token);
    exchange::dbPositions = {{LEG0, -3 * LOT}, {LEG1, LOT}};
    aef::infra::ui_cmd::UIStruct packet{};
    packet.header.message_code = UiMessageCode_SQUARE_OFF;
    packet.header.interface_id = 7;
    exchange::hub->onUIRequest(packet);
    exchange::Pump();  // the strategy's leg2 hedge is cancelled
    Bcast(LEG0, 530, 560);
    Bcast(LEG1, 760, 790);
    exchange::Tick(LEG2, 315, 325);  // a third ask returns
    TickAllLegs();
    const auto net = [&](int token_) { return Position(ID, token_) + Position(0, token_) - squareOffBefore[token_]; };
    CHECK(net(LEG0) == 0 && net(LEG1) == 0 && net(LEG2) == 0);
    CHECK(exchange::LiveOrdersOf(ID) == 0);
    std::printf("  square-off mid-hedge: strategy stopped and forgot its lots, account flat, leg2 never bought\n");
}

// A tick prices square-off too: on 2026-10-14 15:04 no broadcast came and 7 tokens were never sent. An order the
// exchange cancels is dropped, so the next press takes its token again instead of "already running".
void SquareOffFromTicksAndCancel() {
    exchange::dbPositions = {{LEG1, LOT}};
    const size_t sent     = exchange::sent.size();
    aef::infra::ui_cmd::UIStruct packet{};
    packet.header.message_code = UiMessageCode_SQUARE_OFF;
    packet.header.interface_id = 7;
    exchange::hub->onUIRequest(packet);
    exchange::Tick(LEG1, 770, 780, false);  // third bid 760: SELL 65 @760 sent, not answered yet
    CHECK(exchange::sent.size() == sent + 1);
    const oms_transaction sell = exchange::sent[sent];
    CHECK(sell.packet_.product_id_ == LEG1 && sell.packet_.flags_.order_side == SELL_SIDE && sell.packet_.price_ == 760 && sell.packet_.quantity_ == LOT);

    exchange::outbox.clear();  // the exchange cancels it instead
    exchange::Respond(sell, OMS_ORDER_CANCELLED, 760, LOT);
    exchange::hub->onUIRequest(packet);  // pressed again: LEG1 is taken again
    exchange::Tick(LEG1, 770, 780);
    CHECK(exchange::sent.size() == sent + 2 && exchange::sent.back().packet_.price_ == 760 && !exchange::live.count(exchange::sent.back().hdr_.uid_.id_));
    std::printf("  square-off from ticks: SELL 65 @760 on a tick, exchange cancel dropped, next press re-sent and filled\n");
}

// Token-wise square-off (1332) flattens only the token the GUI names; other open tokens stay.
void SquareOffOneToken() {
    exchange::dbPositions = {{LEG1, LOT}, {LEG2, -LOT}};
    const size_t sent     = exchange::sent.size();

    oms_transaction3l request{};
    request.packet_.product_id_[0] = LEG2;
    aef::infra::ui_cmd::UIStruct packet{};
    packet.header.message_code = UiMessageCode_SQUARE_OFF_TOKEN;
    packet.header.interface_id = 7;
    std::memcpy(packet.message, &request, sizeof(request));
    exchange::hub->onUIRequest(packet);
    Bcast(LEG1, 760, 790);
    Bcast(LEG2, 305, 335);
    CHECK(exchange::sent.size() == sent + 1);
    const oms_transaction& buy = exchange::sent[sent];
    CHECK(buy.packet_.product_id_ == LEG2 && buy.packet_.flags_.order_side == BUY_SIDE && buy.packet_.price_ == 335 && buy.packet_.quantity_ == LOT);
    std::printf("  token-wise square-off: short 65 of LEG2 bought @335, LEG1 left open\n");
}

int main() {
    std::ofstream("dry_run_config.json") << R"({"client":1,"algoid":1,"omsid":1})";

    for (const auto& [token, strike] : std::map<int, int>{{LEG0, 2320000}, {LEG1, 2310000}, {LEG2, 2330000}}) {
        ProductDetails details{};
        details.product_id_   = token;
        details.strike_price_ = strike;
        details.lot_size_     = LOT;
        details.tick_size_    = exchange::TICK;
        details.opt_type_     = aef::infra::product::OPTION_TYPE::CE;
        std::strncpy(details.symbol, "NIFTY", sizeof(details.symbol) - 1);
        exchange::products[token] = details;
    }

    MinixStrategy hub(nullptr);
    exchange::hub = &hub;

    std::printf("ratio dry run\n");
    BidHoldsOnlyWhenPassive();
    BidCrossesWhenMarketBeatsTarget();
    BidNotDeeperThanAllowedDepth();
    FullCycle();
    StopRacesPendingBid();
    StopKeepsHedging();
    RejectsUnknownToken();
    HedgeTakesLevel3();
    HedgesHighestPriceFirst();
    HedgeRepricedEveryTick();
    ShortPackFromGuiPayload();
    SlippageInPaise();
    BidFollowsP1();
    UnreachableTargetSendsNothing();
    StrikeGapBackSolve();
    UnhandledRejectRetries();
    DeleteWaitsUntilWhole();
    EarlyHedgeThenFill();
    EarlyHedgeChasesLeg0();
    EarlyHedgePulledWithoutFill();
    ReverseWhenSpreadGone();
    ReversePartialCancelsRest();
    EarlyHedgeSkippedWhenSpreadGone();
    HedgeIgnoresZeroSlice();
    SpreadSentOnlyOnChange();
    QuietTicks();
    ManualOrders();
    SquareOffFlattens();
    SquareOffStopsStrategies();
    SquareOffOneToken();
    SquareOffFromTicksAndCancel();
    std::printf("all scenarios passed\n");
}
