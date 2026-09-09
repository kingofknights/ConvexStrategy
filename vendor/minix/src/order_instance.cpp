/**
 * @file order_instance.cpp
 * @brief Implementation of order_instance lifecycle helpers.
 */
#include "order_instance.hpp"

namespace execution_strat {
order_instance::order_instance(const int token, ORDER_SIDE side, const int64_t max_qty,int32_t client_id, int32_t algo_id,int16_t omsid,ORDER_TYPE odtype,AlgoBase* context)
  : product_id_{token}
  , side_{side}
  , max_qty_{max_qty}
  , client_id_{1} // force default client/algo/oms ids
  , algo_id_{1}
  , omsid_{1}
  , typeoforder{odtype}
  , context_{context}
{
    (void)client_id;
    (void)algo_id;
    (void)omsid;
    myOrder_.packet_.flags_.order_type= typeoforder;

    myOrder_.packet_.product_id_= product_id_;
    myOrder_.packet_.flags_.order_side= side;
    myOrder_.packet_.algo_id= algo_id_;
    myOrder_.packet_.oms_id= omsid_;
    myOrder_.packet_.client_id= client_id_;

//    myOrder_.packet_.flags_.order_type= ORDER_TYPE::IOC_ORDER_TYPE;
}
bool order_instance::is_response_pending() const
{
    if ((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_PLACED) & current_state_)
        || (static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_MODIFY_PLACED) & current_state_)
        || (static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_CXL_PLACED) & current_state_)) {
        return true;
    }
    return false;
}

bool order_instance::is_order_confirmed() const
{
    if (static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_EXCHG_CONF) & current_state_) {
        return true;
    }
    return false;
}

bool order_instance::is_order_to_be_cancelled() const
{
    return pendingCancel_;
}

uint32_t order_instance::place_order(const int32_t price, const int32_t qty,const uint32_t uidno)
{
    if (uid_ != 0) {
        return 0;
    }

    if(is_response_pending())
    {
        context_->LOG_DEBUG(" cannot place order , response is pending, current_state(%d),request_id(%d), strategy_id(%d), side(%d)",  current_state_,static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.request_id),static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.strategy_id),static_cast<uint32_t>(myOrder_.packet_.flags_.order_side));
        return 0;
    }
        
    if(qty <= 0 || price <=0)
    {
        context_->LOG_INFO("(%s-%s-%d) failed to place order :: uid(%d),response_pending(%d), Qty is not greater than 0", uid_, is_response_pending());
        return false;
        
    }
    uid_ = uidno;//context_->get_next_order_id();
    myOrder_.hdr_.transaction_code = OMS_API_TRANS_CODES::OMS_PLACE_REQ;
    myOrder_.hdr_.uid_.id_ = uid_;
    // myOrder_.packet_.product_id_ = product_id_;
    // myOrder_.packet_.flags_.order_side = side_;
    myOrder_.packet_.price_ = price;
    myOrder_.packet_.quantity_ = qty;
    // myOrder_.packet_.client_id = client_id_;
    // myOrder_.packet_.algo_id = algo_id_;
    myOrder_.hdr_.event_timestamp = quote_event_timestamp;
    myOrder_.hdr_.trigger_timestamp = quote_trigger_timestamp;
    myOrder_.hdr_.exchange_timestamp = quote_received_timestamp;
    myOrder_.packet_.exchange_order_id = aef::infra::get_realtime_in_nanos();
    //myOrder_.packet_.oms_id = omsid_ ;
    
    
    current_state_ |= (static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_ORDER_PLACED));
    // std::cout << "Print from place order : " << current_state_ << " , OMSID : " << omsid_ << " , " << myOrder_.packet_.algo_id << std::endl;

    // std::cout << "just before final send : " << myOrder_.packet_.client_id << " " << myOrder_.packet_.oms_id << " " <<  myOrder_.hdr_.uid_.id_ << " " <<  myOrder_.hdr_.uid_.composite_id_.strategy_id << " " <<  myOrder_.hdr_.uid_.composite_id_.client_id << " " << myOrder_.hdr_.uid_.composite_id_.request_id << " " << myOrder_.packet_.product_id_<< " " << std::endl;
    if(context_->send_order(myOrder_)) 
    {   
        //current_state_ |= (static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_ORDER_PLACED));
        return uid_;
    }
    else 
    {
        current_state_ &= ~(static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_ORDER_PLACED));
    }
    context_->LOG_DEBUG(" failed to place new order, queue is full");
    
    return 0;
}
uint32_t order_instance::place_order(const int32_t price, const int32_t qty)
{
    if (uid_ != 0) {
    return 0;
    }

    if(is_response_pending())
    {
        context_->LOG_DEBUG(" cannot place order , response is pending, current_state(%d),request_id(%d), strategy_id(%d), side(%d)",  current_state_,static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.request_id),static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.strategy_id),static_cast<uint32_t>(myOrder_.packet_.flags_.order_side));
        return 0;
    }
        
    if(qty <= 0 || price <=0)
    {
        context_->LOG_INFO("(%s-%s-%d) failed to place order :: uid(%d),response_pending(%d), Qty is not greater than 0", uid_, is_response_pending());
        return false;
        
    }
    uid_ = context_->get_next_order_id();
    myOrder_.hdr_.transaction_code = OMS_API_TRANS_CODES::OMS_PLACE_REQ;
    // myOrder_.packet_.product_id_ = product_id_;
    // myOrder_.packet_.flags_.order_side = side_;
    myOrder_.packet_.price_ = price;
    myOrder_.packet_.quantity_ = qty;
    // myOrder_.packet_.client_id = client_id_;
    // myOrder_.packet_.algo_id = algo_id_;
    myOrder_.hdr_.event_timestamp = quote_event_timestamp;
    myOrder_.hdr_.trigger_timestamp = quote_trigger_timestamp;
    myOrder_.hdr_.exchange_timestamp = quote_received_timestamp;
    myOrder_.packet_.exchange_order_id = aef::infra::get_realtime_in_nanos();
   // myOrder_.packet_.oms_id = omsid_ ;
    
    current_state_ |= (static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_ORDER_PLACED));
    if(context_->send_order(myOrder_)) 
    {   
        //current_state_ |= (static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_ORDER_PLACED));
        return uid_;
    }
    else 
    {
        current_state_ &= ~(static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_ORDER_PLACED));
    }
    context_->LOG_DEBUG(" failed to place new order, queue is full");
    
    return 0;
}
        
bool order_instance::update_order(const int32_t price, const int32_t qty)
{   
    if(open_price_ == price && open_qty_ == qty)
    {
        context_->LOG_INFO(" failed to modify order :: uid(%d),response_pending(%d), Price and Qty both are unchanged", uid_, is_response_pending());
        return false;

    }

    if(qty <= 0 || price <=0)
    {
        context_->LOG_INFO(" failed to modify order :: uid(%d),response_pending(%d), Qty is not greater than 0", uid_, is_response_pending());
        return false;

    }

    context_->LOG_DEBUG(" request_id(%d), strategy_id(%d),trans code(%d),"
            "open_qty(%d),notional_qty(%d),"
            "current_state(%d), price(%d), qty(%d), filled_qty(%d), side(%d)", static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.request_id),
                static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.strategy_id),
                static_cast<uint32_t>(myOrder_.hdr_.transaction_code),
                ((long long)get_open_qty()) ,
                ((long long)get_notional_qty()) ,
                ((long long)get_current_state()),
                price,
                qty,
                get_filled_qty(),
                static_cast<uint32_t>(myOrder_.packet_.flags_.order_side)) ;



    if((uid_ == 0) || (is_response_pending()) || (!is_order_confirmed()))
    {

        context_->LOG_DEBUG(" failed to modify order :: uid(%d),response_pending(%d), order_confirmed(%d),request_id(%d), strategy_id(%d), Side(%d)", 
                 uid_, is_response_pending(), is_order_confirmed(),static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.request_id),static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.strategy_id),static_cast<uint32_t>(myOrder_.packet_.flags_.order_side));

        return false;
    }

    myOrder_.hdr_.transaction_code = OMS_API_TRANS_CODES::OMS_REPLACE_REQ;

    myOrder_.packet_.price_ = price;
    myOrder_.packet_.quantity_ = qty + get_filled_qty();
    myOrder_.hdr_.event_timestamp = quote_event_timestamp;
    myOrder_.hdr_.trigger_timestamp = quote_trigger_timestamp;
    myOrder_.hdr_.exchange_timestamp = quote_received_timestamp;
    myOrder_.packet_.exchange_order_id = aef::infra::get_realtime_in_nanos();
    //myOrder_.packet_.algo_id = algo_id_;
    context_->LOG_DEBUG(" ts1(%" PRIu64 "), ts2(%" PRIu64 "), ts3(%" PRIu64 "),ts4(%" PRIu64 ")",
            static_cast<uint64_t>(myOrder_.hdr_.event_timestamp),
                static_cast<uint64_t>(myOrder_.hdr_.trigger_timestamp),
                static_cast<uint64_t>(myOrder_.hdr_.exchange_timestamp),
                static_cast<uint64_t>(myOrder_.packet_.exchange_order_id));

    current_state_ |= (static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_MODIFY_PLACED));
    if(context_->send_order(myOrder_))
    {
        //current_state_ |= (static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_MODIFY_PLACED));
        return uid_;
    }else {
    current_state_ &= ~(static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_MODIFY_PLACED));
}

    context_->LOG_DEBUG(" failed to modify order(%d), request_id(%d), strategy_id(%d), side(%d) :: queue is full", 
            uid_,static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.request_id),static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.strategy_id),static_cast<uint32_t>(myOrder_.packet_.flags_.order_side));

    return false;
}

bool order_instance::cancel_order()
{

    if((uid_ == 0) || (is_response_pending()) || (!is_order_confirmed()))
    {

        context_->LOG_DEBUG(" failed to cancel order :: uid(%d),response_pending(%d), order_confirmed(%d), request_id(%d), strategy_id(%d), side(%d)", 
                 uid_, is_response_pending(), is_order_confirmed(),static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.request_id),static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.strategy_id),static_cast<uint32_t>(myOrder_.packet_.flags_.order_side));

        return false;
    }

    myOrder_.hdr_.transaction_code = OMS_API_TRANS_CODES::OMS_CANCEL_REQ;
    myOrder_.hdr_.event_timestamp = quote_event_timestamp;
    myOrder_.hdr_.trigger_timestamp = quote_trigger_timestamp;
    myOrder_.hdr_.exchange_timestamp = quote_received_timestamp;
    myOrder_.packet_.exchange_order_id = aef::infra::get_realtime_in_nanos();
    current_state_ |= (static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_CXL_PLACED));
    if(context_->send_order(myOrder_))
    {
        pendingCancel_ = true;
        //current_state_ |= (static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_CXL_PLACED));
        return uid_;
    }
        else {
    current_state_ &= ~(static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_CXL_PLACED));
}

    context_->LOG_DEBUG(" failed to cancel order(%d) :: queue is full",  uid_);

    return 0;
}

void order_instance::reset()
{  

    context_->LOG_DEBUG("reset called for UID(%d), request_id(%d), strategy_id(%d), side(%d) ", 
                 uid_,static_cast<uint32_t>(myOrder_.hdr_.uid_.id_),
                static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.strategy_id),static_cast<uint32_t>(myOrder_.packet_.flags_.order_side));


    open_qty_ = 0;
    notional_open_qty_ = 0;
    filled_qty_ = 0;
    filled_price_ = 0;
    uid_ = 0;
    current_state_ = (static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_INITIAL_STATE));
    open_price_ = 0;
    // memset(&myOrder_, 0, sizeof(oms_transaction));
    myOrder_.packet_.flags_.order_type= typeoforder;
    myOrder_.packet_.quantity_ = 0;
    myOrder_.packet_.price_ = 0;
    myOrder_.hdr_.transaction_code = 0;
    
    pendingCancel_ = false;
}
void order_instance::handle_confirmation(const oms_transaction& response)
{
    switch(response.hdr_.transaction_code)
    {
        case OMS_API_TRANS_CODES::OMS_ORDER_PLACED:
        {
            current_state_ |= (static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_PLACED));
            notional_open_qty_ = response.packet_.quantity_;
        }
        break;

        case OMS_API_TRANS_CODES::OMS_ORDER_MODIFY_PLACED:
        {
            current_state_ |= (static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_MOD_PLACED));
            notional_open_qty_ = response.packet_.quantity_;
        }
        break;

        case OMS_API_TRANS_CODES::OMS_ORDER_CANCEL_ACCEPTED:
        {
            current_state_ |= (static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_MOD_PLACED));
        }
        break;

        case OMS_API_TRANS_CODES::OMS_ORDER_CONFIRMED:
        {
            current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_PLACED));
            current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_ORDER_PLACED));
            current_state_ |= (static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_EXCHG_CONF));
            open_qty_   = response.packet_.quantity_;
            open_price_ = response.packet_.price_;

            notional_open_qty_ = 0;
        }
        break;

        case OMS_API_TRANS_CODES::OMS_ORDER_MODIFY_CONFIRMED:
        {
            current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_MODIFY_PLACED));
            current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_MOD_PLACED));
            open_qty_ = response.packet_.quantity_ - get_filled_qty();
            open_price_ = response.packet_.price_;

            context_->LOG_DEBUG(" open_qty_:%d",open_qty_);


            notional_open_qty_ = 0;
        }
        break;

    case OMS_API_TRANS_CODES::OMS_ORDER_CANCELLED:
    {

        context_->LOG_DEBUG("reset called here");

        reset();
    }
        break;

        case OMS_API_TRANS_CODES::OMS_TRADE:
        {

            context_->LOG_DEBUG(" open_qty_:(%d),filled_qty_:(%d),resp_qty(%d)",open_qty_,filled_qty_,response.packet_.quantity_);

            open_qty_ -= response.packet_.quantity_;
            filled_qty_ += response.packet_.quantity_;
            filled_price_ = response.packet_.price_;

            context_->LOG_DEBUG(" open_qty_:%d,filled_qty_:%d",open_qty_,filled_qty_);

            assert(open_qty_ >= 0);
            if(open_qty_ == 0) 
            {

                context_->LOG_DEBUG(" reset called here" );
                reset();

            }
        }
        break;

        case OMS_API_TRANS_CODES::OMS_REQ_REJ:
        {

            context_->LOG_DEBUG(" OMS request rejection received, error_code(%d),exchg_error_code(%d), side(%d) ", 
 static_cast<int32_t>(response.hdr_.error_code),static_cast<int32_t>(response.packet_.exchange_error_code),static_cast<uint32_t>(myOrder_.packet_.flags_.order_side) );


            switch(response.hdr_.error_code)
            {
                case OMS_API_ERROR_CODES::EXCHG_ERROR:
                {
                    if((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_PLACED) & current_state_)) {

                    context_->LOG_DEBUG(" reset called here"  );

                    reset();

                }
                else if((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_MODIFY_PLACED) & current_state_))
                {
                    current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_MODIFY_PLACED));
                    current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_MOD_PLACED));
                    notional_open_qty_ = 0;

                    if(pendingCancel_)
                    {

                        context_->LOG_DEBUG(" cancel called here");
                       
                    }
                }
                else if((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_CXL_PLACED) & current_state_))
                    {
                        pendingCancel_ = false;
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_CXL_PLACED));
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_CXL_PLACED));
                    }
                }
                break;

                case OMS_API_ERROR_CODES::THROTTLE_LIMIT:
            {
                if ((static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_ORDER_PLACED) & current_state_)) {

                    context_->LOG_DEBUG(" reset called here");

                    reset();
                }
                    else if((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_MODIFY_PLACED) & current_state_))
                    {
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_MODIFY_PLACED));
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_MOD_PLACED));
                        notional_open_qty_ = 0;

                        if(pendingCancel_)
                        {

                            context_->LOG_DEBUG(" cancel called here" );
                        }
                    }
                    else if((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_CXL_PLACED) & current_state_))
                    {
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_CXL_PLACED));
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_CXL_PLACED));

                            context_->LOG_DEBUG(" cancel called here");
                    }
                }
                break;

                case OMS_API_ERROR_CODES::RMS_ERROR:
                {
                    // log error
                    if((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_ORDER_PLACED) & current_state_)) 
                    {

                            context_->LOG_DEBUG(" reset called here");
                    reset();
                }
                    else if((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_MODIFY_PLACED) & current_state_))
                    {
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_MODIFY_PLACED));
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_MOD_PLACED));
                        notional_open_qty_ = 0;
                    }
                    else if((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_CXL_PLACED) & current_state_)) // not possible
                    {
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_CXL_PLACED));
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_CXL_PLACED));
                    
                    }
                }
                break;

                case OMS_API_ERROR_CODES::INVALID_REQ_STATUS:
                {

                    context_->LOG_DEBUG("FATAL :: invalid request status");
                    if((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_ORDER_PLACED) & current_state_)) 
                    {

                            context_->LOG_DEBUG(" reset called here");
                    reset();
                }
                    else if((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_MODIFY_PLACED) & current_state_))
                    {
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_MODIFY_PLACED));
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_MOD_PLACED));
                        notional_open_qty_ = 0;

                    }
                    else if((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_CXL_PLACED) & current_state_)) // not possible
                    {
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_CXL_PLACED));
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_CXL_PLACED));
                       
                    }
                }
                break;

                case OMS_API_ERROR_CODES::INVALID_QTY:
                {

                    context_->LOG_DEBUG("FATAL :: invalid request qty");
                    if((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_ORDER_PLACED) & current_state_)) 
                    {

                            context_->LOG_DEBUG("reset called here");

                    reset();
                    }
                    else if((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_MODIFY_PLACED) & current_state_))
                    {
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_MODIFY_PLACED));
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_MOD_PLACED));
                        notional_open_qty_ = 0;

                    }
                    else if((static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_CXL_PLACED) & current_state_)) // not possible
                    {
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_CXL_PLACED));
                        current_state_ &= ~(static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_OMS_CXL_PLACED));
                    
                    }
                }
                break;

                default:
                {
                    // log error and throw exception
                    // stop strategy

                    context_->LOG_DEBUG(" unhandled error_code(%d) and reason_code(%d) in rejection "
			, static_cast<int32_t>(response.hdr_.error_code), static_cast<int32_t>(response.hdr_.reason_code));

                    //throw std::runtime_error("FATAL Error: Unhandled Error Code");
                }
                break;
            }
        }
        break;
    }

    context_->LOG_DEBUG(" request_id(%d), strategy_id(%d),trans code(%d),", (long long)response.hdr_.uid_.composite_id_.request_id,
                        ((long long)response.hdr_.uid_.composite_id_.strategy_id),
                        ((long long)response.hdr_.transaction_code));


    context_->LOG_DEBUG("error_code(%d),exchange_error_code(%d),open_qty(%d),notional_qty(%d), "
            "current_state(%d), price(%d),qty(%d),side(%d)",
                ((long long)response.hdr_.error_code) ,
                ((long long)response.packet_.exchange_error_code) ,
                ((long long)get_open_qty()) ,
                ((long long)get_notional_qty()) ,
                ((long long)get_current_state()),
                ((long long)response.packet_.price_),
                ((long long)response.packet_.quantity_),
                static_cast<uint32_t>(response.packet_.flags_.order_side)) ;


}

int32_t order_instance::get_open_qty() const
{
    return open_qty_;
}

int32_t order_instance::get_notional_qty() const
{
    return notional_open_qty_;
}

uint32_t order_instance::get_current_state() const
{
    return current_state_;
}

uint32_t order_instance::get_uid() const
{
    return uid_;
}
int32_t order_instance::get_existing_qty() const
{
    return myOrder_.packet_.quantity_;
}
int32_t order_instance::get_existing_price() const
{
    return myOrder_.packet_.price_;
}
int32_t order_instance::get_filled_qty() const
{
    return filled_qty_;
}

void order_instance::set_time_stamps(const uint64_t& qt_event_timestamp,
const uint64_t& qt_trigger_timestamp,
const uint64_t qt_received_timestamp)
{
    quote_event_timestamp = qt_event_timestamp;
    quote_trigger_timestamp = qt_trigger_timestamp;
    quote_received_timestamp = qt_received_timestamp;

}
int32_t order_instance::get_open_price() const
{
    return open_price_;
}
int32_t order_instance::get_filled_price() const
{
    return filled_price_;
}
void order_instance::set_price_type() 
{
    myOrder_.packet_.flags_.price_type= 0b011  ;// PRICE_TYPE::SIM_PRICE;
}
ORDER_SIDE order_instance::get_side() 
{
    return side_;
}

uint32_t order_instance::place_order2L(const int32_t price, const int32_t qty)
{
	if (uid_ != 0) {
		return 0;
	}

        if(is_response_pending())
        {
//            context_->LOG_DEBUG("(%s-%s-%d) cannot place order , response is pending, current_state(%d),request_id(%d), strategy_id(%d), side(%d)", LOG_LOCATION, current_state_,static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.request_id),static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.strategy_id),static_cast<uint32_t>(myOrder_.packet_.flags_.order_side));
            return 0;
        }
            
        if(qty <= 0)
        {
            context_->LOG_INFO("(%s-%s-%d) failed to place order :: uid(%d),response_pending(%d), Qty is not greater than 0", uid_, is_response_pending());
            return false;
            
        }
        uid_ = context_->get_next_order_id();
        myOrder_.hdr_.transaction_code = OMS_API_TRANS_CODES::OMS_PLACE_REQ2L;
    //	myOrder_.packet_.exchange_error_code = token2;
        myOrder_.hdr_.uid_.id_ = uid_;
        // myOrder_.packet_.product_id_ = product_id_;
        // myOrder_.packet_.flags_.order_side = side_;
        myOrder_.packet_.price_ = price;
        myOrder_.packet_.quantity_ = qty;
        // myOrder_.packet_.client_id = client_id_;
        // myOrder_.packet_.algo_id = algo_id_;
        myOrder_.hdr_.event_timestamp = quote_event_timestamp;
        myOrder_.hdr_.trigger_timestamp = quote_trigger_timestamp;
        myOrder_.hdr_.exchange_timestamp = quote_received_timestamp;
        myOrder_.packet_.exchange_order_id = aef::infra::get_realtime_in_nanos();
       // myOrder_.packet_.oms_id = omsid_ ;
        
        current_state_ |= (static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_ORDER_PLACED));
        if(context_->send_order(myOrder_)) 
        {
            
            //current_state_ |= (static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_ORDER_PLACED));
            return uid_;
        }else {
        current_state_ &= ~(static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_ORDER_PLACED));
    }
        
        return 0;
}

bool order_instance::update_order2L(const int32_t price, const int32_t qty)
{   
	if(open_price_ == price && open_qty_ == qty)
	{
		return false;
		
	}
   
	if(qty <= 0)
	{
		return false;
		
	}
	
	// context_->LOG_DEBUG("(%s-%s-%d):: request_id(%d), strategy_id(%d),trans code(%d),"
	// 		"open_qty(%d),notional_qty(%d),"
	// 		"current_state(%d), price(%d), qty(%d), filled_qty(%d), side(%d)",LOG_LOCATION, static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.request_id),
	// 			static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.strategy_id),
	// static_cast<uint32_t>(myOrder_.hdr_.transaction_code),
	// ((long long)get_open_qty()) ,
	// 			((long long)get_notional_qty()) ,
	// 			((long long)get_current_state()),
	// 			price,
	// 			qty,
	// 			get_filled_qty(),
	// 			static_cast<uint32_t>(myOrder_.packet_.flags_.order_side)) ;
	
	
	
	if((uid_ == 0) || (is_response_pending()) || (!is_order_confirmed()))
	{
		
	// 	context_->LOG_DEBUG("(%s-%s-%d) failed to modify order :: uid(%d),response_pending(%d), order_confirmed(%d),request_id(%d), strategy_id(%d), Side(%d)", 
	//  LOG_LOCATION, uid_, is_response_pending(), is_order_confirmed(),static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.request_id),
	//  static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.strategy_id),static_cast<uint32_t>(myOrder_.packet_.flags_.order_side));
		
		return false;
	}

	myOrder_.hdr_.transaction_code = OMS_API_TRANS_CODES::OMS_REPLACE_REQ2L;

	myOrder_.packet_.price_ = price;
	myOrder_.packet_.quantity_ = qty + get_filled_qty();
	//myOrder_.packet_.exchange_error_code = token2;
	myOrder_.hdr_.event_timestamp = quote_event_timestamp;
	myOrder_.hdr_.trigger_timestamp = quote_trigger_timestamp;
	myOrder_.hdr_.exchange_timestamp = quote_received_timestamp;
	myOrder_.packet_.exchange_order_id = aef::infra::get_realtime_in_nanos();
	
	// context_->LOG_DEBUG("(%s-%s-%d):: ts1(%" PRIu64 "), ts2(%" PRIu64 "), ts3(%" PRIu64 "),ts4(%" PRIu64 ")",
	// 		LOG_LOCATION,static_cast<uint64_t>(myOrder_.hdr_.event_timestamp),
	// 			static_cast<uint64_t>(myOrder_.hdr_.trigger_timestamp),
	// 			static_cast<uint64_t>(myOrder_.hdr_.exchange_timestamp),
	// 			static_cast<uint64_t>(myOrder_.packet_.exchange_order_id));
	
	current_state_ |= (static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_MODIFY_PLACED));
	if(context_->send_order(myOrder_))
	{
		//current_state_ |= (static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_MODIFY_PLACED));
		return uid_;
	}else {
	current_state_ &= ~(static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_MODIFY_PLACED));
}
	
	
	return false;
}

bool order_instance::cancel_order2L()
{
	
	if((uid_ == 0) || (is_response_pending()) || (!is_order_confirmed()))
	{
		
		// context_->LOG_DEBUG("(%s-%s-%d) failed to cancel order :: uid(%d),response_pending(%d), order_confirmed(%d), request_id(%d), strategy_id(%d), side(%d)", 
		// 		LOG_LOCATION, uid_, is_response_pending(), is_order_confirmed(),static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.request_id),static_cast<uint32_t>(myOrder_.hdr_.uid_.composite_id_.strategy_id),static_cast<uint32_t>(myOrder_.packet_.flags_.order_side));
		
		return false;
	}
	//myOrder_.packet_.exchange_error_code = token2;
	myOrder_.hdr_.transaction_code = OMS_API_TRANS_CODES::OMS_CANCEL_REQ2L;

	current_state_ |= (static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_CXL_PLACED));
	if(context_->send_order(myOrder_))
	{
		pendingCancel_ = true;
		//current_state_ |= (static_cast<uint32_t>(STRAT_ORDER_STATE::STRAT_CXL_PLACED));
		return uid_;
	}
	 else {
	current_state_ &= ~(static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_CXL_PLACED));
}
	
	// context_->LOG_DEBUG("(%s-%s-%d) failed to cancel order(%d) :: queue is full", 
	// 			LOG_LOCATION, uid_);
	
	return 0;
}


bool order_instance::kill_tokenOrder(const int32_t token)
{
    myOrder_.hdr_.transaction_code = OMS_API_TRANS_CODES::OMS_KILL_SWITCH_TOKEN_WISE;
    myOrder_.packet_.product_id_=token;    
    if(context_->send_order(myOrder_))
    { 
          return true;
    }
   
    return false;  
}

bool order_instance::kill_allOrder()
{
    myOrder_.hdr_.transaction_code = OMS_API_TRANS_CODES::OMS_KILL_SWITCH;
    current_state_ |= (static_cast<uint32_t> (STRAT_ORDER_STATE::STRAT_CXL_PLACED));
    if(context_->send_order(myOrder_))
    {
        return true;
    }    
    return false;
}

}
 // namespace execution_strat
