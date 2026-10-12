#pragma once
#include "Utils.hpp"  // pulls order_instance.hpp, which includes the platform headers in the order AlgoBase.hpp needs
#include "nlohmann/json.hpp"

#include <cstdint>
#include <map>
#include <string>

class MinixStrategy;

// GUI square-off (1331): reads the trader's fills for today from Postgres, then flattens every token with a
// net position. A long sells at the third bid and a short buys at the third ask, so the order crosses the touch;
// it is re-priced on each broadcast until the token is flat.
class SquareOff {
  public:
    // Postgres holding the trader's fills; read from the strategy config keys of the same names.
    struct DbConfig {
        std::string _host, _port, _dbname, _user, _password, _table;
        int32_t     _traderId = 0;
    };

    SquareOff(MinixStrategy* hub_, const nlohmann::json& config_);

    // Loads today's positions and starts flattening them; order responses echo to interface_ on 5001.
    // Ignored while a square-off is running.
    void Start(int32_t interface_);
    void OnBcast(const aef::infra::product::product_data& product_);
    // True when response_ belongs to a square-off order.
    auto OnOrderResponse(const oms_transaction& response_) -> bool;

  private:
    struct Position {
        int32_t         _netQuantity = 0;  // signed: > 0 long, < 0 short
        OrderObjectPtrT _order;
    };

    MinixStrategy*              _hub;
    DbConfig                    _db;
    client_uid                  _clientUid{};
    int32_t                     _interface = 0;  // GUI that pressed square-off
    std::map<int32_t, Position> _positions;  // token -> open position still to flatten
};

// Signed net quantity per token from the trader's fills today, flat tokens left out.
// Throws std::runtime_error when the DB is not configured, unreachable, or the query fails.
auto LoadTodayPositions(const SquareOff::DbConfig& db_) -> std::map<int32_t, int32_t>;
