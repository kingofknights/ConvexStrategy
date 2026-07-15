/**
 * @file AlgoBase.hpp
 * @brief Base class interface for all strategy implementations.
 */
#pragma once
#include <memory>
#include <stdint.h>
#include <inttypes.h>
#include <iostream>
#include <string>

using Quote = aef::infra::quote::Quote;
using ProductDetails = aef::infra::product::product_data;
using RMSClientInfo = aef::oms::rms::client_info;

using namespace std::string_literals;
//!  AlgoBase class.
/*!
  All Strategies should be derived from AlgoBase.
  This class contains member functions for the following :
    1. Subscribe to TBT / Snapshot events of  specific security.
    2. Get Security / Contract Info of token.
    3. Send and Receive orders to OMS.
    4. UID generation & Strategy Identification.
    5. Access to the multithreaded  platform logger.
    6. Send Notifications and Strategy State to GUI via Router.
    7. Interface definitions for Event handling functions / callbacks
*/
class AlgoBase {
  public:
    //! Public Variable
   /*!
O    Platform Context Handle
   */
    using ContextHandle = void*;

  public:
   //! Constructor
   /*!
    Constructor with Context Handle  
   */
    AlgoBase(ContextHandle context_);
   //!Virtual Destructor
   /*!
    Virtual Destructor
   */
    virtual ~AlgoBase();
   //! Member Function
   /*!
      Member Function init with one argument as const char*
      \param log_file_name - File Name To Be Created as Log File.
      \return Success-true, Failure-false
   */
   /*!
      This function is used for initialization ..like creating log file etc..
   */
    bool init(const char* log_file_name);
   //! Member Function
   /*!
      Member Function subscribeProduct with two arguments
      \param product_id - Token to be subscribed
      \param flags      - Type of Broadcast required Ex.-TBT_UPDATE_EVENT,MBP_UPDATE_EVENT
      \return Success-true, Failure-false
   */
   /*!
      This function subscribes for the broadcast for the token.
      User has to call this function for each and every contract/security for which broadcast packet is to be received.
   */
    bool subscribeProduct(const int32_t product_id, const uint16_t flags);
   //! Member Function
   /*!
      Member Function unSubscribeProduct with two arguments
      \param product_id - Token to be unsubscribed
      \param flags      - Type of Broadcast Ex.-TBT_UPDATE_EVENT,MBP_UPDATE_EVENT
      \return Success-true, Failure-false
*/
   /*!
      This function unsubscribes for the broadcast for the token.
User has to call this function for each and every contract/security for which unsubscription is to be sent, in order to not receive broadcast henceforth.
   */
    bool unSubscribeProduct(const int32_t product_id, const uint16_t flags);
   //! Member Function
   /*!
      Member Function getLastQuote with two arguments
      \param product_id      - Token for which Quote(Broadcast Data) is required
      \param quote           - This parameter is Reference type in which Broadcast data -TBT/Snapshot is received.
      \return Success-true, Failure-false
*/
   /*!
      This function gets the last/latest quote for the token.
      From quote structure, user can get latest broadcast data like, top 5 bids and asks data, last traded qty, last traded price, volume etc..
      For more information refer the Quote Structure.
   */
    bool getLastQuote(const int32_t product_id, Quote& value_);
   //! Member Function
   /*!
      Member Function getProductDetails with two arguments
      \param product_id          - Token for which Product Details required
      \param productDetails      - This parameter is Reference type in which Product info is received
      \return Success-true, Failure-false
*/
   /*!
      This function gets the product details for the token like lotsize, tick size etc..
      For more information refer the structure ProductDetails.
   */
    bool getProductDetails(const int32_t product_id, ProductDetails& value_);
    //! Member Function
   /*!
      Member Function getClientInfo with two arguments
      \param client_id           - Mapped Id To Client Name
      \param cli_details         - This is Reference parameter of type RMSClientInfo
      \return Success-true, Failure-false
*/
   /*!
      This function gets the client details for the client_id like client id, name , PAN, Settlor etc...
      For more information refer the structure RMSClientInfo
   */
    bool getClientInfo(const int32_t client_id, RMSClientInfo& cli_details);
   //! Member Function
   /*!
      Member Function get_strategy_id with no argument
      \return Strategy Id
   */
   /*!
      This function gets the strategy id mentioned as in configuration file.
      Basically the Algo exe loads the execution ".so" developed by user along with a     configuration file. In this file, user has to mention unique strategy id for each and every strategy instance.. 
   
      
   */
    int32_t get_strategy_id();
    std::string get_strategy_config_file();
   //! Member Function
   /*!
      Member Function get_next_order_id with no argument
      \return Order Id
   */
   /*!
      This function gets the next order id.
      Basically an order id is unique for evrey new order. 
      For modification and cancellation this order id generated for new order is sent.
   */
    uint32_t get_next_order_id();
   //! Member Function
   /*!
      Member Function reset_order_id with no argument
    */
   /*!
      This function resets order id..
   */
    void reset_order_id();
   //! A member function taking one argument- 
   /*!
      \sa log_info
      \sa log_error
      \param log_string Error String To Be Logged While node_level is mentioned as Debug.
    */
    void log_debug(const char* log_string);
    //! A member function taking one argument- 
    /*!
      \sa log_debug
      \sa log_error
      \param log_string Error String To Be Logged While node_level is mentioned as Info-       Default.
    */
    void log_info(const char* log_string);
    //! A member function taking one argument- 
   /*!
      \sa log_debug
      \sa log_info
      \param log_string Error String To Be Logged.
    */
        
    void log_error(const char* log_string);
    //! Member Function.
    /*!

      \param oms_transaction Order Request Structure .
      \return Success-true, Failure-false
     */
    /*!
      This function sends the Order Request Packet to OMS 
      oms_transaction structure has fields like transaction code, client id, algo id, product id, price ,qty etc..
      Mandatory fields need to be filled in , in order to successfully submit the request...
      For more information refer the structure  oms_transaction
       
    */
    bool send_order(oms_transaction& order_req);
    //! Member Function.
    /*!
      
      \param UIStruct UI Response Structure .
      \return Success-true, Failure-false
    */
    /*!
      This function sends the UI Response Packet to UI. 
      Through this function user can send UI data like pnl, risk factors to their respective UI component via router.

    */
    uint64_t get_latest_exchangetime() ;
    bool sentoUI(const aef::infra::ui_cmd::UIStruct& ui_resp);
    //! Member Function.
    /*!
       \param void* 
    */
    /*! .
      This function resets the context.

    */
    void reset_context(ContextHandle context_);
   
    //! Member Function
   /*!
      Member Function getProductDetails with two arguments
      \param product_name          - Token for which Product Details required
      \param productDetails      - This parameter is Reference type in which Product info is received
      \return Success-true, Failure-false
*/
   /*!
      This function gets the product details for the token like lotsize, tick size etc..
      For more information refer the structure ProductDetails.
   */
    bool getProductDetails(const std::string product_name, ProductDetails& value_);
    bool getProductDetailsbyweeklymonthly(const std::string product_name,const std::string expiry,const std::string strike,const std::string opttype, ProductDetails& value_);
  public:
    //! A member function taking one argument- 
    /*!
      \sa LOG_INFO
      \sa LOG_ERROR
      \param const char* fm Format of the string 
      \param Args&&... args  Arguments to display in log
    */
    /*!
      This function internally calls log_debug to generate the log in debug configuration.
    */
    template <class... Args>
    void LOG_DEBUG(const char* fmt, Args&&... args)
    {
        char buffer[256] = {0};
        snprintf(buffer, 256, fmt, std::forward<Args>(args)...);
        log_debug(buffer);
    }
    //! A member function taking one argument- 
    /*!
      \sa LOG_DEBUG
      \sa LOG_ERROR
      \param const char* fm Format of the string 
      \param Args&&... args  Arguments to display in log
    */
    /*!
      This function internally calls log_info to generate the log in INFO-Default configuration.
    */
    template <class... Args>
    void LOG_INFO(const char* fmt, Args&&... args)
    {
        char buffer[256] = {0};
        snprintf(buffer, 256, fmt, std::forward<Args>(args)...);
        log_info(buffer);
    }
    /*!
      \sa LOG_INFO
      \sa LOG_ERROR
      \param const char* fm Format of the string 
      \param Args&&... args  Arguments to display in log
    */
    /*!
      This function internally calls log_error to generate the log in Error configuration.
    */
    template <class... Args>
    void LOG_ERROR(const char* fmt, Args&&... args)
    {
        char buffer[256] = {0};
        snprintf(buffer, 256, fmt, std::forward<Args>(args)...);
        log_error(buffer);
    }

  public:
    //! A virtual member function 

    /*!
      \param event - This is Reference parameter of type Quote which contains market TBT/Snapshot  data.
    */
    /*! 
    This needs to be overridden to get the TBT/Snapshot quote.
    User will receive broadcast data for the subscribed tokens in this function. 
    For More information kindly refer the Quote structure.
    */
    virtual void OnTick(const Quote& event);
 //! A virtual member function

    /*!
      
      \param oms_transaction - Order Packet structure which contains order response from OMS.
    */
     /*! 
    This needs to be overridden to get all the responses for order packets..
    For each and every order request , a response is received from OMS. 
    This response can be a successful or failure .
    For all the responses kindly refer enum OMS_API_TRANS_CODES
    For all the error codes  kindly refer enum OMS_API_ERROR_CODES
    */
    virtual void OnOrderResponse(const oms_transaction& order_resp);
//! A virtual member function  

    /*!
      \param stream_id   - Stream Id of TBT data.
      \param segment_id  - Segment Id of TBT data.
      \param feed_status - Feed Status of TBT data.
    */
    /*!
     This function is called whenever there is status change in any stream of TBT.
     Foe Example, when the stream goes into recovery mode, or comes back to normal mode.
     Following are the values of feed_status.
     STREAM_IDLE = 0,
    STREAM_NORMAL = 1,
    STREAM_RECOVERY_REQ = 2,
     */
    virtual void onStreamStatusChange(const int16_t stream_id, const int16_t segment_id, const int32_t feed_status);
//! A virtual member function 
  /*!
      \param product_id   - product_id of Snap Shot Data.
      \param aef::infra::mbp_book::st_mbp_info- Reference Parameter Bid Level  - Bid level of Snap Shot Data.Qty,Price and No. Of Orders of Bid upto 5 levels
      \param aef::infra::mbp_book::st_mbp_info- Reference Parameter ask Level -  Ask level of Snap Shot Data.Qty,Price and No. Of Orders of Ask upto 5 levels
    */
    /*!
      Override this function to handle SnapShot data.
      SnapShot of broadcast data for any token is sent by exchange after a predefined time interval.
     * 
    */
    virtual void onBcastData(const aef::infra::product::product_data& product_details_);
//! A virtual member function     
  /*!
      
      \param product_id   -  product_id .
      \param high_level   -  High level of TER.
      \param low_levell   -  Low level of TER.
    */
/*!
    Override this function to get the TER Range update for product.                              
*/
    virtual void onTERData(const int32_t product_id, const int32_t high_level,  const int32_t low_level);
//! A virtual member function                  
  /*!
      \param aef::infra::product::product_data   - Reference parameter which holds product update
    */
/*!
    Override this function to get the Security update for product.                              
*/
    virtual void handleSecurityUpdate(const aef::infra::product::product_data& product_details_);
//! A virtual member function           
  /*!
  
      \param const aef::infra::ui_cmd::UIStruct   - UI Request.
    */
/*!
   Override this function to get UI parameters like config parameters update etc.
*/
    bool TimerEvent(const aef::infra::ui_cmd::UIStruct& req_timer,uint64_t timeinnanos);
    virtual void onUIRequest(const aef::infra::ui_cmd::UIStruct& ui_req);
    virtual void handleTimerCalculations(const aef::infra::ui_cmd::UIStruct& ui_req);
//! A virtual member function 
  /*!
   Override this function to  do work while strategy is in idle state.
   This can be any logic specific to strategy, like computing risk parameters after every second etc..
  */
    virtual int doWork();
	//! A virtual member function                  
  /*!
      \param aef::infra::product::product_data   - Reference parameter which holds product update
    */
/*!
    Override this function to get the OI update for product.                              
*/
    virtual void onOIdata(const aef::infra::product::product_data& product_details_);

  private:
    int32_t log_handle{-1};
    ContextHandle myContext_;
};

typedef AlgoBase* create_t(void*);
typedef void destroy_t(AlgoBase*);
