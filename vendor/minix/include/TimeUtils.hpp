/**
 * @file TimeUtils.hpp
 * @brief Time and TSC utilities for low-latency measurements.
 */

#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <sys/time.h>
#include <time.h>
#include <chrono>
#include <x86intrin.h> 

namespace aef {
namespace infra {

const int64_t BILLION = 1000000000L;


/** @brief Compute nanosecond delta between two timespec instances. */
static inline int64_t time_diff_nanos(struct timespec start, struct timespec stop)
{
    int64_t result = 0;
    if ((stop.tv_nsec - start.tv_nsec) < 0) {
        result = (stop.tv_sec - start.tv_sec - 1) * BILLION;
        result += stop.tv_nsec - start.tv_nsec + BILLION;
    } else {
        result = (stop.tv_sec - start.tv_sec) * BILLION;
        result += stop.tv_nsec - start.tv_nsec;
    }

    return result;
}

/** @brief Subtract two timespecs and return the difference. */
static inline timespec diff(timespec start, timespec end)
{
    timespec temp;
    if ((end.tv_nsec - start.tv_nsec) < 0) {
        temp.tv_sec = end.tv_sec - start.tv_sec - 1;
        temp.tv_nsec = 1000000000 + end.tv_nsec - start.tv_nsec;
    } else {
        temp.tv_sec = end.tv_sec - start.tv_sec;
        temp.tv_nsec = end.tv_nsec - start.tv_nsec;
    }
    return temp;
}

/** @brief Monotonic clock reading. */
static inline struct timespec time_now() // = ~19ns
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

/** @brief Realtime clock reading. */
static inline struct timespec realtime_now() // = ~19ns
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts;
}

/** @brief Monotonic clock value in nanoseconds. */
static inline uint64_t get_time_in_nanos()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec * BILLION + ts.tv_nsec);
}
/** @brief Monotonic raw clock in nanoseconds. */
static inline uint64_t get_monotonictime_in_nanos()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec * BILLION + ts.tv_nsec);
}
/** @brief Convert timestamp counter to nanoseconds given CPU frequency. */
static inline uint64_t get_rdtsc_in_nanos(double tsc_ghz)
{
    unsigned int aux;
    uint64_t tsc = __rdtscp(&aux);  // Serialize and read TSC
    return static_cast<uint64_t>(tsc / (tsc_ghz * 1e-9));  // Convert to nanoseconds
}
/** @brief Realtime clock in nanoseconds. */
static inline uint64_t get_realtime_in_nanos()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec * BILLION + ts.tv_nsec);
}

/** @brief Realtime clock in milliseconds. */
static inline uint64_t get_realtime_in_milis()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec * 1000 + (ts.tv_nsec /1000000));
}
/** @brief Convert timespec to a nanosecond count. */
static inline uint64_t timespec_to_nanos(const timespec& ts)
{
    return static_cast<uint64_t>(ts.tv_sec * BILLION + ts.tv_nsec);
}

/** @brief Convert nanoseconds into a timespec struct. */
static inline timespec nanos_to_timespec(const uint64_t ts)
{
    auto dur = std::chrono::nanoseconds(ts);
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(dur);
    dur -= secs;

    return timespec{secs.count(), dur.count()};
}

/** @brief Format current local time according to the supplied format string. */
static inline std::string getFormattedTime(const char* dateTimeFormat)
{
    assert(dateTimeFormat);
    time_t timestamp = time(nullptr);
    char buffer[64] = {};
    struct tm utcTime;
    localtime_r(&timestamp, &utcTime);
    strftime(buffer, sizeof(buffer), dateTimeFormat, &utcTime);
    return buffer;
}

/** @brief Busy-wait sleep for the requested nanoseconds. */
static inline void __attribute__((optimize("O0"))) precise_nsleep(int64_t nsec)
{

    if (nsec <= 0)
        return;
    struct timespec ts0 = time_now();
    int64_t dtns = 0;
    while (dtns < nsec) {
        volatile int64_t busywait = 1000; // empirical
        while (busywait--)
            __asm__ __volatile__("");
        dtns = time_diff_nanos(ts0, time_now());
        assert(dtns > 0);
    }
}

/** @brief Busy-wait sleep for the requested seconds (fractional). */
static inline void __attribute__((optimize("O0"))) precise_sleep(double sec)
{

    int64_t nsec = sec * BILLION;
    if (nsec <= 0)
        return;
    struct timespec ts0 = time_now();
    int64_t dtns = 0;
    while (dtns < nsec) {
        volatile int32_t busywait = 10000; // empirical
        while (busywait--)
            __asm__ __volatile__("");
        dtns = time_diff_nanos(ts0, time_now());
        assert(dtns > 0);
    }
}

/** @brief Calibrated parameters for converting TSC deltas. */
typedef struct aef_tsc_params {
    uint64_t hz;
    uint64_t tsc_cost;
} aef_tsc_params;

#ifdef __x86_64__

static inline void aef_tsc(uint64_t* pval)
{
    uint64_t low, high;
    __asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
    *pval = (high << 32) | low;
}
#elif defined(__i386__)
#define aef_tsc(pval) __asm__ __volatile__("rdtsc" : "=A"(*(pval)))
#else
#error Unknown processor.
#endif

/** @brief Empirically measure CPU TSC frequency over the given interval. */
static uint64_t measure_hz(int interval_usec)
{
    struct timeval tv_s, tv_e;
    uint64_t tsc_s, tsc_e, tsc_e2;
    uint64_t tsc_gtod, min_tsc_gtod, usec = 0;
    int n;

    aef_tsc(&tsc_s);
    gettimeofday(&tv_s, nullptr);
    aef_tsc(&tsc_e2);
    min_tsc_gtod = tsc_e2 - tsc_s;
    n = 0;
    do {
        aef_tsc(&tsc_s);
        gettimeofday(&tv_s, nullptr);
        aef_tsc(&tsc_e2);
        tsc_gtod = tsc_e2 - tsc_s;
        if (tsc_gtod < min_tsc_gtod)
            min_tsc_gtod = tsc_gtod;
    } while (++n < 20 || (tsc_gtod > min_tsc_gtod * 2 && n < 100));

    do {
        aef_tsc(&tsc_e);
        gettimeofday(&tv_e, nullptr);
        aef_tsc(&tsc_e2);
        if (tsc_e2 < tsc_e) {
            break;
        }
        tsc_gtod = tsc_e2 - tsc_e;
        usec = (tv_e.tv_sec - tv_s.tv_sec) * (uint64_t)1000000;
        usec += tv_e.tv_usec - tv_s.tv_usec;
    } while (usec < interval_usec || tsc_gtod > min_tsc_gtod * 2);

    return (tsc_e - tsc_s) * 1000000 / usec;
}
/** @brief Placeholder for platform-specific TSC cost measurement. */
static uint64_t measure_tsc(void)
{
    return 0; /* ?? TODO */
}
/** @brief Populate TSC conversion parameters. */
static int aef_tsc_get_params(struct aef_tsc_params* params)
{
    measure_hz(100000);
    params->hz = measure_hz(100000);
    params->tsc_cost = measure_tsc();

    return 0;
}
/* Convert tsc delta to microseconds. */
/** @brief Convert TSC delta to microseconds. */
static int64_t aef_tsc_usec(const struct aef_tsc_params* params, int64_t tsc)
{
    return tsc * 1000000 / params->hz;
}
/* Convert tsc delta to nanoseconds. */
/** @brief Convert TSC delta to nanoseconds. */
static int64_t aef_tsc_nsec(const struct aef_tsc_params* params, int64_t tsc)
{
    return tsc * 1000000000 / params->hz;
}
/* Convert milli-seconds delta to tsc. */
/** @brief Convert milliseconds to TSC delta using calibration. */
static int64_t aef_msec_tsc(const struct aef_tsc_params* params, int64_t msecs)
{
    return params->hz * msecs / 1000;
}
/* Convert micro-seconds delta to tsc. */
/** @brief Convert microseconds to TSC delta using calibration. */
static int64_t aef_usec_tsc(const struct aef_tsc_params* params, int64_t usecs)
{
    return params->hz * usecs / 1000000;
}
/* Convert nano-seconds delta to tsc. */
/** @brief Convert nanoseconds to TSC delta using calibration. */
static int64_t aef_nsec_tsc(const struct aef_tsc_params* params, int64_t nsecs)
{
    return params->hz * nsecs / 1000000000;
}
/** @brief Direct RDTSC helper returning the raw cycle count. */
static uint64_t RDTSC()
{

#if defined(__x86_64__)
    unsigned int hi, lo;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__powerpc64__)
    unsigned long long int result = 0;
    __TIME_BASE(result);

    return (result);

#endif
}
/** @brief Convert NSE nanosecond timestamp to UTC nanoseconds. */
static uint64_t GetUTCTimeFromNSETimeNanos(uint64_t nTimeStamp)
{
    return (nTimeStamp + (315513000 * BILLION));
}
/** @brief Convert NSE second timestamp to UTC seconds. */
static uint32_t GetUTCTimeFromNSETime(uint32_t nTimeStamp)
{
    return (nTimeStamp + 315513000);
}
/** @brief Compose a nanosecond timestamp for today at the given wall time. */
static inline int64_t calculateTargetTime(int hour,int min,int sec) {
    // Get current time as time_t
    std::time_t now = std::time(nullptr);
    std::tm* localTime = std::localtime(&now);

    // Set target time values
    localTime->tm_hour = hour;
    localTime->tm_min = min;
    localTime->tm_sec = sec;

    // Convert target time to time_t (epoch time in seconds)
    std::time_t targetEpoch = std::mktime(localTime);

    // Convert epoch time to nanoseconds
    int64_t targetTimeInNanoseconds1 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::system_clock::from_time_t(targetEpoch).time_since_epoch()).count();

    return targetTimeInNanoseconds1;

}


} // namespace infra
} // namespace aef
