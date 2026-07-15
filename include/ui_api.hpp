/**
 * @file ui_api.hpp
 * @brief Message contracts exchanged between strategy and UI router.
 */
#pragma once
#include <stdint.h>
#include <chrono>
namespace aef
{
	namespace infra
	{
		namespace ui_cmd
		{


             /** @brief Control and data codes for UI/strategy communication. */
             enum class UI_TRANS_CODES : int32_t { UI_CONFIG_MSG = 9611,RMS_CONFIG_MSG=9612 ,
                 SEND_UI_PACKET = 9613, LOAD_STRATEGY = 9614, PARAM_MSG = 9619,
                 MOD_STRATEGY=9613,EDIT_STRATEGY=9613,STOP_STRATEGY=9615,
                  START_STRATEGY=9614,ADD_STRATEGY=9612,DELETE_STRATEGY=9616,
                   MANUL_ORDER=9911,  GET_INTERFACE_ID = 8888, ADD_FROM_STRATEGY=7612,EVENT_FROM_STRATEGY=7615,
                   UI_DISCONNECT_EVENT = 7777, UI_CONNECT_EVENT = 6666, UPDATE_FROM_STRATEGY = 8100};

             typedef enum UI_MESS_CODES { SEND_UI_PACKET = 9611, LOAD_STRATEGY = 9612,
                         STOP_STRATEGY = 9613 , START_STRATEGY = 9614 } UI_MESS_CODES;


            
			/*!
			   Use this structure to send UI requests to Strategy.
			*/
			/** @brief Header for any UI message routed through the interface. */
			struct __attribute__((packed)) UIHeader
			{
				int32_t message_code;	/*!< Unique Message Code  */
				int32_t component_id;	/*!< Unique id of the component, to which message will be sent */
				int32_t message_length; /*!< Length of the message */
				int32_t timestamp;		/*!< Time Stamp */
				int32_t interface_id;	/*!< Id of the server */
			};

			/** @brief Wrapper combining the UI header and payload bytes. */
			struct __attribute__((packed)) UIStruct
			{
				UIHeader header;	/*!<UIHeader */
				char message[1500]; /*!<Message to be sent */
			};
       
			/** @brief Lightweight top-of-book update used for UI display. */
			struct __attribute__((packed)) MarketDataUpdate
			{
				UIHeader header;
				int32_t token;
				int32_t bid;
				int32_t bidqty;
				int32_t ask;
				int32_t askqty;
				int32_t LTP;
				int32_t LTQ;
			};
			/** @brief Batch token subscription update pushed to UI. */
			struct __attribute__((packed)) TokenUpdate
			{
				UIHeader header;
				int noofToken;
				int32_t token[50];
			};
			/** @brief Strategy parameters UI payload (sample layout). */
			struct __attribute__((packed)) StrategyParametrs
			{
				int32_t token1 = 53735;
				int32_t token2 = 82222;
				int32_t token3 = 53153;
				int32_t L1Ratio;
				int32_t L2Ratio;
				int32_t L3Ratio;
				int32_t side1;
				int32_t side2;
				int32_t side3;
				int32_t NLots;
				int32_t SLots;
				int32_t TLots;
				int32_t BuyTarget;
				int32_t SellTarget;
				int32_t Strategynumber = 0;
				int32_t StrategytypeN;
				char Strategytype[10];
				char Username[10];
				int32_t start_action;
			};
			/** @brief Expanded parameter set passed from GUI to engine. */
			struct __attribute__((packed)) StrategyParamfromGui
			{
				int Id = 0;
				int Token1 = 0;
				int Token2 = 0;
				char Status[20];
				int RatioNsetoSgx = 0;
				int TotalTradingLots = 0;
				double BuySpread = 0;
				double SellSpread = 0;
				double MKTBuySpread = 0;
				double MKTSellSpread = 0;
				double TradedSpread = 0;
				double Pnl = 0;
				int NiftyQty = 0;
				int SgxQty = 0;
				int NiftyQty3 = 0;
				int NiftyQty4 = 0;
				int TC = 0;
				int USDINR = 0;
				int BuyQtyNSE = 0;
				int SellQtyNSE = 0;
				int BuyQtySGX = 0;
				int SellQtySGX = 0;
				int ATPBuyNse = 0;
				int ATPSellNse = 0;
				int ATPBuySGX = 0;
				int ATPSellSGX = 0;
				int Token3 = 0;
				int Token4 = 0;
				int StrategyType = 0;
				int BuyQty3 = 0;
				int SellQty3 = 0;
				int BuyQty4 = 0;
				int SellQty4 = 0;
				int ATPBuy3 = 0;
				int ATPSell3 = 0;
				int ATPBuy4 = 0;
				int ATPSell4 = 0;
				int clientid = 0;
				int strategyNum = 0;
				int transactionCode = 0;
				int errorCode = 0;
				int stratStopped = 0;
				int tradeCycles = 0;
				int reversedTrades = 0;
				int revPnl = 0;
				int bookedMtm = 0;
				float cost = 0;
				float costLive = 0;
				int tlots = 0;
				bool stoploss = false;
				int slHit = 0;
				int netQtToken1 = 0;
				int netQtToken2 = 0;
				int netQtToken3 = 0;
				int netQtToken4 = 0;
				int buyValue1 = 0;
				int buyValue2 = 0;
				int buyValue3 = 0;
				int buyValue4 = 0;
				int sellValue1 = 0;
				int sellValue2 = 0;
				int sellValue3 = 0;
				int sellValue4 = 0;
				int buyAvg1 = 0;
				int buyAvg2 = 0;
				int buyAvg3 = 0;
				int buyAvg4 = 0;
				int sellAvg1 = 0;
				int sellAvg2 = 0;
				int sellAvg3 = 0;
				int sellAvg4 = 0;
				int buyQty1 = 0;
				int buyQty2 = 0;
				int buyQty3 = 0;
				int buyQty4 = 0;
				int sellQty1 = 0;
				int sellQty2 = 0;
				int sellQty3 = 0;
				int sellQty4 = 0;
				bool bsTrade; // true for buy else sell
				int execBuySp = 0;
				int execSellSp = 0;
				double pnlToken1 = 0;
				double pnlToken2 = 0;
				double pnlToken3 = 0;
				double pnlToken4 = 0;
				int tradeBuySpread = 0;
				int tradeSellSpread = 0;
				float buySlippage = 0;
				float sellSlippage = 0;
			};
			/** @brief Initialization bundle for strategy startup. */
			struct __attribute__((packed)) StrategyInitParam
			{
				int client;
				int omsid;
				int UAT_omsid;
				int algoid;
				int Qty;
				int omsidSGX;
				int32_t time_reversal;
			};
			/** @brief Spread book snapshot for multi-leg strategies. */
			struct __attribute__((packed)) Spreadbook
			{
				int token1;
				int token2;
				int token3;
				int price1;
				int price2;
				int price3;
				int qty1;
				int qty2;
				int qty3;
				int liveparity;
				int UserSpread;
				int side;
				int Strategytype;
				int StrikeofCallPut;
			};
			/** @brief Payload to request or configure market-watch items. */
			struct __attribute__((packed)) mktWatchParametrs
			{
				int32_t token1 = 53735;
				int32_t subscribe_token_flag = 82222;
				int32_t token3 = 53153;
				int32_t L1Ratio;
				int32_t L2Ratio;
				int32_t L3Ratio;
				int32_t side1;
				int32_t side2;
				int32_t side3;
				int32_t NLots;
				int32_t SLots;
				int32_t TLots;
				int32_t BuyTarget;
				int32_t SellTarget;
				int32_t Strategynumber = 0;
				int32_t StrategytypeN;
				char Strategytype[10];
				char Username[10];
				int32_t start_action;
			};

			/** @brief Strategy runtime snapshot shared back to UI. */
			struct __attribute__((packed)) StrategyData
			{
				//    UIHeader header;/*!<UIHeader */
				int client_id;
				int strategy_id;
				int strategy_type;
				int logicType;
				int strategymode;
				int64_t token1;
				int64_t token2;
				int64_t token3;
				int64_t token4;
				// spot tokens for the tokens for NORM
				int64_t token1_Spot;
				int64_t token2_Spot;
				int64_t token3_Spot;
				int64_t token4_Spot;
				int64_t buylive;
				int64_t selllive;
				double buyspread;
				double sellspread;
				int64_t buyStopSpread;
				int64_t sellStopSpread;
				char status[20];
				int tlots;
				int nlots;
				int Tradedlots;
				int64_t buyavg;
				int64_t sellavg;
				int64_t buytargetavg;
				int64_t selltargetavg;
				int64_t avgdisparity;
				int64_t Pnl;
				int token1qty;
				int token2qty;
				int token3qty;
				int token4qty;
				int tc;
				int usdinr;
				char stategytype[50];
				short token1_status;
				short token2_status;
				short token3_status;
				short token4_status;
				// data to store trade details of stratefy
				int32_t transaction_code;
				int token1_ratio;
				int token2_ratio;
				int token3_ratio;
				int token4_ratio;
				int token1_exch;
				int token2_exch;
				int token3_exch;
				int token4_exch;
				int sLBuy;
				int sLSell;
				int tBuy;
				int tSell;
				int spreadType;
				int bidThresh;
				int token1_lotSize;
				int token2_lotSize;
				int token3_lotSize;
				int token4_lotSize;
				char token1_symbol[15] = {'\0'};
				char token2_symbol[15] = {'\0'};
				char token3_symbol[15] = {'\0'};
				char token4_symbol[15] = {'\0'};
				int token1_strike;
				int token2_strike;
				int token3_strike;
				int token4_strike;
				int token1_expiry;
				int token2_expiry;
				int token3_expiry;
				int token4_expiry;
				int token1_cash;
				int token2_cash;
				int token3_cash;
				int token4_cash;
				int avgTime;
				int factorSp1;
				int factorSp2;
				float day1;
				float day2;
			};
			enum BuySell :int
			{
				Buy = 1,
				Sell = -1
			};
			/** @brief Token-level overrides originating from UI. */
			struct __attribute__((packed))  TokenDatafromui   //63 byte
			{
				int token;
				BuySell b_s;
				int ratio;
				int16_t lotsize;
				int32_t strikePrice;
				int undrlineToken;
				bool isbiddingleg;   
				int SL;
				int noofentry;
				int trailingslpercent;
				std::chrono::high_resolution_clock::time_point starttime;
				std::chrono::high_resolution_clock::time_point endtime;
			};
			/** @brief UI request payload configuring a strategy instance. */
			struct __attribute__((packed))  StrategyDatafromui //98 byte
			{
				int clientid;
				int algoid;
				int omsid;
				int strategynumber;
				int strategytype;
				int strategystate;
				int userbuyspread;
				int usersellspread;
				int buySL;
				int SellSL;
				int buyStoporder;
				int SellStoporder;
				int buyNlots;
				int sellNlots;
				int TLots;
				int biddingdepth;
				int orderdepth;
				int thrsoldqty;
				int allowedslippage;
				int normal_bstbid;
				int limit_mktorder;
				int leavasis;
				int revertlegs;
				int timetorevertinmilis;
				int actionforunhedgeqty_nonunhedge_ratiounhedge;
				int buystepcycles;int sellstepcycles;
				int BuyStep = 0;
				int SellStep = 0;
				int Steplotsquaroff = 0;
				bool stepsenable = false, flagStepsquaroff = false;	
				int64_t margine;	

			}; 


		} // namespace ui_cmd
	} // namespace infra
} // namespace aef
