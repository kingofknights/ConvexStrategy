// Dry run of the ratio strategy against a simulated platform and exchange.
//
// Links the real MinixStrategy, RatioLegStrategy and vendor order_instance against a fake
// AlgoBase (no libAlgoBase.so), feeds GUI config through the chunked UI protocol, ticks the
// book, and fills orders the way the exchange would. Every scenario asserts on what the
// exchange saw, not on strategy internals.
//
// Build and run from the repo root (one line):
//   g++ -std=c++20 -O1 -g -Wall -Wextra -DFMT_HEADER_ONLY -I Convex -I Convex/RatioLeg -I vendor/minix/include -I vendor/minix -I vendor
//       tests/ratio_dry_run.cpp Convex/MinixStrategy.cpp Convex/RatioLeg/RatioLegStrategy.cpp vendor/minix/src/order_instance.cpp
//       -o build/ratio_dry_run && (cd build && ./ratio_dry_run)

#include "MinixStrategy.hpp"

#include <cassert>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <string>
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
        }
    }

    // pump_ = false leaves the strategy's requests unanswered, as if the exchange were slow.
    auto Tick(int token_, int bid_, int ask_, bool pump_ = true) -> void {
        books[token_] = Book{bid_, ask_};
        Quote quote{};
        quote.header.product_id = token_;
        for (int level = 0; level < aef::infra::quote::QUOTE_LEVELS; ++level) {
            if (bid_ > 0) quote.message.bid_levels[level] = {300, bid_ - level * TICK, 3, 0};
            if (ask_ > 0) quote.message.ask_levels[level] = {300, ask_ + level * TICK, 3, 0};
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

// ═══ Scenario helpers ════════════════════════════════════════════════════════

constexpr int LEG0 = 49677, LEG1 = 49629, LEG2 = 49713, MISSING_TOKEN = 99999;
constexpr int LOT  = 30;

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

// 3:1:2 ratio, long pack: SELL 3 x leg0 (bids), BUY 1 x leg1, BUY 2 x leg2.
auto RatioConfig(int strategyId_, const std::string& status_, int legToken0_ = LEG0, int marketOrderRetries_ = 0) -> nlohmann::json {
    return {
        {"Strategy", {{"SubType", "3LegRatio"}, {"Status", status_}, {"StrategyId", strategyId_}}},
        {"Legs", {{{"Token", legToken0_}, {"Side", "SELL"}}, {{"Token", LEG1}, {"Side", "BUY"}}, {{"Token", LEG2}, {"Side", "BUY"}}}},
        {"Ratio", {{"LegRatios", {3, 1, 2}}}},
        {"Params", {{"LongBuySoQ", 1}, {"LongBuyQty", 2}, {"LongBuyPrice", 1.40}, {"ShortSellSoQ", 0}, {"ShortSellQty", 0}, {"ShortSellPrice", 0.0}, {"TickSize", 1}, {"OrderDepth", 1}, {"PriceDepth", 1}, {"AllowedBidDepth", 1}, {"ThresholdQty", 100}, {"AllowedSlippage", 10}, {"TradeGear", 0}, {"MarketOrderRetries", marketOrderRetries_}}},
    };
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

// ═══ Scenarios ═══════════════════════════════════════════════════════════════

// Full life: subscribe, apply, partial bid fills, hedge, reprice, second pack, done.
void FullCycle() {
    constexpr int ID = 10;
    SendConfig(RatioConfig(ID, "Subscribed"));
    CHECK(LastStatusEcho()._status == StrategyStatus_ACTIVE);
    TickAllLegs();
    CHECK(exchange::LiveOrdersOf(ID) == 0);  // ACTIVE only watches

    SendConfig(RatioConfig(ID, "Applied"));
    CHECK(LastStatusEcho()._status == StrategyStatus_APPLIED);
    exchange::Tick(LEG0, 525, 535);  // market spread 3*525 - 780 - 2*325 = 145 >= 140: bid
    CHECK(exchange::LiveOrdersOf(ID) == 1);
    const auto& bid = exchange::live.begin()->second;
    CHECK(bid.price == 535 && bid.openQty == 3 * LOT && bid.request.packet_.flags_.order_side == SELL_SIDE);
    CHECK(bid.request.hdr_.uid_.composite_id_.request_id == 1);  // _clientUid starts at 0, first order is 1

    exchange::FillPassive(ID, LEG0, 2 * LOT);  // 2 of 3 lots: no whole pack yet, nothing to hedge
    CHECK(Position(ID, LEG1) == 0 && Position(ID, LEG2) == 0);
    CHECK(exchange::LiveOrdersOf(ID) == 1 && exchange::live.begin()->second.openQty == LOT);

    exchange::FillPassive(ID, LEG0, LOT);  // pack complete: hedges cross at once, then the next slice
    CHECK(Position(ID, LEG0) == -3 * LOT && Position(ID, LEG1) == LOT && Position(ID, LEG2) == 2 * LOT);
    CHECK(CountUi(UiMessageCode_TRADE_TRACER) == 1);
    CHECK(exchange::LiveOrdersOf(ID) == 1 && exchange::live.begin()->second.openQty == 3 * LOT);

    exchange::Tick(LEG0, 530, 540);  // ask moves one tick: bid reprices to 540
    CHECK(exchange::live.begin()->second.price == 540);

    exchange::FillPassive(ID, LEG0, 3 * LOT);
    CHECK(Position(ID, LEG0) == -6 * LOT && Position(ID, LEG1) == 2 * LOT && Position(ID, LEG2) == 4 * LOT);
    CHECK(CountUi(UiMessageCode_TRADE_TRACER) == 2);

    TickAllLegs();
    CHECK(exchange::LiveOrdersOf(ID) == 0);  // total quantity reached: no more orders

    exchange::hub->doWork();
    StrategySpreadUpdate spread{};
    for (const auto& packet : exchange::uiPackets) {
        if (packet.header.message_code == UiMessageCode_STRATEGY_SPREAD) {
            std::memcpy(&spread, packet.message, sizeof(spread));
            if (spread._strategyId == ID) break;
        }
    }
    CHECK(spread._strategyId == ID && spread._bTrQ == 2 && spread._sTrQ == 0);
    std::printf("  full cycle: 2 packs, legs -180/+60/+120, B-TrQ=%d B-ATP=%.2f RLP=%.2f\n", spread._bTrQ, spread._bATP, spread._rlp);
}

// STOP while the new bid awaits its ack: the vendor refuses that cancel, so the ack must pull it.
void StopRacesPendingBid() {
    constexpr int ID = 11;
    SendConfig(RatioConfig(ID, "Applied"));
    exchange::Tick(LEG1, 770, 780);         // hedge legs first: no leg0 price yet, so no bid
    exchange::Tick(LEG2, 315, 325);
    exchange::Tick(LEG0, 525, 535, false);  // bid request sent, not yet acknowledged
    CHECK(exchange::outbox.size() == 1);

    SendConfig(RatioConfig(ID, "Stop"), false);  // STOP arrives before the exchange answers
    CHECK(LastStatusEcho()._status == StrategyStatus_INACTIVE);

    exchange::Pump();  // ack -> ProcessPack -> cancel -> cancelled
    CHECK(exchange::LiveOrdersOf(ID) == 0);
    std::printf("  stop race: late-acked bid cancelled, none left resting\n");
}

// The bid fills while its cancel is in flight and leg2 has no asks; then STOP.
// Hedging must continue after STOP until flat.
void StopKeepsHedging() {
    constexpr int ID = 13;
    SendConfig(RatioConfig(ID, "Applied"));
    TickAllLegs();
    CHECK(exchange::LiveOrdersOf(ID) == 1);

    exchange::Tick(LEG2, 315, 0, false);  // leg2 asks vanish: depth gate sends a cancel for the bid
    CHECK(exchange::outbox.size() == 1 && exchange::outbox.front().hdr_.transaction_code == OMS_CANCEL_REQ);

    exchange::FillPassive(ID, LEG0, 3 * LOT);  // fill beats the cancel; the cancel is rejected
    CHECK(Position(ID, LEG1) == LOT && Position(ID, LEG2) == 0);  // leg2 waits for a real price

    SendConfig(RatioConfig(ID, "Stop"));
    CHECK(LastStatusEcho()._status == StrategyStatus_INACTIVE);
    exchange::Tick(LEG2, 315, 330);  // asks return
    CHECK(Position(ID, LEG2) == 2 * LOT);
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

// With MarketOrderRetries > 0 each pack's hedge starts at the snapshot price, not where the last pack's retries left off.
void HedgeRetriesResetPerPack() {
    constexpr int ID = 14;
    SendConfig(RatioConfig(ID, "Applied", LEG0, 3));
    TickAllLegs();
    for (int pack = 1; pack <= 2; ++pack) {
        exchange::FillPassive(ID, LEG0, 3 * LOT);
        CHECK(Position(ID, LEG2) == pack * 2 * LOT);
        int firstHedgePrice = 0;  // first LEG2 placement of this pack
        int placements      = 0;
        for (const auto& request : exchange::sent) {
            if (request.hdr_.uid_.composite_id_.strategy_id == ID && request.packet_.product_id_ == LEG2 &&
                request.hdr_.transaction_code == OMS_PLACE_REQ && ++placements == pack) {
                firstHedgePrice = request.packet_.price_;
            }
        }
        CHECK(firstHedgePrice == 325);  // snapshot ask, zero retry ticks
    }
    std::printf("  hedge retries reset per pack: both packs hedged leg2 at the 325 snapshot\n");
}

// An idle pack must not hit the vendor's refusal logs on every tick.
void QuietTicks() {
    const int before = exchange::vendorRefusalLogs;
    for (int i = 0; i < 1000; ++i) TickAllLegs();
    std::printf("  3000 idle ticks: %d vendor log lines\n", exchange::vendorRefusalLogs - before);
    CHECK(exchange::vendorRefusalLogs - before == 0);
}

int main() {
    std::ofstream("dry_run_config.json") << R"({"client":1,"algoid":1,"omsid":1})";

    for (const auto& [token, strike] : std::map<int, int>{{LEG0, 6400000}, {LEG1, 6250000}, {LEG2, 6550000}}) {
        ProductDetails details{};
        details.product_id_   = token;
        details.strike_price_ = strike;
        details.lot_size_     = LOT;
        details.tick_size_    = exchange::TICK;
        details.opt_type_     = aef::infra::product::OPTION_TYPE::CE;
        std::strncpy(details.symbol, "BANKNIFTY", sizeof(details.symbol) - 1);
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
    QuietTicks();
    std::printf("all scenarios passed\n");
}
