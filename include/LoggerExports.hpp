/**
 * @file LoggerExports.hpp
 * @brief Logger configuration constants and enums shared with the backend.
 */
#pragma once
#include <iostream>
#include <string>
#include <stdint.h>

namespace aef {
namespace infra {
namespace logger {
using namespace std::string_literals;
const size_t MAX_LOG_MSG_SIZE = 4094;
const size_t LOG_TIME_STAMP_BUF_SIZE = 56;
const size_t FILE_NAME_PREFIX_SIZE = 256;
const size_t FILE_NAME_SIZE = FILE_NAME_PREFIX_SIZE + 65;
const size_t DEFAULT_LOG_FILE_SIZE_IN_BYTES    = 1073741824; // 0//(10GB)   //10485760//(10MB) //(in bytes)
const size_t DEFAULT_LOG_CACHE_SIZE = 10485760;
const char loggerDateFormat[] = "%Y%m%d";
const char loggerTimeFormat[] = "%H%M%S";
const std::string defaultLogDirectory = "/var/log/hft/"s;

enum class AEF_LOG_LEVEL : std::uint32_t {
    INVALID_AEF_LOG_LEVEL = 0,
    AEF_LOG_DEBUG = 1,
    AEF_LOG_INFO = 2,
    AEF_LOG_ERROR = 4,
    AEF_LOG_BINARY = 8,
    AEF_LOG_BROADCAST = 16
};

enum class LOG_FILE_TYPE : std::int32_t { TEXT_LOG = 1, BINARY_LOG = 2 };

enum class LOG_FLUSH_POLICY : std::int32_t { LOG_FLUSH_IMMEDIATE = 1, LOG_FLUSH_BUFFERED = 2 };

} // namespace logger
} // namespace infra
} // namespace aef
