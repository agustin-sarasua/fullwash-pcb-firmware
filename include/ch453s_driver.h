#ifndef CH453S_DRIVER_H
#define CH453S_DRIVER_H

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * CH453S 7-Segment LED Driver
 * 
 * The CH453S is an I2C-based LED controller from WCH that can drive
 * up to 8 digits of 7-segment displays. This driver is configured for
 * dual 4-digit displays (8 digits total).
 * 
 * Display Layout:
 * - Top display (digits 0-3): Shows time left in seconds
 * - Bottom display (digits 4-7): Shows tokens left as decimal (e.g., 1.5)
 */
class CH453SDriver {
public:
    // I2C base address for CH453S (uses command-based protocol)
    static const uint8_t BASE_ADDR = 0x40;
    
    // System command codes
    static const uint16_t CMD_SYS_OFF = 0x0400;    // Turn off display
    static const uint16_t CMD_SYS_ON = 0x0401;     // Turn on display, normal mode
    static const uint16_t CMD_DUTY_MASK = 0x0410;  // Brightness control (add duty value 0-15)
    static const uint16_t CMD_7SEG_ON = 0x0480;    // 7-segment decode enable
    static const uint16_t CMD_8SEG_ON = 0x04C0;    // 8-segment mode (no decode)
    
    // Display data command (add digit address 0-7)
    static const uint16_t CMD_DISP_DATA = 0x1000;  // Base for display data
    
    // Segment patterns for 7-segment display (common cathode, active high)
    // Bit order: DP G F E D C B A
    static const uint8_t SEGMENTS[];
    
    /**
     * Constructor
     * @param wire Reference to TwoWire instance (Wire or Wire1)
     */
    CH453SDriver(TwoWire& wire);
    
    /**
     * Initialize the CH453S driver
     * @param brightness Initial brightness (0-15, default 8)
     * @return true if initialization successful
     */
    bool begin(uint8_t brightness = 8);
    
    /**
     * Set I2C mutex for thread-safe access
     * @param mutex FreeRTOS semaphore handle
     */
    void setI2CMutex(SemaphoreHandle_t mutex);
    
    /**
     * Set display brightness
     * @param brightness Brightness level 0-15 (0=darkest, 15=brightest)
     */
    void setBrightness(uint8_t brightness);
    
    /**
     * Display a 4-digit number on the top display (time in seconds)
     * @param value Number to display (0-9999)
     * @param leadingZeros Show leading zeros if true
     */
    void displayTopNumber(uint16_t value, bool leadingZeros = false);
    
    /**
     * Display a decimal number on the bottom display (tokens with fraction)
     * @param value Number to display (e.g., 1.5 tokens)
     * @param decimalPlaces Number of decimal places (1 for tokens display)
     */
    void displayBottomDecimal(float value, uint8_t decimalPlaces = 1);
    
    /**
     * Display raw segment data on a specific digit
     * @param digit Digit position (0-7, 0-3=top, 4-7=bottom)
     * @param segments Segment pattern (bit 0=A, bit 1=B, ... bit 7=DP)
     */
    void setDigit(uint8_t digit, uint8_t segments);
    
    /**
     * Display a single character on a digit
     * @param digit Digit position (0-7)
     * @param character Character to display ('0'-'9', '-', ' ', etc.)
     * @param decimal Show decimal point
     */
    void setCharacter(uint8_t digit, char character, bool decimal = false);
    
    /**
     * Clear all digits (turn off all segments)
     */
    void clear();
    
    /**
     * Clear only the top display (digits 0-3)
     */
    void clearTop();
    
    /**
     * Clear only the bottom display (digits 4-7)
     */
    void clearBottom();
    
    /**
     * Display "----" on a display to indicate error or loading
     * @param top If true, show on top display; if false, show on bottom
     */
    void displayDashes(bool top = true);
    
    /**
     * Turn display on/off
     * @param on true to turn on, false to turn off
     */
    void setDisplayOn(bool on);
    
    /**
     * Scan I2C bus and log all found devices
     * Useful for debugging connection issues
     */
    void scanI2CBus();
    
private:
    TwoWire& _wire;
    SemaphoreHandle_t _i2cMutex;
    uint8_t _brightness;
    bool _displayOn;
    
    /**
     * Send a 16-bit command to the CH453S
     * The CH453S uses a unique protocol where commands are split into two bytes
     * sent as I2C data bytes with specific addressing
     */
    bool sendCommand(uint16_t cmd);
    
    /**
     * Send CH453-specific command with proper protocol
     * @param cmd 12-bit command (split into I2C address + data)
     */
    bool sendCH453Command(uint16_t cmd);
    
    /**
     * Send system command (0x48-0x4F range)
     * @param cmd System command byte
     */
    bool sendSystemCommand(uint8_t cmd);
    
    /**
     * Write segment data to a specific digit
     * @param digit Digit number (0-15)
     * @param segmentData Segment pattern (bit 7=DP, bits 6-0=G-A)
     */
    bool writeDigitData(uint8_t digit, uint8_t segmentData);
    
    /**
     * Get segment pattern for a character
     */
    uint8_t getSegmentPattern(char c);
};

#endif // CH453S_DRIVER_H
