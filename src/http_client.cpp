#include "http_client.h"

AppHTTPClient::AppHTTPClient() {
  initialized = false;
  client = nullptr;
  modemManager = nullptr;
}

bool AppHTTPClient::begin(ModemManager* modemMgr) {
  if (!modemMgr) {
    Serial.println("Error: ModemManager is null");
    return false;
  }
  
  modemManager = modemMgr;
  
  // Create HTTP client with the modem's client
  client = new HttpClient(*modemManager->getClient(), BACKEND_SERVER, BACKEND_PORT);
  
  // Set connection keep-alive for HTTPS
  client->connectionKeepAlive();
  
  initialized = true;
  return true;
}

bool AppHTTPClient::get(const String& endpoint, String& response) {
  if (!initialized || !client) {
    Serial.println("HTTP client not initialized");
    return false;
  }
  
  if (!modemManager->isConnected()) {
    Serial.println("Modem not connected to network");
    return false;
  }
  
  Serial.print("Performing GET request to: ");
  Serial.println(endpoint);
  
  // Start GET request
  int err = client->get(endpoint);
  
  if (err != 0) {
    Serial.print("HTTP GET request failed with error: ");
    Serial.println(err);
    return false;
  }
  
  // Parse response
  return parseResponse(response);
}

bool AppHTTPClient::post(const String& endpoint, const String& jsonPayload, String& response) {
  if (!initialized || !client) {
    Serial.println("HTTP client not initialized");
    return false;
  }
  
  if (!modemManager->isConnected()) {
    Serial.println("Modem not connected to network");
    return false;
  }
  
  Serial.print("Performing POST request to: ");
  Serial.println(endpoint);
  
  // Start POST request
  int err = client->startRequest(
    endpoint.c_str(),
    HTTP_METHOD_POST,
    "application/json",
    jsonPayload.length(),
    (const byte*)jsonPayload.c_str()
  );
  
  if (err != 0) {
    Serial.print("HTTP POST request failed with error: ");
    Serial.println(err);
    return false;
  }
  
  // Parse response
  return parseResponse(response);
}

bool AppHTTPClient::parseResponse(String& responseBody) {
  int statusCode = client->responseStatusCode();
  Serial.print("Response status code: ");
  Serial.println(statusCode);
  
  if (statusCode <= 0) {
    Serial.println("Failed to get response status code");
    return false;
  }
  
  // Log headers for debugging
  Serial.println("Response headers:");
  while (client->headerAvailable()) {
    String headerName = client->readHeaderName();
    String headerValue = client->readHeaderValue();
    Serial.println("    " + headerName + ": " + headerValue);
  }
  
  // Get content length
  int contentLength = client->contentLength();
  if (contentLength >= 0) {
    Serial.print("Content length: ");
    Serial.println(contentLength);
  }
  
  // Read response body
  responseBody = client->responseBody();
  
  Serial.print("Response body (");
  Serial.print(responseBody.length());
  Serial.println(" bytes):");
  Serial.println(responseBody);
  
  // Check if response is successful (2xx status code)
  bool success = (statusCode >= 200 && statusCode < 300);
  
  // Disconnect
  client->stop();
  
  return success;
}

bool AppHTTPClient::isReady() {
  return initialized && modemManager && modemManager->isConnected();
}