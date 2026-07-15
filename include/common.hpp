/**
 * @file common.hpp
 * @brief Shared timing helpers and lightweight parameter container.
 */
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <utility>
#include <stdint.h>

#pragma once

const int64_t BILLIONS = 1000000000L;

/**
 * @brief Collection of computed variables used by strategy logic.
 */
struct VariableParameters
{  
    double lnReturn{0.0};
    double lnen{0.0};
    double lnPureVolImb{0.0};
    double midprice{0.0};
    
};

#define LOG_LOCATION __FILE__, __FUNCTION__, __LINE__

    
/** @brief Get wall-clock time in nanoseconds. */
static uint64_t get_realtime_in_nanos()
{
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec * BILLIONS  + ts.tv_nsec );
}
/** @brief Convenience wrapper returning epoch seconds. */
static time_t get_realtime()
{
    return time(nullptr);
}



