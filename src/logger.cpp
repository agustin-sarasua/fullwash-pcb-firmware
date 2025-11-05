#include "../include/logger.h"
#include "../include/rtc_manager.h"
#include <stdarg.h>
#include <TimeLib.h>

LogLevel Logger::currentLevel = LOG_DEBUG;
bool Logger::initialized = false;
RTCManager* Logger::rtcManager = nullptr;
const char* Logger::levelNames[] = {"NONE", "ERROR", "WARNING", "INFO", "DEBUG"};

void Logger::init(LogLevel level, unsigned long baudRate) {
    if (!initialized) {
        Serial.begin(baudRate);
        initialized = true;
    }
    currentLevel = level;
    log(LOG_INFO, "Logger initialized with level: %s", levelNames[level]);
}

void Logger::setLogLevel(LogLevel level) {
    currentLevel = level;
    log(LOG_INFO, "Log level changed to: %s", levelNames[level]);
}

LogLevel Logger::getLogLevel() {
    return currentLevel;
}

void Logger::setRTCManager(RTCManager* rtc) {
    rtcManager = rtc;
}

void Logger::getTimestamp(char* buffer, size_t bufferSize) {
    // Try to use RTC if available
    if (rtcManager && rtcManager->isInitialized() && rtcManager->isTimeValid()) {
        time_t now = rtcManager->getDateTime();
        if (now > 0) {
            // Format as HH:MM:SS using TimeLib
            int h = hour(now);
            int m = minute(now);
            int s = second(now);
            snprintf(buffer, bufferSize, "%02d:%02d:%02d", h, m, s);
            return;
        }
    }
    
    // Fallback to uptime from millis()
    unsigned long totalSeconds = millis() / 1000;
    unsigned long hours = totalSeconds / 3600;
    unsigned long minutes = (totalSeconds % 3600) / 60;
    unsigned long seconds = totalSeconds % 60;
    
    snprintf(buffer, bufferSize, "+%02lu:%02lu:%02lu", hours, minutes, seconds);
}

void Logger::error(const char* format, ...) {
    if (currentLevel >= LOG_ERROR) {
        char timestamp[16];
        getTimestamp(timestamp, sizeof(timestamp));
        
        va_list args;
        va_start(args, format);
        Serial.printf("[%s] [ERROR] ", timestamp);
        char buffer[256];
        vsnprintf(buffer, sizeof(buffer), format, args);
        Serial.println(buffer);
        va_end(args);
    }
}

void Logger::warning(const char* format, ...) {
    if (currentLevel >= LOG_WARNING) {
        char timestamp[16];
        getTimestamp(timestamp, sizeof(timestamp));
        
        va_list args;
        va_start(args, format);
        Serial.printf("[%s] [WARNING] ", timestamp);
        char buffer[256];
        vsnprintf(buffer, sizeof(buffer), format, args);
        Serial.println(buffer);
        va_end(args);
    }
}

void Logger::info(const char* format, ...) {
    if (currentLevel >= LOG_INFO) {
        char timestamp[16];
        getTimestamp(timestamp, sizeof(timestamp));
        
        va_list args;
        va_start(args, format);
        Serial.printf("[%s] [INFO] ", timestamp);
        char buffer[256];
        vsnprintf(buffer, sizeof(buffer), format, args);
        Serial.println(buffer);
        va_end(args);
    }
}

void Logger::debug(const char* format, ...) {
    if (currentLevel >= LOG_DEBUG) {
        char timestamp[16];
        getTimestamp(timestamp, sizeof(timestamp));
        
        va_list args;
        va_start(args, format);
        Serial.printf("[%s] [DEBUG] ", timestamp);
        char buffer[256];
        vsnprintf(buffer, sizeof(buffer), format, args);
        Serial.println(buffer);
        va_end(args);
    }
}

void Logger::log(LogLevel level, const char* format, ...) {
    if (currentLevel >= level && level > LOG_NONE) {
        char timestamp[16];
        getTimestamp(timestamp, sizeof(timestamp));
        
        va_list args;
        va_start(args, format);
        Serial.printf("[%s] [%s] ", timestamp, levelNames[level]);
        char buffer[256];
        vsnprintf(buffer, sizeof(buffer), format, args);
        Serial.println(buffer);
        va_end(args);
    }
}