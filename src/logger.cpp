#include "../include/logger.h"
#include <stdarg.h>

LogLevel Logger::currentLevel = LOG_INFO;
bool Logger::initialized = false;
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

void Logger::error(const char* format, ...) {
    if (currentLevel >= LOG_ERROR) {
        va_list args;
        va_start(args, format);
        Serial.print("[ERROR] ");
        char buffer[256];
        vsnprintf(buffer, sizeof(buffer), format, args);
        Serial.println(buffer);
        va_end(args);
    }
}

void Logger::warning(const char* format, ...) {
    if (currentLevel >= LOG_WARNING) {
        va_list args;
        va_start(args, format);
        Serial.print("[WARNING] ");
        char buffer[256];
        vsnprintf(buffer, sizeof(buffer), format, args);
        Serial.println(buffer);
        va_end(args);
    }
}

void Logger::info(const char* format, ...) {
    if (currentLevel >= LOG_INFO) {
        va_list args;
        va_start(args, format);
        Serial.print("[INFO] ");
        char buffer[256];
        vsnprintf(buffer, sizeof(buffer), format, args);
        Serial.println(buffer);
        va_end(args);
    }
}

void Logger::debug(const char* format, ...) {
    if (currentLevel >= LOG_DEBUG) {
        va_list args;
        va_start(args, format);
        Serial.print("[DEBUG] ");
        char buffer[256];
        vsnprintf(buffer, sizeof(buffer), format, args);
        Serial.println(buffer);
        va_end(args);
    }
}

void Logger::log(LogLevel level, const char* format, ...) {
    if (currentLevel >= level && level > LOG_NONE) {
        va_list args;
        va_start(args, format);
        Serial.printf("[%s] ", levelNames[level]);
        char buffer[256];
        vsnprintf(buffer, sizeof(buffer), format, args);
        Serial.println(buffer);
        va_end(args);
    }
}