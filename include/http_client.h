#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <Arduino.h>
#include <ArduinoHttpClient.h>
#include "modem.h"
#include "config.h"

class AppHTTPClient {
public:
  AppHTTPClient();
  
  // Initialize HTTP client with modem manager
  bool begin(ModemManager* modemManager);
  
  // Perform HTTP GET request
  bool get(const String& endpoint, String& response);
  
  // Perform HTTP POST request with JSON payload
  bool post(const String& endpoint, const String& jsonPayload, String& response);
  
  // Check if HTTP client is ready
  bool isReady();

private:
  ModemManager* modemManager;
  HttpClient* client;
  bool initialized;
  
  // Parse HTTP response
  bool parseResponse(String& responseBody);
};

#endif // HTTP_CLIENT_H