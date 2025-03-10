#include <Arduino.h>
#include "mqtt_lte_client.h"
#include "io_expander.h"
#include "utilities.h"

#include "certs/AmazonRootCA.h"
#include "certs/AWSClientCertificate.h"
#include "certs/AWSClientPrivateKey.h"

// Server details
const char* AWS_BROKER = "a3foc0mc6v7ap0-ats.iot.us-east-1.amazonaws.com";
const char* AWS_CLIENT_ID = "fullwash-machine-001";
const uint16_t AWS_BROKER_PORT = 8883;

// GSM connection settings
const char apn[] = "internet"; // Replace with your carrier's APN if needed
const char gprsUser[] = "";
const char gprsPass[] = "";
const char pin[] = "3846";

// Create MQTT LTE client
MqttLteClient mqttClient(SerialAT, MODEM_PWRKEY, MODEM_DTR, MODEM_FLIGHT, MODEM_TX, MODEM_RX);

// Create IO Expander
IoExpander ioExpander(TCA9535_ADDR, I2C_SDA_PIN, I2C_SCL_PIN, INT_PIN);

// Button states
bool buttonState = false;
bool lastButtonState = false;

// MQTT callback function
void callback(char *topic, byte *payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1000); // Give time for serial to initialize
  
  // Set up the built-in LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // Turn ON LED to show power
  
  Serial.println("\n\n");
  Serial.println("======================================");
  Serial.println("ESP32 TCA9535 I/O Expander Debug Mode");
  Serial.println("======================================");
  
  // Blink LED to show we've reached this point
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, LOW);
    delay(100);
    digitalWrite(LED_PIN, HIGH);
    delay(100);
  }
  
  // Initialize the I/O expander
  Serial.println("Trying to initialize TCA9535...");
  bool initSuccess = ioExpander.begin();
  
  if (!initSuccess) {
    Serial.println("Failed to initialize TCA9535!");
    Serial.println("Will continue without initialization. Check connections.");
    
    // Blink LED rapidly to indicate error
    for (int i = 0; i < 10; i++) {
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(100);
    }
    
    // Continue anyway - don't get stuck in a loop
  } else {
    Serial.println("TCA9535 initialization successful!");
    
    // Configure Port 0 (buttons) as inputs (1 = input, 0 = output)
    Serial.println("Configuring Port 0 as inputs...");
    ioExpander.configurePortAsInput(0, 0xFF);
    
    // Configure Port 1 (relays) as outputs (1 = input, 0 = output)
    Serial.println("Configuring Port 1 as outputs...");
    ioExpander.configurePortAsOutput(1, 0xFF);
    
    // Initialize all relays to OFF state
    Serial.println("Setting all relays to OFF...");
    ioExpander.writeRegister(OUTPUT_PORT1, 0x00);
    
    Serial.println("TCA9535 fully initialized. Ready to control relays and read buttons.");
  }

  // Initialize MQTT client with callback
  mqttClient.setCallback(callback);
  
  // Initialize modem and connect to network
  Serial.println("Initializing modem and connecting to network...");
  if (mqttClient.begin(apn, gprsUser, gprsPass, pin)) {
    // Configure SSL certificates
    mqttClient.setCACert(AmazonRootCA);
    mqttClient.setCertificate(AWSClientCertificate);
    mqttClient.setPrivateKey(AWSClientPrivateKey);
    
    // Connect to MQTT broker
    Serial.println("Connecting to MQTT broker...");
    if (mqttClient.connect(AWS_BROKER, AWS_BROKER_PORT, AWS_CLIENT_ID)) {
      Serial.println("Connected to MQTT broker!");
      
      // Subscribe to test topic
      mqttClient.subscribe("machines/machine001/test");
      
      // Publish hello message
      mqttClient.publish("machines/machine001/data", "hello world");
    } else {
      Serial.println("Failed to connect to MQTT broker");
    }
  } else {
    Serial.println("Failed to initialize modem");
  }
  
  // Final blink pattern to indicate setup complete
  for (int i = 0; i < 2; i++) {
    digitalWrite(LED_PIN, LOW);
    delay(200);
    digitalWrite(LED_PIN, HIGH);
    delay(200);
  }
}

void loop() {
  static unsigned long lastStatusCheck = 0;
  static unsigned long lastConnectionAttempt = 0;
  static bool connected = mqttClient.isNetworkConnected();
  
  // Check connection status periodically
  if (!connected && (millis() - lastConnectionAttempt > 30000)) {
    lastConnectionAttempt = millis();
    
    Serial.println("Attempting to connect to cellular network...");
    connected = mqttClient.begin(apn, gprsUser, gprsPass, pin);
    
    if (connected) {
      Serial.println("Successfully connected to the internet!");
      
      // Configure SSL certificates
      mqttClient.setCACert(AmazonRootCA);
      mqttClient.setCertificate(AWSClientCertificate);
      mqttClient.setPrivateKey(AWSClientPrivateKey);
      
      // Connect to MQTT broker
      if (mqttClient.connect(AWS_BROKER, AWS_BROKER_PORT, AWS_CLIENT_ID)) {
        Serial.println("Connected to MQTT broker!");
        
        // Subscribe to test topic
        mqttClient.subscribe("machines/machine001/test");
      }
      
      // Blink the built-in LED to indicate success
      for (int i = 0; i < 5; i++) {
        digitalWrite(LED_PIN, LOW);
        delay(100);
        digitalWrite(LED_PIN, HIGH);
        delay(100);
      }
    }
    
    // Print I/O expander debug info
    ioExpander.printDebugInfo();
    
    // Try reading a specific button
    Serial.print("Button 1 state: ");
    Serial.println(ioExpander.readButton(BUTTON1) ? "PRESSED" : "NOT PRESSED");
    
    // Blink the LED to show the program is running
    digitalWrite(LED_PIN, LOW);
    delay(50);
    digitalWrite(LED_PIN, HIGH);
  }

  if (connected && (millis() - lastStatusCheck > 10000)) {
    lastStatusCheck = millis();
    
    if (mqttClient.isNetworkConnected()) {
      Serial.println("Still connected to cellular network");
      
      // Process MQTT messages
      mqttClient.loop();
    } else {
      Serial.println("Lost connection to cellular network. Will attempt to reconnect.");
      connected = false;
    }
  }
  
  // Read button state (BT1)
  buttonState = ioExpander.readButton(BUTTON1);
  
  // Check if button state changed from not pressed to pressed
  if (buttonState && !lastButtonState) {
    Serial.println("Button pressed! Toggling Relay 1 (clear water)");
    
    // Toggle Relay 1 and get the new state
    bool newState = ioExpander.toggleRelay(RELAY1);
    Serial.println(newState ? "Relay ON" : "Relay OFF");
    
    // Publish relay state change to MQTT if connected
    if (mqttClient.isConnected()) {
      mqttClient.publish("machines/machine001/relay", newState ? "ON" : "OFF");
    }
  }
  
  // Save last button state
  lastButtonState = buttonState;
  
  // Small delay to debounce button
  delay(50);
}