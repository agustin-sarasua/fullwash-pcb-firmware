#ifndef UTILITIES_H
#define UTILITIES_H

// Define modem model for TinyGSM
#define TINY_GSM_MODEM_SIM7600
// #define TINY_GSM_RX_BUFFER 1024  // Set RX buffer to 1Kb

#include <TinyGsmClient.h>
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

// SIM7600G pin definitions (from your PCB documentation)
#define MODEM_TX         26  // ESP32 RXD connected to SIM7600 TXD
#define MODEM_RX         27  // ESP32 TXD connected to SIM7600 RXD
#define MODEM_PWRKEY     4   // Power key pin
#define MODEM_DTR        32  // DTR pin
#define MODEM_FLIGHT     25  // Flight mode control pin

// Define the serial console for debug prints
#define SerialMon Serial
// Set serial for AT commands (to the module)
#define SerialAT Serial1



// Define button and relay pins on TCA9535
#define BUTTON1          5   // BT1 is on P00
#define BUTTON2          4   // BT2 is on P01
#define BUTTON3          3   // BT3 is on P02
#define BUTTON4          2   // BT4 is on P03
#define BUTTON5          1   // BT5 is on P04
#define BUTTON6          0   // BT6 is on P05

// Coin acceptor pins on TCA9535 (Port 0)
#define COIN_SIG         6   // P06 - Coin signal

// Relay pins on TCA9535 (Port 1)
#define RELAY1           0   // P10 - clear water
#define RELAY2           1   // P11 - Foam
#define RELAY3           2   // P12 - vacuum
#define RELAY4           3   // P13 - handwashing
#define RELAY5           4   // P14 - inflatable
#define RELAY6           5   // P15 - disinfect
#define RELAY7           6   // P16 - lighting

#define GSM_PIN          "3846"

// LCD Configuration
#define LCD_ADDR         0x27 // Default I2C address for most PCF8574 LCD adapters
#define LCD_COLS         20   // 20 characters per line
#define LCD_ROWS         4    // 4 lines display
#define LCD_SDA_PIN      21   // Separate I2C pins for LCD
#define LCD_SCL_PIN      22   // Separate I2C pins for LCD

// RTC Configuration
#define RTC_DS1340_ADDR  0x68 // DS1340Z RTC I2C address (shared bus with LCD)

#endif // UTILITIES_H
