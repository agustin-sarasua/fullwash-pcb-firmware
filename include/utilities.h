#ifndef UTILITIES_H
#define UTILITIES_H

// TCA9535 I2C address
// Scanner found the device at address 0x24
#define TCA9535_ADDR 0x24

// TCA9535 register addresses
#define INPUT_PORT0      0x00
#define INPUT_PORT1      0x01
#define OUTPUT_PORT0     0x02
#define OUTPUT_PORT1     0x03
#define POLARITY_PORT0   0x04
#define POLARITY_PORT1   0x05
#define CONFIG_PORT0     0x06
#define CONFIG_PORT1     0x07

// Define pins on ESP32
#define I2C_SDA_PIN      19
#define I2C_SCL_PIN      18
#define INT_PIN          23  // Interrupt pin from IO expander

// Define a built-in LED pin for visual debugging
#define LED_PIN          12   // Blue LED connected to IO12 per schematic

// Define button and relay pins on TCA9535 (Port 0, active LOW, per schematic)
#define BUTTON1          5   // BT1 is on P05
#define BUTTON2          4   // BT2 is on P04
#define BUTTON3          3   // BT3 is on P03
#define BUTTON4          2   // BT4 is on P02
#define BUTTON5          1   // BT5 is on P01
#define BUTTON6          0   // BT6 is on P00

// Coin acceptor pin on TCA9535 (Port 0)
// ACTIVE-HIGH (verified on real hardware 2026-07-04): R64 (10k) pulls the
// line LOW at idle; the acceptor drives it to 3.3V while a coin passes
#define COIN_SIG         6   // P06 - Coin signal

// Relay pins on TCA9535 (Port 1)
#define RELAY1           0   // P10 - clear water
#define RELAY2           1   // P11 - Foam
#define RELAY3           2   // P12 - vacuum
#define RELAY4           3   // P13 - handwashing
#define RELAY5           4   // P14 - inflatable
#define RELAY6           5   // P15 - disinfect
#define RELAY7           6   // P16 - lighting

// 7-Segment Display Configuration (CH453S driver)
// The CH453S uses a command-based protocol; base address is 0x40
// Connected to Wire1 (same bus as old LCD was)
#define CH453S_ADDR      0x40 // CH453S base I2C address
#define DISPLAY_SDA_PIN  21   // I2C SDA pin for display
#define DISPLAY_SCL_PIN  22   // I2C SCL pin for display

// RTC Configuration
#define RTC_DS1340_ADDR  0x68 // DS1340Z RTC I2C address (shared bus with display)

#endif // UTILITIES_H
