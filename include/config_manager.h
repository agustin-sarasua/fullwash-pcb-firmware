#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "logger.h"

// Default values
#define DEFAULT_MACHINE_ID "99"
#define DEFAULT_TOKEN_TIME 120000
#define DEFAULT_USER_INACTIVE_TIMEOUT 120000
#define DEFAULT_AP_PASSWORD "fullwash123"
#define DEFAULT_APN "internet"
#define DEFAULT_SIM_PIN "3846"
#define DEFAULT_AP_SSID "FullWash-Setup"

// Config portal timeout - if no connection or activity in 5 minutes, reboot
#define CONFIG_PORTAL_TIMEOUT 300000

// DNS server port - used for captive portal
#define DNS_PORT 53

class ConfigManager {
public:
    ConfigManager();
    
    // Initialize the configuration manager
    bool begin();
    
    // Start the configuration portal
    bool startConfigPortal(unsigned long timeout = CONFIG_PORTAL_TIMEOUT);
    
    // Get configuration parameters
    String getMachineId() { return _machineId; }
    unsigned long getTokenTime() { return _tokenTime; }
    unsigned long getUserInactiveTimeout() { return _userInactiveTimeout; }
    String getSimPin() { return _simPin; }
    String getApn() { return _apn; }
    
    // Save configuration to permanent storage
    bool saveConfig();
    
    // Indicates whether the device is in setup mode
    bool isInSetupMode() { return _setupMode; }
    
private:
    // Config values
    String _machineId;
    unsigned long _tokenTime;
    unsigned long _userInactiveTimeout;
    String _simPin;
    String _apn;
    String _apPassword;
    String _apSsid;
    
    // State
    bool _setupMode;
    
    // Server objects
    WebServer _webServer;
    DNSServer _dnsServer;
    Preferences _preferences;
    
    // Load config from persistent storage
    bool loadConfig();
    
    // Setup handlers for web server
    void setupWebServer();
    
    // Handle root page
    void handleRoot();
    
    // Handle form submission
    void handleConfigSubmit();
    
    // Handle not found (captive portal)
    void handleNotFound();
    
    // Generate the configuration HTML page
    String generateConfigPage(bool showSuccess = false);
};

#endif // CONFIG_MANAGER_H