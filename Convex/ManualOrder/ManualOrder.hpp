#pragma once
#include "Utils.hpp"  // pulls order_instance.hpp, which includes the platform headers in the order AlgoBase.hpp needs

#include <cstdint>
#include <map>

class MinixStrategy;

// GUI manual orders: the GUI sends oms_transaction3l on 9911 to place up to 3 legs, or to modify / cancel
// one placed earlier; every response for those orders echoes back to the GUI on 5001.
class ManualOrder {
  public:
    explicit ManualOrder(MinixStrategy* hub_);

    void OnRequest(const aef::infra::ui_cmd::UIStruct& request_);
    // True when response_ belongs to a manual order; it has then been echoed to the GUI.
    auto OnOrderResponse(const oms_transaction& response_) -> bool;
    // Echoes response_ to the GUI on 5001. SquareOff uses it too: its orders are strategy 0, like manual ones.
    void SendResponse(const oms_transaction& response_, int32_t interface_);

  private:

    MinixStrategy* _hub;

    // uid -> GUI interface of every manual order placed; routes its responses and later modify / cancel.
    // ponytail: never pruned, a session's manual orders are few.
    std::map<uint32_t, int32_t> _orders;
};
