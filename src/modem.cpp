#include "modem.h"
#include "certs/AmazonRootCA.h"
#include "certs/AWSClientCertificate.h"
#include "certs/AWSClientPrivateKey.h"

// Define the TinyGSM instance based on debugging status
#ifdef DUMP_AT_COMMANDS
#include <StreamDebugger.h>
StreamDebugger debugger(SerialAT, SerialMon);
TinyGsm gsmModem(debugger);
#else
TinyGsm gsmModem(SerialAT);
#endif

ModemManager::ModemManager() {
  modem = &gsmModem;
  client = new TinyGsmClientSecure(gsmModem, 0);
  initialized = false;
  networkConnected = false;
}

bool ModemManager::begin() {
  SerialMon.println("Initializing modem...");
  
  // Initialize modem serial
  SerialAT.begin(MODEM_BAUD_RATE, SERIAL_8N1, MODEM_TX, MODEM_RX);
  delay(1000);
  
  // Power on the modem
  if (!powerOn()) {
    SerialMon.println("Failed to power on modem!");
    return false;
  }
  
  // Try to initialize the modem
  if (!modem->init()) {
    SerialMon.println("Failed to initialize modem!");
    
    // Check if we can talk to the modem directly
    if (testAT()) {
      SerialMon.println("Modem responds to AT commands but TinyGSM init failed.");
      SerialMon.println("This could be a TinyGSM library compatibility issue.");
      
      // Try alternative baud rate
      SerialMon.println("Trying alternative baud rate (9600)...");
      SerialAT.updateBaudRate(9600);
      delay(1000);
      
      if (modem->init()) {
        initialized = true;
        SerialMon.println("Modem initialized with 9600 baud rate");
        
        // Set certificates for secure communication
        setCertificates(AmazonRootCA, AWSClientCertificate, AWSClientPrivateKey);
        
        return true;
      }
      
      return false;
    } else {
      SerialMon.println("Basic AT command communication failed.");
      SerialMon.println("Possible hardware issue - check wiring and power.");
      return false;
    }
  }
  
  // Get modem info
  String modemInfo = modem->getModemInfo();
  SerialMon.print("Modem Info: ");
  SerialMon.println(modemInfo);
  
  // Set automatic network mode (2G/3G/4G)
  String networkMode;
  networkMode = modem -> setNetworkMode(2);
  SerialMon.print("Network mode set: ");
  SerialMon.println(networkMode);
  
  initialized = true;
  
  // Set certificates for secure communication
  setCertificates(AmazonRootCA, AWSClientCertificate, AWSClientPrivateKey);
  
  return true;
}

bool ModemManager::powerOn() {
  SerialMon.println("Powering on modem...");
  
  // Configure control pins
  pinMode(MODEM_PWRKEY, OUTPUT);
  pinMode(MODEM_DTR, OUTPUT);
  pinMode(MODEM_FLIGHT, OUTPUT);
  
  // Set DTR active
  digitalWrite(MODEM_DTR, LOW);
  
  // Disable flight mode
  digitalWrite(MODEM_FLIGHT, HIGH);
  
  // SIM7600G power on sequence
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(1000);
  
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(2000);
  
  digitalWrite(MODEM_PWRKEY, LOW);
  
  SerialMon.println("Waiting for modem to initialize...");
  delay(10000);
  
  clearBuffer();
  
  // Test AT command communication
  if (!testAT()) {
    SerialMon.println("Trying alternative power on sequence...");
    
    digitalWrite(MODEM_PWRKEY, HIGH);
    delay(3000);
    digitalWrite(MODEM_PWRKEY, LOW);
    delay(5000);
    
    clearBuffer();
    
    return testAT();
  }
  
  return true;
}

bool ModemManager::connectNetwork() {
  if (!initialized) {
    SerialMon.println("Modem not initialized!");
    return false;
  }
  
  // Check SIM card
  if (GSM_PIN && modem->getSimStatus() != 3) {
    modem->simUnlock(GSM_PIN);
  }
  
  // Wait for network connection
  SerialMon.print("Waiting for network...");
  if (!modem->waitForNetwork(60000L)) {
    SerialMon.println(" fail");
    networkConnected = false;
    return false;
  }
  SerialMon.println(" success");
  
  if (!modem->isNetworkConnected()) {
    SerialMon.println("Network connection failed");
    networkConnected = false;
    return false;
  }
  
  SerialMon.println("Network connected");
  
  // Connect to GPRS
  SerialMon.print("Connecting to ");
  SerialMon.print(APN);
  if (!modem->gprsConnect(APN, GPRS_USER, GPRS_PASS)) {
    SerialMon.println(" fail");
    networkConnected = false;
    return false;
  }
  SerialMon.println(" success");
  
  if (modem->isGprsConnected()) {
    SerialMon.println("GPRS connected");
    
    // Get IP address
    String ip = modem->localIP().toString();
    SerialMon.print("IP address: ");
    SerialMon.println(ip);
    
    networkConnected = true;
    return true;
  } else {
    SerialMon.println("GPRS connection failed");
    networkConnected = false;
    return false;
  }
}

bool ModemManager::isConnected() {
  if (!initialized) return false;
  
  networkConnected = modem->isGprsConnected();
  return networkConnected;
}

TinyGsmClientSecure* ModemManager::getClient() {
  return client;
}

bool ModemManager::testAT() {
  SerialMon.println("Testing AT communication with modem...");
  
  clearBuffer();
  
  // Send AT command
  SerialMon.println("Sending: AT");
  SerialAT.println("AT");
  
  // Wait for response
  unsigned long start = millis();
  String response = "";
  
  while (millis() - start < 3000) {
    if (SerialAT.available()) {
      char c = SerialAT.read();
      response += c;
    }
  }
  
  SerialMon.println("Response: " + response);
  
  if (response.indexOf("OK") != -1) {
    SerialMon.println("Modem responded to AT command successfully!");
    return true;
  } else {
    SerialMon.println("Modem failed to respond to AT command properly.");
    return false;
  }
}

void ModemManager::clearBuffer() {
  delay(100);
  while (SerialAT.available()) {
    SerialAT.read();
  }
}

void ModemManager::setCertificates(const char* rootCA, const char* clientCert, const char* privateKey) {
  if (!client) return;
  
  client->setCACert(rootCA);
  client->setCertificate(clientCert);
  client->setPrivateKey(privateKey);
}