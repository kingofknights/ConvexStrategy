// Postgres access for SquareOff, kept apart so the dry run can link a fake LoadTodayPositions instead.
#include "SquareOff.hpp"

#include <libpq-fe.h>

#include <ctime>
#include <memory>
#include <stdexcept>

namespace {

    // Trade timestamps count nanoseconds from 1980-01-01 (the NSE epoch), not 1970.
    constexpr int64_t SECONDS_1970_TO_1980 = 10LL * 365 * 24 * 3600 + 2LL * 24 * 3600;  // 10 years + 2 leap days

    // UTC midnight daysAhead_ days from today, in NSE-epoch nanoseconds.
    auto UtcMidnightNs(int daysAhead_) -> int64_t {
        const time_t now = std::time(nullptr);
        tm           utc{};
        gmtime_r(&now, &utc);
        utc.tm_hour = 0;
        utc.tm_min  = 0;
        utc.tm_sec  = 0;
        utc.tm_mday += daysAhead_;
        return (static_cast<int64_t>(timegm(&utc)) - SECONDS_1970_TO_1980) * 1'000'000'000LL;
    }

}  // namespace

auto LoadTodayPositions(const SquareOff::DbConfig& db_) -> std::map<int32_t, int32_t> {
    if (db_._dbname.empty() || db_._table.empty()) {
        throw std::runtime_error{"DB not configured: set dbname and table_name in the strategy config"};
    }

    const char* const keys[]   = {"host", "port", "dbname", "user", "password", nullptr};
    const char* const values[] = {db_._host.c_str(), db_._port.c_str(), db_._dbname.c_str(), db_._user.c_str(), db_._password.c_str(), nullptr};
    const std::unique_ptr<PGconn, decltype(&PQfinish)> connection{PQconnectdbParams(keys, values, 0), &PQfinish};
    if (PQstatus(connection.get()) != CONNECTION_OK) {
        throw std::runtime_error{std::string{"connect failed: "} + PQerrorMessage(connection.get())};
    }

    // The table name comes from config: quote it as an identifier, everything else goes as a parameter.
    char* const quotedTable = PQescapeIdentifier(connection.get(), db_._table.c_str(), db_._table.size());
    if (quotedTable == nullptr) {
        throw std::runtime_error{std::string{"bad table_name: "} + PQerrorMessage(connection.get())};
    }
    const std::string query = std::string{"SELECT \"TokenNumber\", \"BuySellIndicator\", \"FillQty\" FROM "} + quotedTable +
                              " WHERE \"TraderId\" = $1 AND \"FillQty\" IS NOT NULL AND \"TimeStampNs\" >= $2 AND \"TimeStampNs\" < $3";
    PQfreemem(quotedTable);

    const std::string traderId = std::to_string(db_._traderId);
    const std::string from     = std::to_string(UtcMidnightNs(0));
    const std::string to       = std::to_string(UtcMidnightNs(1));
    const char* const params[] = {traderId.c_str(), from.c_str(), to.c_str()};

    const std::unique_ptr<PGresult, decltype(&PQclear)> result{PQexecParams(connection.get(), query.c_str(), 3, nullptr, params, nullptr, nullptr, 0), &PQclear};
    if (PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        throw std::runtime_error{std::string{"query failed: "} + PQresultErrorMessage(result.get())};
    }

    std::map<int32_t, int32_t> netQuantities;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        const int32_t token    = std::stoi(PQgetvalue(result.get(), row, 0));
        const int32_t buySell  = std::stoi(PQgetvalue(result.get(), row, 1));  // 1 buy, 2 sell
        const int32_t quantity = std::stoi(PQgetvalue(result.get(), row, 2));
        netQuantities[token] += buySell == 1 ? quantity : buySell == 2 ? -quantity : 0;
    }
    std::erase_if(netQuantities, [](const auto& entry_) { return entry_.second == 0; });
    return netQuantities;
}
