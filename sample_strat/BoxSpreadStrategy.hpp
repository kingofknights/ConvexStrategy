#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>

#include "order_instance.hpp" // execution_strat::order_instance, ORDER_SIDE, ORDER_TYPE
#include "ui_api.hpp"         // aef::infra::ui_cmd::StrategyDatafromui / TokenDatafromui / BuySell
#include "oms_api.hpp"        // oms_transaction

class MinixStrategy;

// ---------------------------------------------------------------------------
// 4-leg box strategy (NSE index options), Model B: spread-driven buy/sell.
// MinixStrategy creates one instance per `add` (keyed by strategynumber) from
// onUIRequest, and forwards OnTick / onBcastData / OnOrderResponse to every live
// instance, so many boxes scan/execute concurrently.
//
// LEGS, SIDES, STRIKES, RATIO ALL COME FROM THE GUI (4 tokens). The direction is
// decided every tick from the live market spreads vs the GUI B-Pr / S-Pr thresholds
// (all in paise; option premiums are price*100). Frontend convention: SELL legs
// are +ve and BUY legs are -ve; BUY uses ASK, SELL uses BID:
//   BCmp (market BUY spread)  = Sum(sell-leg BID) - Sum(buy-leg ASK)
//   SCmp (market SELL spread) = Sum(sell-leg ASK) - Sum(buy-leg BID)
//   BUY the combo (legs as configured)  when BCmp >= userBuySpread_ (B-Pr)
//   SELL the combo (every leg reversed)  when SCmp <= userSellSpread_ (S-Pr)
// A 0 threshold disables that side. BCmp/SCmp are published to the GUI each second.
//
// Prices use executable bid/ask (never mid). Stale/zero/crossed/missing legs skip.
class BoxSpreadStrategy
{
public:
  using OrderMap = std::unordered_map<int32_t, execution_strat::order_instance>;

  // Execution modes (GUI "mode" field):
  //  1 AGGRESSIVE      : on profit>threshold, sweep all 4 legs at the order-depth touch.
  //  2 BIDDING         : post the isbidding legs improved by biddingDepth; once they
  //                      fill, hedge the other legs aggressively (capture half-spread).
  //  4 ALLLEG_BIDDING  : when the box is NEAR profitable, post improved limit bids on
  //                      ALL 4 legs at once; complete any partial fill aggressively
  //                      after a 1s timeout so no naked leg is carried.
  enum Mode { AGGRESSIVE = 1, BIDDING = 2, ALLLEG_BIDDING = 4 };

  BoxSpreadStrategy(MinixStrategy *ms,
                    const aef::infra::ui_cmd::StrategyDatafromui &s,
                    const std::vector<aef::infra::ui_cmd::TokenDatafromui> &tokens,
                    int mode);
  ~BoxSpreadStrategy();

  // Lifecycle (driven by onUIRequest action add|start|stop|edit|delete).
  void start();
  void stop(); // cancel opens + square off any built legs + halt
  void edit(const aef::infra::ui_cmd::StrategyDatafromui &s,
            const std::vector<aef::infra::ui_cmd::TokenDatafromui> &tokens,
            int mode);
  void unsubscribeTokens();

  // nowTs = raw exchange timestamp in nanoseconds. The box derives ALL timing from it
  // (no localtime/seconds-of-day): the first valid tick is taken as the 09:15 open and
  // the 15:29 EOD square-off is open + a fixed offset; the 1s escalation is a raw diff.
  // Cache-friendly hot path: the full Quote / product_data is passed in, the ticked leg's
  // top-5 book is copied into the leg (contiguous), then run() reads the cached per-leg
  // prices — NO map lookups on the tick path.
  void onTick(const Quote &event, int64_t nowTs);
  void onBcast(const aef::infra::product::product_data &pd, int64_t nowTs);
  void onOrderResponse(const oms_transaction &resp);

  int32_t number() const { return strategynumber_; }
  bool valid() const { return valid_; }
  int64_t bcmp() const { return lastBCmp_; } // last market BUY spread (paise) for GUI display
  int64_t scmp() const { return lastSCmp_; } // last market SELL spread (paise) for GUI display
  int64_t gap() const;
  // Cost shown on the GUI: total transaction cost to TRADE the whole box, computed every
  // tick whether or not it has traded. For each leg: value = LTP * qty (qty = TLots*ratio*
  // lotsize), cost = value * (Rs/crore) / 1e7; summed over all 4 legs. Paise.
  int64_t cost() const { return lastLtpCost_; }
  // Executed (traded) spread of the boxes built so far, in the SAME ratio-weighted paise
  // convention as BCmp/SCmp -> directly comparable to the GUI B-Pr / S-Pr thresholds.
  int64_t tradedSpread() const { return lastTradedSpread_; }
  int      boxesTraded() const { return boxesDone_; } // # of fully-built boxes
  int64_t  pnlPaise() const { bool f; return computePnl(f); } // realized cash + residual MtM - cost
  std::string bookStatus() const;   // per-leg bid/ask from the algo book (diagnostic for BCmp=0)
  std::string spreadDetail() const; // per-leg side/strike/bid/ask + each leg's BCmp/SCmp contribution
  // Qty of the spread actually built = smallest per-leg filled qty (the fully-hedged amount).
  int64_t tradedQty() const
  {
    if (legs_.empty()) return 0;
    int64_t m = -1;
    for (const auto &l : legs_)
    {
      int64_t a = l.signedPos < 0 ? -static_cast<int64_t>(l.signedPos) : static_cast<int64_t>(l.signedPos);
      if (m < 0 || a < m) m = a;
    }
    return m < 0 ? 0 : m;
  }

private:
  struct Leg
  {
    int32_t token = 0;
    int side = 1;        // +1 BUY (pay ask), -1 SELL (recv bid)
    int ratio = 1, lotsize = 1;
    int32_t strike = 0;
    int optType = 0;     // real option type resolved from product details: 1=CE(call), 2=PE(put), 0=unknown
    bool isCall = true;  // inferred from (strike==K1?buy:sell) box structure
    bool isbidding = false; // mode 2: this leg posts passively (improved) vs hedges
    // Cached top-5 book for THIS leg (paise), updated in-place each tick from the Quote.
    // Inline arrays -> contiguous within the leg, sequential across legs_; no map / no
    // pointer-chase on the hot path.
    int bid[5] = {0, 0, 0, 0, 0};
    int ask[5] = {0, 0, 0, 0, 0};
    int ltp = 0; // last traded price (paise) for the LTP-based box cost
    std::vector<OrderMap> slices; // one fresh order_instance per slice (cold path, order mgmt)
    OrderMap sqOff;
    int32_t pos = 0, sliceBase = 0; // cumulative entry-side fills / position at slice start
    int32_t signedPos = 0;          // signed net qty (entry + squareoff) for realized PnL
    int lastMid = 0;                // last touch mid (paise) for EOD mark-to-market
  };

  void computeSlicing(const aef::infra::ui_cmd::StrategyDatafromui &s);
  void classifyLegs(); // derive K1/K2, call/put per leg, validate the box shape
  void run(int32_t event_token, int64_t nowTs);
  int legIndex(int32_t token) const; // linear scan over the 4 legs (faster + cache-friendly vs a hash map)
  bool booksReady() const; // true only if all legs have a valid (non-zero, non-crossed) top-of-book
  int legPrice(const Leg &l, int side, int level) const; // executable price from the leg's cached book, never mid
  int biddingPrice(const Leg &l, int side) const;        // touch improved by biddingDepth
  void placeLeg(Leg &leg, int k, int price, int qty, int side);  // create-or-reprice the slice order (side = effective)
  bool pendingLeg(Leg &leg, int k);
  int32_t sliceFilled(const Leg &leg) const { return leg.pos - leg.sliceBase; }
  void squareOffLeg(Leg &leg);

  int64_t txnCost(int64_t valuePaise, int sign) const;   // per-leg option txn cost (paise)
  int64_t computePnl(bool &flat) const;                  // fill-based PnL: cash + residual MtM - cost

  MinixStrategy *ms_;
  int32_t strategynumber_ = 0, clientid_ = 0, algoid_ = 0, omsid_ = 0;

  std::vector<Leg> legs_; // the 4 box legs (contiguous; the only price store on the hot path)
  std::unordered_map<uint32_t, int> uidLeg_;   // entry order uid -> leg index (slice fill gating; cold path)
  std::unordered_map<uint32_t, std::pair<int, int>> uidSign_; // order uid -> (leg idx, side sign) for PnL
  // Resolved box structure (indices into legs_); -1 until classifyLegs validates.
  int iCallK1_ = -1, iPutK1_ = -1, iCallK2_ = -1, iPutK2_ = -1;
  bool canCanonical_ = false;     // all 4 CE/PE-by-strike roles resolved -> production arb-edge spread
  int64_t lastNetDebit_ = 0;      // live net debit to BUY the box from bid/ask (paise)
  int64_t lastLtpCost_ = 0;       // LTP-based net cost to build the box (paise) -> Cost on GUI
  int32_t K1_ = 0, K2_ = 0;       // low / high strike (points)
  int boxRatio_ = 1, lotsize_ = 1; // number of boxes / shares per lot
  bool valid_ = false;             // a well-formed 4-leg box?

  int orderDepth_ = 5;             // aggressor sweep depth (GUI Order Depth)
  ORDER_TYPE orderType_ = ORDER_TYPE::LIMIT_ORDER_TYPE;
  int mode_ = AGGRESSIVE;          // execution mode
  int biddingDepth_ = 25;          // mode 2/4 passive price improvement, paise (GUI Price Depth)
  int sliceState_ = 0;             // bidding state machine: 0 idle, 1 posted, 2 hedging
  int64_t sliceStartTs_ = 0;       // raw exchange ns when the current slice was posted (revert/escalation timer)
  int64_t revertBidNs_ = 1000000000; // GUI TimeToRevertBidMs -> ns; cross the bidding leg to touch after this

  // ---- Model B: spread-driven buy/sell direction (GUI B-Pr / S-Pr thresholds) --------
  // BUY the combo (legs as configured)  when market BUY spread  (BCmp) >= userBuySpread_  (B-Pr)
  // SELL the combo (reverse every leg)  when market SELL spread (SCmp) <= userSellSpread_ (S-Pr)
  // A threshold of 0 disables that side. activeDir_ locks the chosen direction for the slice.
  int64_t userBuySpread_ = 0;   // B-Pr : buy when BCmp >= this
  int64_t userSellSpread_ = 0;  // S-Pr : sell when SCmp <= this
  int activeDir_ = 0;           // 0 none, +1 buy combo, -1 sell combo (locked while a slice runs)
  int lastDir_ = 0;             // last triggered direction (persists for [BOX_TRADE] reporting)
  int64_t lastBCmp_ = 0, lastSCmp_ = 0; // last computed market buy/sell spread (paise)
  // EOD timing from the raw clock: anchor the first valid tick as 09:15 open, then the
  // 15:29 square-off is openTs_ + a fixed offset (no localtime in the box path).
  bool tsAnchored_ = false;
  int64_t eodTs_ = 0;              // nowTs at/after which to flatten residual (open + offset)
  int64_t anchorTs_ = 0, lastNowTs_ = 0; // diagnostics: first/last clock value seen by run()

  // Slicing: total boxes executed in sliceLots-sized child orders.
  int totalLots_ = 1, sliceLots_ = 1, numSlices_ = 1, placedSlices_ = 0;
  bool running_ = false, subscribed_ = false;
  bool eodFlat_ = false;                              // latch: EOD square-off fires once

  // Realized PnL (fill-based): every fill moves cash (buy pays / sell receives) and
  // costs txn; any residual signed position is marked to market at the last touch mid
  // at teardown. Total PnL = cashFlow + residual MtM - txn cost. Paise.
  int64_t cashFlow_ = 0, txnCostPaise_ = 0;
  int entryOrders_ = 0, fills_ = 0; // counters for the [BOX_PNL] summary

  // Executed-spread tracking. Accumulated from ENTRY fills in the BCmp/SCmp convention
  // (a leg's CONFIGURED side: sell +fill, buy -fill), so the result is the actual spread
  // we traded, directly comparable to the GUI B-Pr / S-Pr. Recomputed each completed box.
  int64_t entrySpreadCash_ = 0; // Sum over entry fills of sign_config * price * qty (paise)
  int boxesDone_ = 0;           // # of fully-built boxes = min_leg(entryFills / (ratio*lotsize))
  int64_t lastTradedSpread_ = 0;// per-share ratio-weighted executed spread so far (paise)
  // Execution-quality counters (mode 4): how often the all-4-leg bid did/didn't capture
  // the box passively, and how many residual positions had to be reversed (flattened).
  int spSliceClean_ = 0;     // slices where all 4 legs filled at the bid (no aggression)
  int spSliceAggr_  = 0;     // slices that needed aggressive escalation to complete
  int spReversals_  = 0;     // residual legs force-flattened (EOD / stop reversal)
  bool sliceEscalated_ = false; // did the current slice already cross aggressively?
};
