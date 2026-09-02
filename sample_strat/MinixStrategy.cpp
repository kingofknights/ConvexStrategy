/**
 * @file MinixStrategy.cpp
 * @brief Implementation of the sample Minix strategy.
 */
#include "MinixStrategy.hpp"

#include "RatioLegStrategy.hpp"
#include "Utils.hpp"
#include "oms_api.hpp"

#include <nlohmann/json.hpp>
#include <unistd.h>

#include <boost/program_options/options_description.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <cstdint>

using json = nlohmann::json;
using namespace std;
using namespace std::string_literals;
using json = nlohmann::json;

/**
 * @brief Convert nanosecond timestamp to human readable UTC string.
 */
std::ifstream openStream(const char* filename) {
    std::ifstream in{filename};
    if (!in) {
        throw std::runtime_error{"Could not open file: "s + filename};
    }
    return in;
}

/**
 * @brief Load manual order instructions from CSV into memory.
 * @param filename CSV file path relative to working directory.
 */
std::string toLower(const std::string& s) {
    std::string res = s;
    std::transform(res.begin(), res.end(), res.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return res;
}

// Convert to uppercase (for comparison)
static int64_t ljInt(const json& json_, const char* key_, int64_t default_ = 0) {
    if (!json_.contains(key_) || json_[key_].is_null()) {
        return default_;
    }
    const auto& value = json_[key_];
    if (value.is_number_integer()) {
        return value.get<int64_t>();
    }
    if (value.is_number_unsigned()) {
        return static_cast<int64_t>(value.get<uint64_t>());
    }
    if (value.is_number_float()) {
        return static_cast<int64_t>(value.get<double>());
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? 1 : 0;
    }
    if (value.is_string()) {
        try {
            return std::stoll(value.get<std::string>());
        } catch (...) {
            return default_;
        }
    }
    return default_;
}
static double ljDouble(const json& o, const char* k, double def = 0.0) {
    if (!o.contains(k) || o[k].is_null())
        return def;
    const auto& v = o[k];
    if (v.is_number())
        return v.get<double>();
    if (v.is_boolean())
        return v.get<bool>() ? 1.0 : 0.0;
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (...) {
            return def;
        }
    }
    return def;
}
static bool ljBool(const json& o, const char* k, bool def = false) {
    if (!o.contains(k) || o[k].is_null())
        return def;
    const auto& v = o[k];
    if (v.is_boolean())
        return v.get<bool>();
    if (v.is_number())
        return v.get<double>() != 0;
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        return s == "1" || s == "true" || s == "YES" || s == "yes";
    }
    return def;
}

/**
 * @brief Receive a 2L/3L/4L leg/ratio strategy as JSON (sent from the GUI via
 *        connector/receiver, message_code 9621). Parses into the canonical wire
 *        structs (StrategyDatafromui + TokenDatafromui) and stores them keyed
 * by strategynumber. Schema: { "strategytype":"LEG3",
 * "strategy":{<StrategyDatafromui fields>}, "tokens":[ {<TokenDatafromui
 * fields>}, ... ] }
 */
void MinixStrategy::applyLegStrategyJson(int32_t interface_, const std::string& jsonText_) {
    using aef::infra::ui_cmd::BuySell;
    using aef::infra::ui_cmd::StrategyDatafromui;
    using aef::infra::ui_cmd::TokenDatafromui;

    auto sendStatus = [this, interface_](std::string status_, int strategyId_) {
        json response;
        response["Status"]     = status_;
        response["StrategyId"] = strategyId_;
        std::cout << "SendStatus " << response.dump() << std::endl;
        sendJsonChunkedToUI(9621, interface_, response.dump());
    };

    std::cout << "applyLegStrategyJson" << std::endl;
    std::cout << "jsonText: " << jsonText_ << std::endl;
    try {
        json  root     = json::parse(jsonText_);
        json& strategy = root["Strategy"];

        std::string name       = strategy.value("SubType", "");
        std::string status     = strategy.value("Status", "");
        int         strategyId = ljInt(strategy, "StrategyId", 0);
        std::cout << name << " " << status << " " << strategyId << std::endl;

        if (name == "2LegRatio") {
            sendStatus(status, strategyId);
            handleRatioLegStrategy(root, jsonText_, 2, interface_, false);
        } else if (name == "3LegRatio") {
            sendStatus(status, strategyId);
            handleRatioLegStrategy(root, jsonText_, 3, interface_, false);
        } else if (name == "4LegRatio") {
            sendStatus(status, strategyId);
            handleRatioLegStrategy(root, jsonText_, 4, interface_, false);
        } else if (name == "5LegRatio") {
            sendStatus(status, strategyId);
            handleRatioLegStrategy(root, jsonText_, 5, interface_, false);
        } else if (name == "6LegRatio") {
            sendStatus(status, strategyId);
            handleRatioLegStrategy(root, jsonText_, 6, interface_, false);
        } else if (name == "Butterfly") {
            sendStatus(status, strategyId);
            handleRatioLegStrategy(root, jsonText_, 3, interface_, false);
        } else if (name == "Box") {
            sendStatus(status, strategyId);
            handleRatioLegStrategy(root, jsonText_, 4, interface_, true);
        } else if (name == "ConRev") {
            sendStatus(status, strategyId);
            handleRatioLegStrategy(root, jsonText_, 3, interface_, true);
        }

    } catch (const std::exception& e) {
        std::cout << "applyLegStrategyJson failed: " << e.what() << std::endl;
    }
}

void MinixStrategy::handleRatioLegStrategy(const nlohmann::json& root_,
                                           const std::string&    jsonText_,
                                           size_t numLegs_, int32_t interface_, bool gapDiff_) {
    try {
        auto     strategy         = root_["Strategy"];
        auto     status           = strategy["Status"].get<std::string>();
        uint32_t strategyID       = strategy["StrategyId"].get<uint32_t>();
        strategyJson_[strategyID] = jsonText_;
        if (status == "Subscribed" or status == "New") {
            auto iterator = ratioStrats_.find(strategyID);
            if (iterator == ratioStrats_.end()) {
                ratioStrats_[strategyID] = new RatioLegStrategy(this, strategyID, interface_, root_, numLegs_, gapDiff_);
            }
        } else if (status == "Applied") {
            auto iterator = ratioStrats_.find(strategyID);
            if (iterator != ratioStrats_.end()) {
                iterator->second->ParamUpdate(root_);
            } else {
                auto strat               = new RatioLegStrategy(this, strategyID, interface_, root_, numLegs_, gapDiff_);
                ratioStrats_[strategyID] = strat;
                strat->ParamUpdate(root_);
            }
        } else if (status == "Unsubscribed") {
            auto iterator = ratioStrats_.find(strategyID);
            if (iterator != ratioStrats_.end()) {
                iterator->second->Stop();
            }
        } else if (status == "Deleted") {
            auto iterator = ratioStrats_.find(strategyID);
            if (iterator != ratioStrats_.end()) {
                iterator->second->Stop();
                delete iterator->second;
                ratioStrats_.erase(iterator);
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[handleRatioLegStrategy] failed: " << e.what() << std::endl;
    }
}

/**
 * @brief Initialize strategy configuration, subscriptions, and order handles.
 */
MinixStrategy::MinixStrategy(AlgoBase::ContextHandle context_)
    : AlgoBase(context_) {
    namespace pt = boost::property_tree;
    pt::ptree root;
    auto      config_file = get_strategy_config_file();
    std::cout << "filename " << config_file << std::endl;
    std::cerr << ">>> [CTOR-ENTRY] MinixStrategy reading config='" << config_file
              << "'" << std::endl;
    auto is = openStream(config_file.c_str());
    pt::read_json(is, root);

    _client = root.get<int32_t>("client");
    _algoid = root.get<int32_t>("algoid");
    _omsid  = root.get<int32_t>("omsid");

    std::cout << std::fixed << std::setprecision(2);

    // Market-data event flags requested per token: TER + MBP depth + OI + TBT.
    _flags = static_cast<uint16_t>(
                 aef::infra::product::SNAPSHOT_FLAGS::TER_UPDATE_EVENT) |
             static_cast<uint16_t>(
                 aef::infra::product::SNAPSHOT_FLAGS::MBP_UPDATE_EVENT) |
             static_cast<uint16_t>(
                 aef::infra::product::SNAPSHOT_FLAGS::OI_UPDATE_EVENT) |
             static_cast<uint16_t>(
                 aef::infra::product::SNAPSHOT_FLAGS::TBT_UPDATE_EVENT);

    std::cout << "Algo for Box Strategy with client ID : " << _client << std::endl;
    clientUID.composite_id_.client_id   = _client;
    clientUID.composite_id_.strategy_id = 1;

    // Subscribe any tokens listed up front in bcast.csv (one token id per line)
    // so their market data flows to OnTick/onBcastData. Each box also subscribes
    // its own legs.
    std::ifstream file_subtok("bcast.csv");
    std::string   line_subtok;
    while (std::getline(file_subtok, line_subtok)) {
        if (line_subtok.empty())
            continue;
        try {
            subscribeProduct(std::stoi(line_subtok), _flags);
        } catch (const std::exception&) { /* skip non-numeric lines */
        }
    }
}

MinixStrategy::~MinixStrategy() {
    log_info("sample_strat : destructor");
    for (auto& kv : ratioStrats_) delete kv.second;
    ratioStrats_.clear();
}

bool MinixStrategy::subscribeProduct(const int32_t  product_id_,
                                     const uint16_t flags_) {
    LOG_DEBUG("[SUB] subscribeProduct product_id=%d flags=%d", product_id_, flags_);
    return AlgoBase::subscribeProduct(product_id_, flags_);
}

bool MinixStrategy::unSubscribeProduct(const int32_t  product_id_,
                                       const uint16_t flags_) {
    LOG_DEBUG("[SUB] unSubscribeProduct product_id=%d flags=%d", product_id_,
              flags_);
    return AlgoBase::unSubscribeProduct(product_id_, flags_);
}

// function to squareoff using traderId
std::string format_time(std::time_t t_) {
    std::tm*           timeInfo = std::localtime(&t_);
    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << timeInfo->tm_hour << ":"
        << std::setw(2) << std::setfill('0') << timeInfo->tm_min << ":"
        << std::setw(2) << std::setfill('0') << timeInfo->tm_sec;
    return oss.str();
}

void MinixStrategy::OnTick(const Quote& event_) {
    LOG_DEBUG("[TICK] OnTick product_id=%d seq=%d ts=%lu ltp=%d",
              event_.header.product_id, event_.header.sequence_no,
              event_.header.exchange_timestamp, event_.message.ltp_);

    int64_t lastTickTs = 0;
    for (auto& kv : ratioStrats_) {
        kv.second->OnTick(event_, lastTickTs);
        if (kv.second->IsStopped()) {
            kv.second->Stop();
        }
    }
}

// --- clean order-lifecycle logging helpers --------------------------------
static const char* omsEventName(int code_) {
    switch (code_) {
        case 1111:
            return "NEW_REQ";
        case 1112:
            return "MODIFY_REQ";
        case 1113:
            return "CANCEL_REQ";
        case 2222:
            return "PLACED@OMS";
        case 2223:
            return "CONFIRMED@EXCH";
        case 3333:
            return "MODIFY_PLACED@OMS";
        case 3334:
            return "MODIFY_CONFIRMED@EXCH";
        case 4444:
            return "CANCEL_ACCEPTED@OMS";
        case 5555:
            return "CANCELLED@EXCH";
        case 6666:
            return "TRADE_FILL";
        case 6667:
            return "TRADE_PREV_SESSION";
        case 7777:
            return "REJECTED";
        default:
            return "OTHER";
    }
}
static const char* omsSideName(int s_) {
    return s_ == 1 ? "BUY" : (s_ == 2 ? "SELL" : "?");
}

void MinixStrategy::OnOrderResponse(const oms_transaction& order_resp_) {
    for (auto& kv : ratioStrats_) kv.second->OnOrderResponse(order_resp_);

    LOG_DEBUG(
        "[ORDER] RECV %-18s token=%d side=%-4s qty=%d price=%d uid=%d "
        "err=%d reason=%d",
        omsEventName(order_resp_.hdr_.transaction_code),
        order_resp_.packet_.product_id_,
        omsSideName(static_cast<int>(order_resp_.packet_.flags_.order_side)),
        order_resp_.packet_.quantity_, order_resp_.packet_.price_,
        order_resp_.hdr_.uid_.composite_id_.request_id,
        order_resp_.hdr_.error_code, order_resp_.hdr_.reason_code);

    // ponytail: query product details to identify options vs futures
    ProductDetails details;
    bool           is_option = false;
    if (getProductDetails(order_resp_.packet_.product_id_, details)) {
        is_option = details.opt_type_ != aef::infra::product::OPTION_TYPE::FUTXX;
    }

    portfolio_mgr_.on_order_response(order_resp_, is_option);
}

int MinixStrategy::doWork() {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    if (now - lastSpreadSendMs_ >= 1000) {
        lastSpreadSendMs_ = now;
        sendStrategySpreadsToUI();
    }

    for (auto& kv : ratioStrats_) {
        kv.second->CheckHedgeTimeout();
    }

    if (!_strategiesToTerminate.empty()) {
        for (int strategyId : _strategiesToTerminate) {
            auto it = ratioStrats_.find(static_cast<uint32_t>(strategyId));
            if (it != ratioStrats_.end()) {
                nlohmann::json response;
                response["Status"]     = "Unsubscribed";
                response["StrategyId"] = strategyId;
                sendJsonChunkedToUI(9621, it->second->GetInterface(), response.dump());

                it->second->Stop();
            }
        }
        _strategiesToTerminate.clear();
    }
    return 0;
}

void MinixStrategy::Registerfortermination(int strategyId_) {
    _strategiesToTerminate.push_back(strategyId_);
}

void MinixStrategy::sendStrategySpreadsToUI() {
    auto sendRatioUI = [&](auto* ratio) {
        json j;
        j["StrategyId"] = ratio->GetStrategyID();
        j["Status"]     = "Updates";
        j["BCmp"]       = ratio->GetBCmp()._spread / 100.0F;
        j["SCmp"]       = ratio->GetSCmp()._spread / 100.0F;
        j["Cost"]       = static_cast<float>(ratio->GetCost()) / 100.0F;
        j["FLP"]        = static_cast<float>(ratio->GetFLP()) / 100.0F;
        j["Gap"]        = ratio->GetGap();
        j["B-TrQ"]      = ratio->GetLongTradedLots();
        j["S-TrQ"]      = ratio->GetShortTradedLots();
        j["M2M"]        = static_cast<float>(ratio->GetM2M()) / 100.0F;
        j["NLP"]        = static_cast<float>(ratio->GetNetPL()) / 100.0F;
        j["RLP"]        = static_cast<float>(ratio->GetRLP()) / 100.0F;
        j["CLP"]        = static_cast<float>(ratio->GetCutPL()) / 100.0F;
        j["TrSpread"]   = static_cast<float>(ratio->GetRLP()) / 100.0F;
        j["B-ATP"]      = static_cast<float>(ratio->GetBATP()) / 100.0F;
        j["S-ATP"]      = static_cast<float>(ratio->GetSATP()) / 100.0F;
        sendJsonChunkedToUI(9612, ratio->GetInterface(), j.dump());
        ratio->Print();
    };

    for (auto& kv : ratioStrats_) sendRatioUI(kv.second);
}

void MinixStrategy::sendJsonChunkedToUI(int32_t message_code_, int32_t interface_, const std::string& payload_) {
    constexpr size_t max_chunk_size = 1500;
    int32_t          current_ts     = static_cast<int32_t>(
        std::chrono::system_clock::now().time_since_epoch().count() / 1000000);
    int packet_count =
        static_cast<int>((payload_.size() + max_chunk_size - 1) / max_chunk_size);
    if (packet_count < 1)
        packet_count = 1;

    {
        aef::infra::ui_cmd::UIStruct ui{};
        ui.header.message_code   = message_code_;
        ui.header.component_id   = 1;
        ui.header.timestamp      = current_ts;
        ui.header.interface_id   = interface_;
        ui.header.message_length = 1520;
        nlohmann::json header;
        header["packet_count"] = packet_count;
        header["timestamp"]    = current_ts;
        std::string h          = header.dump();
        std::memset(ui.message, 0, sizeof(ui.message));
        std::memcpy(ui.message, h.c_str(), std::min(h.size(), max_chunk_size));
        sentoUI(ui);
    }
    for (int i = 0; i < packet_count; i++) {
        std::string chunk =
            payload_.substr(static_cast<size_t>(i) * max_chunk_size, max_chunk_size);
        aef::infra::ui_cmd::UIStruct ui{};
        ui.header.message_code   = message_code_;
        ui.header.interface_id   = interface_;
        ui.header.message_length = 1520;
        ui.header.component_id   = 1;
        ui.header.timestamp      = current_ts;
        std::memset(ui.message, 0, sizeof(ui.message));
        std::memcpy(ui.message, chunk.c_str(), chunk.size());
        sentoUI(ui);
    }
}

void MinixStrategy::onBcastData(
    const aef::infra::product::product_data& product_details_) {
    if (product_details_.LastTradeTime > 0) {
        std::time_t bt = static_cast<std::time_t>(product_details_.LastTradeTime) +
                         315513000;
        std::string tt = format_time(bt);
        if (tt.size() >= 8 && tt >= "09:14:00" && tt <= "15:31:00")
            _lastTickTs =
                (static_cast<int64_t>((tt[0] - '0') * 36000 + (tt[1] - '0') * 3600 +
                                      (tt[3] - '0') * 600 + (tt[4] - '0') * 60 +
                                      (tt[6] - '0') * 10 + (tt[7] - '0'))) *
                1000000000LL;
    }
    for (auto& kv : ratioStrats_) kv.second->OnBcast(product_details_, _lastTickTs);
}

/**
 * @brief Evaluate parsed CSV rows and fire configured orders.
 */
void MinixStrategy::onUIRequest(const aef::infra::ui_cmd::UIStruct& ui_req_) {
    std::cout << ">>> [GUI-RX] onUIRequest code=" << ui_req_.header.message_code
              << " msg_len=" << ui_req_.header.message_length
              << " iface=" << ui_req_.header.interface_id
              << " comp=" << ui_req_.header.component_id << std::endl;

    // The only UI channel the box uses: chunked strategy-config JSON. The GUI
    // connector (sendStrategyConfig) sends a METADATA packet
    // {"packet_count":N,"timestamp":..} zero-padded to 1500B, then N raw 1500B
    // UTF-8 slices of the JSON, all on the same message_code (9612 config echo
    // channel; 9620/9621 legacy aliases). Reassemble per code, then hand the full
    // JSON to applyLegStrategyJson.
    if (ui_req_.header.message_code == 9612 ||
        ui_req_.header.message_code == 9620 ||
        ui_req_.header.message_code == 9621) {
        const int kMsgBytes = static_cast<int>(sizeof(ui_req_.message));  // 1500
        int       len       = kMsgBytes;
        while (len > 0 && ui_req_.message[len - 1] == '\0')
            --len;  // JSON text never contains NUL
        std::string chunk(ui_req_.message, len);

        JsonReassembly& ra = jsonReassembly_[ui_req_.header.message_code];

        // Metadata header? It is the only fully-parseable packet that carries
        // "packet_count".
        json meta = json::parse(chunk, nullptr, false);
        if (meta.is_object() && meta.contains("packet_count")) {
            ra.expected = static_cast<int>(meta.value("packet_count", 0));
            ra.received = 0;
            ra.buf.clear();
            ra.active = ra.expected > 0;
            std::cout << ">>> [GUI-RX] META code=" << ui_req_.header.message_code
                      << " packet_count=" << ra.expected << std::endl;
            return;
        }

        if (!ra.active) {
            std::cout << ">>> [GUI-RX] WARNING data chunk before metadata (code="
                      << ui_req_.header.message_code << ", " << len << "B) -- ignored"
                      << std::endl;
            return;
        }

        ra.buf += chunk;
        ra.received++;
        std::cout << ">>> [GUI-RX] CHUNK " << ra.received << "/" << ra.expected
                  << " code=" << ui_req_.header.message_code << " bytes=" << len
                  << " (accumulated=" << ra.buf.size() << "B)" << std::endl;

        if (ra.received >= ra.expected) {
            std::string full = std::move(ra.buf);
            ra               = JsonReassembly{};  // reset for the next transfer
            while (!full.empty() && full.back() == '\0')
                full.pop_back();  // strip any padding
            std::cout << ">>> [GUI-RX] REASSEMBLED code="
                      << ui_req_.header.message_code << " total=" << full.size()
                      << "B  JSON below:\n"
                      << full << "\n>>> [GUI-RX] END-JSON" << std::endl;
            applyLegStrategyJson(ui_req_.header.interface_id, full);
        }
        return;
    }
}

/**
 * @brief Push a heartbeat update to the UI layer.
 */
int MinixStrategy::update_order(OrderMap& order_handle_, int32_t token_, int32_t cur_price_, int32_t qty_) {
    // Quote quoteC;
    // getLastQuote(token, quoteC);
    int  uid = 0;
    auto itr = order_handle_.find(token_);
    // std::cout << "Token from Update order : " << token << "cliuid: " <<
    // clientUID.id_ << std::endl;
    if (itr != order_handle_.end()) {
        auto& order = itr->second;
        qty_ -= order.get_filled_qty();
        // std::cout << "QTY from UPdate order : " << qty << std::endl;
        if (order.get_current_state() !=
            static_cast<uint32_t>(
                execution_strat::STRAT_ORDER_STATE::STRAT_INITIAL_STATE)) {
            if (!order.is_response_pending()) {
                if (qty_ > 0 && order.get_open_price() != cur_price_) {
                    order.set_time_stamps(event_timestamp_, trigger_timestamp_,
                                          aef::infra::get_realtime_in_nanos());
                    uid = order.update_order(cur_price_, qty_);
                    LOG_DEBUG(
                        "[ORDER] FIRE MODIFY token=%d side=%-4s price=%d qty=%d uid=%d",
                        token_, omsSideName(static_cast<int>(order.get_side())), cur_price_,
                        qty_, uid);
                    if (uid) {
                        portfolio_mgr_.on_order_modify(order.get_uid(), cur_price_, qty_);
                    }
                }
            }
        } else {
            if (!order.is_response_pending()) {
                if (qty_ > 0 && order.get_open_price() != cur_price_) {
                    clientUID.composite_id_.request_id =
                        ++requestId;  // print_depth(token);
                    if (clientUID.composite_id_.request_id >= MAX_REQUEST_ID) {
                        LOG_ERROR(" Stop Trading AS max allowed Orders Breach");
                    } else {
                        order.set_time_stamps(event_timestamp_, trigger_timestamp_,
                                              aef::infra::get_realtime_in_nanos());
                        uid = order.place_order(cur_price_, qty_, clientUID.id_);
                        LOG_DEBUG(
                            "[ORDER] FIRE NEW    token=%d side=%-4s price=%d qty=%d uid=%d",
                            token_, omsSideName(static_cast<int>(order.get_side())),
                            cur_price_, qty_, uid);
                        if (uid) {
                            portfolio_mgr_.on_order_placed(uid, token_, order.get_side(),
                                                           cur_price_, qty_);
                        }
                        //     cout << strategyNumber << " " <<
                        //     clientUID.composite_id_.client_id << " " <<
                        //     clientUID.composite_id_.strategy_id
                        //     << " " << uid << " Token " << token << " New Strat Order
                        //     Place_Order----->  " << cur_price << " qty " << qty
                        //     << " " << clientUID.id_ << " " << order.get_side() << endl;
                    }
                }
            }
        }
    }
    return uid;
}

auto MinixStrategy::update_order(OrderObjectPtrT& order_, int32_t token_, int32_t price_, int32_t quantity_, client_uid& clientUid_) -> int {
    // Quote quoteC;
    // getLastQuote(token, quoteC);
    int uid = 0;
    // std::cout << "Token from Update order : " << token << "cliuid: " <<
    // clientUID.id_ << std::endl;
    auto& order = order_;
    quantity_ -= order->get_filled_qty();
    if (order->get_current_state() != static_cast<uint32_t>(execution_strat::STRAT_ORDER_STATE::STRAT_INITIAL_STATE)) {
        if (!order->is_response_pending()) {
            if (quantity_ > 0 && order->get_open_price() != price_) {
                order->set_time_stamps(event_timestamp_, trigger_timestamp_, aef::infra::get_realtime_in_nanos());
                uid = order->update_order(price_, quantity_);
                LOG_DEBUG("[ORDER] FIRE MODIFY token=%d side=%-4s price=%d qty=%d uid=%d", token_, omsSideName(static_cast<int>(order->get_side())), price_, quantity_, uid);
                if (uid) {
                    portfolio_mgr_.on_order_modify(order->get_uid(), price_, quantity_);
                }
            }
        }
    } else {
        if (!order->is_response_pending()) {
            if (quantity_ > 0 && order->get_open_price() != price_) {
                clientUid_.composite_id_.request_id = ++requestId;  // print_depth(token);
                if (clientUid_.composite_id_.request_id >= MAX_REQUEST_ID) {
                    LOG_ERROR(" Stop Trading AS max allowed Orders Breach");
                } else {
                    order->set_time_stamps(event_timestamp_, trigger_timestamp_, aef::infra::get_realtime_in_nanos());
                    uid = order->place_order(price_, quantity_, clientUid_.id_);
                    LOG_DEBUG(
                        "[ORDER] FIRE NEW    token=%d side=%-4s price=%d qty=%d uid=%d", token_, omsSideName(static_cast<int>(order->get_side())),
                        price_, quantity_, uid);
                    if (uid) {
                        portfolio_mgr_.on_order_placed(uid, token_, order->get_side(), price_, quantity_);
                    }
                    //     cout << strategyNumber << " " <<
                    //     clientUID.composite_id_.client_id << " " <<
                    //     clientUID.composite_id_.strategy_id
                    //     << " " << uid << " Token " << token << " New Strat Order
                    //     Place_Order----->  " << cur_price << " qty " << qty
                    //     << " " << clientUID.id_ << " " << order.get_side() << endl;
                }
            }
        }
    }
    return uid;
}

void MinixStrategy::sendOrderResponse(const oms_transaction& response_, int32_t interface_, std::string_view name_) {
    constexpr static double      TenYearsInSeconds = 315513000 * 10e9;
    aef::infra::ui_cmd::UIStruct ui{};
    ui.header.message_code   = 9955;
    ui.header.interface_id   = interface_;
    ui.header.message_length = 1520;
    ui.header.component_id   = 1;
    ui.header.timestamp      = 0;
    std::memset(ui.message, 0, sizeof(ui.message));

    ExternalOrderResponse response           = {};
    response._information._orderId           = response_.packet_.exchange_order_id;
    response._information._fields._orderType = response_.packet_.flags_.order_type == ORDER_TYPE::IOC_ORDER_TYPE ? OrderType_IOC : OrderType_LIMIT;
    response._information._fields._side      = response_.packet_.flags_.order_side == BUY_SIDE ? Side_BUY : Side_SELL;
    std::memcpy(response._information._parent, name_.data(), std::min(name_.length(), ParentLength));

    UserDetails details;
    details._compositeID._fields._user      = response_.hdr_.uid_.composite_id_.client_id;
    details._compositeID._fields._portfolio = response_.hdr_.uid_.composite_id_.strategy_id;
    response._userDetails                   = details;

    OrderResponseInfoT info = {};
    info._response          = GetOrderResponsee(response_.hdr_.transaction_code);
    info._timestamp         = response_.hdr_.exchange_timestamp;
    info._orderId           = response_.packet_.exchange_order_id;
    info._uniqueId          = response_.hdr_.uid_.id_;
    info._fillNumber        = response_.packet_.exchange_fill_id;
    info._placed._price     = response_.packet_.price_;
    info._placed._quantity  = response_.packet_.quantity_;
    info._token             = response_.packet_.product_id_;
    info._errorCode         = response_.packet_.exchange_response_code;
    if (info._fillNumber != 0) {
        info._traded = info._placed;
    }

    response._response = info;
    if (info._response == OrderResponse_NONE) {
        return;
    }
    std::memcpy(ui.message, &response, sizeof(response));
    sentoUI(ui);
}
void MinixStrategy::sendTradeTracerToUI(const TradeTracer& tracer_, int32_t interface_) {
    std::cout << __FUNCTION__ << std::endl;
    aef::infra::ui_cmd::UIStruct ui{};
    ui.header.message_code   = 9956;
    ui.header.interface_id   = interface_;
    ui.header.message_length = 1520;
    ui.header.component_id   = 1;
    ui.header.timestamp      = 0;
    std::memset(ui.message, 0, sizeof(ui.message));
    std::memcpy(ui.message, &tracer_, sizeof(tracer_));
    sentoUI(ui);
}
/**
 * @brief Cancel an order if eligible and notify portfolio manager.
 */
bool MinixStrategy::cancel_order(OrderMap& order_handle_, int32_t token_) {
    auto itr = order_handle_.find(token_);
    if (itr != order_handle_.end()) {
        // std::cout<<" Cancel"<<std::endl;
        auto& order1 = itr->second;
        if (!order1.is_response_pending()) {
            // Quote quoteC;
            // getLastQuote(token, quoteC);
            order1.set_time_stamps(event_timestamp_, trigger_timestamp_,
                                   aef::infra::get_realtime_in_nanos());
            LOG_DEBUG("[ORDER] FIRE CANCEL token=%d side=%-4s", token_,
                      omsSideName(static_cast<int>(order1.get_side())));
            if (order1.cancel_order()) {
                portfolio_mgr_.on_order_cancel(order1.get_uid());
            }
            // std::cout<<" UID "<<order1.cancel_order()<<std::endl;
        }
    }
    //  std::cout<<" UID Cancel done"<<std::endl;
    return true;
}

extern "C" AlgoBase* create(void* context_) {
    std::cout << __FILE__ << ":" << __FUNCTION__ << std::endl;
    return new MinixStrategy(static_cast<AlgoBase::ContextHandle>(context_));
}

extern "C" void destroy(AlgoBase* strat_) { delete strat_; }
