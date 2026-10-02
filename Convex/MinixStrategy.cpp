/**
 * @file MinixStrategy.cpp
 * @brief Strategy hub: GUI config intake, strategy lifecycle, event fan-out and UI echo.
 */
#include "MinixStrategy.hpp"

#include "RatioLegStrategy.hpp"

#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

    auto OpenStream(const std::string& filename_) -> std::ifstream {
        std::ifstream in{filename_};
        if (!in) {
            throw std::runtime_error{"Could not open file: " + filename_};
        }
        return in;
    }

}  // namespace

// ═══ Lifecycle ═══════════════════════════════════════════════════════════════

MinixStrategy::MinixStrategy(AlgoBase::ContextHandle context_)
    : AlgoBase(context_) {
    const std::string configFile = get_strategy_config_file();
    std::cerr << ">>> [CTOR-ENTRY] MinixStrategy reading config='" << configFile << "'" << std::endl;

    const nlohmann::json root = nlohmann::json::parse(OpenStream(configFile));
    root.at("client").get_to(_clientId);
    root.at("algoid").get_to(_algoId);
    root.at("omsid").get_to(_omsId);

    // Market-data event flags requested per token: TER + MBP depth + OI + TBT.
    _feedFlags = static_cast<uint16_t>(aef::infra::product::SNAPSHOT_FLAGS::TER_UPDATE_EVENT) |
             static_cast<uint16_t>(aef::infra::product::SNAPSHOT_FLAGS::MBP_UPDATE_EVENT) |
             static_cast<uint16_t>(aef::infra::product::SNAPSHOT_FLAGS::OI_UPDATE_EVENT) |
             static_cast<uint16_t>(aef::infra::product::SNAPSHOT_FLAGS::TBT_UPDATE_EVENT);

    std::cout << "Convex ratio strategies ready for client ID : " << _clientId << std::endl;
}

MinixStrategy::~MinixStrategy() { log_info("Convex : destructor"); }

auto MinixStrategy::subscribeProduct(const int32_t product_id_, const uint16_t flags_) -> bool {
    LOG_DEBUG("[SUB] subscribeProduct product_id=%d flags=%d", product_id_, flags_);
    return AlgoBase::subscribeProduct(product_id_, flags_);
}

// ═══ GUI config intake ═══════════════════════════════════════════════════════

void MinixStrategy::onUIRequest(const aef::infra::ui_cmd::UIStruct& request_) {
    std::cout << ">>> [GUI-RX] onUIRequest code=" << request_.header.message_code
              << " msg_len=" << request_.header.message_length
              << " iface=" << request_.header.interface_id
              << " comp=" << request_.header.component_id << std::endl;

    if (request_.header.message_code != UiMessageCode_STRATEGY_CONFIG) {
        return;
    }

    // The GUI sends a metadata packet {"packet_count":N,"timestamp":..} zero-padded to 1500 B,
    // then N raw 1500 B UTF-8 slices of the strategy JSON. JSON text never contains NUL.
    size_t length = sizeof(request_.message);
    while (length > 0 && request_.message[length - 1] == '\0') {
        --length;
    }
    const std::string_view chunk(request_.message, length);
    JsonReassembly&        reassembly = _jsonReassembly;

    const nlohmann::json meta = nlohmann::json::parse(chunk, nullptr, false);
    if (meta.is_object() && meta.contains("packet_count")) {
        reassembly           = JsonReassembly{};
        reassembly._expected = meta.value("packet_count", 0);
        reassembly._active   = reassembly._expected > 0;
        std::cout << ">>> [GUI-RX] META packet_count=" << reassembly._expected << '\n';
        return;
    }

    if (!reassembly._active) {
        std::cout << ">>> [GUI-RX] WARNING data chunk before metadata (" << length << "B) -- ignored" << '\n';
        return;
    }

    reassembly._buffer += chunk;
    ++reassembly._received;
    std::cout << ">>> [GUI-RX] CHUNK " << reassembly._received << "/" << reassembly._expected
              << " bytes=" << length << " (accumulated=" << reassembly._buffer.size() << "B)" << '\n';

    if (reassembly._received >= reassembly._expected) {
        const std::string full = std::move(reassembly._buffer);
        reassembly             = JsonReassembly{};  // ready for the next transfer
        std::cout << ">>> [GUI-RX] REASSEMBLED total=" << full.size() << "B  JSON below:\n"
                  << full << "\n>>> [GUI-RX] END-JSON" << '\n';
        ApplyLegStrategyJson(request_.header.interface_id, full);
    }
}

void MinixStrategy::ApplyLegStrategyJson(int32_t interface_, const std::string& jsonText_) {
    // SubType -> leg count and whether the strike gap is part of the spread (Box / ConRev).
    struct RatioType {
        std::string_view _name;
        size_t           _legCount;
        bool             _hasStrikeGap;
    };
    static constexpr std::array<RatioType, 8> RatioTypes{{
        {"2LegRatio", 2, false},
        {"3LegRatio", 3, false},
        {"4LegRatio", 4, false},
        {"5LegRatio", 5, false},
        {"6LegRatio", 6, false},
        {"Butterfly", 3, false},
        {"Box", 4, true},
        {"ConRev", 3, true},
    }};

    try {
        const nlohmann::json root     = nlohmann::json::parse(jsonText_);
        const auto&          strategy = root.at("Strategy");

        const std::string name       = strategy.value("SubType", "");
        const std::string status     = strategy.value("Status", "");
        const auto        strategyId = strategy.value("StrategyId", 0U);

        std::cout << "[applyLegStrategyJson] SubType='" << name << "' Status='" << status << "' StrategyId=" << strategyId << std::endl;

        for (const RatioType& type : RatioTypes) {
            if (name != type._name) continue;

            HandleRatioLegStrategy(root, type._legCount, interface_, type._hasStrikeGap);

            // Echo the status the strategy actually has, so a failed create or a delete reads INACTIVE.
            const auto iterator = _strategies.find(strategyId);
            const StrategyStatusUpdate update{strategyId, iterator != _strategies.end() ? iterator->second->GetStatus() : StrategyStatus_INACTIVE};
            SendToUi(UiMessageCode_STRATEGY_STATUS, interface_, update);
            break;
        }
    } catch (const std::exception& e) {
        std::cout << "applyLegStrategyJson failed: " << e.what() << std::endl;
    }
}

void MinixStrategy::HandleRatioLegStrategy(const nlohmann::json& root_, size_t legCount_, int32_t interface_, bool hasStrikeGap_) {
    try {
        const auto&       strategy   = root_.at("Strategy");
        const std::string status     = ToUpper(strategy.at("Status").get<std::string>());
        const auto        strategyId = strategy.at("StrategyId").get<uint32_t>();

        auto iterator = _strategies.find(strategyId);

        if (status == "SUBSCRIBED" || status == "SUBSCRIBE" || status == "NEW" || status == "ACTIVE" ||
            status == "APPLIED" || status == "APPLY") {
            if (iterator == _strategies.end()) {
                std::cout << "[handleRatioLegStrategy] Creating new RatioLegStrategy for strat=" << strategyId << " with status=" << status << std::endl;
                iterator = _strategies.emplace(strategyId, std::make_unique<RatioLegStrategy>(this, strategyId, interface_, root_, legCount_, hasStrikeGap_)).first;
            } else {
                std::cout << "[handleRatioLegStrategy] Updating existing RatioLegStrategy for strat=" << strategyId << " with status=" << status << std::endl;
                iterator->second->ParamUpdate(root_);
            }
            iterator->second->SetStatus(StringToStrategyStatus(status));
        } else if (iterator == _strategies.end()) {
            return;
        } else if (status == "UNSUBSCRIBED" || status == "UNSUBSCRIBE" || status == "STOP") {
            std::cout << "[handleRatioLegStrategy] Stopping strat=" << strategyId << std::endl;
            iterator->second->Stop();
        } else if (status == "DELETED" || status == "DELETE") {
            std::cout << "[handleRatioLegStrategy] Deleting strat=" << strategyId << std::endl;
            iterator->second->Stop();
            _strategies.erase(iterator);
        }
    } catch (const std::exception& e) {
        std::cout << "[handleRatioLegStrategy] failed: " << e.what() << std::endl;
    }
}

// ═══ Event fan-out ═══════════════════════════════════════════════════════════

void MinixStrategy::OnTick(const Quote& quote_) {
    _eventTimestamp   = quote_.header.event_timestamp;
    _triggerTimestamp = quote_.header.trigger_timestamp;
    for (const auto& [strategyId, strategy] : _strategies) {
        strategy->OnTick(quote_);
    }
}

void MinixStrategy::OnOrderResponse(const oms_transaction& response_) {
    const uint32_t strategyId = response_.hdr_.uid_.composite_id_.strategy_id;
    if (const auto iterator = _strategies.find(strategyId); iterator != _strategies.end()) {
        iterator->second->OnOrderResponse(response_);
        SendOrderResponse(response_, iterator->second->GetInterface());
    }
}

void MinixStrategy::onBcastData([[maybe_unused]] const aef::infra::product::product_data& product_details_) {}

auto MinixStrategy::doWork() -> int {
    // steady_clock: a wall-clock step (NTP) must not stall or burst the UI echo.
    const auto now = std::chrono::steady_clock::now();
    if (now - _lastSpreadSend >= std::chrono::seconds(1)) {
        _lastSpreadSend = now;
        SendStrategySpreadsToUi();
    }
    return 0;
}

// ═══ Orders ══════════════════════════════════════════════════════════════════

auto MinixStrategy::UpdateOrder(OrderObjectT& order_, int32_t price_, int32_t quantity_, client_uid& clientUid_) -> uint32_t {
    // Checked here, once, for every caller: the vendor refuses these too, but logs each refusal.
    if (quantity_ <= 0 || price_ <= 0 || order_.is_response_pending()) {
        return 0;
    }

    if (order_.get_current_state() != static_cast<uint32_t>(execution_strat::STRAT_ORDER_STATE::STRAT_INITIAL_STATE)) {
        // Modify on a price or a size change (a resting hedge grows when more lots are needed).
        // quantity_ is the open quantity wanted; update_order adds the filled quantity back itself.
        if (!order_.is_order_confirmed() || (order_.get_open_price() == price_ && order_.get_open_qty() == quantity_)) {
            return 0;
        }
        order_.set_time_stamps(_eventTimestamp, _triggerTimestamp, aef::infra::get_realtime_in_nanos());
        return order_.update_order(price_, quantity_) ? order_.get_uid() : 0;
    }

    // request_id is a 22-bit field: stop at the limit instead of wrapping and reusing ids.
    if (clientUid_.composite_id_.request_id >= MAX_REQUEST_ID) {
        return 0;
    }
    if (++clientUid_.composite_id_.request_id == MAX_REQUEST_ID) {
        std::cout << "[UpdateOrder] strat=" << clientUid_.composite_id_.strategy_id
                  << " request_id limit " << MAX_REQUEST_ID << " reached, no new orders will be placed" << std::endl;
        return 0;
    }
    order_.set_time_stamps(_eventTimestamp, _triggerTimestamp, aef::infra::get_realtime_in_nanos());
    return order_.place_order(price_, quantity_, clientUid_.id_);
}

// ═══ UI echo ═════════════════════════════════════════════════════════════════

void MinixStrategy::SendStrategySpreadsToUi() {
    for (const auto& [strategyId, strategy] : _strategies) {
        StrategySpreadUpdate update{};
        update._strategyId = strategy->GetStrategyID();
        update._status     = strategy->GetStatus();
        update._bcmp       = strategy->GetBCmp()._spread / 100.0F;
        update._scmp       = strategy->GetSCmp()._spread / 100.0F;
        update._cost       = static_cast<float>(strategy->GetCost()) / 100.0F;
        update._flp        = static_cast<float>(strategy->GetFLP()) / 100.0F;
        update._gap        = static_cast<float>(strategy->GetGap());
        update._bTrQ       = strategy->GetLongTradedPacks();
        update._sTrQ       = strategy->GetShortTradedPacks();
        update._m2m        = static_cast<float>(strategy->GetM2M()) / 100.0F;
        update._netPL      = static_cast<float>(strategy->GetNetPL()) / 100.0F;
        update._rlp        = static_cast<float>(strategy->GetRLP()) / 100.0F;
        update._cutPL      = static_cast<float>(strategy->GetCutPL()) / 100.0F;
        update._bATP       = static_cast<float>(strategy->GetBATP()) / 100.0F;
        update._sATP       = static_cast<float>(strategy->GetSATP()) / 100.0F;
        SendToUi(UiMessageCode_STRATEGY_SPREAD, strategy->GetInterface(), update);
    }
}

void MinixStrategy::SendOrderResponse(const oms_transaction& response_, int32_t interface_) {
    const OrderResponse responseType = GetOrderResponse(response_.hdr_.transaction_code);
    if (responseType == OrderResponse_NONE) {
        return;
    }

    ExternalOrderResponse response           = {};
    response._information._orderId           = response_.packet_.exchange_order_id;
    response._information._fields._orderType = response_.packet_.flags_.order_type == ORDER_TYPE::IOC_ORDER_TYPE ? OrderType_IOC : OrderType_LIMIT;
    response._information._fields._side      = response_.packet_.flags_.order_side == BUY_SIDE ? Side_BUY : Side_SELL;

    response._userDetails._compositeID._fields._user      = response_.hdr_.uid_.composite_id_.client_id;
    response._userDetails._compositeID._fields._portfolio = response_.hdr_.uid_.composite_id_.strategy_id;

    OrderResponseInfoT& info = response._response;
    info._response           = responseType;
    info._timestamp          = response_.hdr_.exchange_timestamp;
    info._orderId            = response_.packet_.exchange_order_id;
    info._uniqueId           = response_.hdr_.uid_.id_;
    info._fillNumber         = static_cast<IndexT>(response_.packet_.exchange_fill_id);
    info._placed._price      = response_.packet_.price_;
    info._placed._quantity   = response_.packet_.quantity_;
    info._token              = static_cast<TokenT>(response_.packet_.product_id_);
    info._errorCode          = response_.packet_.exchange_error_code;
    if (info._fillNumber != 0) {
        info._traded = info._placed;
    }

    SendToUi(UiMessageCode_ORDER_RESPONSE, interface_, response);
}

extern "C" AlgoBase* create(void* context_) {
    std::cout << __FILE__ << ":" << __FUNCTION__ << std::endl;
    return new MinixStrategy(static_cast<AlgoBase::ContextHandle>(context_));
}

extern "C" void destroy(AlgoBase* strat_) { delete strat_; }
