#include "BoxSpreadStrategy.hpp"
#include "MinixStrategy.hpp" // update_order, (un)subscribeProduct, getProductDetails, ORDER_SIDE/TYPE

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <fstream>

using aef::infra::ui_cmd::BuySell;
using aef::infra::ui_cmd::StrategyDatafromui;
using aef::infra::ui_cmd::TokenDatafromui;

namespace {
// All box logging goes to a dedicated debug FILE — never std::cout. Path from env
// BOX_LOG (default "box_debug.log"); opened once per process, shared by all box
// instances (single STRAT thread), and FLUSHED per line so nothing is lost on
// SIGTERM/SIGKILL (the engine's buffered log_debug was unreliable — wrote 0 bytes).
std::ofstream &boxLogFile()
{
  static std::ofstream f([] {
    const char *p = getenv("BOX_LOG");
    return (p && *p) ? std::string(p) : std::string("box_debug.log");
  }(), std::ios::out | std::ios::trunc);
  return f;
}
struct DbgLine
{
  std::ostringstream os_;
  explicit DbgLine(MinixStrategy * /*ms*/) {}
  ~DbgLine() { std::ofstream &f = boxLogFile(); if (f) { f << os_.str() << '\n'; f.flush(); } }
  template <class T> DbgLine &operator<<(const T &v) { os_ << v; return *this; }
  DbgLine &operator<<(std::ostream &(*)(std::ostream &)) { return *this; } // swallow std::endl
};
constexpr int64_t kOneSecNs = 1000000000LL;               // 1s in ns for the relative escalation timer
// 15:29 EOD square-off, expressed as a fixed offset from the 09:15 market open so it
// can be applied to the raw exchange clock (first valid tick = open) — no localtime.
constexpr int64_t kMktOpenSec = 9 * 3600 + 15 * 60;       // 09:15:00
constexpr int64_t kEodSec     = 15 * 3600 + 29 * 60;      // 15:29:00
constexpr int64_t kOpenToEodNs = (kEodSec - kMktOpenSec) * kOneSecNs; // 22440 s -> ns
// Option trading cost in Rs per crore of traded value (brokerage+charges), applied to every
// box leg to compute the GUI "Cost" (total cost to trade the box).
constexpr int64_t kBoxCostPerCrore = 7000;
// Market-data event flags to request per leg (TER + MBP depth + OI + TBT). Subscribing
// with 0 (the previous value) requests NO events, so the legs never get bid/ask -> NOBOOK.
constexpr uint16_t kBoxSubFlags =
    static_cast<uint16_t>(aef::infra::product::SNAPSHOT_FLAGS::TER_UPDATE_EVENT) |
    static_cast<uint16_t>(aef::infra::product::SNAPSHOT_FLAGS::MBP_UPDATE_EVENT) |
    static_cast<uint16_t>(aef::infra::product::SNAPSHOT_FLAGS::OI_UPDATE_EVENT) |
    static_cast<uint16_t>(aef::infra::product::SNAPSHOT_FLAGS::TBT_UPDATE_EVENT);
} // namespace

BoxSpreadStrategy::BoxSpreadStrategy(MinixStrategy *ms, const StrategyDatafromui &s,
                                     const std::vector<TokenDatafromui> &tokens, int mode)
    : ms_(ms)
{
  strategynumber_ = s.strategynumber;
  clientid_ = s.clientid ? s.clientid : ms_->client;
  algoid_ = s.algoid ? s.algoid : ms_->algoid;
  omsid_ = s.omsid ? s.omsid : ms_->omsid;
  orderDepth_ = (s.orderdepth >= 1 && s.orderdepth <= 5) ? s.orderdepth : 5;
  mode_ = (mode == BIDDING || mode == ALLLEG_BIDDING) ? mode : AGGRESSIVE;
  biddingDepth_ = s.biddingdepth > 0 ? s.biddingdepth : 25;
  revertBidNs_ = s.timetorevertinmilis > 0 ? static_cast<int64_t>(s.timetorevertinmilis) * 1000000LL : kOneSecNs;
  userBuySpread_  = s.userbuyspread;   // B-Pr threshold
  userSellSpread_ = s.usersellspread;  // S-Pr threshold
  // Passive (bidding) modes rest limit orders; aggressive uses GUI limit/IOC choice.
  orderType_ = (mode_ != AGGRESSIVE) ? ORDER_TYPE::LIMIT_ORDER_TYPE
             : ((s.limit_mktorder != 0) ? ORDER_TYPE::LIMIT_ORDER_TYPE : ORDER_TYPE::IOC_ORDER_TYPE);
  running_ = (s.strategystate != 0);

  for (size_t i = 0; i < tokens.size(); ++i)
  {
    Leg leg;
    leg.token = tokens[i].token;
    leg.side = (tokens[i].b_s == BuySell::Sell) ? -1 : 1;
    leg.ratio = std::max(1, tokens[i].ratio);
    leg.lotsize = std::max(1, static_cast<int>(tokens[i].lotsize));
    leg.strike = tokens[i].strikePrice;
    leg.isbidding = tokens[i].isbiddingleg;
    // Resolve the REAL option type (CE/PE) so classifyLegs can verify this is a
    // genuine box. The GUI omits the contract lot size and type; pull both from
    // product details (best-effort: in sim this may be unavailable, leaving
    // optType=0 which simply skips the advisory check below).
    ProductDetails pd{};
    if (ms_->getProductDetails(leg.token, pd))
    {
      if (pd.opt_type_ == aef::infra::product::OPTION_TYPE::CE ||
          pd.opt_type_ == aef::infra::product::OPTION_TYPE::CA)
        leg.optType = 1;
      else if (pd.opt_type_ == aef::infra::product::OPTION_TYPE::PE ||
               pd.opt_type_ == aef::infra::product::OPTION_TYPE::PA)
        leg.optType = 2;
      if (leg.strike <= 0) leg.strike = pd.strike_price_;
    }
    legs_.push_back(std::move(leg));
  }
  boxRatio_ = legs_.empty() ? 1 : legs_[0].ratio;
  lotsize_ = legs_.empty() ? 1 : legs_[0].lotsize;

  classifyLegs();
  // Mode 2 default: if no leg was flagged bidding, bid the two BUY legs (C@K1, P@K2)
  // and hedge the two SELL legs aggressively on fill.
  if (mode_ == BIDDING)
  {
    bool any = false; for (auto &l : legs_) if (l.isbidding) any = true;
    if (!any) for (auto &l : legs_) if (l.side > 0) l.isbidding = true;
  }
  // Mode 4: every leg bids.
  if (mode_ == ALLLEG_BIDDING) for (auto &l : legs_) l.isbidding = true;

  computeSlicing(s);

  if (running_ && valid_ && !getenv("SKIP_SUBSCRIBE"))
  {
    uint16_t flags = kBoxSubFlags;
    for (auto &l : legs_)
    {
      DbgLine(ms_) << "[BOX_SUB] subscribing leg token=" << l.token << " flags=" << flags << " for strat " << strategynumber_ << std::endl;
      ms_->subscribeProduct(l.token, flags);
    }
  }
  if (running_ && valid_) subscribed_ = true;

  DbgLine(ms_) << "[BOX] setup strat " << strategynumber_ << " valid=" << valid_ << " mode=" << mode_
            << " K1=" << K1_ << " K2=" << K2_ << " gapPts=" << (K2_ - K1_)
            << " lot=" << lotsize_ << " ratio=" << boxRatio_ << " biddingDepth=" << biddingDepth_
            << " B-Pr=" << userBuySpread_ << " S-Pr=" << userSellSpread_
            << " total=" << totalLots_ << "box running=" << running_ << std::endl;
}

BoxSpreadStrategy::~BoxSpreadStrategy()
{
  // Final PnL + executed-spread summary for every instance that placed an order.
  if (entryOrders_ > 0 || cashFlow_ != 0)
  {
    bool flat; int64_t pnl = computePnl(flat);
    DbgLine(ms_) << "[BOX_PNL] strat=" << strategynumber_ << " K1=" << K1_ << " K2=" << K2_
              << " entries=" << entryOrders_ << " fills=" << fills_ << " boxesTraded=" << boxesDone_
              << " flat=" << (flat ? 1 : 0)
              << " tradedSpread=" << lastTradedSpread_ << "p"
              << " pnlRs=" << (pnl / 100.0) << " (pnlPaise=" << pnl << ")" << std::endl;
    // Execution-quality breakdown: how many boxes captured all-4 at the bid, how many
    // needed aggressive completion, and how many residuals had to be reversed.
    DbgLine(ms_) << "[BOX_EXEC] strat=" << strategynumber_ << " K1=" << K1_ << " K2=" << K2_
              << " allBidSlices=" << spSliceClean_ << " aggrCompleted=" << spSliceAggr_
              << " reversals=" << spReversals_ << std::endl;
    DbgLine(ms_) << "[BOX_CLK] strat=" << strategynumber_ << " anchorSod=" << (anchorTs_ / 1000000000)
              << " lastSod=" << (lastNowTs_ / 1000000000) << " eodSod=" << (eodTs_ / 1000000000)
              << " eodFlat=" << (eodFlat_ ? 1 : 0) << std::endl;
  }
}

// Fill-based PnL (paise): net cash from every fill (buy pays / sell receives) + mark-to-
// market of any unsquared residual at the last touch mid, minus per-fill txn cost and the
// cost to close the residual. `flat` is set false if any leg still carries a net position.
int64_t BoxSpreadStrategy::computePnl(bool &flat) const
{
  int64_t resCash = 0, closeCost = 0;
  flat = true;
  for (const auto &l : legs_)
    if (l.signedPos != 0)
    {
      flat = false;
      resCash   += static_cast<int64_t>(l.signedPos) * l.lastMid;
      closeCost += txnCost(std::llabs(static_cast<int64_t>(l.signedPos)) * l.lastMid, l.signedPos > 0 ? -1 : 1);
    }
  return (cashFlow_ + resCash) - (txnCostPaise_ + closeCost);
}

// Identify the call/put at K1/K2 by OPTION TYPE + strike (independent of the user's buy/sell
// side) so the production arb-edge spread can be computed. Sets iCallK1_/iPutK1_/iCallK2_/
// iPutK2_ and canCanonical_ (true when all four CE/PE roles resolve). valid_ needs 4 legs +
// 2 strikes; if option types aren't resolved we fall back to side-based roles (raw spread).
void BoxSpreadStrategy::classifyLegs()
{
  valid_ = false; canCanonical_ = false;
  iCallK1_ = iPutK1_ = iCallK2_ = iPutK2_ = -1;
  if (legs_.size() != 4) { DbgLine(ms_) << "[BOX] strat " << strategynumber_ << " need exactly 4 legs (got " << legs_.size() << ")" << std::endl; return; }
  int32_t lo = legs_[0].strike, hi = legs_[0].strike;
  for (auto &l : legs_) { lo = std::min(lo, l.strike); hi = std::max(hi, l.strike); }
  if (lo <= 0 || hi <= lo) { DbgLine(ms_) << "[BOX] strat " << strategynumber_ << " bad strikes K1=" << lo << " K2=" << hi << std::endl; return; }
  K1_ = lo; K2_ = hi;

  // Preferred: roles by real option type (1=CE, 2=PE) + strike.
  for (size_t i = 0; i < legs_.size(); ++i)
  {
    const Leg &l = legs_[i];
    const bool isK1 = (l.strike == K1_);
    if      (l.optType == 1) { if (isK1) iCallK1_ = i; else iCallK2_ = i; } // CE
    else if (l.optType == 2) { if (isK1) iPutK1_  = i; else iPutK2_  = i; } // PE
  }
  if (iCallK1_ >= 0 && iPutK1_ >= 0 && iCallK2_ >= 0 && iPutK2_ >= 0)
  {
    canCanonical_ = true;                       // production arb-edge spread available
    legs_[iCallK1_].isCall = legs_[iCallK2_].isCall = true;
    legs_[iPutK1_].isCall  = legs_[iPutK2_].isCall  = false;
    valid_ = true;
    DbgLine(ms_) << "[BOX] strat " << strategynumber_ << " canonical roles by optType: CE@K1="
              << legs_[iCallK1_].token << " PE@K1=" << legs_[iPutK1_].token
              << " CE@K2=" << legs_[iCallK2_].token << " PE@K2=" << legs_[iPutK2_].token << std::endl;
    return;
  }

  // Fallback: option types not resolved -> assign roles by side (raw spread only).
  iCallK1_ = iPutK1_ = iCallK2_ = iPutK2_ = -1;
  for (size_t i = 0; i < legs_.size(); ++i)
  {
    Leg &l = legs_[i];
    if (l.strike == K1_) { if (l.side > 0) { l.isCall = true; iCallK1_ = i; } else { l.isCall = false; iPutK1_ = i; } }
    else                 { if (l.side < 0) { l.isCall = true; iCallK2_ = i; } else { l.isCall = false; iPutK2_ = i; } }
  }
  if (iCallK1_ < 0 || iPutK1_ < 0 || iCallK2_ < 0 || iPutK2_ < 0)
  { DbgLine(ms_) << "[BOX] strat " << strategynumber_ << " malformed box (need CE+PE at each strike)" << std::endl; return; }
  DbgLine(ms_) << "[BOX] WARNING strat " << strategynumber_ << " option types unresolved -> raw spread (not gap-subtracted)" << std::endl;
  valid_ = true;
}

void BoxSpreadStrategy::computeSlicing(const StrategyDatafromui &s)
{
  totalLots_ = std::max(1, s.TLots);
  sliceLots_ = (s.buyNlots > 0) ? s.buyNlots : totalLots_;
  sliceLots_ = std::max(1, std::min(sliceLots_, totalLots_));
  numSlices_ = (totalLots_ + sliceLots_ - 1) / sliceLots_;
  placedSlices_ = 0;
  for (auto &l : legs_) l.slices.assign(numSlices_, OrderMap{});
}

void BoxSpreadStrategy::start()
{
  if (!valid_) return;
  if (!subscribed_ && !getenv("SKIP_SUBSCRIBE"))
  {
    uint16_t flags = kBoxSubFlags;
    for (auto &l : legs_)
    {
      DbgLine(ms_) << "[BOX_SUB] subscribing leg token=" << l.token << " flags=" << flags << " for strat " << strategynumber_ << std::endl;
      ms_->subscribeProduct(l.token, flags);
    }
  }
  subscribed_ = true;
  running_ = true;
  DbgLine(ms_) << "[BOX] started strat " << strategynumber_ << std::endl;
}

void BoxSpreadStrategy::stop()
{
  running_ = false;
  for (auto &l : legs_)
    for (auto &m : l.slices)
      for (auto &kv : m) kv.second.cancel_order();
  for (auto &l : legs_) squareOffLeg(l);
  DbgLine(ms_) << "[BOX] STOP strat " << strategynumber_ << " -> cancelled opens + squared off any built legs" << std::endl;
}

void BoxSpreadStrategy::edit(const StrategyDatafromui &s, const std::vector<TokenDatafromui> &tokens,
                             int mode)
{
  // Tokens/sides/strikes are immutable (they define the box); ratio, B-Pr/S-Pr
  // thresholds, mode, bidding depth and order depth are editable.
  if (mode == AGGRESSIVE || mode == BIDDING || mode == ALLLEG_BIDDING) mode_ = mode;
  if (s.biddingdepth > 0) biddingDepth_ = s.biddingdepth;
  if (s.timetorevertinmilis > 0) revertBidNs_ = static_cast<int64_t>(s.timetorevertinmilis) * 1000000LL;
  userBuySpread_  = s.userbuyspread;
  userSellSpread_ = s.usersellspread;
  if (s.orderdepth >= 1 && s.orderdepth <= 5) orderDepth_ = s.orderdepth;
  for (size_t i = 0; i < tokens.size() && i < legs_.size(); ++i)
    legs_[i].ratio = std::max(1, tokens[i].ratio);
  boxRatio_ = legs_.empty() ? 1 : legs_[0].ratio;
  if (placedSlices_ == 0) computeSlicing(s);
  DbgLine(ms_) << "[BOX] edited strat " << strategynumber_ << " ratio=" << boxRatio_
            << " B-Pr=" << userBuySpread_ << " S-Pr=" << userSellSpread_ << std::endl;
}

void BoxSpreadStrategy::unsubscribeTokens()
{
  if (!subscribed_) return;
  uint16_t flags = kBoxSubFlags;
  for (auto &l : legs_)
  {
    DbgLine(ms_) << "[BOX_SUB] unsubscribing leg token=" << l.token << " flags=" << flags << " for strat " << strategynumber_ << std::endl;
    ms_->unSubscribeProduct(l.token, flags);
  }
  subscribed_ = false;
}

void BoxSpreadStrategy::onOrderResponse(const oms_transaction &resp)
{
  if (resp.hdr_.transaction_code != OMS_API_TRANS_CODES::OMS_TRADE) return;
  uint32_t uid = resp.hdr_.uid_.id_;
  int32_t qty = resp.packet_.quantity_;
  int32_t fillPx = resp.packet_.price_;          // actual fill price (paise)
  int64_t value = static_cast<int64_t>(fillPx) * qty;

  // Entry-side fills advance leg.pos (drives slice gating + box completion); square-off
  // fills do not. A uid is in uidLeg_ only if it was an ENTRY order.
  bool isEntry = false;
  if (auto it = uidLeg_.find(uid); it != uidLeg_.end()) { legs_[it->second].pos += qty; isEntry = true; }

  auto si = uidSign_.find(uid);
  if (si == uidSign_.end()) return;
  int li = si->second.first, sgn = si->second.second; // sgn +1 buy, -1 sell (effective side)
  Leg &leg = legs_[li];
  leg.signedPos += sgn * qty;

  // ---- PnL: every fill moves cash (buy pays / sell receives) + per-fill txn cost --------
  cashFlow_ += (sgn > 0 ? -value : value);
  txnCostPaise_ += txnCost(value, sgn);
  fills_++;
  DbgLine(ms_) << "[BOX][TRADE] strat " << strategynumber_ << " FILL leg " << leg.token
            << " " << (sgn > 0 ? "BUY" : "SELL") << " qty=" << qty << " @ " << fillPx
            << " signedPos=" << leg.signedPos << std::endl;

  if (!isEntry) return; // square-off fills don't define the traded spread

  // ---- Executed spread: accumulate entry fills in the BCmp/SCmp convention (a leg's
  // CONFIGURED side: sell +fill, buy -fill) so it is directly comparable to B-Pr / S-Pr.
  entrySpreadCash_ += (leg.side < 0 ? +value : -value);
  // A box is fully built once EVERY leg has another (ratio*lotsize) of entry fills.
  int done = INT32_MAX;
  for (const auto &l : legs_) done = std::min(done, l.pos / std::max(1, l.ratio * l.lotsize));
  if (done > boxesDone_)
  {
    boxesDone_ = done;
    // Per-share, ratio-weighted executed spread of the boxes built so far (paise).
    lastTradedSpread_ = entrySpreadCash_ / (static_cast<int64_t>(boxesDone_) * std::max(1, lotsize_));
    bool flat; int64_t pnl = computePnl(flat);
    DbgLine(ms_) << "[BOX_TRADE] strat=" << strategynumber_ << " box " << boxesDone_ << "/" << totalLots_
              << " dir=" << (lastDir_ >= 0 ? "BUY" : "SELL")
              << " tradedSpread=" << lastTradedSpread_ << "p"
              << " (B-Pr=" << userBuySpread_ << " S-Pr=" << userSellSpread_ << ")"
              << " BCmp=" << lastBCmp_ << " SCmp=" << lastSCmp_
              << " pnlRs=" << (pnl / 100.0) << std::endl;
  }
}

// Linear scan over the (4) legs — no hash, all in cache. Returns -1 if not a box token.
int BoxSpreadStrategy::legIndex(int32_t token) const
{
  for (int i = 0; i < static_cast<int>(legs_.size()); ++i)
    if (legs_[i].token == token) return i;
  return -1;
}

// deepest valid (>0) executable price at or above `level` for `side` (buy=ask, sell=bid),
// read from the leg's own cached top-5 book (no map).
int BoxSpreadStrategy::legPrice(const Leg &l, int side, int level) const
{
  int maxL = level < 4 ? level : 4;
  for (int L = maxL; L >= 0; --L)
  {
    int p = side > 0 ? l.ask[L] : l.bid[L];
    if (p > 0) return p;
  }
  return 0;
}

// Passive "bidding" price: improve the touch by biddingDepth toward a fill — a BUY
// leg posts biddingDepth below the ask, a SELL leg biddingDepth above the bid.
int BoxSpreadStrategy::biddingPrice(const Leg &l, int side) const
{
  int touch = legPrice(l, side, 0);
  if (touch <= 0) return 0;
  int px = touch - biddingDepth_ * side; // buy: ask-bd ; sell: bid+bd
  return px > 0 ? px : touch;
}

// True only if EVERY leg has a valid (non-zero, non-crossed) cached top-of-book.
bool BoxSpreadStrategy::booksReady() const
{
  for (const auto &l : legs_)
  {
    if (l.bid[0] <= 0 || l.ask[0] <= 0) return false;
    // Reject crossed/locked books (bid >= ask): a stale one-sided quote otherwise
    // fabricates a huge phantom edge.
    if (l.bid[0] >= l.ask[0]) return false;
  }
  return true;
}

// Update the ticked leg's cached top-5 book in place (contiguous), then run the logic.
void BoxSpreadStrategy::onTick(const Quote &event, int64_t nowTs)
{
  const int i = legIndex(event.header.product_id);
  if (i < 0) return;
  Leg &l = legs_[i];
  DbgLine(ms_) << "[BOX_TICK] strat=" << strategynumber_ << " tok=" << event.header.product_id 
               << " bid0=" << event.message.bid_levels[0].price << " ask0=" << event.message.ask_levels[0].price
               << " ltp=" << event.message.ltp_ << " seq=" << event.header.sequence_no << std::endl;
  for (int j = 0; j < 5; ++j)
  {
    l.bid[j] = event.message.bid_levels[j].price;
    l.ask[j] = event.message.ask_levels[j].price;
  }
  if (event.message.ltp_ > 0) l.ltp = event.message.ltp_;
  run(event.header.product_id, nowTs);
}

// Broadcast/snapshot path: refresh the ticked leg's cached book from product_data depth.
void BoxSpreadStrategy::onBcast(const aef::infra::product::product_data &pd, int64_t nowTs)
{
  const int i = legIndex(pd.product_id_);
  // Verify broadcast data is being received: dump the raw product_data top-of-book
  // for every broadcast packet (even tokens not part of this strat -> i<0).
  DbgLine(ms_) << "[BOX_BCAST] strat=" << strategynumber_ << " tok=" << pd.product_id_
               << " legIdx=" << i << " ltp=" << static_cast<int>(pd.ltp_)
               << " bid0=" << static_cast<int>(pd.bid_mbp[0].price)
               << " (q=" << static_cast<int>(pd.bid_mbp[0].qty) << ")"
               << " ask0=" << static_cast<int>(pd.ask_mbp[0].price)
               << " (q=" << static_cast<int>(pd.ask_mbp[0].qty) << ")"
               << " nowTs=" << nowTs << std::endl;
  if (i < 0) return;
  Leg &l = legs_[i];
  for (int j = 0; j < 5; ++j)
  {
    l.bid[j] = static_cast<int>(pd.bid_mbp[j].price);
    l.ask[j] = static_cast<int>(pd.ask_mbp[j].price);
  }
  if (pd.ltp_ > 0) l.ltp = static_cast<int>(pd.ltp_);
  run(pd.product_id_, nowTs);
}

// Diagnostic: per-leg cached top-of-book (no map). bid=0/ask=0 means no tick received yet.
std::string BoxSpreadStrategy::bookStatus() const
{
  std::string s;
  for (const auto &l : legs_)
  {
    char b[96];
    if (l.bid[0] == 0 && l.ask[0] == 0)
      std::snprintf(b, sizeof(b), "[tok=%d NODATA]", l.token);
    else
      std::snprintf(b, sizeof(b), "[tok=%d bid=%d ask=%d]", l.token, l.bid[0], l.ask[0]);
    s += b;
  }
  return s;
}

// Per-leg breakdown of the spread so the values can be verified by hand. For each leg shows
// side/strike/bid/ask and its signed contribution to BCmp and SCmp (ratio-weighted, paise):
//   BUY leg  -> BCmp: -ask*ratio , SCmp: -bid*ratio   (you pay, so negative)
//   SELL leg -> BCmp: +bid*ratio , SCmp: +ask*ratio   (you receive, so positive)
// BCmp = Sum(sell bid) - Sum(buy ask) ; SCmp = Sum(sell ask) - Sum(buy bid).
std::string BoxSpreadStrategy::spreadDetail() const
{
  char b[200];
  std::string s;
  if (canCanonical_)
  {
    const Leg &cK1 = legs_[iCallK1_], &pK1 = legs_[iPutK1_], &cK2 = legs_[iCallK2_], &pK2 = legs_[iPutK2_];
    const int64_t gap = static_cast<int64_t>(K2_ - K1_) * 100 * boxRatio_;
    int64_t netDebit  = (int64_t)cK1.ask[0]*cK1.ratio - (int64_t)pK1.bid[0]*pK1.ratio
                      - (int64_t)cK2.bid[0]*cK2.ratio + (int64_t)pK2.ask[0]*pK2.ratio;
    int64_t netCredit = (int64_t)cK1.bid[0]*cK1.ratio - (int64_t)pK1.ask[0]*pK1.ratio
                      - (int64_t)cK2.ask[0]*cK2.ratio + (int64_t)pK2.bid[0]*pK2.ratio;
    std::snprintf(b, sizeof(b), " CE@K%d(%d) bid=%d ask=%d | PE@K%d(%d) bid=%d ask=%d",
                  K1_, cK1.token, cK1.bid[0], cK1.ask[0], K1_, pK1.token, pK1.bid[0], pK1.ask[0]); s += b;
    std::snprintf(b, sizeof(b), " | CE@K%d(%d) bid=%d ask=%d | PE@K%d(%d) bid=%d ask=%d",
                  K2_, cK2.token, cK2.bid[0], cK2.ask[0], K2_, pK2.token, pK2.bid[0], pK2.ask[0]); s += b;
    std::snprintf(b, sizeof(b),
                  "  => gap=%lld netDebit=%lld(ask_CEK1-bid_PEK1-bid_CEK2+ask_PEK2) netCredit=%lld"
                  "  BCmp=gap-netDebit=%lld  SCmp=netCredit-gap=%lld",
                  (long long)gap, (long long)netDebit, (long long)netCredit,
                  (long long)(gap - netDebit), (long long)(netCredit - gap)); s += b;
  }
  else
  {
    int64_t bcmp = 0, scmp = 0;
    for (const auto &l : legs_)
    {
      int bc = (l.side > 0) ? -l.ask[0] * l.ratio : +l.bid[0] * l.ratio;
      int sc = (l.side > 0) ? -l.bid[0] * l.ratio : +l.ask[0] * l.ratio;
      bcmp += bc; scmp += sc;
      std::snprintf(b, sizeof(b), " {tok=%d %s K%d bid=%d ask=%d Bc=%+d Sc=%+d}",
                    l.token, (l.side > 0 ? "BUY" : "SELL"), l.strike, l.bid[0], l.ask[0], bc, sc); s += b;
    }
    std::snprintf(b, sizeof(b), "  => (raw) BCmp=%lld SCmp=%lld", (long long)bcmp, (long long)scmp); s += b;
  }
  return s;
}

void BoxSpreadStrategy::placeLeg(Leg &leg, int k, int price, int qty, int side)
{
  if (leg.slices[k].find(leg.token) == leg.slices[k].end())
    leg.slices[k].emplace(OrderMap::value_type(leg.token, {leg.token, side > 0 ? ORDER_SIDE::BUY_SIDE : ORDER_SIDE::SELL_SIDE,
                                                           leg.lotsize, clientid_, algoid_, static_cast<int16_t>(omsid_),
                                                           orderType_, ms_}));
  ms_->update_order(leg.slices[k], leg.token, price, qty);
  auto it = leg.slices[k].find(leg.token);
  if (it != leg.slices[k].end())
  {
    uint32_t uid = it->second.get_uid();
    if (uid) { int li_ = static_cast<int>(&leg - legs_.data()); uidLeg_[uid] = li_; uidSign_[uid] = {li_, side}; }
  }
  entryOrders_++;
  DbgLine(ms_) << "[BOX][TRADE] strat " << strategynumber_ << " ENTRY slice " << (k + 1) << "/" << numSlices_
            << " leg " << leg.token << (leg.isCall ? " C@" : " P@") << leg.strike << " "
            << (side > 0 ? "BUY" : "SELL") << " qty=" << qty << " @ " << price << std::endl;
}

bool BoxSpreadStrategy::pendingLeg(Leg &leg, int k)
{
  auto it = leg.slices[k].find(leg.token);
  return (it != leg.slices[k].end()) && it->second.is_response_pending();
}

// Flatten a leg's signed residual position (entry fills minus any prior squareoff).
void BoxSpreadStrategy::squareOffLeg(Leg &leg)
{
  if (leg.signedPos == 0) return;
  int sqSide = (leg.signedPos > 0) ? -1 : 1; // sell if long, buy if short
  int px = (sqSide > 0) ? leg.ask[0] : leg.bid[0]; // from the leg's cached book (no map)
  if (px <= 0) return;
  if (leg.sqOff.find(leg.token) == leg.sqOff.end())
    leg.sqOff.emplace(OrderMap::value_type(leg.token, {leg.token, sqSide > 0 ? ORDER_SIDE::BUY_SIDE : ORDER_SIDE::SELL_SIDE,
                                                       leg.lotsize, clientid_, algoid_, static_cast<int16_t>(omsid_),
                                                       orderType_, ms_}));
  int32_t qty = std::abs(leg.signedPos);
  ms_->update_order(leg.sqOff, leg.token, px, qty);
  ++spReversals_; // a residual leg is being reversed/flattened (EOD or stop)
  auto oit = leg.sqOff.find(leg.token);
  if (oit != leg.sqOff.end()) { uint32_t uid = oit->second.get_uid(); if (uid) uidSign_[uid] = {static_cast<int>(&leg - legs_.data()), sqSide}; }
  DbgLine(ms_) << "[BOX][TRADE] strat " << strategynumber_ << " SQUAREOFF leg " << leg.token << " "
            << (sqSide > 0 ? "BUY" : "SELL") << " " << qty << " @ " << px << std::endl;
}

// per-leg option transaction cost (paise) on traded value, sign +1 buy / -1 sell.
int64_t BoxSpreadStrategy::txnCost(int64_t valuePaise, int sign) const
{
  int64_t rate = (sign > 0) ? 6000 : 7000; // Rs/crore: option buy / sell
  return valuePaise * rate / 10000000;
}

void BoxSpreadStrategy::run(int32_t event_token, int64_t nowTs)
{
  if (!running_ || !valid_) return;
  if (legIndex(event_token) < 0) return;

  if (!booksReady()) return; // all legs must have a valid cached top-of-book

  // Mark each leg's last touch mid for EOD / teardown mark-to-market of any residual.
  for (auto &l : legs_)
    l.lastMid = (l.bid[0] + l.ask[0]) / 2;

  // ---- Live box spread from the TBT book (paise, ratio-weighted) — production convention.
  // Computed on EVERY tick straight from the cached book — no clock, no broadcast needed.
  // BCmp/SCmp are the box ARBITRAGE EDGE vs the (K2-K1) settlement gap (near 0 for a fair
  // box), using canonical CE/PE-by-strike roles:
  //   net_debit  = ask(CE@K1) - bid(PE@K1) - bid(CE@K2) + ask(PE@K2)   (cost to BUY the box)
  //   net_credit = bid(CE@K1) - ask(PE@K1) - ask(CE@K2) + bid(PE@K2)   (proceeds to SELL it)
  //   BCmp = gap - net_debit   (buy edge)     SCmp = net_credit - gap   (sell edge)
  int64_t bcmp = 0, scmp = 0;
  if (canCanonical_)
  {
    const Leg &cK1 = legs_[iCallK1_], &pK1 = legs_[iPutK1_], &cK2 = legs_[iCallK2_], &pK2 = legs_[iPutK2_];
    const int64_t gap = static_cast<int64_t>(K2_ - K1_) * 100 * boxRatio_; // settlement value (paise)
    int64_t netDebit  = static_cast<int64_t>(cK1.ask[0]) * cK1.ratio - static_cast<int64_t>(pK1.bid[0]) * pK1.ratio
                      - static_cast<int64_t>(cK2.bid[0]) * cK2.ratio + static_cast<int64_t>(pK2.ask[0]) * pK2.ratio;
    int64_t netCredit = static_cast<int64_t>(cK1.bid[0]) * cK1.ratio - static_cast<int64_t>(pK1.ask[0]) * pK1.ratio
                      - static_cast<int64_t>(cK2.ask[0]) * cK2.ratio + static_cast<int64_t>(pK2.bid[0]) * pK2.ratio;
    bcmp = gap - netDebit;   // buy edge
    scmp = netCredit - gap;  // sell edge
    lastNetDebit_ = netDebit;
  }
  else // fallback (option types unresolved): raw spread, sells +, buys - ; no gap subtraction
  {
    for (auto &l : legs_)
    {
      if (l.side > 0) { bcmp -= static_cast<int64_t>(l.ask[0]) * l.ratio; scmp -= static_cast<int64_t>(l.bid[0]) * l.ratio; }
      else            { bcmp += static_cast<int64_t>(l.bid[0]) * l.ratio; scmp += static_cast<int64_t>(l.ask[0]) * l.ratio; }
    }
    lastNetDebit_ = -bcmp;
  }
  lastBCmp_ = bcmp; lastSCmp_ = scmp; // published to the GUI regardless of clock/trading state

  // ---- Cost (GUI) = transaction cost to TRADE the whole box, per-crore on each leg's value.
  // For every leg: value = LTP * qty (qty = TLots * ratio * lotsize; LTP falls back to the
  // touch mid before a leg trades), cost = value * kBoxCostPerCrore / 1e7 (Rs/crore). Summed
  // over all 4 legs. Shown whether or not the box has actually traded.
  {
    int64_t c = 0;
    for (const auto &l : legs_)
    {
      int px = (l.ltp > 0) ? l.ltp : (l.bid[0] + l.ask[0]) / 2;
      int64_t qty = static_cast<int64_t>(totalLots_) * l.ratio * l.lotsize;
      int64_t valuePaise = static_cast<int64_t>(px) * qty;
      c += valuePaise * kBoxCostPerCrore / 10000000;
    }
    lastLtpCost_ = c;
  }

  // ---- Trading + EOD need a clock. Anchor the first tick with a valid clock as the open;
  // EOD = open + fixed offset. Until a clock arrives we still publish the spread above but
  // do not trade. (BOX_EOD_OFFSET_SEC overrides the offset for testing.) ----
  if (!tsAnchored_) { if (nowTs <= 0) return;
    const char *eo = getenv("BOX_EOD_OFFSET_SEC");
    eodTs_ = nowTs + (eo ? static_cast<int64_t>(atoll(eo)) * kOneSecNs : kOpenToEodNs);
    tsAnchored_ = true; anchorTs_ = nowTs;
    DbgLine(ms_) << "[BOX_CLK] strat " << strategynumber_ << " anchored sod=" << (nowTs / 1000000000)
              << " eodSod=" << (eodTs_ / 1000000000) << std::endl; }
  lastNowTs_ = nowTs;
  if (getenv("LOG_SPREAD"))
    DbgLine(ms_) << "[BOX_SPREAD] strat " << strategynumber_ << " BCmp=" << bcmp << " SCmp=" << scmp
              << " B-Pr=" << userBuySpread_ << " S-Pr=" << userSellSpread_ << std::endl;

  // EOD: flatten any residual one-legged exposure ONCE so no naked leg is carried over.
  if (nowTs >= eodTs_)
  {
    if (!eodFlat_)
    {
      bool anyPos = false; for (auto &l : legs_) if (l.signedPos != 0) anyPos = true;
      if (anyPos) for (auto &l : legs_) squareOffLeg(l);
      eodFlat_ = true;
    }
    return;
  }

  if (placedSlices_ >= numSlices_) return;

  const int k = placedSlices_;
  const int sliceLotsNow = std::min(sliceLots_, totalLots_ - k * sliceLots_);
  if (sliceLotsNow <= 0) { placedSlices_ = numSlices_; return; }
  auto qtyOf = [&](const Leg &l) { return sliceLotsNow * l.ratio * l.lotsize; };

  // ---- Model B direction: pick BUY vs SELL from the GUI B-Pr / S-Pr thresholds --------
  // BUY the combo when BCmp >= B-Pr ; SELL (reverse) when SCmp <= S-Pr. A 0 threshold
  // disables that side. activeDir_ is locked once a slice is in progress.
  if (sliceState_ == 0 && activeDir_ == 0)
  {
    // Sells +ve / buys -ve convention: a HIGHER buy-spread is more favourable to BUY, a
    // LOWER sell-spread is more favourable to SELL.
    if      (userBuySpread_  != 0 && bcmp >= userBuySpread_)  activeDir_ = +1; // BUY combo
    else if (userSellSpread_ != 0 && scmp <= userSellSpread_) activeDir_ = -1; // SELL combo (reverse)
    else return; // neither threshold met -> wait
    lastDir_ = activeDir_; // persist for [BOX_TRADE] (activeDir_ resets after the last slice)
    DbgLine(ms_) << "[BOX] strat " << strategynumber_ << (activeDir_ > 0 ? " BUY" : " SELL")
              << " triggered: BCmp=" << bcmp << " SCmp=" << scmp
              << " B-Pr=" << userBuySpread_ << " S-Pr=" << userSellSpread_ << std::endl;
  }
  // Effective per-leg order side: as configured for BUY-combo, reversed for SELL-combo.
  auto es = [&](const Leg &l) { return activeDir_ >= 0 ? l.side : -l.side; };

  // ===== MODE 1: AGGRESSIVE — place ALL legs at the order-depth touch =====
  if (mode_ == AGGRESSIVE)
  {
    if (k > 0) for (auto &l : legs_) if (pendingLeg(l, k - 1)) return; // gate on prev slice ack
    const int lvl = orderDepth_ - 1;
    for (auto &l : legs_)
      placeLeg(l, k, legPrice(l, es(l), lvl), qtyOf(l), es(l));
    placedSlices_++;
    DbgLine(ms_) << "[BOX] strat " << strategynumber_ << " AGGR SLICE " << placedSlices_ << "/" << numSlices_
              << " (" << sliceLotsNow << " lot) " << (activeDir_ > 0 ? "BUY-combo" : "SELL-combo")
              << " BCmp=" << bcmp << " SCmp=" << scmp << std::endl;
    if (placedSlices_ >= numSlices_) activeDir_ = 0;
    return;
  }

  // Partition legs into bidding (passive) vs hedge (aggressive). Mode 4: all bid.
  auto sliceFilled = [&](const Leg &l) { return l.pos - l.sliceBase; };
  auto armSlice = [&]() { for (auto &l : legs_) l.sliceBase = l.pos; sliceEscalated_ = false; };

  // ===== MODE 2: BIDDING — post bidding leg(s) improved; hedge the rest on fill =====
  if (mode_ == BIDDING)
  {
    int nbid = 0; for (auto &l : legs_) if (l.isbidding) ++nbid;
    if (sliceState_ == 0)
    {
      armSlice();
      for (auto &l : legs_) if (l.isbidding)
        placeLeg(l, k, biddingPrice(l, es(l)), qtyOf(l), es(l));
      sliceState_ = 1; sliceStartTs_ = nowTs;
      DbgLine(ms_) << "[BOX] strat " << strategynumber_ << " MODE2 " << (activeDir_ > 0 ? "BUY-combo" : "SELL-combo")
                << " posted " << nbid << " bidding leg(s) improved " << biddingDepth_ << "p" << std::endl;
      return;
    }
    if (sliceState_ == 1) // bidding posted: hedge once filled; else chase / revert after TimeToRevertBidMs
    {
      int minBid = INT32_MAX; for (auto &l : legs_) if (l.isbidding) minBid = std::min(minBid, sliceFilled(l));
      if (minBid > 0)
      {
        for (auto &l : legs_) if (!l.isbidding)
          placeLeg(l, k, legPrice(l, es(l), orderDepth_ - 1), qtyOf(l), es(l));
        sliceState_ = 2;
        DbgLine(ms_) << "[BOX] strat " << strategynumber_ << " MODE2 bidding filled -> hedged aggressively" << std::endl;
      }
      else
      {
        // GUI TimeToRevertBidMs: if the bidding leg has not filled within revertBidNs_,
        // cross it to the touch; otherwise keep improving toward the touch.
        const bool revert = revertBidNs_ > 0 && (nowTs - sliceStartTs_ >= revertBidNs_);
        for (auto &l : legs_) if (l.isbidding && !pendingLeg(l, k))
          ms_->update_order(l.slices[k], l.token,
                            revert ? legPrice(l, es(l), orderDepth_ - 1)
                                   : biddingPrice(l, es(l)),
                            qtyOf(l));
      }
      return;
    }
    if (sliceState_ == 2) // wait for hedge legs to match the bidding fill -> slice done
    {
      int minBid = INT32_MAX; for (auto &l : legs_) if (l.isbidding) minBid = std::min(minBid, sliceFilled(l));
      bool hedged = true; for (auto &l : legs_) if (!l.isbidding && sliceFilled(l) < minBid) hedged = false;
      if (minBid > 0 && hedged)
      {
        ++spSliceAggr_;
        placedSlices_++; sliceState_ = 0;
        if (placedSlices_ >= numSlices_) activeDir_ = 0;
        DbgLine(ms_) << "[BOX] strat " << strategynumber_ << " MODE2 SLICE " << placedSlices_ << "/" << numSlices_ << " complete" << std::endl;
      }
      else // retry any unfilled hedge legs at the touch until the slice is complete
        for (auto &l : legs_) if (!l.isbidding && sliceFilled(l) < minBid && !pendingLeg(l, k))
          ms_->update_order(l.slices[k], l.token, legPrice(l, es(l), orderDepth_ - 1), qtyOf(l));
      return;
    }
    return;
  }

  // ===== MODE 4: ALL-LEG BIDDING — post improved bids on ALL legs; complete aggressively
  // after the GUI revert timeout (TimeToRevertBidMs) so no leg is left one-legged. =====
  if (mode_ == ALLLEG_BIDDING)
  {
    if (sliceState_ == 0)
    {
      armSlice();
      for (auto &l : legs_)
        placeLeg(l, k, biddingPrice(l, es(l)), qtyOf(l), es(l));
      sliceState_ = 1; sliceStartTs_ = nowTs;
      DbgLine(ms_) << "[BOX] strat " << strategynumber_ << " MODE4 " << (activeDir_ > 0 ? "BUY-combo" : "SELL-combo")
                << " posted all legs improved " << biddingDepth_ << "p" << std::endl;
      return;
    }
    if (sliceState_ == 1)
    {
      bool allFilled = true; for (auto &l : legs_) if (sliceFilled(l) < qtyOf(l)) allFilled = false;
      if (allFilled)
      {
        if (sliceEscalated_) ++spSliceAggr_; else ++spSliceClean_;
        placedSlices_++; sliceState_ = 0;
        if (placedSlices_ >= numSlices_) activeDir_ = 0;
        DbgLine(ms_) << "[BOX] strat " << strategynumber_ << " MODE4 SLICE " << placedSlices_ << "/" << numSlices_
                  << " all legs executed (" << (sliceEscalated_ ? "aggr-completed" : "all-bid") << ")" << std::endl;
        return;
      }
      // Once any leg fills OR the revert timeout elapses, escalate unfilled legs aggressively.
      bool anyFill = false; for (auto &l : legs_) if (sliceFilled(l) > 0) anyFill = true;
      const bool revert = revertBidNs_ > 0 && (nowTs - sliceStartTs_ >= revertBidNs_);
      if (anyFill || revert)
      {
        if (!sliceEscalated_)
        {
          sliceEscalated_ = true;
          DbgLine(ms_) << "[BOX] strat " << strategynumber_ << " MODE4 -> AGGRESSIVE complete (filled "
                    << anyFill << ", " << (nowTs - sliceStartTs_) / 1000000 << "ms)" << std::endl;
        }
        for (auto &l : legs_) if (sliceFilled(l) < qtyOf(l) && !pendingLeg(l, k))
          ms_->update_order(l.slices[k], l.token, legPrice(l, es(l), orderDepth_ - 1), qtyOf(l));
      }
      else // still fully unfilled and fresh: chase the improved touch
        for (auto &l : legs_) if (!pendingLeg(l, k))
          ms_->update_order(l.slices[k], l.token, biddingPrice(l, es(l)), qtyOf(l));
      return;
    }
    return;
  }
}
