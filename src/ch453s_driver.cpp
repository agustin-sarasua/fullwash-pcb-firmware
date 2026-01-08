#include "ch453s_driver.h"
#include "logger.h"

// 7-segment patterns for digits 0-9 and some characters
// Bit order: DP G F E D C B A (active high, common cathode)
// Based on CH453 datasheet: SEG7=DP, SEG6-0 = G-A segments
const uint8_t CH453SDriver::SEGMENTS[] = {
    0x3F,  // 0: A B C D E F
    0x06,  // 1: B C
    0x5B,  // 2: A B D E G
    0x4F,  // 3: A B C D G
    0x66,  // 4: B C F G
    0x6D,  // 5: A C D F G
    0x7D,  // 6: A C D E F G
    0x07,  // 7: A B C
    0x7F,  // 8: A B C D E F G
    0x6F,  // 9: A B C D F G
};

// I2C pins for software I2C
static uint8_t _sdaPin = 21;
static uint8_t _sclPin = 22;

// Software I2C timing (microseconds)
static const int I2C_DELAY = 5;

CH453SDriver::CH453SDriver(TwoWire& wire)
    : _wire(wire), _i2cMutex(NULL), _brightness(8), _displayOn(false) {
}

// ============ Software I2C Implementation ============
// The CH453 requires full 8-bit control over the "address" byte
// which standard Arduino Wire library cannot provide (it reserves LSB for R/W)

static void i2c_delay() {
    delayMicroseconds(I2C_DELAY);
}

static void sda_high() {
    pinMode(_sdaPin, INPUT_PULLUP);  // Release SDA (pulled high)
}

static void sda_low() {
    pinMode(_sdaPin, OUTPUT);
    digitalWrite(_sdaPin, LOW);
}

static void scl_high() {
    pinMode(_sclPin, INPUT_PULLUP);  // Release SCL (pulled high)
    // Wait for clock stretching
    while (digitalRead(_sclPin) == LOW);
}

static void scl_low() {
    pinMode(_sclPin, OUTPUT);
    digitalWrite(_sclPin, LOW);
}

static void i2c_start() {
    sda_high();
    i2c_delay();
    scl_high();
    i2c_delay();
    sda_low();  // SDA goes low while SCL is high = START
    i2c_delay();
    scl_low();
    i2c_delay();
}

static void i2c_stop() {
    sda_low();
    i2c_delay();
    scl_high();
    i2c_delay();
    sda_high();  // SDA goes high while SCL is high = STOP
    i2c_delay();
}

static bool i2c_write_byte(uint8_t data) {
    // Send 8 bits, MSB first
    for (int i = 7; i >= 0; i--) {
        if (data & (1 << i)) {
            sda_high();
        } else {
            sda_low();
        }
        i2c_delay();
        scl_high();
        i2c_delay();
        scl_low();
        i2c_delay();
    }
    
    // Read ACK (9th clock)
    sda_high();  // Release SDA for slave to ACK
    i2c_delay();
    scl_high();
    i2c_delay();
    bool ack = (digitalRead(_sdaPin) == LOW);  // ACK = SDA low
    scl_low();
    i2c_delay();
    
    return ack;
}

// Send a CH453 command using software I2C
// Format: START + cmdByte + dataByte + STOP
static bool ch453_send(uint8_t cmdByte, uint8_t dataByte) {
    i2c_start();
    
    bool ack1 = i2c_write_byte(cmdByte);
    bool ack2 = i2c_write_byte(dataByte);
    
    i2c_stop();
    
    return ack1 && ack2;
}

// Send a CH453 command (single byte, no data)
static bool ch453_send_cmd(uint8_t cmdByte) {
    i2c_start();
    bool ack = i2c_write_byte(cmdByte);
    i2c_stop();
    return ack;
}

// ============ End Software I2C ============

bool CH453SDriver::begin(uint8_t brightness) {
    _brightness = brightness;
    
    LOG_INFO("=== CH453 Initialization (Software I2C) ===");
    
    // Configure I2C pins
    _sdaPin = 21;  // From utilities.h DISPLAY_SDA_PIN
    _sclPin = 22;  // From utilities.h DISPLAY_SCL_PIN
    
    // Initialize pins as inputs with pull-up (I2C idle state)
    pinMode(_sdaPin, INPUT_PULLUP);
    pinMode(_sclPin, INPUT_PULLUP);
    delay(10);
    
    LOG_INFO("I2C pins: SDA=%d, SCL=%d", _sdaPin, _sclPin);
    
    // Send system command to turn on display
    // Command 0x49 = 0100_1001 = Display ON, 8-segment mode
    LOG_INFO("Step 1: Turn on display (cmd 0x49)...");
    bool success = ch453_send_cmd(0x49);
    LOG_INFO("  System cmd 0x49: %s", success ? "ACK" : "NACK");
    delay(10);
    
    // Try alternative if first fails
    if (!success) {
        LOG_INFO("  Trying cmd 0x48 then 0x49...");
        ch453_send_cmd(0x48);
        delay(5);
        success = ch453_send_cmd(0x49);
        delay(5);
    }
    
    // Step 2: Test write to each digit
    LOG_INFO("Step 2: Testing all 8 digits...");
    uint8_t pattern8 = SEGMENTS[8];  // '8' shows all segments
    
    for (uint8_t digit = 0; digit < 8; digit++) {
        uint8_t cmdByte = 0x60 + digit;  // 0x60-0x67 for digits 0-7
        bool ack = ch453_send(cmdByte, pattern8);
        LOG_INFO("  Digit %d (cmd 0x%02X): %s", digit, cmdByte, ack ? "ACK" : "NACK");
        delay(10);
    }
    
    // Brief delay to see test pattern
    LOG_INFO("Test pattern displayed for 1 second...");
    delay(1000);
    
    // Clear all digits
    LOG_INFO("Step 3: Clearing display...");
    for (uint8_t digit = 0; digit < 8; digit++) {
        ch453_send(0x60 + digit, 0x00);
        delay(5);
    }
    
    _displayOn = true;
    LOG_INFO("=== CH453 Init complete ===");
    
    return success;
}

void CH453SDriver::scanI2CBus() {
    LOG_INFO("I2C Bus Scan:");
    int devicesFound = 0;
    
    for (uint8_t addr = 1; addr < 127; addr++) {
        _wire.beginTransmission(addr);
        uint8_t error = _wire.endTransmission();
        
        if (error == 0) {
            LOG_INFO("  Found: 0x%02X", addr);
            devicesFound++;
        }
    }
    
    LOG_INFO("  Total: %d device(s)", devicesFound);
}

bool CH453SDriver::sendSystemCommand(uint8_t cmd) {
    bool hasMutex = false;
    if (_i2cMutex != NULL) {
        if (xSemaphoreTake(_i2cMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            hasMutex = true;
        } else {
            return false;
        }
    }
    
    bool result = ch453_send_cmd(cmd);
    
    if (hasMutex) {
        xSemaphoreGive(_i2cMutex);
    }
    
    return result;
}

bool CH453SDriver::writeDigitData(uint8_t digit, uint8_t segmentData) {
    if (digit > 15) return false;
    
    uint8_t cmdByte;
    if (digit < 8) {
        cmdByte = 0x60 + digit;  // DIG0-DIG7: 0x60-0x67
    } else {
        cmdByte = 0x70 + (digit - 8);  // DIG8-DIG15: 0x70-0x77
    }
    
    bool hasMutex = false;
    if (_i2cMutex != NULL) {
        if (xSemaphoreTake(_i2cMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            hasMutex = true;
        } else {
            return false;
        }
    }
    
    bool result = ch453_send(cmdByte, segmentData);
    
    if (hasMutex) {
        xSemaphoreGive(_i2cMutex);
    }
    
    return result;
}

void CH453SDriver::setI2CMutex(SemaphoreHandle_t mutex) {
    _i2cMutex = mutex;
}

void CH453SDriver::setBrightness(uint8_t brightness) {
    if (brightness > 15) brightness = 15;
    _brightness = brightness;
    // CH453 brightness is limited - try different system commands
    // For now, just ensure display is on
    sendSystemCommand(0x49);
}

bool CH453SDriver::sendCommand(uint16_t cmd) {
    if ((cmd & 0xFF00) == 0x0400) {
        return sendSystemCommand(cmd & 0xFF);
    } else if ((cmd & 0xF000) == 0x1000) {
        uint8_t digit = (cmd >> 8) & 0x0F;
        uint8_t data = cmd & 0xFF;
        return writeDigitData(digit, data);
    }
    return false;
}

bool CH453SDriver::sendCH453Command(uint16_t cmd) {
    return sendCommand(cmd);
}

uint8_t CH453SDriver::getSegmentPattern(char c) {
    if (c >= '0' && c <= '9') {
        return SEGMENTS[c - '0'];
    }
    
    switch (c) {
        case '-': return 0x40;  // G segment only
        case '_': return 0x08;  // D segment only
        case ' ': return 0x00;  // All off
        case 'A': case 'a': return 0x77;
        case 'b': return 0x7C;
        case 'C': return 0x39;
        case 'c': return 0x58;
        case 'd': return 0x5E;
        case 'E': case 'e': return 0x79;
        case 'F': case 'f': return 0x71;
        case 'H': return 0x76;
        case 'h': return 0x74;
        case 'I': case 'i': return 0x06;
        case 'J': case 'j': return 0x1E;
        case 'L': case 'l': return 0x38;
        case 'n': return 0x54;
        case 'o': return 0x5C;
        case 'P': case 'p': return 0x73;
        case 'r': return 0x50;
        case 'S': case 's': return 0x6D;
        case 't': return 0x78;
        case 'U': return 0x3E;
        case 'u': return 0x1C;
        case 'Y': case 'y': return 0x6E;
        default: return 0x00;
    }
}

void CH453SDriver::setDigit(uint8_t digit, uint8_t segments) {
    if (digit > 15) return;
    writeDigitData(digit, segments);
}

void CH453SDriver::setCharacter(uint8_t digit, char character, bool decimal) {
    uint8_t pattern = getSegmentPattern(character);
    if (decimal) {
        pattern |= 0x80;  // Set decimal point bit (DP = SEG7)
    }
    setDigit(digit, pattern);
}

void CH453SDriver::clear() {
    for (uint8_t i = 0; i < 8; i++) {
        setDigit(i, 0x00);
    }
}

void CH453SDriver::clearTop() {
    for (uint8_t i = 0; i < 4; i++) {
        setDigit(i, 0x00);
    }
}

void CH453SDriver::clearBottom() {
    for (uint8_t i = 4; i < 8; i++) {
        setDigit(i, 0x00);
    }
}

void CH453SDriver::displayTopNumber(uint16_t value, bool leadingZeros) {
    if (value > 9999) value = 9999;
    
    // Extract digits
    uint8_t thousands = (value / 1000) % 10;
    uint8_t hundreds = (value / 100) % 10;
    uint8_t tens = (value / 10) % 10;
    uint8_t ones = value % 10;
    
    // Determine first non-zero for leading zero suppression
    int firstNonZero = 3;
    if (thousands > 0) firstNonZero = 0;
    else if (hundreds > 0) firstNonZero = 1;
    else if (tens > 0) firstNonZero = 2;
    
    // Top display mapping - adjust based on physical wiring
    // Try: DIG0=rightmost (ones), DIG3=leftmost (thousands)
    if (leadingZeros || firstNonZero <= 0)
        setDigit(3, SEGMENTS[thousands]);
    else
        setDigit(3, 0x00);
        
    if (leadingZeros || firstNonZero <= 1)
        setDigit(2, SEGMENTS[hundreds]);
    else
        setDigit(2, 0x00);
        
    if (leadingZeros || firstNonZero <= 2)
        setDigit(1, SEGMENTS[tens]);
    else
        setDigit(1, 0x00);
        
    setDigit(0, SEGMENTS[ones]);  // Always show ones
}

void CH453SDriver::displayBottomDecimal(float value, uint8_t decimalPlaces) {
    if (value < 0) value = 0;
    if (value > 99.9f) value = 99.9f;
    
    uint16_t scaledValue = (uint16_t)(value * 10 + 0.5f);
    
    uint8_t tens = (scaledValue / 100) % 10;
    uint8_t ones = (scaledValue / 10) % 10;
    uint8_t tenths = scaledValue % 10;
    
    // Bottom display: DIG4-DIG7
    // DIG7=leftmost, DIG4=rightmost (reversed)
    if (tens > 0) {
        setDigit(7, SEGMENTS[tens]);
    } else {
        setDigit(7, 0x00);  // Blank
    }
    
    setDigit(6, SEGMENTS[ones] | 0x80);  // Ones with decimal point
    setDigit(5, SEGMENTS[tenths]);        // Tenths
    setDigit(4, 0x00);                    // Blank (rightmost)
}

void CH453SDriver::displayDashes(bool top) {
    uint8_t dashPattern = 0x40;  // G segment = dash
    
    if (top) {
        for (uint8_t i = 0; i < 4; i++) {
            setDigit(i, dashPattern);
        }
    } else {
        for (uint8_t i = 4; i < 8; i++) {
            setDigit(i, dashPattern);
        }
    }
}

void CH453SDriver::setDisplayOn(bool on) {
    if (on) {
        sendSystemCommand(0x49);  // Display ON
    } else {
        sendSystemCommand(0x48);  // Display OFF
    }
    _displayOn = on;
}
