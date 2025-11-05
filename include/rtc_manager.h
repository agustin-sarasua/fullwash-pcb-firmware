#ifndef RTC_MANAGER_H
#define RTC_MANAGER_H

#include <Arduino.h>
#include <Wire.h>
#include <TimeLib.h>
#include "logger.h"

/**
 * @brief RTC Manager for DS1340Z Real-Time Clock
 * 
 * This class provides an interface to communicate with the DS1340Z RTC chip
 * via I2C. The RTC maintains accurate time even when the main system is powered off
 * (with backup battery) and is used as the primary time source for the system.
 * 
 * Features:
 * - Read/write date and time
 * - Initialize RTC with current time from server
 * - Battery backup support
 * - ISO 8601 timestamp formatting
 * - 24-hour format
 * 
 * I2C Connection:
 * - Uses Wire1 (same bus as LCD)
 * - Default address: 0x68
 * - SCL: IO22, SDA: IO21
 */
class RTCManager {
public:
    /**
     * @brief Construct a new RTCManager object
     * 
     * @param address I2C address of the DS1340 (default 0x68)
     * @param wireInterface Pointer to Wire interface to use (default &Wire1)
     */
    RTCManager(uint8_t address = 0x68, TwoWire* wireInterface = &Wire1);
    
    /**
     * @brief Initialize the RTC
     * 
     * Checks if the RTC is responding and verifies oscillator is running.
     * Does NOT set the time - use setDateTime() for that.
     * 
     * @return true if RTC is responding and oscillator is running
     * @return false if RTC is not found or oscillator is stopped
     */
    bool begin();
    
    /**
     * @brief Check if RTC is initialized and responding
     * 
     * @return true if RTC is ready
     * @return false if RTC is not initialized
     */
    bool isInitialized() const { return _initialized; }
    
    /**
     * @brief Set the RTC date and time from epoch timestamp
     * 
     * @param epochTime Unix timestamp (seconds since 1970-01-01 00:00:00 UTC)
     * @return true if time was set successfully
     * @return false if RTC is not initialized or write failed
     */
    bool setDateTime(time_t epochTime);
    
    /**
     * @brief Set the RTC date and time from individual components
     * 
     * @param year Year (2000-2099)
     * @param month Month (1-12)
     * @param day Day of month (1-31)
     * @param hour Hour (0-23)
     * @param minute Minute (0-59)
     * @param second Second (0-59)
     * @return true if time was set successfully
     * @return false if RTC is not initialized or parameters invalid
     */
    bool setDateTime(uint16_t year, uint8_t month, uint8_t day, 
                     uint8_t hour, uint8_t minute, uint8_t second);
    
    /**
     * @brief Set the RTC time from an ISO 8601 timestamp string
     * 
     * Parses timestamps in format: "2024-10-29T15:30:45.123Z" or "2024-10-29T15:30:45.123+00:00"
     * 
     * @param isoTimestamp ISO 8601 formatted timestamp string
     * @return true if parsing and setting succeeded
     * @return false if parsing failed or RTC not initialized
     */
    bool setDateTimeFromISO(const String& isoTimestamp);
    
    /**
     * @brief Get current time from RTC as epoch timestamp
     * 
     * @return time_t Unix timestamp, or 0 if RTC not initialized or read failed
     */
    time_t getDateTime();
    
    /**
     * @brief Get current time as ISO 8601 formatted string
     * 
     * Format: "2024-10-29T15:30:45Z"
     * 
     * @return String ISO 8601 timestamp, or "RTC Error" if read failed
     */
    String getTimestamp();
    
    /**
     * @brief Get current time as ISO 8601 formatted string with milliseconds
     * 
     * Uses millis() to add sub-second precision
     * Format: "2024-10-29T15:30:45.123Z"
     * 
     * @return String ISO 8601 timestamp with milliseconds
     */
    String getTimestampWithMillis();
    
    /**
     * @brief Check if the RTC oscillator is running
     * 
     * @return true if oscillator is running (time is being kept)
     * @return false if oscillator is stopped (time is not being kept)
     */
    bool isOscillatorRunning();
    
    /**
     * @brief Start the RTC oscillator if it's stopped
     * 
     * @return true if oscillator was started successfully
     * @return false if RTC not initialized or operation failed
     */
    bool startOscillator();
    
    /**
     * @brief Check if RTC time is valid and doesn't need sync
     * 
     * RTC time is considered valid if:
     * - RTC is initialized
     * - Time is after 2020-01-01 (reasonable starting point)
     * - Oscillator is running
     * 
     * @return true if RTC time is valid (no sync needed)
     * @return false if RTC time is invalid or needs sync
     */
    bool isTimeValid();
    
    /**
     * @brief Print current RTC time to serial for debugging
     */
    void printDebugInfo();

private:
    uint8_t _address;           ///< I2C address of the DS1340
    TwoWire* _wire;             ///< Pointer to Wire interface (Wire1)
    bool _initialized;          ///< Initialization status
    unsigned long _lastReadMillis; ///< Millis value from last RTC read
    time_t _lastReadTime;       ///< Time value from last RTC read
    
    // DS1340 Register addresses
    static const uint8_t REG_SECONDS = 0x00;
    static const uint8_t REG_MINUTES = 0x01;
    static const uint8_t REG_HOURS = 0x02;
    static const uint8_t REG_DAY = 0x03;
    static const uint8_t REG_DATE = 0x04;
    static const uint8_t REG_MONTH = 0x05;
    static const uint8_t REG_YEAR = 0x06;
    static const uint8_t REG_CONTROL = 0x07;
    
    // Control register bits
    static const uint8_t CTRL_EOSC = 0x80;  ///< Enable oscillator bit (0 = enabled, 1 = disabled)
    
    /**
     * @brief Convert decimal to BCD (Binary Coded Decimal)
     * 
     * @param val Decimal value (0-99)
     * @return uint8_t BCD encoded value
     */
    uint8_t decToBcd(uint8_t val);
    
    /**
     * @brief Convert BCD to decimal
     * 
     * @param val BCD encoded value
     * @return uint8_t Decimal value
     */
    uint8_t bcdToDec(uint8_t val);
    
    /**
     * @brief Write a single byte to RTC register
     * 
     * @param reg Register address
     * @param value Value to write
     * @return true if write succeeded
     * @return false if write failed
     */
    bool writeRegister(uint8_t reg, uint8_t value);
    
    /**
     * @brief Read a single byte from RTC register
     * 
     * @param reg Register address
     * @return uint8_t Register value, or 0 if read failed
     */
    uint8_t readRegister(uint8_t reg);
    
    /**
     * @brief Read multiple bytes from RTC starting at register
     * 
     * @param reg Starting register address
     * @param buffer Buffer to store read data
     * @param length Number of bytes to read
     * @return true if read succeeded
     * @return false if read failed
     */
    bool readRegisters(uint8_t reg, uint8_t* buffer, uint8_t length);
    
    /**
     * @brief Write multiple bytes to RTC starting at register
     * 
     * @param reg Starting register address
     * @param buffer Buffer containing data to write
     * @param length Number of bytes to write
     * @return true if write succeeded
     * @return false if write failed
     */
    bool writeRegisters(uint8_t reg, const uint8_t* buffer, uint8_t length);
};

#endif // RTC_MANAGER_H


