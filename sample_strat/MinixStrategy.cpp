/**
 * @file MinixStrategy.cpp
 * @brief Implementation of the sample Minix strategy.
 */
#include "MinixStrategy.hpp"

#include "Butterfly/ButterflyStrategy.hpp"
#include "Ratio2Leg/Ratio2LegStrategy.hpp"
#include "Ratio3Leg/Ratio3LegStrategy.hpp"
#include "Ratio4Leg/Ratio4LegStrategy.hpp"
#include "Ratio5Leg/Ratio5LegStrategy.hpp"
#include "Ratio6Leg/Ratio6LegStrategy.hpp"
#include "Utils.hpp"
#include "oms_api.hpp"

#include <nlohmann/json.hpp>
#include <unistd.h>

#include <boost/program_options/options_description.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

using json = nlohmann::json;

int date = 0;

using namespace std;
int32_t omsid;
namespace po = boost::program_options;
namespace pt = boost::property_tree;
using namespace std::string_literals;

using json = nlohmann::json;
std::unordered_map<std::string, double> parameterStorage_global;
std::unordered_map<std::string, double> parameterStorage_token;

std::unordered_map<uint32_t, int32_t>
    interface_map;  // Interface map to store interface ids

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
static int64_t ljInt(const json& o, const char* k, int64_t def = 0) {
    if (!o.contains(k) || o[k].is_null())
        return def;
    const auto& v = o[k];
    if (v.is_number_integer())
        return v.get<int64_t>();
    if (v.is_number_unsigned())
        return static_cast<int64_t>(v.get<uint64_t>());
    if (v.is_number_float())
        return static_cast<int64_t>(v.get<double>());
    if (v.is_boolean())
        return v.get<bool>() ? 1 : 0;
    if (v.is_string()) {
        try {
            return std::stoll(v.get<std::string>());
        } catch (...) {
            return def;
        }
    }
    return def;
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
void MinixStrategy::applyLegStrategyJson(const std::string& jsonText) {
    using aef::infra::ui_cmd::BuySell;
    using aef::infra::ui_cmd::StrategyDatafromui;
    using aef::infra::ui_cmd::TokenDatafromui;

    auto sendStatus = [this](std::string status, int strategyId) {
        json response;
        response["Status"]     = status;
        response["StrategyId"] = strategyId;
        std::cout << "SendStatus " << response.dump() << std::endl;
        sendJsonChunkedToUI(9621, response.dump());
    };

    std::cout << "applyLegStrategyJson" << std::endl;
    std::cout << "jsonText: " << jsonText << std::endl;
    try {
        json  root     = json::parse(jsonText);
        json& strategy = root["Strategy"];

        std::string name       = strategy.value("SubType", "");
        std::string status     = strategy.value("Status", "");
        int         strategyId = ljInt(strategy, "StrategyId", 0);
        std::cout << name << " " << status << " " << strategyId << std::endl;

        if (name == "Ratio2" || name == "ratio2" || name == "RATIO2" ||
            name == "Ratio 2 Leg" || name == "ratio 2 leg" ||
            name == "Ratio2Leg" || name == "2LegRatio") {
            sendStatus(status, strategyId);
            handleRatio2LegStrategy(root, jsonText);
        } else if (name == "Ratio3" || name == "ratio3" || name == "RATIO3" ||
                   name == "Ratio 3 Leg" || name == "ratio 3 leg" ||
                   name == "Ratio3Leg" || name == "3LegRatio") {
            sendStatus(status, strategyId);
            handleRatio3LegStrategy(root, jsonText);
        } else if (name == "Ratio4" || name == "ratio4" || name == "RATIO4" ||
                   name == "Ratio 4 Leg" || name == "ratio 4 leg" ||
                   name == "Ratio4Leg" || name == "4LegRatio") {
            sendStatus(status, strategyId);
            handleRatio4LegStrategy(root, jsonText);
        } else if (name == "Ratio5" || name == "ratio5" || name == "RATIO5" ||
                   name == "Ratio 5 Leg" || name == "ratio 5 leg" ||
                   name == "Ratio5Leg" || name == "5LegRatio") {
            sendStatus(status, strategyId);
            handleRatio5LegStrategy(root, jsonText);
        } else if (name == "Ratio6" || name == "ratio6" || name == "RATIO6" ||
                   name == "Ratio 6 Leg" || name == "ratio 6 leg" ||
                   name == "Ratio6Leg" || name == "6LegRatio") {
            sendStatus(status, strategyId);
            handleRatio6LegStrategy(root, jsonText);
        } else if (name == "Butterfly" || name == "butterfly" || name == "BUTTERFLY" ||
                   name == "ButterflyStrategy" || name == "Fly") {
            sendStatus(status, strategyId);
            handleButterflyStrategy(root, jsonText);
        }

    } catch (const std::exception& e) {
        std::cout << "applyLegStrategyJson failed: " << e.what() << std::endl;
    }
}

void MinixStrategy::handleRatio2LegStrategy(const nlohmann::json& root,
                                            const std::string&    jsonText) {
    try {
        auto strategy             = root["Strategy"];
        auto status               = strategy["Status"].get<std::string>();
        int  strategyID           = strategy["StrategyId"].get<int>();
        strategyJson_[strategyID] = jsonText;
        if (status == "Subscribed") {
            auto iterator = ratio2Strats_.find(strategyID);
            if (iterator == ratio2Strats_.end()) {
                ratio2Strats_[strategyID] = new Ratio2LegStrategy(this, strategyID, root);
            }
        } else if (status == "Applied") {
            auto iterator = ratio2Strats_.find(strategyID);
            if (iterator != ratio2Strats_.end()) {
                iterator->second->ParamUpdate(root);
            }
        } else if (status == "Unsubscribed" || status == "Cancelled") {
            auto iterator = ratio2Strats_.find(strategyID);
            if (iterator != ratio2Strats_.end()) {
                delete iterator->second;
                ratio2Strats_.erase(iterator);
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[handleRatio2LegStrategy] failed: " << e.what() << std::endl;
    }
}

void MinixStrategy::handleRatio3LegStrategy(const nlohmann::json& root, const std::string& jsonText) {
    try {
        auto strategy             = root["Strategy"];
        auto status               = strategy["Status"].get<std::string>();
        int  strategyID           = strategy["StrategyId"].get<int>();
        strategyJson_[strategyID] = jsonText;
        if (status == "Subscribed") {
            auto iterator = ratio3Strats_.find(strategyID);
            if (iterator == ratio3Strats_.end()) {
                ratio3Strats_[strategyID] = new Ratio3LegStrategy(this, strategyID, root);
            }
        } else if (status == "Applied") {
            auto iterator = ratio3Strats_.find(strategyID);
            if (iterator != ratio3Strats_.end()) {
                iterator->second->ParamUpdate(root);
            }
        } else if (status == "Unsubscribed" || status == "Cancelled") {
            auto iterator = ratio3Strats_.find(strategyID);
            if (iterator != ratio3Strats_.end()) {
                delete iterator->second;
                ratio3Strats_.erase(iterator);
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[handleRatio3LegStrategy] failed: " << e.what() << std::endl;
    }
}

void MinixStrategy::handleRatio4LegStrategy(const nlohmann::json& root, const std::string& jsonText) {
    try {
        auto strategy             = root["Strategy"];
        auto status               = strategy["Status"].get<std::string>();
        int  strategyID           = strategy["StrategyId"].get<int>();
        strategyJson_[strategyID] = jsonText;
        if (status == "Subscribed") {
            auto iterator = ratio4Strats_.find(strategyID);
            if (iterator == ratio4Strats_.end()) {
                ratio4Strats_[strategyID] = new Ratio4LegStrategy(this, strategyID, root);
            }
        } else if (status == "Applied") {
            auto iterator = ratio4Strats_.find(strategyID);
            if (iterator != ratio4Strats_.end()) {
                iterator->second->ParamUpdate(root);
            }
        } else if (status == "Unsubscribed" || status == "Cancelled") {
            auto iterator = ratio4Strats_.find(strategyID);
            if (iterator != ratio4Strats_.end()) {
                delete iterator->second;
                ratio4Strats_.erase(iterator);
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[handleRatio4LegStrategy] failed: " << e.what() << std::endl;
    }
}

void MinixStrategy::handleRatio5LegStrategy(const nlohmann::json& root, const std::string& jsonText) {
    try {
        auto strategy             = root["Strategy"];
        auto status               = strategy["Status"].get<std::string>();
        int  strategyID           = strategy["StrategyId"].get<int>();
        strategyJson_[strategyID] = jsonText;
        if (status == "Subscribed") {
            auto iterator = ratio5Strats_.find(strategyID);
            if (iterator == ratio5Strats_.end()) {
                ratio5Strats_[strategyID] = new Ratio5LegStrategy(this, strategyID, root);
            }
        } else if (status == "Applied") {
            auto iterator = ratio5Strats_.find(strategyID);
            if (iterator != ratio5Strats_.end()) {
                iterator->second->ParamUpdate(root);
            }
        } else if (status == "Unsubscribed" || status == "Cancelled") {
            auto iterator = ratio5Strats_.find(strategyID);
            if (iterator != ratio5Strats_.end()) {
                delete iterator->second;
                ratio5Strats_.erase(iterator);
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[handleRatio5LegStrategy] failed: " << e.what() << std::endl;
    }
}

void MinixStrategy::handleRatio6LegStrategy(const nlohmann::json& root, const std::string& jsonText) {
    try {
        auto strategy             = root["Strategy"];
        auto status               = strategy["Status"].get<std::string>();
        int  strategyID           = strategy["StrategyId"].get<int>();
        strategyJson_[strategyID] = jsonText;
        if (status == "Subscribed") {
            auto iterator = ratio6Strats_.find(strategyID);
            if (iterator == ratio6Strats_.end()) {
                ratio6Strats_[strategyID] = new Ratio6LegStrategy(this, strategyID, root);
            }
        } else if (status == "Applied") {
            auto iterator = ratio6Strats_.find(strategyID);
            if (iterator != ratio6Strats_.end()) {
                iterator->second->ParamUpdate(root);
            }
        } else if (status == "Unsubscribed" || status == "Cancelled") {
            auto iterator = ratio6Strats_.find(strategyID);
            if (iterator != ratio6Strats_.end()) {
                delete iterator->second;
                ratio6Strats_.erase(iterator);
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[handleRatio6LegStrategy] failed: " << e.what() << std::endl;
    }
}

void MinixStrategy::handleButterflyStrategy(const nlohmann::json& root, const std::string& jsonText) {
    try {
        auto strategy             = root["Strategy"];
        auto status               = strategy["Status"].get<std::string>();
        int  strategyID           = strategy["StrategyId"].get<int>();
        strategyJson_[strategyID] = jsonText;
        if (status == "Subscribed") {
            auto iterator = butterflyStrats_.find(strategyID);
            if (iterator == butterflyStrats_.end()) {
                butterflyStrats_[strategyID] = new ButterflyStrategy(this, strategyID, root);
            }
        } else if (status == "Applied") {
            auto iterator = butterflyStrats_.find(strategyID);
            if (iterator != butterflyStrats_.end()) {
                iterator->second->ParamUpdate(root);
            }
        } else if (status == "Unsubscribed" || status == "Cancelled") {
            auto iterator = butterflyStrats_.find(strategyID);
            if (iterator != butterflyStrats_.end()) {
                delete iterator->second;
                butterflyStrats_.erase(iterator);
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[handleButterflyStrategy] failed: " << e.what() << std::endl;
    }
}
/**
 * @brief Initialize strategy configuration, subscriptions, and order handles.
 */
MinixStrategy::MinixStrategy(AlgoBase::ContextHandle context)
    : AlgoBase(context) {
    pt::ptree root;
    auto      config_file = get_strategy_config_file();
    std::cout << "filename " << config_file << std::endl;
    std::cerr << ">>> [CTOR-ENTRY] MinixStrategy reading config='" << config_file
              << "'" << std::endl;
    auto is = openStream(config_file.c_str());
    pt::read_json(is, root);

    client = root.get<int32_t>("client");
    algoid = root.get<int32_t>("algoid");
    omsid  = root.get<int32_t>("omsid");

    std::cout << std::fixed << std::setprecision(2);

    // Market-data event flags requested per token: TER + MBP depth + OI + TBT.
    flags = static_cast<uint16_t>(
                aef::infra::product::SNAPSHOT_FLAGS::TER_UPDATE_EVENT) |
            static_cast<uint16_t>(
                aef::infra::product::SNAPSHOT_FLAGS::MBP_UPDATE_EVENT) |
            static_cast<uint16_t>(
                aef::infra::product::SNAPSHOT_FLAGS::OI_UPDATE_EVENT) |
            static_cast<uint16_t>(
                aef::infra::product::SNAPSHOT_FLAGS::TBT_UPDATE_EVENT);

    std::cout << "Algo for Box Strategy with client ID : " << client << std::endl;
    clientUID.composite_id_.client_id   = client;
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
            subscribeProduct(std::stoi(line_subtok), flags);
        } catch (const std::exception&) { /* skip non-numeric lines */
        }
    }

    // SIM/test hook: optionally inject box strategies from a JSON file so the
    // simulator can exercise applyLegStrategyJson without the UI/connector
    // pipeline. Set "LegStrategy_json" in order_info.json (a single doc or a JSON
    // array of docs).
    std::string legJsonFile = root.get<std::string>("LegStrategy_json", "");
    if (!legJsonFile.empty()) {
        std::ifstream lf(legJsonFile);
        if (lf) {
            std::stringstream ss;
            ss << lf.rdbuf();
            std::string content = ss.str();
            try {
                json parsed = json::parse(content);
                if (parsed.is_array()) {
                    std::cout << "[SIM] injecting " << parsed.size()
                              << " leg strategies from " << legJsonFile << std::endl;
                    for (auto& el : parsed)
                        applyLegStrategyJson(el.dump());
                } else {
                    std::cout << "[SIM] injecting leg strategy from " << legJsonFile
                              << std::endl;
                    applyLegStrategyJson(content);
                }
            } catch (const std::exception& e) {
                std::cout << "[SIM] LegStrategy_json parse error: " << e.what()
                          << std::endl;
            }
        } else
            std::cout << "[SIM] LegStrategy_json file not found: " << legJsonFile
                      << std::endl;
    }
}

MinixStrategy::~MinixStrategy() {
    log_info("sample_strat : destructor");
    for (auto& kv : ratio2Strats_) delete kv.second;
    for (auto& kv : ratio3Strats_) delete kv.second;
    for (auto& kv : ratio4Strats_) delete kv.second;
    for (auto& kv : ratio5Strats_) delete kv.second;
    for (auto& kv : ratio6Strats_) delete kv.second;
    for (auto& kv : butterflyStrats_) delete kv.second;
    ratio2Strats_.clear();
    ratio3Strats_.clear();
    ratio4Strats_.clear();
    ratio5Strats_.clear();
    ratio6Strats_.clear();
    butterflyStrats_.clear();
}

bool MinixStrategy::subscribeProduct(const int32_t  product_id,
                                     const uint16_t flags) {
    LOG_DEBUG("[SUB] subscribeProduct product_id=%d flags=%d", product_id, flags);
    return AlgoBase::subscribeProduct(product_id, flags);
}

bool MinixStrategy::unSubscribeProduct(const int32_t  product_id,
                                       const uint16_t flags) {
    LOG_DEBUG("[SUB] unSubscribeProduct product_id=%d flags=%d", product_id,
              flags);
    return AlgoBase::unSubscribeProduct(product_id, flags);
}

// function to squareoff using traderId
std::string format_time(std::time_t t) {
    std::tm*           timeInfo = std::localtime(&t);
    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << timeInfo->tm_hour << ":"
        << std::setw(2) << std::setfill('0') << timeInfo->tm_min << ":"
        << std::setw(2) << std::setfill('0') << timeInfo->tm_sec;
    return oss.str();
}

void MinixStrategy::OnTick(const Quote& event) {
    LOG_DEBUG("[TICK] OnTick product_id=%d seq=%d ts=%lu ltp=%d",
              event.header.product_id, event.header.sequence_no,
              event.header.exchange_timestamp, event.message.ltp_);

    auto sodFrom = [](uint64_t ns) -> int64_t {
        uint32_t    s  = static_cast<uint32_t>(ns / 1000000000ULL);
        std::string tt = format_time(
            static_cast<std::time_t>(aef::infra::GetUTCTimeFromNSETime(s)));
        if (tt.size() >= 8 && tt >= "09:14:00" && tt <= "15:31:00")
            return (static_cast<int64_t>((tt[0] - '0') * 36000 +
                                         (tt[1] - '0') * 3600 + (tt[3] - '0') * 600 +
                                         (tt[4] - '0') * 60 + (tt[6] - '0') * 10 +
                                         (tt[7] - '0'))) *
                   1000000000LL;
        return -1;
    };
    int64_t clk = sodFrom(event.header.exchange_timestamp);
    if (clk < 0)
        clk = sodFrom(event.header.event_timestamp);
    if (clk >= 0)
        lastTickTs_ = clk;

    for (auto& kv : ratio2Strats_) kv.second->OnTick(event, lastTickTs_);
    for (auto& kv : ratio3Strats_) kv.second->OnTick(event, lastTickTs_);
    for (auto& kv : ratio4Strats_) kv.second->OnTick(event, lastTickTs_);
    for (auto& kv : ratio5Strats_) kv.second->OnTick(event, lastTickTs_);
    for (auto& kv : ratio6Strats_) kv.second->OnTick(event, lastTickTs_);
    for (auto& kv : butterflyStrats_) kv.second->OnTick(event, lastTickTs_);
}

// --- clean order-lifecycle logging helpers --------------------------------
static const char* omsEventName(int code) {
    switch (code) {
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
static const char* omsSideName(int s) {
    return s == 1 ? "BUY" : (s == 2 ? "SELL" : "?");
}

void MinixStrategy::OnOrderResponse(const oms_transaction& order_resp) {
    for (auto& kv : ratio2Strats_) kv.second->OnOrderResponse(order_resp);
    for (auto& kv : ratio3Strats_) kv.second->OnOrderResponse(order_resp);
    for (auto& kv : ratio4Strats_) kv.second->OnOrderResponse(order_resp);
    for (auto& kv : ratio5Strats_) kv.second->OnOrderResponse(order_resp);
    for (auto& kv : ratio6Strats_) kv.second->OnOrderResponse(order_resp);
    for (auto& kv : butterflyStrats_) kv.second->OnOrderResponse(order_resp);

    LOG_DEBUG(
        "[ORDER] RECV %-18s token=%d side=%-4s qty=%d price=%d uid=%d "
        "err=%d reason=%d",
        omsEventName(order_resp.hdr_.transaction_code),
        order_resp.packet_.product_id_,
        omsSideName(static_cast<int>(order_resp.packet_.flags_.order_side)),
        order_resp.packet_.quantity_, order_resp.packet_.price_,
        order_resp.hdr_.uid_.composite_id_.request_id,
        order_resp.hdr_.error_code, order_resp.hdr_.reason_code);

    portfolio_mgr_.on_order_response(order_resp);
}

int  count       = 0;
bool orderPlaced = 0;
int  MinixStrategy::doWork() {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    if (now - lastSpreadSendMs_ >= 5000) {
        lastSpreadSendMs_ = now;
        sendStrategySpreadsToUI();
    }
    return 0;
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
        j["Net P/L"]    = static_cast<float>(ratio->GetNetPL()) / 100.0F;
        j["RLP"]        = static_cast<float>(ratio->GetRLP()) / 100.0F;
        j["Cut P/L"]    = static_cast<float>(ratio->GetCutPL()) / 100.0F;
        j["TrSpread"]   = static_cast<float>(ratio->GetRLP()) / 100.0F;
        j["B-ATP"]      = static_cast<float>(ratio->GetBATP()) / 100.0F;
        j["S-ATP"]      = static_cast<float>(ratio->GetSATP()) / 100.0F;
        j["B-Buy"]      = static_cast<float>(ratio->GetBATP()) / 100.0F;
        j["B-Sell"]     = static_cast<float>(ratio->GetSATP()) / 100.0F;
        sendJsonChunkedToUI(9612, j.dump());
    };

    for (auto& kv : ratio2Strats_) sendRatioUI(kv.second);
    for (auto& kv : ratio3Strats_) sendRatioUI(kv.second);
    for (auto& kv : ratio4Strats_) sendRatioUI(kv.second);
    for (auto& kv : ratio5Strats_) sendRatioUI(kv.second);
    for (auto& kv : ratio6Strats_) sendRatioUI(kv.second);
    for (auto& kv : butterflyStrats_) sendRatioUI(kv.second);
}

void MinixStrategy::sendJsonChunkedToUI(int32_t            message_code,
                                        const std::string& payload) {
    constexpr size_t max_chunk_size = 1500;
    int32_t          current_ts     = static_cast<int32_t>(
        std::chrono::system_clock::now().time_since_epoch().count() / 1000000);
    int packet_count =
        static_cast<int>((payload.size() + max_chunk_size - 1) / max_chunk_size);
    if (packet_count < 1)
        packet_count = 1;

    {
        aef::infra::ui_cmd::UIStruct ui{};
        ui.header.message_code   = message_code;
        ui.header.component_id   = 1;
        ui.header.timestamp      = current_ts;
        ui.header.interface_id   = 22;
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
            payload.substr(static_cast<size_t>(i) * max_chunk_size, max_chunk_size);
        aef::infra::ui_cmd::UIStruct ui{};
        ui.header.message_code   = message_code;
        ui.header.interface_id   = 22;
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
            lastTickTs_ =
                (static_cast<int64_t>((tt[0] - '0') * 36000 + (tt[1] - '0') * 3600 +
                                      (tt[3] - '0') * 600 + (tt[4] - '0') * 60 +
                                      (tt[6] - '0') * 10 + (tt[7] - '0'))) *
                1000000000LL;
    }
    for (auto& kv : ratio2Strats_) kv.second->OnBcast(product_details_, lastTickTs_);
    for (auto& kv : ratio3Strats_) kv.second->OnBcast(product_details_, lastTickTs_);
    for (auto& kv : ratio4Strats_) kv.second->OnBcast(product_details_, lastTickTs_);
    for (auto& kv : ratio5Strats_) kv.second->OnBcast(product_details_, lastTickTs_);
    for (auto& kv : ratio6Strats_) kv.second->OnBcast(product_details_, lastTickTs_);
    for (auto& kv : butterflyStrats_) kv.second->OnBcast(product_details_, lastTickTs_);
}

/**
 * @brief Evaluate parsed CSV rows and fire configured orders.
 */
void MinixStrategy::onUIRequest(const aef::infra::ui_cmd::UIStruct& ui_req) {
    std::cout << ">>> [GUI-RX] onUIRequest code=" << ui_req.header.message_code
              << " msg_len=" << ui_req.header.message_length
              << " iface=" << ui_req.header.interface_id
              << " comp=" << ui_req.header.component_id << std::endl;

    // The only UI channel the box uses: chunked strategy-config JSON. The GUI
    // connector (sendStrategyConfig) sends a METADATA packet
    // {"packet_count":N,"timestamp":..} zero-padded to 1500B, then N raw 1500B
    // UTF-8 slices of the JSON, all on the same message_code (9612 config echo
    // channel; 9620/9621 legacy aliases). Reassemble per code, then hand the full
    // JSON to applyLegStrategyJson.
    if (ui_req.header.message_code == 9612 ||
        ui_req.header.message_code == 9620 ||
        ui_req.header.message_code == 9621) {
        const int kMsgBytes = static_cast<int>(sizeof(ui_req.message));  // 1500
        int       len       = kMsgBytes;
        while (len > 0 && ui_req.message[len - 1] == '\0')
            --len;  // JSON text never contains NUL
        std::string chunk(ui_req.message, len);

        JsonReassembly& ra = jsonReassembly_[ui_req.header.message_code];

        // Metadata header? It is the only fully-parseable packet that carries
        // "packet_count".
        json meta = json::parse(chunk, nullptr, false);
        if (meta.is_object() && meta.contains("packet_count")) {
            ra.expected = static_cast<int>(meta.value("packet_count", 0));
            ra.received = 0;
            ra.buf.clear();
            ra.active = ra.expected > 0;
            std::cout << ">>> [GUI-RX] META code=" << ui_req.header.message_code
                      << " packet_count=" << ra.expected << std::endl;
            return;
        }

        if (!ra.active) {
            std::cout << ">>> [GUI-RX] WARNING data chunk before metadata (code="
                      << ui_req.header.message_code << ", " << len << "B) -- ignored"
                      << std::endl;
            return;
        }

        ra.buf += chunk;
        ra.received++;
        std::cout << ">>> [GUI-RX] CHUNK " << ra.received << "/" << ra.expected
                  << " code=" << ui_req.header.message_code << " bytes=" << len
                  << " (accumulated=" << ra.buf.size() << "B)" << std::endl;

        if (ra.received >= ra.expected) {
            std::string full = std::move(ra.buf);
            ra               = JsonReassembly{};  // reset for the next transfer
            while (!full.empty() && full.back() == '\0')
                full.pop_back();  // strip any padding
            std::cout << ">>> [GUI-RX] REASSEMBLED code="
                      << ui_req.header.message_code << " total=" << full.size()
                      << "B  JSON below:\n"
                      << full << "\n>>> [GUI-RX] END-JSON" << std::endl;
            applyLegStrategyJson(full);
        }
        return;
    }
}

/**
 * @brief Push a heartbeat update to the UI layer.
 */
int MinixStrategy::update_order(OrderMap& order_handle_, int32_t token, int32_t cur_price, int32_t qty) {
    // Quote quoteC;
    // getLastQuote(token, quoteC);
    int  uid = 0;
    auto itr = order_handle_.find(token);
    // std::cout << "Token from Update order : " << token << "cliuid: " <<
    // clientUID.id_ << std::endl;
    if (itr != order_handle_.end()) {
        auto& order = itr->second;
        qty -= order.get_filled_qty();
        // std::cout << "QTY from UPdate order : " << qty << std::endl;
        if (order.get_current_state() !=
            static_cast<uint32_t>(
                execution_strat::STRAT_ORDER_STATE::STRAT_INITIAL_STATE)) {
            if (!order.is_response_pending()) {
                if (qty > 0 && order.get_open_price() != cur_price) {
                    order.set_time_stamps(event_timestamp_, trigger_timestamp_,
                                          aef::infra::get_realtime_in_nanos());
                    uid = order.update_order(cur_price, qty);
                    LOG_DEBUG(
                        "[ORDER] FIRE MODIFY token=%d side=%-4s price=%d qty=%d uid=%d",
                        token, omsSideName(static_cast<int>(order.get_side())), cur_price,
                        qty, uid);
                    if (uid) {
                        portfolio_mgr_.on_order_modify(order.get_uid(), cur_price, qty);
                    }
                }
            }
        } else {
            if (!order.is_response_pending()) {
                if (qty > 0 && order.get_open_price() != cur_price) {
                    clientUID.composite_id_.request_id =
                        ++requestId;  // print_depth(token);
                    if (clientUID.composite_id_.request_id >= MAX_REQUEST_ID) {
                        LOG_ERROR(" Stop Trading AS max allowed Orders Breach");
                    } else {
                        order.set_time_stamps(event_timestamp_, trigger_timestamp_,
                                              aef::infra::get_realtime_in_nanos());
                        uid = order.place_order(cur_price, qty, clientUID.id_);
                        LOG_DEBUG(
                            "[ORDER] FIRE NEW    token=%d side=%-4s price=%d qty=%d uid=%d",
                            token, omsSideName(static_cast<int>(order.get_side())),
                            cur_price, qty, uid);
                        if (uid) {
                            portfolio_mgr_.on_order_placed(uid, token, order.get_side(),
                                                           cur_price, qty);
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

void MinixStrategy::sendOrderResponse(const oms_transaction& response_, std::string name_) {
    constexpr static double      TenYearsInSeconds = 315513000 * 10e9;
    aef::infra::ui_cmd::UIStruct ui{};
    ui.header.message_code   = 9955;
    ui.header.interface_id   = 22;
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
/**
 * @brief Cancel an order if eligible and notify portfolio manager.
 */
bool MinixStrategy::cancel_order(OrderMap& order_handle_, int32_t token) {
    auto itr = order_handle_.find(token);
    if (itr != order_handle_.end()) {
        // std::cout<<" Cancel"<<std::endl;
        auto& order1 = itr->second;
        if (!order1.is_response_pending()) {
            // Quote quoteC;
            // getLastQuote(token, quoteC);
            order1.set_time_stamps(event_timestamp_, trigger_timestamp_,
                                   aef::infra::get_realtime_in_nanos());
            LOG_DEBUG("[ORDER] FIRE CANCEL token=%d side=%-4s", token,
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

extern "C" AlgoBase* create(void* context) {
    std::cout << __FILE__ << ":" << __FUNCTION__ << std::endl;
    return new MinixStrategy(static_cast<AlgoBase::ContextHandle>(context));
}

extern "C" void destroy(AlgoBase* strat) { delete strat; }
