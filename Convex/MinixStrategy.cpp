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

#include <cstdint>
#include <fstream>

/**
 * @brief Convert nanosecond timestamp to human readable UTC string.
 */
auto OpenStream(const char* filename) -> std::ifstream {
    std::ifstream in{filename};
    if (!in) {
        throw std::runtime_error{"Could not open file: "s + filename};
    }
    return in;
}

void MinixStrategy::ApplyLegStrategyJson(int32_t interface_, const std::string& jsonText_) {
    using aef::infra::ui_cmd::BuySell;
    using aef::infra::ui_cmd::StrategyDatafromui;
    using aef::infra::ui_cmd::TokenDatafromui;

    // ponytail: send binary status update directly without JSON
    auto sendStatus = [this, interface_](std::string_view status_, int strategyId_) {
        const auto statusEnum = StringToStrategyStatus(status_);
        SendStrategyStatusToUi(static_cast<uint32_t>(strategyId_), statusEnum, interface_);
    };

    try {
        nlohmann::json  root     = nlohmann::json::parse(jsonText_);
        nlohmann::json& strategy = root["Strategy"];

        std::string name       = strategy.value("SubType", "");
        std::string status     = strategy.value("Status", "");
        int         strategyId = strategy.value("StrategyId", 0);

        std::cout << "[applyLegStrategyJson] SubType='" << name << "' Status='" << status << "' StrategyId=" << strategyId << std::endl;

        if (name == "2LegRatio") {
            sendStatus(status, strategyId);
            HandleRatioLegStrategy(root, jsonText_, 2, interface_, false);
        } else if (name == "3LegRatio") {
            sendStatus(status, strategyId);
            HandleRatioLegStrategy(root, jsonText_, 3, interface_, false);
        } else if (name == "4LegRatio") {
            sendStatus(status, strategyId);
            HandleRatioLegStrategy(root, jsonText_, 4, interface_, false);
        } else if (name == "5LegRatio") {
            sendStatus(status, strategyId);
            HandleRatioLegStrategy(root, jsonText_, 5, interface_, false);
        } else if (name == "6LegRatio") {
            sendStatus(status, strategyId);
            HandleRatioLegStrategy(root, jsonText_, 6, interface_, false);
        } else if (name == "Butterfly") {
            sendStatus(status, strategyId);
            HandleRatioLegStrategy(root, jsonText_, 3, interface_, false);
        } else if (name == "Box") {
            sendStatus(status, strategyId);
            HandleRatioLegStrategy(root, jsonText_, 4, interface_, true);
        } else if (name == "ConRev") {
            sendStatus(status, strategyId);
            HandleRatioLegStrategy(root, jsonText_, 3, interface_, true);
        }

    } catch (const std::exception& e) {
        std::cout << "applyLegStrategyJson failed: " << e.what() << std::endl;
    }
}

void MinixStrategy::HandleRatioLegStrategy(const nlohmann::json& root_,
                                           const std::string&    jsonText_,
                                           size_t numLegs_, int32_t interface_, bool gapDiff_) {
    try {
        auto     strategy         = root_["Strategy"];
        auto     status           = strategy["Status"].get<std::string>();
        uint32_t strategyID       = strategy["StrategyId"].get<uint32_t>();
        _strategyJson[strategyID] = jsonText_;

        const StrategyStatus targetStatus = StringToStrategyStatus(status);
        std::string          upperStatus;
        upperStatus.reserve(status.size());
        for (char c : status) {
            upperStatus.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }

        if (upperStatus == "SUBSCRIBED" || upperStatus == "SUBSCRIBE" || upperStatus == "NEW" || upperStatus == "ACTIVE") {
            auto iterator = _ratioStrats.find(strategyID);
            if (iterator == _ratioStrats.end()) {
                std::cout << "[handleRatioLegStrategy] Creating new RatioLegStrategy for strat=" << strategyID
                          << " with status=" << StrategyStatusToString(targetStatus) << std::endl;
                auto* strat = new RatioLegStrategy(this, strategyID, interface_, root_, numLegs_, gapDiff_);
                strat->SetStatus(targetStatus);
                _ratioStrats[strategyID] = strat;
            } else {
                std::cout << "[handleRatioLegStrategy] Updating existing RatioLegStrategy for strat=" << strategyID
                          << " with status=" << StrategyStatusToString(targetStatus) << std::endl;
                iterator->second->ParamUpdate(root_);
                iterator->second->SetStatus(targetStatus);
            }
        } else if (upperStatus == "APPLIED" || upperStatus == "APPLY") {
            auto iterator = _ratioStrats.find(strategyID);
            if (iterator != _ratioStrats.end()) {
                iterator->second->ParamUpdate(root_);
                iterator->second->SetStatus(targetStatus);
            } else {
                auto* strat = new RatioLegStrategy(this, strategyID, interface_, root_, numLegs_, gapDiff_);
                strat->SetStatus(targetStatus);
                _ratioStrats[strategyID] = strat;
                strat->ParamUpdate(root_);
            }
        } else if (upperStatus == "UNSUBSCRIBED" || upperStatus == "UNSUBSCRIBE" || upperStatus == "STOP") {
            auto iterator = _ratioStrats.find(strategyID);
            if (iterator != _ratioStrats.end()) {
                std::cout << "[handleRatioLegStrategy] Stopping strat=" << strategyID << std::endl;
                iterator->second->Stop();
            }
        } else if (upperStatus == "DELETED" || upperStatus == "DELETE") {
            auto iterator = _ratioStrats.find(strategyID);
            if (iterator != _ratioStrats.end()) {
                std::cout << "[handleRatioLegStrategy] Deleting strat=" << strategyID << std::endl;
                iterator->second->Stop();
                delete iterator->second;
                _ratioStrats.erase(iterator);
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
    auto configFile = get_strategy_config_file();
    std::cout << "filename " << configFile << std::endl;
    std::cerr << ">>> [CTOR-ENTRY] MinixStrategy reading config='" << configFile << "'" << std::endl;
    auto is = OpenStream(configFile.c_str());

    nlohmann::json root = nlohmann::json::parse(is);
    is.close();

    root["client"].get_to(_client);
    root["algoid"].get_to(_algoid);
    root["omsid"].get_to(_omsid);

    // Market-data event flags requested per token: TER + MBP depth + OI + TBT.
    _flags = static_cast<uint16_t>(aef::infra::product::SNAPSHOT_FLAGS::TER_UPDATE_EVENT) |
             static_cast<uint16_t>(aef::infra::product::SNAPSHOT_FLAGS::MBP_UPDATE_EVENT) |
             static_cast<uint16_t>(aef::infra::product::SNAPSHOT_FLAGS::OI_UPDATE_EVENT) |
             static_cast<uint16_t>(aef::infra::product::SNAPSHOT_FLAGS::TBT_UPDATE_EVENT);

    std::cout << "Algo for Box Strategy with client ID : " << _client << std::endl;
}

MinixStrategy::~MinixStrategy() {
    log_info("Convex : destructor");
    for (auto& [strategy, ratio] : _ratioStrats) {
        delete ratio;
    }
    _ratioStrats.clear();
}

auto MinixStrategy::subscribeProduct(const int32_t product_id_, const uint16_t flags_) -> bool {
    LOG_DEBUG("[SUB] subscribeProduct product_id=%d flags=%d", product_id_, flags_);
    return AlgoBase::subscribeProduct(product_id_, flags_);
}

auto MinixStrategy::unSubscribeProduct(const int32_t product_id_, const uint16_t flags_) -> bool {
    LOG_DEBUG("[SUB] unSubscribeProduct product_id=%d flags=%d", product_id_, flags_);
    return AlgoBase::unSubscribeProduct(product_id_, flags_);
}

void MinixStrategy::OnTick(const Quote& event_) {
    int64_t lastTickTs = 0;
    for (auto& [strategyId, ratio] : _ratioStrats) {
        ratio->OnTick(event_, lastTickTs);
        if (ratio->IsStopped()) {
            ratio->Stop();
        }
    }
}

void MinixStrategy::OnOrderResponse(const oms_transaction& order_resp_) {
    auto strategyId = order_resp_.hdr_.uid_.composite_id_.strategy_id;
    if (const auto iterator = _ratioStrats.find(strategyId); iterator != _ratioStrats.cend()) {
        iterator->second->OnOrderResponse(order_resp_);
        SendOrderResponse(order_resp_, iterator->second->GetInterface());
    }

    _portfolio_mgr.on_order_response(order_resp_);
}

auto MinixStrategy::doWork() -> int {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (now - _lastSpreadSendMs >= 1000) {
        _lastSpreadSendMs = now;
        SendStrategySpreadsToUi();
    }

    for (auto& [strategyId, ratio] : _ratioStrats) {
        ratio->CheckHedgeTimeout();
    }
    return 0;
}

void MinixStrategy::SendStrategySpreadsToUi() {
    auto sendRatioUI = [this](auto* ratio_) -> void {
        StrategySpreadUpdate update{};
        update._strategyId = ratio_->GetStrategyID();
        update._status     = ratio_->GetStatus();
        update._bcmp       = ratio_->GetBCmp()._spread / 100.0F;
        update._scmp       = ratio_->GetSCmp()._spread / 100.0F;
        update._cost       = static_cast<float>(ratio_->GetCost()) / 100.0F;
        update._flp        = static_cast<float>(ratio_->GetFLP()) / 100.0F;
        update._gap        = static_cast<float>(ratio_->GetGap());
        update._bTrQ       = ratio_->GetLongTradedLots();
        update._sTrQ       = ratio_->GetShortTradedLots();
        update._m2m        = static_cast<float>(ratio_->GetM2M()) / 100.0F;
        update._netPL      = static_cast<float>(ratio_->GetNetPL()) / 100.0F;
        update._rlp        = static_cast<float>(ratio_->GetRLP()) / 100.0F;
        update._cutPL      = static_cast<float>(ratio_->GetCutPL()) / 100.0F;
        update._trSpread   = static_cast<float>(ratio_->GetRLP()) / 100.0F;
        update._bATP       = static_cast<float>(ratio_->GetBATP()) / 100.0F;
        update._sATP       = static_cast<float>(ratio_->GetSATP()) / 100.0F;
        SendStrategySpreadToUi(update, ratio_->GetInterface());
        // ratio->Print();
    };

    for (auto& [strategy, ratio] : _ratioStrats) {
        sendRatioUI(ratio);
    }
}

// ponytail: send binary spread update to UI matching trade tracer pattern (code 100002)
void MinixStrategy::SendStrategySpreadToUi(const StrategySpreadUpdate& update_, int32_t interface_) {
    aef::infra::ui_cmd::UIStruct update{};
    update.header.message_code   = 100002;
    update.header.interface_id   = interface_;
    update.header.message_length = 1520;
    update.header.component_id   = 1;
    update.header.timestamp      = 0;
    std::memset(update.message, 0, sizeof(update.message));
    std::memcpy(update.message, &update_, sizeof(update_));
    sentoUI(update);
}

// ponytail: send binary lifecycle status update to UI (code 100001)
void MinixStrategy::SendStrategyStatusToUi(uint32_t strategyId_, StrategyStatus status_, int32_t interface_) {
    StrategyStatusUpdate update{};
    update._strategyId = strategyId_;
    update._status     = status_;

    aef::infra::ui_cmd::UIStruct updateUI{};
    updateUI.header.message_code   = 100001;
    updateUI.header.interface_id   = interface_;
    updateUI.header.message_length = 1520;
    updateUI.header.component_id   = 1;
    updateUI.header.timestamp      = 0;
    std::memset(updateUI.message, 0, sizeof(updateUI.message));
    std::memcpy(updateUI.message, &update, sizeof(update));
    sentoUI(updateUI);
}

void MinixStrategy::onBcastData(const aef::infra::product::product_data& product_details_) {
    for (auto& [strategy, ratio] : _ratioStrats) {
        ratio->OnBcast(product_details_, _lastTickTs);
    }
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
    if (ui_req_.header.message_code == 100001) {
        const int kMsgBytes = static_cast<int>(sizeof(ui_req_.message));  // 1500
        size_t    len       = kMsgBytes;
        while (len > 0 && ui_req_.message[len - 1] == '\0') {
            --len;  // JSON text never contains NUL
        }
        std::string chunk(ui_req_.message, len);

        JsonReassembly& ra = _jsonReassembly[ui_req_.header.message_code];

        nlohmann::json meta = nlohmann::json::parse(chunk, nullptr, false);
        if (meta.is_object() && meta.contains("packet_count")) {
            ra._expected = static_cast<int>(meta.value("packet_count", 0));
            ra._received = 0;
            ra._buf.clear();
            ra._active = ra._expected > 0;
            std::cout << ">>> [GUI-RX] META code=" << ui_req_.header.message_code << " packet_count=" << ra._expected << '\n';
            return;
        }

        if (!ra._active) {
            std::cout << ">>> [GUI-RX] WARNING data chunk before metadata (code=" << ui_req_.header.message_code << ", " << len << "B) -- ignored" << '\n';
            return;
        }

        ra._buf += chunk;
        ra._received++;
        std::cout << ">>> [GUI-RX] CHUNK " << ra._received << "/" << ra._expected
                  << " code=" << ui_req_.header.message_code << " bytes=" << len
                  << " (accumulated=" << ra._buf.size() << "B)" << '\n';

        if (ra._received >= ra._expected) {
            std::string full = std::move(ra._buf);
            ra               = JsonReassembly{};  // reset for the next transfer
            while (!full.empty() && full.back() == '\0')
                full.pop_back();  // strip any padding
            std::cout << ">>> [GUI-RX] REASSEMBLED code="
                      << ui_req_.header.message_code << " total=" << full.size()
                      << "B  JSON below:\n"
                      << full << "\n>>> [GUI-RX] END-JSON" << '\n';
            ApplyLegStrategyJson(ui_req_.header.interface_id, full);
        }
        return;
    }
}

auto MinixStrategy::UpdateOrder(OrderObjectPtrT& order_, int32_t token_, int32_t price_, int32_t quantity_, client_uid& clientUid_) -> uint32_t {
    uint32_t uid   = clientUid_.id_;
    auto&    order = order_;
    quantity_ -= order->get_filled_qty();
    if (order->get_current_state() != static_cast<uint32_t>(execution_strat::STRAT_ORDER_STATE::STRAT_INITIAL_STATE)) {
        if (order->is_response_pending()) {
            return uid;
        }
        if (quantity_ > 0 && order->get_open_price() != price_) {
            order->set_time_stamps(_event_timestamp, _trigger_timestamp, aef::infra::get_realtime_in_nanos());
            const bool status = order->update_order(price_, quantity_);
            if (status) {
                _portfolio_mgr.on_order_modify(order->get_uid(), price_, quantity_);
            }
        }
    } else {
        if (order->is_response_pending()) {
            return uid;
        }
        if (quantity_ <= 0 || order->get_open_price() == price_) {
            return uid;
        }
        ++clientUid_.composite_id_.request_id;  // print_depth(token);
        if (clientUid_.composite_id_.request_id >= MAX_REQUEST_ID) {
            return 0;
        }
        order->set_time_stamps(_event_timestamp, _trigger_timestamp, aef::infra::get_realtime_in_nanos());
        uid = order->place_order(price_, quantity_, clientUid_.id_);
        if (uid != 0U) {
            _portfolio_mgr.on_order_placed(uid, token_, order->get_side(), price_, quantity_);
        }
    }
    return uid;
}

void MinixStrategy::SendOrderResponse(const oms_transaction& response_, int32_t interface_) {
    OrderResponse responseType = GetOrderResponsee(response_.hdr_.transaction_code);

    if (responseType == OrderResponse_NONE) {
        return;
    }

    aef::infra::ui_cmd::UIStruct update{};
    update.header.message_code   = 100003;
    update.header.interface_id   = interface_;
    update.header.message_length = 1520;
    update.header.component_id   = 1;
    update.header.timestamp      = 0;
    std::memset(update.message, 0, sizeof(update.message));

    ExternalOrderResponse response           = {};
    response._information._orderId           = response_.packet_.exchange_order_id;
    response._information._fields._orderType = response_.packet_.flags_.order_type == ORDER_TYPE::IOC_ORDER_TYPE ? OrderType_IOC : OrderType_LIMIT;
    response._information._fields._side      = response_.packet_.flags_.order_side == BUY_SIDE ? Side_BUY : Side_SELL;

    UserDetails details;
    details._compositeID._fields._user      = response_.hdr_.uid_.composite_id_.client_id;
    details._compositeID._fields._portfolio = response_.hdr_.uid_.composite_id_.strategy_id;
    response._userDetails                   = details;

    OrderResponseInfoT info = {};
    info._response          = responseType;
    info._timestamp         = response_.hdr_.exchange_timestamp;
    info._orderId           = response_.packet_.exchange_order_id;
    info._uniqueId          = response_.hdr_.uid_.id_;
    info._fillNumber        = static_cast<IndexT>(response_.packet_.exchange_fill_id);
    info._placed._price     = response_.packet_.price_;
    info._placed._quantity  = response_.packet_.quantity_;
    info._token             = static_cast<TokenT>(response_.packet_.product_id_);
    info._errorCode         = response_.packet_.exchange_response_code;
    if (info._fillNumber != 0) {
        info._traded = info._placed;
    }

    response._response = info;
    std::memcpy(update.message, &response, sizeof(response));
    sentoUI(update);
}

void MinixStrategy::SendTradeTracerToUi(const TradeTracer& tracer_, int32_t interface_) {
    aef::infra::ui_cmd::UIStruct update{};
    update.header.message_code   = 100004;
    update.header.interface_id   = interface_;
    update.header.message_length = 1520;
    update.header.component_id   = 1;
    update.header.timestamp      = 0;
    std::memset(update.message, 0, sizeof(update.message));
    std::memcpy(update.message, &tracer_, sizeof(tracer_));
    sentoUI(update);
}

extern "C" AlgoBase* create(void* context_) {
    std::cout << __FILE__ << ":" << __FUNCTION__ << std::endl;
    return new MinixStrategy(static_cast<AlgoBase::ContextHandle>(context_));
}

extern "C" void destroy(AlgoBase* strat_) { delete strat_; }
