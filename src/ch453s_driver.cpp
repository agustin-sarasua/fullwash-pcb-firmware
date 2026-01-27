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

// CH453 "System Parameter" command is always 2 bytes:
// byte1 = 0x48, byte2 = [SLEEP][INTENS]0[X_INT]0[KEYB][DISP]
// See datasheet section 6.1. "Setting of System Parameter Commands"
static uint8_t ch453_build_sys_param(bool disp, uint8_t intens2bits, bool keyb, bool x_int, bool sleep) {
    intens2bits &= 0x03;
    return (sleep ? 0x80 : 0x00) |
           ((intens2bits & 0x03) << 5) |
           (x_int ? 0x08 : 0x00) |
           (keyb ? 0x02 : 0x00) |
           (disp ? 0x01 : 0x00);
}

static uint8_t ch453_intensity_from_brightness(uint8_t brightness0_15) {
    // CH453 only supports 2-bit intensity selection (plus a "no limiter" mode),
    // so we map the 0-15 UI brightness into a reasonable default.
    // INTENS:
    // - 00: duty 4/4 with internal current limiter enabled (brightest, recommended)
    // - 01: duty 1/4 with limiter enabled
    // - 10: duty 2/4 with limiter enabled
    // - 11: duty 4/4 but limiter disabled (needs external resistors)
    if (brightness0_15 <= 5) return 0x01;   // 1/4
    if (brightness0_15 <= 10) return 0x02;  // 2/4
    return 0x00;                            // 4/4 (limiter enabled)
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
    
    // Proper system parameter setup: byte1 = 0x48, byte2 = config bits
    // We keep KEYB=0 (no keyboard scan), X_INT=0 (support DIG0-DIG15), SLEEP=0.
    uint8_t intens = ch453_intensity_from_brightness(_brightness);
    uint8_t sysParam = ch453_build_sys_param(true /*disp*/, intens, false /*keyb*/, false /*x_int*/, false /*sleep*/);

    LOG_INFO("Step 1: Configure system params (cmd 0x48, param 0x%02X)...", sysParam);
    bool success = ch453_send(0x48, sysParam);
    LOG_INFO("  System cmd 0x48: %s", success ? "ACK" : "NACK");
    delay(10);
    
    // Step 2: Test write to each digit register (0-15)
    // Datasheet: "Word-data loading command" byte1 is 0x60,0x62,...,0x7E (even only)
    LOG_INFO("Step 2: Testing digit registers 0-15...");
    uint8_t pattern8 = SEGMENTS[8];  // '8' shows all segments
    
    for (uint8_t digit = 0; digit < 16; digit++) {
        uint8_t cmdByte = 0x60 + (digit << 1);  // 0x60,0x62,...0x7E
        bool ack = ch453_send(cmdByte, pattern8);
        LOG_INFO("  DIG%u (cmd 0x%02X): %s", digit, cmdByte, ack ? "ACK" : "NACK");
        delay(10);
    }
    
    // Brief delay to see test pattern
    LOG_INFO("Test pattern displayed for 1 second...");
    delay(1000);
    
    // Clear all digit registers (0-15)
    LOG_INFO("Step 3: Clearing display...");
    for (uint8_t digit = 0; digit < 16; digit++) {
        ch453_send(0x60 + (digit << 1), 0x00);
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
    
    // 'cmd' here is the system parameter byte2 (config bits).
    bool result = ch453_send(0x48, cmd);
    
    if (hasMutex) {
        xSemaphoreGive(_i2cMutex);
    }
    
    return result;
}

bool CH453SDriver::writeDigitData(uint8_t digit, uint8_t segmentData) {
    if (digit > 15) return false;
    
    // Datasheet section 6.2: byte1 is 011[DIG_ADDR]0B => 0x60,0x62,...,0x7E
    uint8_t cmdByte = 0x60 + (digit << 1);
    
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
    // Re-apply system params with updated intensity.
    uint8_t intens = ch453_intensity_from_brightness(_brightness);
    uint8_t sysParam = ch453_build_sys_param(true /*disp*/, intens, false /*keyb*/, false /*x_int*/, false /*sleep*/);
    sendSystemCommand(sysParam);
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
    // Display time in MM.SS format (Minutes:Seconds)
    // value is in seconds (e.g., 120 seconds = 02:00 = 2 minutes, 0 seconds)
    // 
    // SWAPPED: Now displays on BOTTOM display (digits 4-7) to show TIME on bottom
    
    if (value > 5999) value = 5999;  // Max 99:59 (99 minutes 59 seconds)
    
    // Convert seconds to MM.SS
    uint8_t minutes = value / 60;
    uint8_t seconds = value % 60;
    
    // Extract individual digits
    uint8_t minuteTens = minutes / 10;      // Tens of minutes (0-9)
    uint8_t minuteOnes = minutes % 10;      // Ones of minutes (0-9)
    uint8_t secondTens = seconds / 10;      // Tens of seconds (0-5)
    uint8_t secondOnes = seconds % 10;      // Ones of seconds (0-9)
    
    // Physical digit order:
    // BOTTOM display: DIG4, DIG5, DIG6, DIG7 (left to right)
    // We want: [minute tens][minute ones].[second tens][second ones]
    //
    // So:
    // - DIG4: minute tens (blank if !leadingZeros and minutes < 10)
    // - DIG5: minute ones + decimal point
    // - DIG6: second tens
    // - DIG7: second ones
    if (!leadingZeros && minuteTens == 0) {
        setDigit(4, 0x00);
    } else {
        setDigit(4, SEGMENTS[minuteTens]);
    }
    setDigit(5, SEGMENTS[minuteOnes] | 0x80);
    setDigit(6, SEGMENTS[secondTens]);
    setDigit(7, SEGMENTS[secondOnes]);
}

void CH453SDriver::displayBottomDecimal(float value, uint8_t decimalPlaces) {
    // Display token count as decimal (e.g., 1.00 tokens)
    // Format: "TT.UU" (two digits, decimal point, two digits)
    // Examples:
    // - 1.00 -> "01.00"
    // - 0.99 -> "00.99"
    //
    // SWAPPED: Now displays on TOP display (digits 0-3) to show COINS on top
    
    if (value < 0) value = 0;
    // Clamp to what fits in TT.UU (00.00 to 99.99)
    if (value > 99.99f) value = 99.99f;

    // Respect decimalPlaces if caller passes it, but default behavior is 2dp.
    // (In practice we want 2dp for tokens on a 4-digit module.)
    uint8_t dp = (decimalPlaces >= 2) ? 2 : 2;

    // Scale to integer representation (e.g., 1.00 -> 100, 0.99 -> 99)
    uint16_t scaledValue = (uint16_t)(value * 100.0f + 0.5f);
    uint8_t intPart = (scaledValue / 100) % 100;     // 0-99
    uint8_t fracPart = scaledValue % 100;            // 0-99

    uint8_t tens = intPart / 10;
    uint8_t ones = intPart % 10;
    uint8_t tenths = fracPart / 10;
    uint8_t hundredths = fracPart % 10;

    // Physical digit order: TOP display is DIG0, DIG1, DIG2, DIG3 (left to right)
    // We want: [tens][ones].[tenths][hundredths]
    setDigit(0, SEGMENTS[tens]);
    setDigit(1, SEGMENTS[ones] | 0x80);  // decimal point after ones
    setDigit(2, SEGMENTS[tenths]);
    setDigit(3, SEGMENTS[hundredths]);
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
    uint8_t intens = ch453_intensity_from_brightness(_brightness);
    uint8_t sysParam = ch453_build_sys_param(on /*disp*/, intens, false /*keyb*/, false /*x_int*/, false /*sleep*/);
    sendSystemCommand(sysParam);
    _displayOn = on;
}

void CH453SDriver::testDigitOrder() {
    // Test function to verify digit order
    // Displays "0123" on top and "4567" on bottom
    LOG_INFO("Testing digit order - Top: 0123, Bottom: 4567");
    
    // Top display
    // With current mapping, TOP left-to-right is DIG0..DIG3
    setDigit(0, SEGMENTS[0]);  // leftmost
    setDigit(1, SEGMENTS[1]);
    setDigit(2, SEGMENTS[2]);
    setDigit(3, SEGMENTS[3]);  // rightmost
    
    // Bottom display
    // BOTTOM left-to-right is DIG4..DIG7
    setDigit(4, SEGMENTS[4]);  // leftmost
    setDigit(5, SEGMENTS[5]);
    setDigit(6, SEGMENTS[6]);
    setDigit(7, SEGMENTS[7]);  // rightmost
}
