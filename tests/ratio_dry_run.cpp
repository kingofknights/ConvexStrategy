// Dry run of the ratio strategy against a simulated platform and exchange.
//
// Links the real MinixStrategy, RatioLegStrategy and vendor order_instance against a fake
// AlgoBase (no libAlgoBase.so) and a fake trade DB (no Postgres), feeds GUI config through the chunked
// UI protocol, ticks the book, and fills orders the way the exchange would. Every scenario asserts on what the
// exchange saw, not on strategy internals.
//
// Build and run from the repo root (one line):
//   g++ -std=c++20 -O1 -g -Wall -Wextra -DFMT_HEADER_ONLY -I Convex -I Convex/RatioLeg -I Convex/ManualOrder -I Convex/SquareOff
//       -I vendor/minix/include -I vendor/minix -I vendor tests/ratio_dry_run.cpp Convex/MinixStrategy.cpp Convex/RatioLeg/RatioLegStrategy.cpp
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
            switch (request.hdr_.transaction_code) {
                case OMS_PLACE_REQ:
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
    // Every level holds levelQty_; level n is n ticks away from the touch.
    auto Tick(int token_, int bid_, int ask_, bool pump_ = true, int levelQty_ = 300) -> void {
        books[token_] = Book{bid_, ask_};
        Quote quote{};
        quote.header.product_id = token_;
        for (int level = 0; level < aef::infra::quote::QUOTE_LEVELS; ++level) {
            if (bid_ > 0) quote.message.bid_levels[level] = {levelQty_, bid_ - level * TICK, 3, 0};
            if (ask_ > 0) quote.message.ask_levels[level] = {levelQty_, ask_ + level * TICK, 3, 0};
        }
        hub->OnTick(quote);
        if (pump_) {
            MatchCrossing();
            Pump();
        }
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

// The GUI payload as sent, with only id and status replaced.
auto GuiConfig(int strategyId_, const std::string& status_) -> nlohmann::json {
    nlohmann::json config               = nlohmann::json::parse(GUI_PAYLOAD);
    config["Strategy"]["StrategyId"]    = strategyId_;
    config["Strategy"]["Status"]        = status_;
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

auto CountUi(UiMessageCode code_) -> int {
    int count = 0;
    for (const auto& packet : exchange::uiPackets) count += packet.header.message_code == code_;
    return count;
}

void TickAllLegs() {
    exchange::Tick(LEG0, 525, 535);
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
}

auto Position(int strategy_, int token_) -> long { return exchange::position[{strategy_, token_}]; }

// Price of a strategy's working order on a token, 0 when none.
auto LivePrice(int strategy_, int token_) -> int {
    for (const auto& [uid, order] : exchange::live) {
        if (order.strategy == strategy_ && order.request.packet_.product_id_ == token_) return order.price;
    }
    return 0;
}

// ═══ Scenarios ═══════════════════════════════════════════════════════════════

// Full life: subscribe, apply, bid rests at the target price while the market misses, fills once the bid reaches it, done.
void FullCycle() {
    constexpr int ID = 10;
    SendConfig(RatioConfig(ID, "Subscribed"));
    CHECK(LastStatusEcho()._status == StrategyStatus_ACTIVE);
    TickAllLegs();
    CHECK(exchange::LiveOrdersOf(ID) == 0);  // ACTIVE only watches

    SendConfig(RatioConfig(ID, "Applied"));
    CHECK(LastStatusEcho()._status == StrategyStatus_APPLIED);
    exchange::Tick(LEG0, 500, 510);  // market 3*500 - 780 - 2*325 = 70 < 140: SELL rests at (140 + 1430) / 3 = 523.33 -> 525
    const oms_transaction bid = NthPlace(ID, LEG0, 1);
    CHECK(bid.packet_.flags_.order_type == LIMIT_ORDER_TYPE && bid.packet_.price_ == 525 && bid.packet_.quantity_ == 3 * LOT);
    CHECK(bid.hdr_.uid_.composite_id_.request_id == 1);  // _clientUid starts at 0, first order is 1
    CHECK(exchange::LiveOrdersOf(ID) == 1 && Position(ID, LEG0) == 0);

    exchange::Tick(LEG0, 525, 535);  // bid reaches 525: the resting SELL fills, the next pack crosses at once
    CHECK(Position(ID, LEG0) == -6 * LOT && Position(ID, LEG1) == 2 * LOT && Position(ID, LEG2) == 4 * LOT);
    CHECK(CountUi(UiMessageCode_TRADE_TRACER) == 2);
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
    std::printf("  full cycle: rests @525 at market 70, 2 packs filled @525, legs %ld/%+ld/%+ld, B-TrQ=%d B-ATP=%.2f RLP=%.2f\n",
                Position(ID, LEG0), Position(ID, LEG1), Position(ID, LEG2), spread._bTrQ, spread._bATP, spread._rlp);
}

// Market spread meets target but a hedge leg is thin: every evaluation logs the missed entry and its level.
void MissedEntryLogsEveryTick() {
    constexpr int ID = 17;
    SendConfig(RatioConfig(ID, "Applied"));
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325, true, 10);  // 4 levels x 10 < 130 needed for 2 lots
    exchange::Tick(LEG0, 525, 535);            // market 145 >= 140
    CHECK(exchange::LiveOrdersOf(ID) == 0);
    std::filesystem::path logName;  // newest: log/ keeps the files of earlier runs
    for (const auto& entry : std::filesystem::recursive_directory_iterator("log")) {
        if (entry.path().filename().string().find("Ratio_17_") != std::string::npos &&
            (logName.empty() || entry.last_write_time() > std::filesystem::last_write_time(logName))) {
            logName = entry.path();
        }
    }
    const auto countLines = [&](std::string_view text_) {
        std::ifstream file(logName);
        int           count = 0;
        for (std::string line; std::getline(file, line);) count += line.find(text_) != std::string::npos;
        return count;
    };
    const int before = countLines("thin hedge leg=2");
    exchange::Tick(LEG0, 525, 535);
    exchange::Tick(LEG0, 525, 535);
    CHECK(countLines("thin hedge leg=2") == before + 2);
    CHECK(countLines("quantity 40 over 4 levels < required 130") >= 2);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  missed entry: thin hedge logged on each tick while market meets target\n");
}

// STOP while a crossing bid awaits the exchange: the bid still fills, and its hedges complete after STOP.
void StopRacesPendingBid() {
    constexpr int ID = 11;
    SendConfig(RatioConfig(ID, "Applied"));
    exchange::Tick(LEG1, 770, 780);         // hedge legs first: no leg0 price yet, so no bid
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 525, 535, false);  // SELL 525 sent at the 525 bid, not yet answered
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
    exchange::Tick(LEG0, 525, 535, false);  // bid sent at the 325 leg2 snapshot
    exchange::Tick(LEG2, 315, 0, false);    // leg2 asks vanish before the exchange answers
    exchange::Pump();                       // bid fills: leg1 hedges, leg2 rests at 325 with no ask to take
    CHECK(Position(ID, LEG0) == -3 * LOT && Position(ID, LEG1) == LOT && Position(ID, LEG2) == 0);

    SendConfig(RatioConfig(ID, "Stop"));
    CHECK(LastStatusEcho()._status == StrategyStatus_ACTIVE);
    exchange::Tick(LEG2, 315, 330);  // asks return
    CHECK(Position(ID, LEG2) == 2 * LOT);
    CHECK(NthPlacePrice(ID, LEG2, 1) == 325);  // MarketRetries 5: starts at the 325 snapshot, steps to 330
    CHECK(exchange::LiveOrdersOf(ID) == 0);
    TickAllLegs();
    CHECK(exchange::LiveOrdersOf(ID) == 0);  // stopped: no new bid
    std::printf("  stop keeps hedging: leg2 hedged after STOP once asks returned, flat\n");
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

// With MarketRetries > 0 each pack's hedge starts at the snapshot price, not where the last pack's retries left off.
void HedgeRetriesResetPerPack() {
    constexpr int ID = 14;
    SendConfig(RatioConfig(ID, "Applied"));
    TickAllLegs();  // market 145 >= 140: two packs cross back to back
    CHECK(Position(ID, LEG2) == 2 * 2 * LOT);
    CHECK(NthPlacePrice(ID, LEG2, 1) == 325 && NthPlacePrice(ID, LEG2, 2) == 325);  // snapshot ask, zero retry ticks
    std::printf("  hedge retries reset per pack: both packs hedged leg2 at the 325 snapshot\n");
}

// The GUI payload verbatim: short pack, 5 packs at a credit of at least -0.05.
// Short credit = bid(23100) + 2 x bid(23300) - 3 x ask(23200).
void ShortPackFromGuiPayload() {
    constexpr int ID = 15;
    SendConfig(GuiConfig(ID, "Applied"));
    TickAllLegs();  // credit 770 + 2*315 - 3*535 = -205 < -5: BUY rests at (1400 + 5) / 3 = 468.33 -> 465 (rounded down)
    const oms_transaction bid = NthPlace(ID, LEG0, 1);
    CHECK(bid.packet_.flags_.order_type == LIMIT_ORDER_TYPE && bid.packet_.flags_.order_side == BUY_SIDE && bid.packet_.price_ == 465);
    exchange::Tick(LEG0, 470, 475);  // credit -25 < -5: still resting
    CHECK(exchange::LiveOrdersOf(ID) == 1 && LivePrice(ID, LEG0) == 465 && Position(ID, LEG0) == 0);

    exchange::Tick(LEG0, 460, 465);  // ask reaches 465: fills, then each pack crosses until done
    CHECK(Position(ID, LEG0) == 15 * LOT && Position(ID, LEG1) == -5 * LOT && Position(ID, LEG2) == -10 * LOT);
    CHECK(exchange::LiveOrdersOf(ID) == 0);
    std::printf("  GUI payload verbatim: short pack BUY rests @465, 5 packs filled, hedged SELL 23100 + 2 x 23300\n");
}

// AllowedSlippage is in paise. Leg2 hedges at 330 instead of 325: traded 1575 - 780 - 660 = 135, 5 paise under target 140.
// AllowedSlippage 4 stops before the second pack; 10 lets it trade.
auto SlippedPacks(int strategyId_, int allowedSlippagePaise_) -> long {
    nlohmann::json config               = RatioConfig(strategyId_, "Applied");
    config["Params"]["AllowedSlippage"] = allowedSlippagePaise_;
    SendConfig(config);
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 525, 535, false);  // bid sent at the 325 leg2 snapshot
    exchange::Tick(LEG2, 315, 0, false);    // leg2 asks vanish before the exchange answers
    exchange::Pump();                       // bid fills, leg2 hedge rests at 325
    exchange::Tick(LEG2, 315, 330);         // hedge retries up to 330: pack done, 5 paise slipped
    CHECK(Position(strategyId_, LEG2) == 2 * LOT);
    exchange::Tick(LEG2, 315, 325);         // market 145 >= 140 again: second pack unless stopped
    SendConfig(RatioConfig(strategyId_, "Stop"));
    return -Position(strategyId_, LEG0) / (3 * LOT);
}

void SlippageInPaise() {
    CHECK(SlippedPacks(18, 4) == 1);
    CHECK(SlippedPacks(19, 10) == 2);
    std::printf("  slippage in paise: 5 paise slip stops at AllowedSlippage 4, trades on at 10\n");
}

// The resting bid follows the hedge legs, modified only when its price moves by TickSize ticks (2 x 5 paise).
void BidTracksHedgeLegs() {
    constexpr int  ID              = 20;
    nlohmann::json config          = RatioConfig(ID, "Applied");
    config["Params"]["TickSize"]   = 2;
    SendConfig(config);
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 500, 510);  // (140 + 780 + 2*325) / 3 = 523.33 -> SELL rests at 525
    CHECK(LivePrice(ID, LEG0) == 525);
    exchange::Tick(LEG2, 320, 330);  // (140 + 780 + 2*330) / 3 = 526.67 -> 530: moved 5 < 10, not modified
    CHECK(LivePrice(ID, LEG0) == 525);
    exchange::Tick(LEG2, 330, 340);  // (140 + 780 + 2*340) / 3 = 533.33 -> 535: moved 10, modified
    CHECK(LivePrice(ID, LEG0) == 535);
    exchange::Tick(LEG0, 535, 545);  // bid reaches 535: both packs fill, traded 1605 - 780 - 680 = 145 >= 140
    CHECK(Position(ID, LEG0) == -6 * LOT && Position(ID, LEG1) == 2 * LOT && Position(ID, LEG2) == 4 * LOT);
    std::printf("  bid tracks hedges: 525 kept on a 5 paise move, 535 after a 10 paise move, filled\n");
}

// Market beats the target: the bid is still priced at the target, so it crosses past the touch.
void BidCrossesWhenMarketBeatsTarget() {
    constexpr int ID = 21;
    SendConfig(RatioConfig(ID, "Applied"));
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 540, 550);  // market 3*540 - 1430 = 190 > 140: SELL priced at 525, under the 540 bid
    CHECK(NthPlacePrice(ID, LEG0, 1) == 525 && NthPlacePrice(ID, LEG0, 2) == 525);
    CHECK(Position(ID, LEG0) == -6 * LOT && exchange::LiveOrdersOf(ID) == 0);
    std::printf("  market 190 > target 140: SELL @525 crosses the 540 bid, 2 packs filled\n");
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
    exchange::Tick(LEG0, 500, 510);  // raw market 70 >= 0: raw target -9860 + 10000 = 140 -> SELL rests at 525
    CHECK(NthPlacePrice(ID, LEG0, 1) == 525 && Position(ID, LEG0) == 0);
    config["Strategy"]["Status"] = "Stop";
    SendConfig(config);
    std::printf("  strike gap: ConRev target -98.60 rests at 525\n");
}

// Leg 0 fills 1 of the 3 lots a pack needs: no hedge covers it. After TimeToRevertBidMs the rest of the bid is
// cancelled, the lot is bought back at the ask, and the bid rests again for a whole pack.
void PartialPackUnwinds() {
    constexpr int  ID                     = 24;
    nlohmann::json config                 = RatioConfig(ID, "Applied");
    config["Params"]["TimeToRevertBidMs"] = 20;
    SendConfig(config);
    exchange::Tick(LEG1, 770, 780);
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 500, 510);  // market 70 < 140: SELL 3 lots rests at 525
    CHECK(LivePrice(ID, LEG0) == 525);

    exchange::FillPassive(ID, LEG0, LOT);  // 1 of 3 lots: part pack, nothing to hedge
    exchange::Tick(LEG0, 500, 510);
    CHECK(Position(ID, LEG0) == -LOT && Position(ID, LEG1) == 0 && Position(ID, LEG2) == 0);
    CHECK(LivePrice(ID, LEG0) == 525);  // rest of the pack still bids before the timeout

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    exchange::Tick(LEG0, 500, 510);  // expired: cancel the bid, BUY 1 lot IOC at the 510 ask, then rest again
    const oms_transaction unwind = NthPlace(ID, LEG0, 2);
    CHECK(unwind.packet_.flags_.order_type == IOC_ORDER_TYPE && unwind.packet_.flags_.order_side == BUY_SIDE);
    CHECK(unwind.packet_.price_ == 510 && unwind.packet_.quantity_ == LOT);
    CHECK(Position(ID, LEG0) == 0 && Position(ID, LEG1) == 0 && Position(ID, LEG2) == 0);
    const oms_transaction rebid = NthPlace(ID, LEG0, 3);
    CHECK(rebid.packet_.flags_.order_side == SELL_SIDE && rebid.packet_.price_ == 525 && rebid.packet_.quantity_ == 3 * LOT);

    exchange::Tick(LEG0, 525, 535);  // bid reaches 525: both packs fill whole and hedge
    CHECK(Position(ID, LEG0) == -6 * LOT && Position(ID, LEG1) == 2 * LOT && Position(ID, LEG2) == 4 * LOT);
    SendConfig(RatioConfig(ID, "Stop"));
    std::printf("  part pack: 1 of 3 lots sold @525, bought back @510 after 20 ms, 2 whole packs traded after\n");
}

auto StrategyLogBytes() -> uintmax_t {
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

    SendManual(OMS_REPLACE_REQ, 1, {{LEG1, BUY_SIDE, 105, LOT}});
    CHECK(exchange::live.at(placed.hdr_.uid_.id_).price == 105);
    SendManual(OMS_CANCEL_REQ, 1, {{LEG1, BUY_SIDE, 105, LOT}});
    CHECK(!exchange::live.count(placed.hdr_.uid_.id_));
    CHECK(ManualEchoes(ui).back().hdr_.transaction_code == OMS_ORDER_CANCELLED);

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

    // Two legs: each order goes out on its own leg's token.
    SendManual(OMS_PLACE_REQ, 2, {{LEG1, BUY_SIDE, 100, LOT}, {LEG2, SELL_SIDE, 900, LOT}});
    CHECK(exchange::sent.size() == sent + 2);
    CHECK(exchange::sent[sent].packet_.product_id_ == LEG1 && exchange::sent[sent + 1].packet_.product_id_ == LEG2);
    CHECK(exchange::sent[sent + 1].packet_.flags_.order_side == SELL_SIDE && exchange::sent[sent + 1].packet_.price_ == 900);
    std::printf("  manual orders: place / modify / cancel echoed on 5001, bad requests rejected, 2 legs on their own tokens\n");
}

// ═══ Square-off (GUI 1331) ═══════════════════════════════════════════════════

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

    Bcast(LEG1, 760, 790);  // flat now: nothing more
    CHECK(exchange::sent.size() == sent + 2);
    std::printf("  square-off: long 130 sold @760, short 65 bought @335, echoed on 5001, flat, repeat press and later broadcasts ignored\n");
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
    FullCycle();
    StopRacesPendingBid();
    StopKeepsHedging();
    RejectsUnknownToken();
    HedgeRetriesResetPerPack();
    ShortPackFromGuiPayload();
    MissedEntryLogsEveryTick();
    SlippageInPaise();
    BidTracksHedgeLegs();
    BidCrossesWhenMarketBeatsTarget();
    UnreachableTargetSendsNothing();
    StrikeGapBackSolve();
    PartialPackUnwinds();
    QuietTicks();
    ManualOrders();
    SquareOffFlattens();
    std::printf("all scenarios passed\n");
}
