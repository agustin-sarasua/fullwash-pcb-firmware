#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>

// Forward declaration to avoid circular dependency
class RTCManager;

// Log Levels
enum LogLevel {
    LOG_NONE = 0,     // No logging
    LOG_ERROR = 1,    // Only errors
    LOG_WARNING = 2,  // Errors and warnings
    LOG_INFO = 3,     // Normal operation information
    LOG_DEBUG = 4     // Detailed debug information
};

class Logger {
private:
    static LogLevel currentLevel;
    static const char* levelNames[];
    static bool initialized;
    static RTCManager* rtcManager;
    
    // Helper function to get formatted timestamp
    static void getTimestamp(char* buffer, size_t bufferSize);

public:
    static void init(LogLevel level = LOG_DEBUG, unsigned long baudRate = 115200);
    static void setLogLevel(LogLevel level);
    static LogLevel getLogLevel();
    
    // Set RTC manager for timestamp generation (optional)
    static void setRTCManager(RTCManager* rtc);
    
    static void error(const char* format, ...);
    static void warning(const char* format, ...);
    static void info(const char* format, ...);
    static void debug(const char* format, ...);
    
    static void log(LogLevel level, const char* format, ...);
};

// Convenience macros for logging
#define LOG_ERROR(format, ...) Logger::error(format, ##__VA_ARGS__)
#define LOG_WARNING(format, ...) Logger::warning(format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) Logger::info(format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) Logger::debug(format, ##__VA_ARGS__)

#endif // LOGGER_H