#ifndef MODEM_H
#define MODEM_H

#include <Arduino.h>
#include "config.h"

// Define serial interfaces
#define SerialMon Serial
#define SerialAT Serial1

class ModemManager {
public:
  ModemManager();
  
  // Initialize the modem
  bool begin();
  
  // Power on the modem
  bool powerOn();
  
  // Connect to the cellular network
  bool connectNetwork();
  
  // Check if the modem is connected to the network
  bool isConnected();
  
  // Get the TinyGSM client for MQTT or HTTP communication
  TinyGsmClientSecure* getClient();
  
  // Test if modem responds to AT commands
  bool testAT();
  
  // Set the certificate for secure communication
  void setCertificates(const char* rootCA, const char* clientCert, const char* privateKey);

private:
  TinyGsm* modem;
  TinyGsmClientSecure* client;
  bool initialized;
  bool networkConnected;
  
  // Clear the modem serial buffer
  void clearBuffer();
};

#endif // MODEM_H