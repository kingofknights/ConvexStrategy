/**
 * @file LoggerFrontend.hpp
 * @brief Front-end wrapper for emitting log messages to the platform backend.
 */
#pragma once
#include <memory>
#include "LoggerExports.hpp"

namespace aef {
namespace infra {
namespace logger {

void destroy_logger(void* loggerObj);

class LoggerFE {
  private:
    std::unique_ptr<void, void (*)(void*)> myLogBackend_;
    const std::uint32_t currentLogLevel_;

  private:
    void LogMessageImpl(const AEF_LOG_LEVEL level, const int32_t instance, const char* fmt,
                        size_t length);
    void LogMessageTsImpl(const AEF_LOG_LEVEL level, const int32_t instance,
                          const uint64_t timestamp, const char* fmt, size_t length);

  public:
    /** @brief Construct a logger frontend bound to a thread name/core. */
    LoggerFE(const std::string threadName, const int core,
             const AEF_LOG_LEVEL minimumLevel = AEF_LOG_LEVEL::AEF_LOG_INFO);

    ~LoggerFE();

    template <class... Args>
    void LogMessage(const AEF_LOG_LEVEL level, const int32_t instance, const char* fmt,
                    Args&&... args)
    {
        if (static_cast<std::uint32_t>(level) >= currentLogLevel_) {
            char buffer[MAX_LOG_MSG_SIZE + 2] = {0};
            int lnReturnValue
                = snprintf(buffer, MAX_LOG_MSG_SIZE, fmt, std::forward<Args>(args)...);
            LogMessageImpl(level, instance, buffer, lnReturnValue + 1);
        }
    }

    template <class... Args>
    void LogMessageTs(const AEF_LOG_LEVEL level, const int32_t instance, const uint64_t timestamp,
                      const char* fmt, Args&&... args)
    {
        if (static_cast<std::uint32_t>(level) >= currentLogLevel_) {
            char buffer[MAX_LOG_MSG_SIZE + 2] = {0};
            int lnReturnValue
                = snprintf(buffer, MAX_LOG_MSG_SIZE, fmt, std::forward<Args>(args)...);
            LogMessageTsImpl(level, instance, timestamp, buffer, lnReturnValue + 1);
        }
    }

    /** @brief Forward a binary packet to the logger backend. */
    void capturePacket(const int32_t instance, const char* buffer, const size_t len);

    /** @brief Initialize backend resources. */
    bool startLogger();
    /** @brief Flush and tear down backend resources. */
    bool stopLogger();

    /// returns file index
    int32_t registerLogInstance(const char* pcFileName, LOG_FILE_TYPE file_type,
                                LOG_FLUSH_POLICY flush_policy
                                = LOG_FLUSH_POLICY::LOG_FLUSH_IMMEDIATE,
                                size_t max_file_size = DEFAULT_LOG_FILE_SIZE_IN_BYTES);
    /** @brief Resolve the filename backing a registered log instance. */
    bool getLogInstanceFileName(const int32_t logIndex, std::string& fileName);
};

} // namespace logger
} // namespace infra
} // namespace aef
