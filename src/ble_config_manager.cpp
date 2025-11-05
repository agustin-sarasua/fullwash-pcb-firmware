#include "ble_config_manager.h"

BLEConfigManager::BLEConfigManager() 
    : pServer(nullptr), 
      pService(nullptr), 
      pAuthCharacteristic(nullptr),
      pMachineNumCharacteristic(nullptr),
      pEnvironmentCharacteristic(nullptr),
      pStatusCharacteristic(nullptr),
      deviceConnected(false),
      authenticated(false),
      authTimestamp(0),
      machineNumber("99"),
      environment("prod"),
      masterPassword(DEFAULT_MASTER_PASSWORD),
      bleInitialized(false) {
}

BLEConfigManager::~BLEConfigManager() {
    if (pServer) {
        pServer->getAdvertising()->stop();
    }
    preferences.end();
}

bool BLEConfigManager::begin() {
    LOG_INFO("Initializing BLE Config Manager...");
    
    // Initialize Preferences
    preferences.begin(PREFS_NAMESPACE, false);
    
    // Load stored machine number (default to "99" if not set)
    machineNumber = preferences.getString(PREFS_MACHINE_NUM, "99");
    LOG_INFO("Loaded machine number from storage: %s", machineNumber.c_str());
    
    // Load stored environment (default to "prod" if not set)
    environment = preferences.getString(PREFS_ENVIRONMENT, "prod");
    LOG_INFO("Loaded environment from storage: %s", environment.c_str());
    
    // Load stored master password (default to DEFAULT_MASTER_PASSWORD if not set)
    masterPassword = preferences.getString(PREFS_BLE_PASSWORD, DEFAULT_MASTER_PASSWORD);
    LOG_INFO("Master password loaded from storage");
    
    // Initialize BLE
    BLEDevice::init(BLE_DEVICE_NAME);
    
    // Create BLE Server
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(this);
    
    // Create BLE Service
    pService = pServer->createService(SERVICE_UUID);
    
    // Create Authentication Characteristic (Write Only)
    pAuthCharacteristic = pService->createCharacteristic(
        AUTH_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    pAuthCharacteristic->setCallbacks(this);
    pAuthCharacteristic->setValue("Enter password");
    
    // Add user-friendly name/description (Characteristic User Description Descriptor 0x2901)
    BLEDescriptor* authDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    authDesc->setValue("Authentication - Write master password here");
    pAuthCharacteristic->addDescriptor(authDesc);
    
    // Create Machine Number Characteristic (Read/Write - but write requires auth)
    pMachineNumCharacteristic = pService->createCharacteristic(
        MACHINE_NUM_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | 
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pMachineNumCharacteristic->setCallbacks(this);
    pMachineNumCharacteristic->setValue(machineNumber.c_str());
    pMachineNumCharacteristic->addDescriptor(new BLE2902());
    
    // Add user-friendly name/description
    BLEDescriptor* machineNumDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    machineNumDesc->setValue("Machine Number - Read/Write machine ID (requires auth)");
    pMachineNumCharacteristic->addDescriptor(machineNumDesc);
    
    // Create Environment Characteristic (Read/Write - but write requires auth)
    pEnvironmentCharacteristic = pService->createCharacteristic(
        ENVIRONMENT_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | 
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pEnvironmentCharacteristic->setCallbacks(this);
    pEnvironmentCharacteristic->setValue(environment.c_str());
    pEnvironmentCharacteristic->addDescriptor(new BLE2902());
    
    // Add user-friendly name/description
    BLEDescriptor* environmentDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    environmentDesc->setValue("Environment - Read/Write environment (local/prod, requires auth)");
    pEnvironmentCharacteristic->addDescriptor(environmentDesc);
    
    // Create Status Characteristic (Read/Notify)
    pStatusCharacteristic = pService->createCharacteristic(
        STATUS_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | 
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pStatusCharacteristic->setValue("Not authenticated");
    pStatusCharacteristic->addDescriptor(new BLE2902());
    
    // Add user-friendly name/description
    BLEDescriptor* statusDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    statusDesc->setValue("Status - Read authentication and operation status");
    pStatusCharacteristic->addDescriptor(statusDesc);
    
    // Start the service
    pService->start();
    
    // Start advertising
    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);  // functions that help with iPhone connections issue
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
    
    LOG_INFO("BLE Config Manager initialized. Device name: %s", BLE_DEVICE_NAME);
    LOG_INFO("Waiting for BLE client connection to configure machine...");
    
    bleInitialized = true;
    return true;
}

void BLEConfigManager::onConnect(BLEServer* pServer) {
    deviceConnected = true;
    LOG_INFO("BLE client connected");
    updateStatusCharacteristic("Connected - Please authenticate");
}

void BLEConfigManager::onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    authenticated = false;
    LOG_INFO("BLE client disconnected");
    
    // Restart advertising
    delay(500);
    pServer->startAdvertising();
    LOG_INFO("BLE advertising restarted");
}

void BLEConfigManager::onWrite(BLECharacteristic* pCharacteristic) {
    String uuid = pCharacteristic->getUUID().toString().c_str();
    std::string value = pCharacteristic->getValue();
    String valueStr = String(value.c_str());
    
    LOG_DEBUG("BLE Write received on characteristic: %s", uuid.c_str());
    
    // Authentication Characteristic
    if (uuid == AUTH_CHAR_UUID) {
        LOG_INFO("Authentication attempt received");
        
        if (verifyPassword(valueStr)) {
            authenticated = true;
            authTimestamp = millis();
            LOG_INFO("Authentication successful! Session valid for %d seconds", AUTH_TIMEOUT_MS / 1000);
            char statusMsg[64];
            snprintf(statusMsg, sizeof(statusMsg), "Authenticated - Valid for %d seconds", AUTH_TIMEOUT_MS / 1000);
            updateStatusCharacteristic(statusMsg);
            
            // Clear the password from the characteristic for security
            pAuthCharacteristic->setValue("***");
        } else {
            authenticated = false;
            LOG_WARNING("Authentication failed - incorrect password");
            updateStatusCharacteristic("Authentication failed - Incorrect password");
            
            // Clear the password from the characteristic
            pAuthCharacteristic->setValue("***");
        }
    }
    // Machine Number Characteristic
    else if (uuid == MACHINE_NUM_CHAR_UUID) {
        if (!authenticated) {
            LOG_WARNING("Unauthorized attempt to change machine number");
            updateStatusCharacteristic("Error: Not authenticated");
            
            // Revert to current value
            pMachineNumCharacteristic->setValue(machineNumber.c_str());
            pMachineNumCharacteristic->notify();
            return;
        }
        
        // Check if auth is still valid
        if (millis() - authTimestamp > AUTH_TIMEOUT_MS) {
            LOG_WARNING("Authentication expired");
            authenticated = false;
            updateStatusCharacteristic("Error: Authentication expired");
            
            // Revert to current value
            pMachineNumCharacteristic->setValue(machineNumber.c_str());
            pMachineNumCharacteristic->notify();
            return;
        }
        
        // Validate machine number (should be numeric and reasonable length)
        if (valueStr.length() == 0 || valueStr.length() > 10) {
            LOG_WARNING("Invalid machine number format: %s", valueStr.c_str());
            updateStatusCharacteristic("Error: Invalid machine number");
            
            // Revert to current value
            pMachineNumCharacteristic->setValue(machineNumber.c_str());
            pMachineNumCharacteristic->notify();
            return;
        }
        
        // Update machine number
        LOG_INFO("Updating machine number from '%s' to '%s'", machineNumber.c_str(), valueStr.c_str());
        
        machineNumber = valueStr;
        preferences.putString(PREFS_MACHINE_NUM, machineNumber);
        
        // Confirm the change
        pMachineNumCharacteristic->setValue(machineNumber.c_str());
        pMachineNumCharacteristic->notify();
        
        updateStatusCharacteristic("Machine number updated successfully");
        LOG_INFO("Machine number saved to persistent storage: %s", machineNumber.c_str());
        LOG_INFO("*** RESTART REQUIRED FOR CHANGES TO TAKE EFFECT ***");
    }
    // Environment Characteristic
    else if (uuid == ENVIRONMENT_CHAR_UUID) {
        if (!authenticated) {
            LOG_WARNING("Unauthorized attempt to change environment");
            updateStatusCharacteristic("Error: Not authenticated");
            
            // Revert to current value
            pEnvironmentCharacteristic->setValue(environment.c_str());
            pEnvironmentCharacteristic->notify();
            return;
        }
        
        // Check if auth is still valid
        if (millis() - authTimestamp > AUTH_TIMEOUT_MS) {
            LOG_WARNING("Authentication expired");
            authenticated = false;
            updateStatusCharacteristic("Error: Authentication expired");
            
            // Revert to current value
            pEnvironmentCharacteristic->setValue(environment.c_str());
            pEnvironmentCharacteristic->notify();
            return;
        }
        
        // Validate environment (should be "local" or "prod")
        valueStr.toLowerCase();
        if (valueStr != "local" && valueStr != "prod") {
            LOG_WARNING("Invalid environment value: %s (must be 'local' or 'prod')", valueStr.c_str());
            updateStatusCharacteristic("Error: Invalid environment (must be 'local' or 'prod')");
            
            // Revert to current value
            pEnvironmentCharacteristic->setValue(environment.c_str());
            pEnvironmentCharacteristic->notify();
            return;
        }
        
        // Update environment
        LOG_INFO("Updating environment from '%s' to '%s'", environment.c_str(), valueStr.c_str());
        
        environment = valueStr;
        preferences.putString(PREFS_ENVIRONMENT, environment);
        
        // Confirm the change
        pEnvironmentCharacteristic->setValue(environment.c_str());
        pEnvironmentCharacteristic->notify();
        
        updateStatusCharacteristic("Environment updated successfully");
        LOG_INFO("Environment saved to persistent storage: %s", environment.c_str());
        LOG_INFO("*** RESTART REQUIRED FOR CHANGES TO TAKE EFFECT ***");
    }
}

bool BLEConfigManager::verifyPassword(const String& password) {
    // Constant-time comparison to prevent timing attacks
    if (password.length() != masterPassword.length()) {
        return false;
    }
    
    bool match = true;
    for (size_t i = 0; i < password.length(); i++) {
        if (password[i] != masterPassword[i]) {
            match = false;
        }
    }
    
    return match;
}

void BLEConfigManager::updateStatusCharacteristic(const String& status) {
    if (pStatusCharacteristic && deviceConnected) {
        pStatusCharacteristic->setValue(status.c_str());
        pStatusCharacteristic->notify();
        LOG_DEBUG("Status updated: %s", status.c_str());
    }
}

void BLEConfigManager::resetAuthentication() {
    authenticated = false;
    authTimestamp = 0;
    updateStatusCharacteristic("Authentication expired");
    LOG_INFO("Authentication reset due to timeout");
}

bool BLEConfigManager::isAuthenticated() {
    if (!authenticated) {
        return false;
    }
    
    // Check if authentication has expired
    if (millis() - authTimestamp > AUTH_TIMEOUT_MS) {
        resetAuthentication();
        return false;
    }
    
    return true;
}

String BLEConfigManager::getMachineNumber() {
    return machineNumber;
}

bool BLEConfigManager::setMachineNumber(const String& number) {
    if (number.length() == 0 || number.length() > 10) {
        LOG_WARNING("Invalid machine number format");
        return false;
    }
    
    machineNumber = number;
    preferences.putString(PREFS_MACHINE_NUM, machineNumber);
    
    if (pMachineNumCharacteristic) {
        pMachineNumCharacteristic->setValue(machineNumber.c_str());
        if (deviceConnected) {
            pMachineNumCharacteristic->notify();
        }
    }
    
    LOG_INFO("Machine number updated: %s", machineNumber.c_str());
    return true;
}

bool BLEConfigManager::isConnected() {
    return deviceConnected;
}

void BLEConfigManager::update() {
    // Check if authentication should be expired
    if (authenticated && (millis() - authTimestamp > AUTH_TIMEOUT_MS)) {
        resetAuthentication();
    }
}

String BLEConfigManager::getMasterPassword() {
    return masterPassword;
}

void BLEConfigManager::setMasterPassword(const String& password) {
    if (password.length() < 8) {
        LOG_WARNING("Password too short, must be at least 8 characters");
        return;
    }
    
    masterPassword = password;
    preferences.putString(PREFS_BLE_PASSWORD, masterPassword);
    LOG_INFO("Master password updated and saved to storage");
}

String BLEConfigManager::getEnvironment() {
    return environment;
}

bool BLEConfigManager::setEnvironment(const String& env) {
    String envLower = env;
    envLower.toLowerCase();
    
    if (envLower != "local" && envLower != "prod") {
        LOG_WARNING("Invalid environment value: %s (must be 'local' or 'prod')", env.c_str());
        return false;
    }
    
    environment = envLower;
    preferences.putString(PREFS_ENVIRONMENT, environment);
    
    if (pEnvironmentCharacteristic) {
        pEnvironmentCharacteristic->setValue(environment.c_str());
        if (deviceConnected) {
            pEnvironmentCharacteristic->notify();
        }
    }
    
    LOG_INFO("Environment updated: %s", environment.c_str());
    return true;
}

void BLEConfigManager::resetToDefaults() {
    LOG_WARNING("Resetting BLE configuration to defaults");
    
    preferences.clear();
    
    machineNumber = "99";
    environment = "prod";
    masterPassword = DEFAULT_MASTER_PASSWORD;
    
    preferences.putString(PREFS_MACHINE_NUM, machineNumber);
    preferences.putString(PREFS_ENVIRONMENT, environment);
    preferences.putString(PREFS_BLE_PASSWORD, masterPassword);
    
    LOG_INFO("Configuration reset complete");
}

void BLEConfigManager::deinit() {
    if (!bleInitialized) {
        LOG_DEBUG("BLE already deinitialized");
        return;
    }
    
    LOG_INFO("Deinitializing BLE to free memory for MQTT/SSL...");
    
    // Stop advertising
    if (pServer) {
        pServer->getAdvertising()->stop();
        LOG_DEBUG("BLE advertising stopped");
    }
    
    // Deinitialize BLE device (frees ~30-40KB of heap)
    BLEDevice::deinit(true);
    LOG_INFO("BLE deinitialized - memory freed");
    
    // Clear pointers (they're now invalid)
    pServer = nullptr;
    pService = nullptr;
    pAuthCharacteristic = nullptr;
    pMachineNumCharacteristic = nullptr;
    pEnvironmentCharacteristic = nullptr;
    pStatusCharacteristic = nullptr;
    
    bleInitialized = false;
    deviceConnected = false;
    authenticated = false;
    
    LOG_INFO("BLE memory freed. Heap should now be available for SSL/MQTT.");
}

bool BLEConfigManager::isInitialized() {
    return bleInitialized;
}

