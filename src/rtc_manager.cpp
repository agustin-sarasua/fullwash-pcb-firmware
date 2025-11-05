#include "rtc_manager.h"

RTCManager::RTCManager(uint8_t address, TwoWire* wireInterface)
    : _address(address), _wire(wireInterface), _initialized(false),
      _lastReadMillis(0), _lastReadTime(0) {
}

bool RTCManager::begin() {
    if (!_wire) {
        LOG_ERROR("Wire interface is null!");
        return false;
    }
    
    // Note: Wire1 should already be initialized with begin(SDA, SCL) in main.cpp
    // We don't call _wire->begin() here to avoid re-initializing
    
    LOG_INFO("Initializing DS1340 RTC at address 0x%02X", _address);
    
    // Check if RTC is responding
    _wire->beginTransmission(_address);
    uint8_t error = _wire->endTransmission();
    
    if (error != 0) {
        LOG_ERROR("DS1340 RTC not found! I2C error code: %d", error);
        LOG_ERROR("  0=success, 1=data too long, 2=NACK on address, 3=NACK on data, 4=other");
        return false;
    }
    
    LOG_INFO("DS1340 RTC found!");
    
    // Mark as initialized now that we've confirmed the RTC responds
    // This allows isOscillatorRunning() and startOscillator() to work
    _initialized = true;
    
    // Check if oscillator is running
    if (!isOscillatorRunning()) {
        LOG_WARNING("RTC oscillator is stopped! Starting it now...");
        if (!startOscillator()) {
            LOG_ERROR("Failed to start RTC oscillator!");
            return false;
        }
        LOG_INFO("RTC oscillator started successfully");
    } else {
        LOG_INFO("RTC oscillator is running");
    }
    
    // Read current time for verification
    time_t currentTime = getDateTime();
    if (currentTime > 0) {
        LOG_INFO("RTC current time: %lu (epoch)", (unsigned long)currentTime);
        
        // Check if time is reasonable (after 2020-01-01)
        if (currentTime < 1577836800UL) { // 2020-01-01 00:00:00 UTC
            LOG_WARNING("RTC time seems too old (before 2020). Time needs to be set.");
        } else {
            LOG_INFO("RTC time is valid: %s", getTimestamp().c_str());
        }
    } else {
        LOG_WARNING("Failed to read RTC time during initialization");
    }
    
    return true;
}

uint8_t RTCManager::decToBcd(uint8_t val) {
    return ((val / 10) << 4) | (val % 10);
}

uint8_t RTCManager::bcdToDec(uint8_t val) {
    return ((val >> 4) * 10) + (val & 0x0F);
}

bool RTCManager::writeRegister(uint8_t reg, uint8_t value) {
    if (!_initialized) return false;
    
    _wire->beginTransmission(_address);
    _wire->write(reg);
    _wire->write(value);
    uint8_t error = _wire->endTransmission();
    
    if (error != 0) {
        LOG_ERROR("Failed to write RTC register 0x%02X: error %d", reg, error);
        return false;
    }
    
    return true;
}

uint8_t RTCManager::readRegister(uint8_t reg) {
    if (!_initialized) return 0;
    
    _wire->beginTransmission(_address);
    _wire->write(reg);
    uint8_t error = _wire->endTransmission();
    
    if (error != 0) {
        LOG_ERROR("Failed to set RTC register pointer to 0x%02X: error %d", reg, error);
        return 0;
    }
    
    uint8_t bytesReceived = _wire->requestFrom(_address, (uint8_t)1);
    if (bytesReceived != 1) {
        LOG_ERROR("Failed to read RTC register 0x%02X: expected 1 byte, got %d", reg, bytesReceived);
        return 0;
    }
    
    return _wire->read();
}

bool RTCManager::readRegisters(uint8_t reg, uint8_t* buffer, uint8_t length) {
    if (!_initialized || !buffer) return false;
    
    _wire->beginTransmission(_address);
    _wire->write(reg);
    uint8_t error = _wire->endTransmission();
    
    if (error != 0) {
        LOG_ERROR("Failed to set RTC register pointer: error %d", error);
        return false;
    }
    
    uint8_t bytesReceived = _wire->requestFrom(_address, length);
    if (bytesReceived != length) {
        LOG_ERROR("Failed to read %d RTC registers: got %d bytes", length, bytesReceived);
        return false;
    }
    
    for (uint8_t i = 0; i < length; i++) {
        buffer[i] = _wire->read();
    }
    
    return true;
}

bool RTCManager::writeRegisters(uint8_t reg, const uint8_t* buffer, uint8_t length) {
    if (!_initialized || !buffer) return false;
    
    _wire->beginTransmission(_address);
    _wire->write(reg);
    
    for (uint8_t i = 0; i < length; i++) {
        _wire->write(buffer[i]);
    }
    
    uint8_t error = _wire->endTransmission();
    
    if (error != 0) {
        LOG_ERROR("Failed to write %d RTC registers: error %d", length, error);
        return false;
    }
    
    return true;
}

bool RTCManager::isOscillatorRunning() {
    if (!_initialized) return false;
    
    uint8_t seconds = readRegister(REG_SECONDS);
    // Bit 7 of seconds register is the oscillator stop flag (OSF)
    // If bit 7 is 1, oscillator is stopped or was stopped
    bool oscillatorStopped = (seconds & 0x80) != 0;
    
    return !oscillatorStopped;
}

bool RTCManager::startOscillator() {
    if (!_initialized) return false;
    
    // Clear the oscillator stop flag (bit 7 of seconds register)
    uint8_t seconds = readRegister(REG_SECONDS);
    seconds &= 0x7F; // Clear bit 7
    
    return writeRegister(REG_SECONDS, seconds);
}

bool RTCManager::isTimeValid() {
    if (!_initialized) {
        return false;
    }
    
    // Check if oscillator is running
    if (!isOscillatorRunning()) {
        LOG_DEBUG("RTC time invalid: oscillator is stopped");
        return false;
    }
    
    // Check if time is reasonable (after 2020-01-01)
    time_t currentTime = getDateTime();
    if (currentTime == 0) {
        LOG_DEBUG("RTC time invalid: failed to read time");
        return false;
    }
    
    // 2020-01-01 00:00:00 UTC = 1577836800
    if (currentTime < 1577836800UL) {
        LOG_WARNING("RTC time invalid: time is before 2020 (epoch: %lu, ISO: %s)", 
                   (unsigned long)currentTime, getTimestamp().c_str());
        return false;
    }
    
    // Check if time is not too far in the future (e.g., 2100-01-01 = 4102444800)
    if (currentTime > 4102444800UL) {
        LOG_DEBUG("RTC time invalid: time is too far in future (epoch: %lu)", (unsigned long)currentTime);
        return false;
    }
    
    return true;
}

bool RTCManager::setDateTime(time_t epochTime) {
    if (!_initialized || epochTime == 0) return false;
    
    // Convert epoch time to date/time components
    tmElements_t tm;
    breakTime(epochTime, tm);
    
    // DS1340 uses year 0-99 (representing 2000-2099)
    uint16_t year = tm.Year + 1970;
    if (year < 2000 || year > 2099) {
        LOG_ERROR("Year %d out of range for DS1340 (2000-2099)", year);
        return false;
    }
    
    return setDateTime(year, tm.Month, tm.Day, tm.Hour, tm.Minute, tm.Second);
}

bool RTCManager::setDateTime(uint16_t year, uint8_t month, uint8_t day, 
                              uint8_t hour, uint8_t minute, uint8_t second) {
    if (!_initialized) return false;
    
    // Validate inputs
    if (year < 2000 || year > 2099 || month < 1 || month > 12 || 
        day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
        LOG_ERROR("Invalid date/time parameters");
        return false;
    }
    
    // Prepare data buffer
    uint8_t data[7];
    data[0] = decToBcd(second) & 0x7F; // Clear oscillator stop flag (bit 7)
    data[1] = decToBcd(minute);
    data[2] = decToBcd(hour);          // 24-hour format
    data[3] = 1;                       // Day of week (1-7, not used but required)
    data[4] = decToBcd(day);
    data[5] = decToBcd(month);
    data[6] = decToBcd(year - 2000);   // DS1340 stores year as offset from 2000
    
    LOG_INFO("Setting RTC time: %04d-%02d-%02d %02d:%02d:%02d", 
             year, month, day, hour, minute, second);
    
    // Log what we're about to write
    LOG_DEBUG("Writing RTC registers: [0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X]",
              data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
    
    // Write all registers at once
    bool success = writeRegisters(REG_SECONDS, data, 7);
    
    if (success) {
        LOG_INFO("RTC time set successfully");
        
        // Verify the write by reading back immediately
        delay(10); // Small delay to ensure write is complete
        time_t readBack = getDateTime();
        if (readBack > 0) {
            LOG_INFO("RTC write verified - read back: epoch=%lu", (unsigned long)readBack);
            if (readBack < 1577836800UL) {
                LOG_ERROR("RTC write verification FAILED - read back invalid time (epoch=%lu)", 
                         (unsigned long)readBack);
            }
        } else {
            LOG_WARNING("RTC write verification - failed to read back time");
        }
        
        // Update cached values
        _lastReadMillis = millis();
        
        // Reconstruct epoch time for cache
        tmElements_t tm;
        tm.Year = year - 1970;
        tm.Month = month;
        tm.Day = day;
        tm.Hour = hour;
        tm.Minute = minute;
        tm.Second = second;
        _lastReadTime = makeTime(tm);
    } else {
        LOG_ERROR("Failed to set RTC time");
    }
    
    return success;
}

bool RTCManager::setDateTimeFromISO(const String& isoTimestamp) {
    if (!_initialized || isoTimestamp.length() == 0) return false;
    
    LOG_INFO("Parsing ISO timestamp: %s", isoTimestamp.c_str());
    
    // Parse ISO 8601 format: "2024-10-29T15:30:45.123Z" or "2024-10-29T15:30:45.123+00:00"
    String ts = isoTimestamp;
    int tPos = ts.indexOf('T');
    
    if (tPos <= 0) {
        LOG_ERROR("Invalid ISO timestamp format: no 'T' separator");
        return false;
    }
    
    // Extract date components
    int year = ts.substring(0, 4).toInt();
    int month = ts.substring(5, 7).toInt();
    int day = ts.substring(8, 10).toInt();
    
    // Extract time components
    int hour = ts.substring(tPos + 1, tPos + 3).toInt();
    int minute = ts.substring(tPos + 4, tPos + 6).toInt();
    int second = ts.substring(tPos + 7, tPos + 9).toInt();
    
    LOG_DEBUG("Parsed components: %04d-%02d-%02d %02d:%02d:%02d", 
              year, month, day, hour, minute, second);
    
    return setDateTime(year, month, day, hour, minute, second);
}

time_t RTCManager::getDateTime() {
    if (!_initialized) return 0;
    
    // Read all time registers at once
    uint8_t data[7];
    if (!readRegisters(REG_SECONDS, data, 7)) {
        LOG_ERROR("Failed to read RTC time");
        return 0;
    }
    
    // Convert BCD to decimal
    uint8_t second = bcdToDec(data[0] & 0x7F); // Mask out oscillator stop flag
    uint8_t minute = bcdToDec(data[1]);
    uint8_t hour = bcdToDec(data[2] & 0x3F);   // Mask out unused bits
    // data[3] is day of week, skip it
    uint8_t day = bcdToDec(data[4]);
    uint8_t month = bcdToDec(data[5]);
    uint8_t year = bcdToDec(data[6]);
    
    // Log raw register values for debugging
    LOG_DEBUG("RTC raw registers: [0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X] -> "
              "%04d-%02d-%02d %02d:%02d:%02d",
              data[0], data[1], data[2], data[3], data[4], data[5], data[6],
              2000 + year, month, day, hour, minute, second);
    
    // Check if oscillator was stopped
    if (data[0] & 0x80) {
        LOG_WARNING("RTC oscillator stop flag is set! Time may be invalid.");
    }
    
    // Convert to epoch time
    tmElements_t tm;
    tm.Year = year + 2000 - 1970; // Convert to years since 1970
    tm.Month = month;
    tm.Day = day;
    tm.Hour = hour;
    tm.Minute = minute;
    tm.Second = second;
    
    time_t epochTime = makeTime(tm);
    
    // Log if time seems invalid
    if (epochTime < 1577836800UL) { // Before 2020-01-01
        LOG_WARNING("RTC read invalid time: epoch=%lu -> %04d-%02d-%02d %02d:%02d:%02d (year=%d)",
                   (unsigned long)epochTime, 2000 + year, month, day, hour, minute, second, year);
    }
    
    // Cache the reading
    _lastReadTime = epochTime;
    _lastReadMillis = millis();
    
    return epochTime;
}

String RTCManager::getTimestamp() {
    time_t currentTime = getDateTime();
    
    if (currentTime == 0) {
        return "RTC Error";
    }
    
    // Convert to time structure
    tmElements_t tm;
    breakTime(currentTime, tm);
    
    // Format as ISO 8601: "2024-10-29T15:30:45Z"
    char timestamp[25];
    sprintf(timestamp, "%04d-%02d-%02dT%02d:%02d:%02dZ",
            tm.Year + 1970, tm.Month, tm.Day,
            tm.Hour, tm.Minute, tm.Second);
    
    return String(timestamp);
}

String RTCManager::getTimestampWithMillis() {
    time_t currentTime = getDateTime();
    
    if (currentTime == 0) {
        LOG_WARNING("RTC getDateTime() returned 0 - RTC read failed");
        return "RTC Error";
    }
    
    // Log if time is invalid (for debugging)
    if (!isTimeValid()) {
        LOG_WARNING("RTC time is invalid (epoch: %lu) but still returning timestamp", (unsigned long)currentTime);
    }
    
    // Calculate milliseconds since last RTC read
    unsigned long currentMillis = millis();
    unsigned long millisOffset = currentMillis - _lastReadMillis;
    
    // Adjust time if more than 1 second has passed
    time_t adjustedTime = currentTime;
    int milliseconds = millisOffset % 1000;
    
    if (millisOffset >= 1000) {
        adjustedTime += millisOffset / 1000;
    }
    
    // Convert to time structure
    tmElements_t tm;
    breakTime(adjustedTime, tm);
    
    // Format as ISO 8601 with milliseconds: "2024-10-29T15:30:45.123Z"
    char timestamp[30];
    sprintf(timestamp, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
            tm.Year + 1970, tm.Month, tm.Day,
            tm.Hour, tm.Minute, tm.Second, milliseconds);
    
    return String(timestamp);
}

void RTCManager::printDebugInfo() {
    if (!_initialized) {
        LOG_WARNING("RTC not initialized");
        return;
    }
    
    LOG_DEBUG("==== RTC Debug Info ====");
    LOG_DEBUG("I2C Address: 0x%02X", _address);
    LOG_DEBUG("Oscillator Running: %s", isOscillatorRunning() ? "Yes" : "No");
    
    time_t currentTime = getDateTime();
    if (currentTime > 0) {
        LOG_DEBUG("Current Time (epoch): %lu", (unsigned long)currentTime);
        LOG_DEBUG("Current Time (ISO): %s", getTimestamp().c_str());
        LOG_DEBUG("With Millis: %s", getTimestampWithMillis().c_str());
    } else {
        LOG_DEBUG("Failed to read current time");
    }
    
    // Read raw register values
    uint8_t data[7];
    if (readRegisters(REG_SECONDS, data, 7)) {
        LOG_DEBUG("Raw Registers:");
        LOG_DEBUG("  Seconds: 0x%02X", data[0]);
        LOG_DEBUG("  Minutes: 0x%02X", data[1]);
        LOG_DEBUG("  Hours:   0x%02X", data[2]);
        LOG_DEBUG("  DoW:     0x%02X", data[3]);
        LOG_DEBUG("  Date:    0x%02X", data[4]);
        LOG_DEBUG("  Month:   0x%02X", data[5]);
        LOG_DEBUG("  Year:    0x%02X", data[6]);
    }
    
    LOG_DEBUG("========================");
}


