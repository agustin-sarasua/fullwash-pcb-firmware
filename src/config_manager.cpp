#include "config_manager.h"

// Constructor
ConfigManager::ConfigManager() 
    : _webServer(80), 
      _setupMode(false),
      _machineId(DEFAULT_MACHINE_ID),
      _tokenTime(DEFAULT_TOKEN_TIME),
      _userInactiveTimeout(DEFAULT_USER_INACTIVE_TIMEOUT),
      _simPin(DEFAULT_SIM_PIN),
      _apn(DEFAULT_APN),
      _apPassword(DEFAULT_AP_PASSWORD),
      _apSsid(DEFAULT_AP_SSID) {
}

// Initialize configuration manager
bool ConfigManager::begin() {
    LOG_INFO("Initializing configuration manager");
    
    // Initialize Preferences
    if (!_preferences.begin("fullwash", false)) {
        LOG_ERROR("Failed to initialize preferences");
        return false;
    }
    
    // Load configuration from persistent storage
    loadConfig();
    
    return true;
}

// Load configuration from persistent storage
bool ConfigManager::loadConfig() {
    LOG_INFO("Loading configuration from Preferences");
    
    // Load values from Preferences with defaults if not found
    _machineId = _preferences.getString("machineId", DEFAULT_MACHINE_ID);
    _tokenTime = _preferences.getULong("tokenTime", DEFAULT_TOKEN_TIME);
    _userInactiveTimeout = _preferences.getULong("userTimeout", DEFAULT_USER_INACTIVE_TIMEOUT);
    _simPin = _preferences.getString("simPin", DEFAULT_SIM_PIN);
    _apn = _preferences.getString("apn", DEFAULT_APN);
    _apPassword = _preferences.getString("apPassword", DEFAULT_AP_PASSWORD);
    
    LOG_INFO("Configuration loaded - Machine ID: %s, Token Time: %lu, User Timeout: %lu",
        _machineId.c_str(), _tokenTime, _userInactiveTimeout);
    
    return true;
}

// Save configuration to permanent storage
bool ConfigManager::saveConfig() {
    LOG_INFO("Saving configuration to Preferences");
    
    // Save values to Preferences
    _preferences.putString("machineId", _machineId);
    _preferences.putULong("tokenTime", _tokenTime);
    _preferences.putULong("userTimeout", _userInactiveTimeout);
    _preferences.putString("simPin", _simPin);
    _preferences.putString("apn", _apn);
    _preferences.putString("apPassword", _apPassword);
    
    LOG_INFO("Configuration saved");
    return true;
}

// Start configuration portal
bool ConfigManager::startConfigPortal(unsigned long timeout) {
    LOG_INFO("Starting configuration portal");
    
    // Set up access point
    WiFi.mode(WIFI_AP);
    
    // Generate unique SSID if needed (using chip ID)
    String ssid = _apSsid;
    if (ssid == DEFAULT_AP_SSID) {
        // Append last 4 digits of MAC address for uniqueness
        uint8_t mac[6];
        WiFi.macAddress(mac);
        char suffix[5];
        sprintf(suffix, "-%02X%02X", mac[4], mac[5]);
        ssid += suffix;
    }
    
    LOG_INFO("Setting up AP: %s with password: %s", 
        ssid.c_str(), _apPassword.c_str());
    
    // Start AP with SSID and password
    WiFi.softAP(ssid.c_str(), _apPassword.c_str());
    
    // Set up DNS server for captive portal
    IPAddress myIP = WiFi.softAPIP();
    LOG_INFO("AP IP address: %s", myIP.toString().c_str());
    
    _dnsServer.start(DNS_PORT, "*", myIP);
    
    // Set up web server
    setupWebServer();
    _webServer.begin();
    
    LOG_INFO("Configuration portal started");
    
    // Mark that we're in setup mode
    _setupMode = true;
    
    // Track when we started
    unsigned long startTime = millis();
    
    // Loop while in setup mode (until timeout or configuration complete)
    while (_setupMode && (timeout == 0 || millis() - startTime < timeout)) {
        _dnsServer.processNextRequest();
        _webServer.handleClient();
        delay(10);
    }
    
    // Clean up when we exit
    _webServer.stop();
    _dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    _setupMode = false;
    
    LOG_INFO("Configuration portal ended");
    return true;
}

// Set up web server handlers
void ConfigManager::setupWebServer() {
    LOG_INFO("Setting up web server handlers");
    
    // Root page - configuration form
    _webServer.on("/", [this]() { this->handleRoot(); });
    
    // Config form submission handler
    _webServer.on("/save", HTTP_POST, [this]() { this->handleConfigSubmit(); });
    
    // Captive portal handler
    _webServer.onNotFound([this]() { this->handleNotFound(); });
}

// Handle root page
void ConfigManager::handleRoot() {
    LOG_DEBUG("Serving root page");
    _webServer.send(200, "text/html", generateConfigPage());
}

// Handle form submission
void ConfigManager::handleConfigSubmit() {
    LOG_INFO("Processing configuration form submission");
    
    bool changed = false;
    
    // Process submitted form data
    if (_webServer.hasArg("machineId")) {
        String newMachineId = _webServer.arg("machineId");
        if (newMachineId != _machineId) {
            _machineId = newMachineId;
            changed = true;
        }
    }
    
    if (_webServer.hasArg("tokenTime")) {
        unsigned long newTokenTime = _webServer.arg("tokenTime").toInt() * 1000; // Convert seconds to ms
        if (newTokenTime != _tokenTime) {
            _tokenTime = newTokenTime;
            changed = true;
        }
    }
    
    if (_webServer.hasArg("userTimeout")) {
        unsigned long newTimeout = _webServer.arg("userTimeout").toInt() * 1000; // Convert seconds to ms
        if (newTimeout != _userInactiveTimeout) {
            _userInactiveTimeout = newTimeout;
            changed = true;
        }
    }
    
    if (_webServer.hasArg("simPin")) {
        String newSimPin = _webServer.arg("simPin");
        if (newSimPin != _simPin) {
            _simPin = newSimPin;
            changed = true;
        }
    }
    
    if (_webServer.hasArg("apn")) {
        String newApn = _webServer.arg("apn");
        if (newApn != _apn) {
            _apn = newApn;
            changed = true;
        }
    }
    
    if (_webServer.hasArg("apPassword")) {
        String newPassword = _webServer.arg("apPassword");
        if (newPassword.length() >= 8 && newPassword != _apPassword) {
            _apPassword = newPassword;
            changed = true;
        }
    }
    
    // If anything changed, save the configuration
    if (changed) {
        saveConfig();
    }
    
    // Send response
    _webServer.send(200, "text/html", generateConfigPage(true));
    
    // If restart requested, exit setup mode
    if (_webServer.hasArg("restart") && _webServer.arg("restart") == "1") {
        delay(1000); // Give time for the response to be sent
        _setupMode = false; // Exit setup mode
    }
}

// Handle not found - redirect to configuration page (captive portal)
void ConfigManager::handleNotFound() {
    LOG_DEBUG("Redirecting to captive portal");
    
    // Redirect to the configuration page
    _webServer.sendHeader("Location", "/", true);
    _webServer.send(302, "text/plain", "");
}

// Generate the configuration HTML page
String ConfigManager::generateConfigPage(bool showSuccess) {
    String html = "<!DOCTYPE html>"
      "<html>"
      "<head>"
      "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
      "<title>FullWash Configuration</title>"
      "<style>"
      "body {"
      "  font-family: Arial, sans-serif;"
      "  margin: 0;"
      "  padding: 0;"
      "  background-color: #f5f5f5;"
      "}"
      ".container {"
      "  max-width: 500px;"
      "  margin: 20px auto;"
      "  padding: 20px;"
      "  background: white;"
      "  border-radius: 5px;"
      "  box-shadow: 0 2px 5px rgba(0,0,0,0.1);"
      "}"
      "h1 {"
      "  color: #2c3e50;"
      "  text-align: center;"
      "}"
      "label {"
      "  display: block;"
      "  margin-top: 10px;"
      "  font-weight: bold;"
      "}"
      "input, select {"
      "  width: 100%;"
      "  padding: 8px;"
      "  margin-top: 5px;"
      "  margin-bottom: 15px;"
      "  border: 1px solid #ddd;"
      "  border-radius: 4px;"
      "  box-sizing: border-box;"
      "}"
      "button {"
      "  background-color: #4CAF50;"
      "  color: white;"
      "  padding: 10px 15px;"
      "  border: none;"
      "  border-radius: 4px;"
      "  cursor: pointer;"
      "  font-size: 16px;"
      "  width: 100%;"
      "}"
      "button:hover {"
      "  background-color: #45a049;"
      "}"
      ".success {"
      "  background-color: #d4edda;"
      "  color: #155724;"
      "  padding: 10px;"
      "  margin-bottom: 15px;"
      "  border-radius: 4px;"
      "  display: " + String(showSuccess ? "block" : "none") + ";"
      "}"
      "</style>"
      "</head>"
      "<body>"
      "<div class='container'>"
      "<h1>FullWash Configuration</h1>";
    
    // Success message (shown only after successful save)
    html += "<div class='success'>Configuration saved successfully!</div>";
    
    // Form
    html += "<form action='/save' method='post'>"
      "<label for='machineId'>Machine ID:</label>"
      "<input type='text' id='machineId' name='machineId' value='" + _machineId + "' required>"
      
      "<label for='tokenTime'>Token Time (seconds):</label>"
      "<input type='number' id='tokenTime' name='tokenTime' value='" + String(_tokenTime / 1000) + "' min='1' required>"
      
      "<label for='userTimeout'>User Inactive Timeout (seconds):</label>"
      "<input type='number' id='userTimeout' name='userTimeout' value='" + String(_userInactiveTimeout / 1000) + "' min='1' required>"
      
      "<label for='simPin'>SIM Card PIN:</label>"
      "<input type='text' id='simPin' name='simPin' value='" + _simPin + "'>"
      
      "<label for='apn'>Network APN:</label>"
      "<input type='text' id='apn' name='apn' value='" + _apn + "'>"
      
      "<label for='apPassword'>WiFi Setup Password (min 8 chars):</label>"
      "<input type='password' id='apPassword' name='apPassword' minlength='8'>"
      
      "<button type='submit' name='restart' value='0'>Save Configuration</button>"
      "<br><br>"
      "<button type='submit' name='restart' value='1' style='background-color: #f39c12;'>Save and Exit Setup Mode</button>"
      "</form>"
      "</div>"
      "</body>"
      "</html>";
    
    return html;
}