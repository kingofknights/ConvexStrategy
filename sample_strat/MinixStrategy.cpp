/**
 * @file MinixStrategy.cpp
 * @brief Implementation of the sample Minix strategy.
 */
#include "MinixStrategy.hpp"
#include "BoxSpreadStrategy.hpp"
#include <algorithm>
#include <boost/program_options/options_description.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <unistd.h>

#include <chrono>
#include <thread>

#include <cstring>
#include <regex>

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
    interface_map; // Interface map to store interface ids

/**
 * @brief Convert nanosecond timestamp to human readable UTC string.
 */
std::ifstream openStream(const char *filename) {
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
std::string toLower(const std::string &s) {
  std::string res = s;
  std::transform(res.begin(), res.end(), res.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return res;
}

// Convert to uppercase (for comparison)
static int64_t ljInt(const json &o, const char *k, int64_t def = 0) {
  if (!o.contains(k) || o[k].is_null())
    return def;
  const auto &v = o[k];
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
static double ljDouble(const json &o, const char *k, double def = 0.0) {
  if (!o.contains(k) || o[k].is_null())
    return def;
  const auto &v = o[k];
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
static bool ljBool(const json &o, const char *k, bool def = false) {
  if (!o.contains(k) || o[k].is_null())
    return def;
  const auto &v = o[k];
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
void MinixStrategy::applyLegStrategyJson(const std::string &jsonText) {
  using aef::infra::ui_cmd::BuySell;
  using aef::infra::ui_cmd::StrategyDatafromui;
  using aef::infra::ui_cmd::TokenDatafromui;

  auto sendStatus = [this](std::string status, int strategyId) {
    json response;
    response["Status"] = status;
    response["StrategyId"] = strategyId;
    std::cout << "SendStatus " << response.dump() << std::endl;
    sendJsonChunkedToUI(9621, response.dump());
  };

  std::cout << "applyLegStrategyJson" << std::endl;
  std::cout << "jsonText: " << jsonText << std::endl;
  try {
    json root = json::parse(jsonText);
    json &strategy = root["Strategy"];

    std::string name = strategy.value("SubType", "");
    std::string status = strategy.value("Status", "");
    int strategyId = ljInt(strategy, "StrategyId", 0);
    std::cout << name << " " << status << " " << strategyId << std::endl;

    if (name == "BOX" || name == "Box" || name == "box") {
      sendStatus(status, strategyId);
      handleBoxStrategy(root, jsonText);
    }

  } catch (const std::exception &e) {
    std::cout << "applyLegStrategyJson failed: " << e.what() << std::endl;
  }
}

void MinixStrategy::handleBoxStrategy(const nlohmann::json &root,
                                      const std::string &jsonText) {
  using aef::infra::ui_cmd::BuySell;
  using aef::infra::ui_cmd::StrategyDatafromui;
  using aef::infra::ui_cmd::TokenDatafromui;

  // add the logs in this function

  try {
    if (!root.contains("Legs") || !root["Legs"].is_array() ||
        !root.contains("Strategy") || !root["Strategy"].is_object()) {
      std::cout << "[handleBoxStrategy] missing Strategy or Legs in JSON"
                << std::endl;
      return;
    }

    const json &S = root["Strategy"];
    const json &L = root["Legs"];
    const json P = root.value("Params", json::object());
    const json R = root.value("Ratio", json::object());

    int strategyId = ljInt(S, "StrategyId", 0);
    std::string statusL = toLower(S.value("Status", "pause"));

    // Check for delete/stop cases
    if (statusL == "cancelled" || statusL == "unsubscribed") {
      if (auto it = boxStrats_.find(strategyId); it != boxStrats_.end()) {
        it->second->stop();
        it->second->unsubscribeTokens();
        delete it->second;
        boxStrats_.erase(it);
      }
      legStrategies_.erase(strategyId);
      strategyJson_.erase(strategyId);
      std::cout << ">>> [handleBoxStrategy] BoxSpreadStrategy deleted strat="
                << strategyId << std::endl;
      return;
    }

    // Determine state (running vs paused)
    const int state =
        (statusL == "running" || statusL == "run" || statusL == "start" ||
         statusL == "active" || statusL == "play" || statusL == "resume" ||
         statusL == "applied" || statusL == "subscribed")
            ? 1
            : 0;

    // Determine mode
    bool stratBid = S.value("IsBidding", false);
    bool anyLegBid = false, allLegBid = !L.empty();
    for (const auto &lj : L) {
      bool e = lj.value("EnableBid", false);
      anyLegBid |= e;
      allLegBid = allLegBid && e;
    }
    int mode = 1; // AGGRESSIVE
    if (allLegBid && (stratBid || anyLegBid))
      mode = 4; // ALLLEG_BIDDING
    else if (stratBid || anyLegBid)
      mode = 2; // BIDDING

    StrategyDatafromui s{};
    s.clientid = client;
    s.algoid = algoid;
    s.omsid = omsid;
    s.strategynumber = strategyId;
    s.strategytype = 1; // BOX
    s.strategystate = state;
    s.userbuyspread = static_cast<int>(std::round(
        ljDouble(P, "BPr",
                 ljDouble(S, "BPr", ljDouble(R, "LongBuyPrice", 0.0))) *
        100.0));
    s.usersellspread = static_cast<int>(std::round(
        ljDouble(P, "SPr",
                 ljDouble(S, "SPr", ljDouble(R, "ShortSellPrice", 0.0))) *
        100.0));
    s.buySL = ljInt(S, "buySL", 0);
    s.SellSL = ljInt(S, "SellSL", 0);
    s.buyStoporder = ljInt(S, "buyStoporder", 0);
    s.SellStoporder = ljInt(S, "SellStoporder", 0);
    s.buyNlots = ljInt(S, "buyNlots", 0);
    s.sellNlots = ljInt(S, "sellNlots", 0);
    s.TLots = ljInt(P, "LotSize", 1);
    s.biddingdepth = ljInt(P, "PriceDepth", 0);
    s.orderdepth = ljInt(P, "OrderDepth", 0);
    s.thrsoldqty = ljInt(P, "ThresholdQty", 0);
    s.allowedslippage = ljInt(P, "AllowedSlippage", 0);
    s.normal_bstbid = ljInt(S, "normal_bstbid", 0);
    s.limit_mktorder =
        (toLower(P.value("OrdersType", std::string("Limit"))) == "limit") ? 1
                                                                          : 0;
    s.leavasis = (toLower(P.value("PriceExecutionRange",
                                  std::string("LeaveAsIs"))) == "leaveasis")
                     ? 1
                     : 0;
    s.revertlegs = ljInt(S, "revertlegs", 0);
    s.timetorevertinmilis = ljInt(P, "TimeToRevertBidMs", 0);
    s.actionforunhedgeqty_nonunhedge_ratiounhedge =
        (toLower(P.value("UnhedgedAction", std::string("Ratio"))) == "ratio")
            ? 1
            : 0;
    s.buystepcycles = ljInt(S, "buystepcycles", 0);
    s.sellstepcycles = ljInt(S, "sellstepcycles", 0);
    s.BuyStep = ljInt(S, "BuyStep", 0);
    s.SellStep = ljInt(S, "SellStep", 0);
    s.Steplotsquaroff = ljInt(S, "Steplotsquaroff", 0);
    s.stepsenable = ljBool(S, "stepsenable", false);
    s.flagStepsquaroff = ljBool(S, "flagStepsquaroff", false);
    s.margine = ljInt(S, "margine", 0);

    std::vector<TokenDatafromui> tokens;
    const json *legRatios =
        (R.contains("LegRatios") && R["LegRatios"].is_array()) ? &R["LegRatios"]
                                                               : nullptr;

    for (size_t i = 0; i < L.size(); ++i) {
      const json &lj = L[i];
      TokenDatafromui td{};
      td.token = static_cast<int>(ljInt(lj, "Token", 0));
      std::string sideL = toLower(lj.value("Side", std::string("BUY")));
      td.b_s = (sideL == "sell" || sideL == "s" || sideL == "-1")
                   ? BuySell::Sell
                   : BuySell::Buy;

      int ratio = 1;
      if (legRatios && legRatios->size() > i && (*legRatios)[i].is_number()) {
        ratio = (*legRatios)[i].get<int>();
      } else {
        ratio = static_cast<int>(ljInt(lj, "Lots", 1));
      }
      td.ratio = std::max(1, ratio);

      int strike = static_cast<int>(ljInt(lj, "strikePrice", 0));
      int lot = 0;
      ProductDetails pd{};
      if (getProductDetails(td.token, pd)) {
        lot = pd.lot_size_;
        if (strike <= 0)
          strike = pd.strike_price_;
      }
      if (lot <= 0) {
        lot = static_cast<int>(ljInt(P, "LotSize", 1)); // last-ditch fallback
      }
      td.lotsize = std::max((int16_t)1, (int16_t)lot);
      td.strikePrice = strike;
      td.isbiddingleg = lj.value("EnableBid", false);
      td.undrlineToken = static_cast<int>(ljInt(lj, "undrlineToken", 0));
      td.SL = static_cast<int>(ljInt(lj, "SL", 0));
      td.noofentry = static_cast<int>(ljInt(lj, "noofentry", 0));
      td.trailingslpercent =
          static_cast<int>(ljInt(lj, "trailingslpercent", 0));

      int64_t st = ljInt(lj, "starttime", 0), en = ljInt(lj, "endtime", 0);
      std::memcpy(&td.starttime, &st, sizeof(int64_t));
      std::memcpy(&td.endtime, &en, sizeof(int64_t));

      tokens.push_back(td);
    }

    auto it = boxStrats_.find(s.strategynumber);
    if (it != boxStrats_.end()) {
      it->second->edit(s, tokens, mode);
      std::cout << ">>> [handleBoxStrategy] BoxSpreadStrategy edited strat="
                << s.strategynumber << std::endl;
    } else {
      boxStrats_[s.strategynumber] =
          new BoxSpreadStrategy(this, s, tokens, mode);
      std::cout << ">>> [handleBoxStrategy] BoxSpreadStrategy created strat="
                << s.strategynumber << std::endl;
      it = boxStrats_.find(s.strategynumber);
    }

    // Explicitly start or stop based on the target state
    if (state == 1) {
      it->second->start();
    } else {
      it->second->stop();
    }

    legStrategies_[s.strategynumber] = {s, tokens};
    strategyJson_[s.strategynumber] =
        jsonText; // keep the original GUI JSON to echo back
  } catch (const std::exception &e) {
    std::cout << "[handleBoxStrategy] failed: " << e.what() << std::endl;
  }
}
/**
 * @brief Initialize strategy configuration, subscriptions, and order handles.
 */
MinixStrategy::MinixStrategy(AlgoBase::ContextHandle context)
    : AlgoBase(context) {
  pt::ptree root;
  auto config_file = get_strategy_config_file();
  std::cout << "filename " << config_file << std::endl;
  std::cerr << ">>> [CTOR-ENTRY] MinixStrategy reading config='" << config_file
            << "'" << std::endl;
  auto is = openStream(config_file.c_str());
  pt::read_json(is, root);

  client = root.get<int32_t>("client");
  algoid = root.get<int32_t>("algoid");
  omsid = root.get<int32_t>("omsid");

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
  clientUID.composite_id_.client_id = client;
  clientUID.composite_id_.strategy_id = 1;

  // Subscribe any tokens listed up front in bcast.csv (one token id per line)
  // so their market data flows to OnTick/onBcastData. Each box also subscribes
  // its own legs.
  std::ifstream file_subtok("bcast.csv");
  std::string line_subtok;
  while (std::getline(file_subtok, line_subtok)) {
    if (line_subtok.empty())
      continue;
    try {
      subscribeProduct(std::stoi(line_subtok), flags);
    } catch (const std::exception &) { /* skip non-numeric lines */
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
          for (auto &el : parsed)
            applyLegStrategyJson(el.dump());
        } else {
          std::cout << "[SIM] injecting leg strategy from " << legJsonFile
                    << std::endl;
          applyLegStrategyJson(content);
        }
      } catch (const std::exception &e) {
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
  // Free the box strategies (each prints its own [BOX_PNL]/[BOX_EXEC] summary).
  for (auto &kv : boxStrats_)
    delete kv.second;
  boxStrats_.clear();
}

bool MinixStrategy::subscribeProduct(const int32_t product_id,
                                     const uint16_t flags) {
  LOG_DEBUG("[SUB] subscribeProduct product_id=%d flags=%d", product_id, flags);
  return AlgoBase::subscribeProduct(product_id, flags);
}

bool MinixStrategy::unSubscribeProduct(const int32_t product_id,
                                       const uint16_t flags) {
  LOG_DEBUG("[SUB] unSubscribeProduct product_id=%d flags=%d", product_id,
            flags);
  return AlgoBase::unSubscribeProduct(product_id, flags);
}

// function to squareoff using traderId
std::string format_time(std::time_t t) {
  std::tm *timeInfo = std::localtime(&t);
  std::ostringstream oss;
  oss << std::setw(2) << std::setfill('0') << timeInfo->tm_hour << ":"
      << std::setw(2) << std::setfill('0') << timeInfo->tm_min << ":"
      << std::setw(2) << std::setfill('0') << timeInfo->tm_sec;
  return oss.str();
}

void MinixStrategy::OnTick(const Quote &event) {
  LOG_DEBUG("[TICK] OnTick product_id=%d seq=%d ts=%lu ltp=%d",
            event.header.product_id, event.header.sequence_no,
            event.header.exchange_timestamp, event.message.ltp_);

  // Clock (seconds-of-day*1e9) for the box 1s/EOD timers, derived from the
  // EXCHANGE timestamp (decodes reliably) with event_timestamp as a fallback.
  // This is BEST-EFFORT and only drives timers — it must NOT gate the book
  // update / spread calc.
  auto sodFrom = [](uint64_t ns) -> int64_t {
    uint32_t s = static_cast<uint32_t>(ns / 1000000000ULL);
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

  // ALWAYS forward the tick: the box computes its market spread from the TBT
  // book on every tick (the HFT path) and must never depend on the broadcast
  // feed or on a decodable timestamp. The clock only gates trading/EOD inside
  // run(), not the spread.
  for (auto &kv : boxStrats_)
    kv.second->onTick(event, lastTickTs_);
}

// --- clean order-lifecycle logging helpers --------------------------------
// Decode the OMS transaction code (OMS_API_TRANS_CODES) into a readable event
// name, and the order side, so the platform log prints human-readable order
// lifecycle lines instead of raw numeric codes.
static const char *omsEventName(int code) {
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
static const char *omsSideName(int s) {
  return s == 1 ? "BUY" : (s == 2 ? "SELL" : "?");
}

/**
 * @brief Handle confirmations, trades, and rejects from OMS.
 */
void MinixStrategy::OnOrderResponse(const oms_transaction &order_resp) {
  // Forward to every live box (fill tracking / hedge / squareoff).
  for (auto &kv : boxStrats_)
    kv.second->onOrderResponse(order_resp);

  // Readable order-lifecycle log line.
  LOG_DEBUG("[ORDER] RECV %-18s token=%d side=%-4s qty=%d price=%d uid=%d "
            "err=%d reason=%d",
            omsEventName(order_resp.hdr_.transaction_code),
            order_resp.packet_.product_id_,
            omsSideName(static_cast<int>(order_resp.packet_.flags_.order_side)),
            order_resp.packet_.quantity_, order_resp.packet_.price_,
            order_resp.hdr_.uid_.composite_id_.request_id,
            order_resp.hdr_.error_code, order_resp.hdr_.reason_code);

  // Keep the portfolio order manager in sync (positions/PnL bookkeeping).
  portfolio_mgr_.on_order_response(order_resp);
}

/**
 * @brief Periodic hook invoked by the engine thread.
 */
int count = 0;
bool orderPlaced = 0;
int MinixStrategy::doWork() {
  // Push live market spreads (BCmp/SCmp) to the GUI roughly once per second.
  auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();
  if (now - lastSpreadSendMs_ >= 1000) {
    lastSpreadSendMs_ = now;
    sendStrategySpreadsToUI();
  }
  return 0;
}

/**
 * @brief Every ~1s, echo each strategy's ORIGINAL GUI JSON back to the GUI with
 * the live fields filled in: StrategyUpdates name/value for "BCmp" (market buy
 * spread), "SCmp" (market sell spread) and "Cost" (realized cost to build the
 * spread), plus a top-level "TotalQtyTraded". All spread/cost values are in
 * PAISE (GUI /100 for its rupee display). Sent with the same chunked framing
 * the GUI uses, message_code 8100 (UPDATE_FROM_STRATEGY).
 */
void MinixStrategy::sendStrategySpreadsToUI() {
  for (auto &kv : boxStrats_) {
    BoxSpreadStrategy *box = kv.second;
    if (!box || !box->valid())
      continue;
    auto jit = strategyJson_.find(box->number());
    if (jit == strategyJson_.end())
      continue;

    json jo = json::parse(jit->second, nullptr, false);
    int strategyId = ljInt(jo["Strategy"], "StrategyId", 0);

    json j;

    const int64_t bcmpP = box->bcmp(); // market BUY spread  (paise)
    const int64_t scmpP = box->scmp(); // market SELL spread (paise)
    const int64_t costP = box->cost(); // net cash to build the spread (paise)
    const int64_t trspP =
        box->tradedSpread(); // executed (traded) spread so far (paise)
    const int64_t pnlP = box->pnlPaise(); // realized + MtM PnL (paise)
    const int64_t qty = box->tradedQty();

    // Fields the GUI displays. Spreads/cost/PnL go back in RUPEES (float =
    // paise/100); Gap in paise (string). Matches the production
    // ConversionReversal sender.
    j["StrategyId"] = strategyId;
    j["Status"] = "Updates";
    j["BCmp"] = static_cast<float>(bcmpP) / 100.0f;
    j["SCmp"] = static_cast<float>(scmpP) / 100.0f;
    j["Cost"] = static_cast<float>(costP) / 100.0f;
    j["Gap"] = std::to_string(bcmpP);
    j["B-TrQ"] = qty;
    j["S-TrQ"] = qty;
    j["M2M"] = static_cast<float>(pnlP) / 100.0f; // mark-to-market PnL (rupees)
    j["Net P/L"] = static_cast<float>(pnlP) / 100.0f; // net PnL (rupees)
    j["RLP"] =
        static_cast<float>(trspP) / 100.0f; // realized (traded) spread (rupees)
    j["TrSpread"] = static_cast<float>(trspP) /
                    100.0f; // executed spread, comparable to B-Pr/S-Pr
    j["B-ATP"] = 0.0;
    j["S-ATP"] = 0.0;
    j["B-Buy"] = 0;
    j["B-Sell"] = 0;
    // Also fill the StrategyUpdates name/value list (rupees).

    std::cout << ">>> [BOX_UPD] strat=" << box->number()
              << " BCmp(paise)=" << bcmpP << " SCmp(paise)=" << scmpP
              << " Cost(paise)=" << costP << " tradedSpread(paise)=" << trspP
              << " pnl(paise)=" << pnlP << " boxes=" << box->boxesTraded()
              << " qty=" << qty << "  -> GUI(rs) BCmp=" << (bcmpP / 100.0)
              << " SCmp=" << (scmpP / 100.0) << " books=" << box->bookStatus()
              << std::endl;
    std::cout << ">>> [BOX_LEGS] strat=" << box->number() << box->spreadDetail()
              << std::endl;

    sendJsonChunkedToUI(
        9612, j.dump()); // GUI listens on 9612 for the echo (same code it sent)
  }
}

/**
 * @brief Send a JSON payload to the GUI using the SAME framing the connector
 * uses to send TO us (sendStrategyConfig): one metadata packet
 * {"packet_count":N,"timestamp":..} followed by N raw 1500-byte UTF-8 slices,
 * all with the given message_code.
 */
void MinixStrategy::sendJsonChunkedToUI(int32_t message_code,
                                        const std::string &payload) {
  // Framing matched to the production ConversionReversal sender: one metadata
  // packet
  // {"packet_count":N,"timestamp":<ms>} then N 1500-byte chunks, all on
  // message_code (9612), interface_id 22, message_length 1520, timestamp =
  // current_ts.
  constexpr size_t max_chunk_size = 1500;
  int32_t current_ts = static_cast<int32_t>(
      std::chrono::system_clock::now().time_since_epoch().count() / 1000000);
  int packet_count =
      static_cast<int>((payload.size() + max_chunk_size - 1) / max_chunk_size);
  if (packet_count < 1)
    packet_count = 1;

  // 1) metadata packet
  {
    aef::infra::ui_cmd::UIStruct ui{};
    ui.header.message_code = message_code;
    ui.header.component_id = 1;
    ui.header.timestamp = current_ts;
    ui.header.interface_id = 22;
    ui.header.message_length = 1520;
    nlohmann::json header;
    header["packet_count"] = packet_count;
    header["timestamp"] = current_ts;
    std::string h = header.dump();
    std::memset(ui.message, 0, sizeof(ui.message));
    std::memcpy(ui.message, h.c_str(), std::min(h.size(), max_chunk_size));
    sentoUI(ui);
  }
  // 2) data chunks
  for (int i = 0; i < packet_count; i++) {
    std::string chunk =
        payload.substr(static_cast<size_t>(i) * max_chunk_size, max_chunk_size);
    aef::infra::ui_cmd::UIStruct ui{};
    ui.header.message_code = message_code;
    ui.header.interface_id = 22;
    ui.header.message_length = 1520;
    ui.header.component_id = 1;
    ui.header.timestamp = current_ts;
    std::memset(ui.message, 0, sizeof(ui.message));
    std::memcpy(ui.message, chunk.c_str(), chunk.size());
    sentoUI(ui);
  }
}

/**
 * @brief Run timer-driven tasks at coarse intervals.
 */
void MinixStrategy::onBcastData(
    const aef::infra::product::product_data &product_details_) {
  // Clock for the boxes' 1s/EOD timers, set to the CURRENT tick's market
  // time-of-day (seconds-of-day*1e9) derived from the broadcast LastTradeTime.
  // OnTick uses the same units, so a TBT feed (when present) and the broadcast
  // feed drive one chronological clock; a broadcast-only feed still advances
  // EOD/1s timers. (Not a running max: the clock must reflect the current tick
  // so the EOD anchor lands at the open, not late.)
  if (product_details_.LastTradeTime > 0) {
    std::time_t bt = static_cast<std::time_t>(product_details_.LastTradeTime) +
                     315513000; // NSE 1980-epoch -> Unix
    std::string tt = format_time(bt);
    if (tt.size() >= 8 && tt >= "09:14:00" && tt <= "15:31:00")
      lastTickTs_ =
          (static_cast<int64_t>((tt[0] - '0') * 36000 + (tt[1] - '0') * 3600 +
                                (tt[3] - '0') * 600 + (tt[4] - '0') * 60 +
                                (tt[6] - '0') * 10 + (tt[7] - '0'))) *
          1000000000LL;
  }
  for (auto &kv : boxStrats_)
    kv.second->onBcast(product_details_, lastTickTs_);
}

/**
 * @brief Evaluate parsed CSV rows and fire configured orders.
 */
void MinixStrategy::onUIRequest(const aef::infra::ui_cmd::UIStruct &ui_req) {
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
    const int kMsgBytes = static_cast<int>(sizeof(ui_req.message)); // 1500
    int len = kMsgBytes;
    while (len > 0 && ui_req.message[len - 1] == '\0')
      --len; // JSON text never contains NUL
    std::string chunk(ui_req.message, len);

    JsonReassembly &ra = jsonReassembly_[ui_req.header.message_code];

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
      ra = JsonReassembly{}; // reset for the next transfer
      while (!full.empty() && full.back() == '\0')
        full.pop_back(); // strip any padding
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
int MinixStrategy::update_order(OrderMap &order_handle_, int32_t token,
                                int32_t cur_price, int32_t qty) {

  // Quote quoteC;
  // getLastQuote(token, quoteC);
  int uid = 0;
  auto itr = order_handle_.find(token);
  // std::cout << "Token from Update order : " << token << "cliuid: " <<
  // clientUID.id_ << std::endl;
  if (itr != order_handle_.end()) {
    auto &order = itr->second;
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
              ++requestId; // print_depth(token);
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
/**
 * @brief Cancel an order if eligible and notify portfolio manager.
 */
bool MinixStrategy::cancel_order(OrderMap &order_handle_, int32_t token) {
  auto itr = order_handle_.find(token);
  if (itr != order_handle_.end()) {
    // std::cout<<" Cancel"<<std::endl;
    auto &order1 = itr->second;
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

extern "C" AlgoBase *create(void *context) {
  return new MinixStrategy(static_cast<AlgoBase::ContextHandle>(context));
}

extern "C" void destroy(AlgoBase *strat) { delete strat; }
