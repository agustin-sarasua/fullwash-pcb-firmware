#include "lcd_i2c_custom.h"

LcdI2cCustom::LcdI2cCustom(uint8_t lcd_addr, uint8_t lcd_cols, uint8_t lcd_rows, TwoWire& wireInstance)
    : _addr(lcd_addr), _cols(lcd_cols), _rows(lcd_rows), _wire(wireInstance), _i2cMutex(NULL) {
    _backlightval = LCD_BACKLIGHT;
    _displayfunction = LCD_4BITMODE | LCD_1LINE | LCD_5x8DOTS;
    
    if (_rows > 1) {
        _displayfunction |= LCD_2LINE;
    }
}

void LcdI2cCustom::setI2CMutex(SemaphoreHandle_t mutex) {
    _i2cMutex = mutex;
}

void LcdI2cCustom::begin() {
    // Assume I2C (Wire1) is already initialized and configured in main.cpp
    // Do not call _wire.begin() here to avoid resetting custom SDA/SCL pins
    // Small settle delay for the expander
    delay(50);
    
    // Initialize with 4-bit interface
    _displayfunction = LCD_4BITMODE | LCD_2LINE | LCD_5x8DOTS;
    
    // According to datasheet, we need at least 40ms after power rises above 2.7V
    // before sending commands. Arduino can turn on way before 4.5V.
    delay(50);
    
    // Put the LCD into 4 bit mode
    // And set displaymode, displaycontrol, etc.
    expanderWrite(_backlightval);
    // Short delay is sufficient for expander stabilization
    delay(5);
    
    // 4-bit mode init sequence as per HD44780 datasheet
    write4bits(0x03 << 4);
    delayMicroseconds(4500);
    write4bits(0x03 << 4);
    delayMicroseconds(4500);
    write4bits(0x03 << 4);
    delayMicroseconds(150);
    write4bits(0x02 << 4);
    
    // Set # lines, font size, etc.
    command(LCD_FUNCTIONSET | _displayfunction);
    
    // Turn the display on with no cursor or blinking default
    _displaycontrol = LCD_DISPLAYON | LCD_CURSOROFF | LCD_BLINKOFF;
    display();
    
    // Clear it off
    clear();
    
    // Set the entry mode
    _displaymode = LCD_ENTRYLEFT | LCD_ENTRYSHIFTDECREMENT;
    command(LCD_ENTRYMODESET | _displaymode);
    
    home();
    
    LOG_INFO("LCD initialized successfully");
}

void LcdI2cCustom::clear() {
    command(LCD_CLEARDISPLAY);
    delayMicroseconds(2000);
}

void LcdI2cCustom::home() {
    command(LCD_RETURNHOME);
    delayMicroseconds(2000);
}

void LcdI2cCustom::setCursor(uint8_t col, uint8_t row) {
    int row_offsets[] = { 0x00, 0x40, 0x14, 0x54 };
    if (row >= _rows) {
        row = _rows - 1;
    }
    command(LCD_SETDDRAMADDR | (col + row_offsets[row]));
}

void LcdI2cCustom::noDisplay() {
    _displaycontrol &= ~LCD_DISPLAYON;
    command(LCD_DISPLAYCONTROL | _displaycontrol);
}

void LcdI2cCustom::display() {
    _displaycontrol |= LCD_DISPLAYON;
    command(LCD_DISPLAYCONTROL | _displaycontrol);
}

void LcdI2cCustom::noCursor() {
    _displaycontrol &= ~LCD_CURSORON;
    command(LCD_DISPLAYCONTROL | _displaycontrol);
}

void LcdI2cCustom::cursor() {
    _displaycontrol |= LCD_CURSORON;
    command(LCD_DISPLAYCONTROL | _displaycontrol);
}

void LcdI2cCustom::noBlink() {
    _displaycontrol &= ~LCD_BLINKON;
    command(LCD_DISPLAYCONTROL | _displaycontrol);
}

void LcdI2cCustom::blink() {
    _displaycontrol |= LCD_BLINKON;
    command(LCD_DISPLAYCONTROL | _displaycontrol);
}

void LcdI2cCustom::scrollDisplayLeft() {
    command(LCD_CURSORSHIFT | LCD_DISPLAYMOVE | LCD_MOVELEFT);
}

void LcdI2cCustom::scrollDisplayRight() {
    command(LCD_CURSORSHIFT | LCD_DISPLAYMOVE | LCD_MOVERIGHT);
}

void LcdI2cCustom::leftToRight() {
    _displaymode |= LCD_ENTRYLEFT;
    command(LCD_ENTRYMODESET | _displaymode);
}

void LcdI2cCustom::rightToLeft() {
    _displaymode &= ~LCD_ENTRYLEFT;
    command(LCD_ENTRYMODESET | _displaymode);
}

void LcdI2cCustom::autoscroll() {
    _displaymode |= LCD_ENTRYSHIFTINCREMENT;
    command(LCD_ENTRYMODESET | _displaymode);
}

void LcdI2cCustom::noAutoscroll() {
    _displaymode &= ~LCD_ENTRYSHIFTINCREMENT;
    command(LCD_ENTRYMODESET | _displaymode);
}

void LcdI2cCustom::noBacklight() {
    _backlightval = LCD_NOBACKLIGHT;
    expanderWrite(0);
}

void LcdI2cCustom::backlight() {
    _backlightval = LCD_BACKLIGHT;
    expanderWrite(0);
}

void LcdI2cCustom::command(uint8_t value) {
    write4bits(value & 0xF0);
    write4bits((value << 4) & 0xF0);
}

void LcdI2cCustom::print(const String& text) {
    for (size_t i = 0; i < text.length(); i++) {
        print(text[i]);
    }
}

void LcdI2cCustom::print(const char* text) {
    while (*text) {
        print(*text++);
    }
}

void LcdI2cCustom::print(char c) {
    uint8_t value = c;
    write4bits((value & 0xF0) | Rs);
    write4bits(((value << 4) & 0xF0) | Rs);
}

void LcdI2cCustom::print(int number) {
    print(String(number));
}

void LcdI2cCustom::write4bits(uint8_t value) {
    expanderWrite(value);
    pulseEnable(value);
}

void LcdI2cCustom::expanderWrite(uint8_t data) {
    _wire.beginTransmission(_addr);
    _wire.write(data | _backlightval);
    _wire.endTransmission();
}

void LcdI2cCustom::pulseEnable(uint8_t data) {
    expanderWrite(data | En);
    delayMicroseconds(1);
    expanderWrite(data & ~En);
    delayMicroseconds(50);
}