#pragma once
#include "Utils.hpp"  // pulls order_instance.hpp, which includes the platform headers in the order AlgoBase.hpp needs
#include "nlohmann/json.hpp"

#include <cstdint>
#include <map>
#include <string>

class MinixStrategy;

// GUI square-off: reads the trader's fills for today from Postgres, then flattens every token (1331) or one
// token (1332) with a net position. A long sells at the third bid and a short buys at the third ask, so the order crosses the touch;
// it is priced from the last quote at once, then re-priced on each tick or broadcast until the token is flat.
class SquareOff {
  public:
    // Postgres holding the trader's fills; read from the strategy config keys of the same names.
    struct DbConfig {
        std::string _host, _port, _dbname, _user, _password, _table;
        int32_t     _traderId = 0;
    };

    SquareOff(MinixStrategy* hub_, const nlohmann::json& config_);

    // Loads today's positions and starts flattening them, only token_'s when it is not 0; order responses echo
    // to interface_ on 5001. Tokens already being flattened are left alone. True when it took positions to flatten.
    auto Start(int32_t interface_, int32_t token_ = 0) -> bool;
    void OnQuote(const Quote& quote_);
    void OnBcast(const aef::infra::product::product_data& product_);
    // True when response_ belongs to a square-off order.
    auto OnOrderResponse(const oms_transaction& response_) -> bool;

  private:
    struct Position {
        int32_t         _netQuantity = 0;  // signed: > 0 long, < 0 short
        OrderObjectPtrT _order;
    };

    // Third-level prices of a book, or the deepest level present when it is thinner.
    template <typename Level>
    void Reprice(int32_t token_, const Level* bids_, const Level* asks_);

    MinixStrategy*              _hub;
    DbConfig                    _db;
    client_uid                  _clientUid{};
    int32_t                     _interface = 0;  // GUI that pressed square-off
    std::map<int32_t, Position> _positions;  // token -> open position still to flatten
};

// Signed net quantity per token from the trader's fills today, flat tokens left out.
// Throws std::runtime_error when the DB is not configured, unreachable, or the query fails.
auto LoadTodayPositions(const SquareOff::DbConfig& db_) -> std::map<int32_t, int32_t>;
