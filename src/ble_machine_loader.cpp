#include "ble_machine_loader.h"
#include "car_wash_controller.h"
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

BLEMachineLoader::BLEMachineLoader() 
    : pServer(nullptr), 
      pService(nullptr), 
      pUserIdCharacteristic(nullptr),
      pUserNameCharacteristic(nullptr),
      pTokensCharacteristic(nullptr),
      pLoadCommandCharacteristic(nullptr),
      pLoadStatusCharacteristic(nullptr),
      pMachineStateCharacteristic(nullptr),
      deviceConnected(false),
      bleInitialized(false),
      controller(nullptr),
      machineId("") {
    resetLoadData();
}

BLEMachineLoader::~BLEMachineLoader() {
    if (pServer && bleInitialized) {
        pServer->getAdvertising()->stop();
    }
}

bool BLEMachineLoader::begin(const String& machineId, CarWashController* ctrl) {
    LOG_INFO("Initializing BLE Machine Loader...");
    
    if (ctrl == nullptr) {
        LOG_ERROR("Controller cannot be null");
        return false;
    }
    
    this->machineId = machineId;
    this->controller = ctrl;
    
    // Initialize BLE with machine-specific name
    String deviceName = String(BLE_MACHINE_DEVICE_NAME) + machineId;
    BLEDevice::init(deviceName.c_str());
    
    // Create BLE Server
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(this);
    
    // Create BLE Service
    pService = pServer->createService(MACHINE_LOAD_SERVICE_UUID);
    
    // Create User ID Characteristic (Write)
    pUserIdCharacteristic = pService->createCharacteristic(
        USER_ID_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ
    );
    pUserIdCharacteristic->setCallbacks(this);
    pUserIdCharacteristic->setValue("Enter User ID");
    
    BLEDescriptor* userIdDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    userIdDesc->setValue("User ID - Write to set user ID");
    pUserIdCharacteristic->addDescriptor(userIdDesc);
    
    // Create User Name Characteristic (Write)
    pUserNameCharacteristic = pService->createCharacteristic(
        USER_NAME_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ
    );
    pUserNameCharacteristic->setCallbacks(this);
    pUserNameCharacteristic->setValue("Enter User Name");
    
    BLEDescriptor* userNameDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    userNameDesc->setValue("User Name - Write to set user name");
    pUserNameCharacteristic->addDescriptor(userNameDesc);
    
    // Create Tokens Characteristic (Write)
    pTokensCharacteristic = pService->createCharacteristic(
        TOKENS_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ
    );
    pTokensCharacteristic->setCallbacks(this);
    pTokensCharacteristic->setValue("0");
    
    BLEDescriptor* tokensDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    tokensDesc->setValue("Tokens - Write number of tokens to load");
    pTokensCharacteristic->addDescriptor(tokensDesc);
    
    // Create Load Command Characteristic (Write)
    // Format: "LOAD|authToken" where authToken is the authorization token from backend
    pLoadCommandCharacteristic = pService->createCharacteristic(
        LOAD_COMMAND_CHAR_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    pLoadCommandCharacteristic->setCallbacks(this);
    
    BLEDescriptor* loadCmdDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    loadCmdDesc->setValue("Load Command - Write 'LOAD|authToken' to initiate machine loading");
    pLoadCommandCharacteristic->addDescriptor(loadCmdDesc);
    
    // Create Load Status Characteristic (Read/Notify)
    LOG_INFO("Creating Load Status characteristic...");
    pLoadStatusCharacteristic = pService->createCharacteristic(
        LOAD_STATUS_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | 
        BLECharacteristic::PROPERTY_NOTIFY
    );
    if (pLoadStatusCharacteristic == nullptr) {
        LOG_ERROR("Failed to create Load Status characteristic!");
        return false;
    }
    LOG_INFO("Load Status characteristic created successfully");
    pLoadStatusCharacteristic->setCallbacks(this);
    // Add BLE2902 descriptor FIRST (required for notifications)
    pLoadStatusCharacteristic->addDescriptor(new BLE2902());
    LOG_INFO("BLE2902 descriptor added to Load Status characteristic");
    pLoadStatusCharacteristic->setValue("Ready");
    
    BLEDescriptor* statusDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    statusDesc->setValue("Load Status - Read current loading status");
    pLoadStatusCharacteristic->addDescriptor(statusDesc);
    LOG_INFO("Load Status characteristic fully configured");
    
    // Create Machine State Characteristic (Read/Notify)
    LOG_INFO("Creating Machine State characteristic...");
    pMachineStateCharacteristic = pService->createCharacteristic(
        MACHINE_STATE_CHAR_UUID,
        BLECharacteristic::PROPERTY_READ | 
        BLECharacteristic::PROPERTY_NOTIFY
    );
    if (pMachineStateCharacteristic == nullptr) {
        LOG_ERROR("Failed to create Machine State characteristic!");
        return false;
    }
    LOG_INFO("Machine State characteristic created successfully");
    pMachineStateCharacteristic->setCallbacks(this);
    // Add BLE2902 descriptor FIRST (required for notifications)
    pMachineStateCharacteristic->addDescriptor(new BLE2902());
    LOG_INFO("BLE2902 descriptor added to Machine State characteristic");
    pMachineStateCharacteristic->setValue("FREE");
    
    BLEDescriptor* machineStateDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    machineStateDesc->setValue("Machine State - Current machine state (FREE/IDLE/RUNNING/PAUSED)");
    pMachineStateCharacteristic->addDescriptor(machineStateDesc);
    LOG_INFO("Machine State characteristic fully configured");
    
    // Start the service
    LOG_INFO("Starting BLE service with all characteristics...");
    pService->start();
    LOG_INFO("BLE service started successfully");
    LOG_INFO("5 required characteristics created: User ID, User Name, Tokens, Load Command (with auth), Load Status");
    LOG_INFO("1 optional characteristic created: Machine State");
    
    // Mark as initialized before starting advertising
    bleInitialized = true;
    
    // Start advertising only if machine is FREE
    if (controller && controller->getCurrentState() == STATE_FREE) {
        startAdvertising();
        LOG_INFO("BLE Machine Loader initialized. Device name: %s", deviceName.c_str());
        LOG_INFO("Machine is FREE - BLE advertising started");
    } else {
        LOG_INFO("BLE Machine Loader initialized. Device name: %s", deviceName.c_str());
        LOG_INFO("Machine is not FREE - BLE advertising will start when machine becomes FREE");
    }
    
    return true;
}

void BLEMachineLoader::startAdvertising() {
    if (!bleInitialized || !pServer) {
        LOG_WARNING("Cannot start advertising - BLE not initialized");
        return;
    }
    
    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(MACHINE_LOAD_SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();
    
    LOG_INFO("BLE advertising started for machine loading");
}

void BLEMachineLoader::stopAdvertising() {
    if (!bleInitialized || !pServer) {
        return;
    }
    
    pServer->getAdvertising()->stop();
    LOG_INFO("BLE advertising stopped");
}

void BLEMachineLoader::onConnect(BLEServer* pServer) {
    deviceConnected = true;
    LOG_INFO("BLE client connected for machine loading");
    updateLoadStatusCharacteristic("Connected - Send user data and LOAD command");
}

void BLEMachineLoader::onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    LOG_INFO("BLE client disconnected from machine loading");
    
    // Reset load data on disconnect
    if (!loadData.loadComplete) {
        resetLoadData();
    }
    
    // Restart advertising only if machine is still FREE
    if (controller && controller->getCurrentState() == STATE_FREE) {
        delay(500);
        startAdvertising();
        LOG_INFO("BLE advertising restarted");
    }
}

void BLEMachineLoader::onWrite(BLECharacteristic* pCharacteristic) {
    String uuid = pCharacteristic->getUUID().toString().c_str();
    std::string value = pCharacteristic->getValue();
    
    // Convert to String, handling null bytes and trimming
    String valueStr = "";
    for (size_t i = 0; i < value.length(); i++) {
        if (value[i] == '\0') break;  // Stop at null terminator
        valueStr += (char)value[i];
    }
    valueStr.trim();  // Remove leading/trailing whitespace
    
    LOG_INFO("BLE Write received on characteristic: %s, value length: %d, value: '%s'", 
             uuid.c_str(), valueStr.length(), valueStr.c_str());
    
    // User ID Characteristic
    if (uuid == USER_ID_CHAR_UUID) {
        if (valueStr.length() > 0 && valueStr.length() <= 100) {
            loadData.userId = valueStr;
            LOG_INFO("User ID set: %s", loadData.userId.c_str());
            updateLoadStatusCharacteristic("User ID received");
        } else {
            LOG_WARNING("Invalid user ID length: %d (must be 1-100)", valueStr.length());
            updateLoadStatusCharacteristic("Error: Invalid user ID");
        }
    }
    // User Name Characteristic
    else if (uuid == USER_NAME_CHAR_UUID) {
        if (valueStr.length() > 0 && valueStr.length() <= 100) {
            loadData.userName = valueStr;
            LOG_INFO("User Name set: %s", loadData.userName.c_str());
            updateLoadStatusCharacteristic("User name received");
        } else {
            LOG_WARNING("Invalid user name length: %d (must be 1-100), received: '%s'", 
                       valueStr.length(), valueStr.c_str());
            updateLoadStatusCharacteristic("Error: Invalid user name");
        }
    }
    // Tokens Characteristic
    else if (uuid == TOKENS_CHAR_UUID) {
        int tokens = valueStr.toInt();
        if (tokens > 0 && tokens <= 100) {
            loadData.tokens = tokens;
            LOG_INFO("Tokens set: %d", loadData.tokens);
            updateLoadStatusCharacteristic("Tokens received");
        } else {
            LOG_WARNING("Invalid token count: %d", tokens);
            updateLoadStatusCharacteristic("Error: Invalid token count");
        }
    }
    // Load Command Characteristic
    // Format: "LOAD|authToken" where authToken is the authorization token
    else if (uuid == LOAD_COMMAND_CHAR_UUID) {
        // Check if command starts with "LOAD"
        if (valueStr.startsWith("LOAD")) {
            // Parse auth token from command
            int separatorIndex = valueStr.indexOf('|');
            if (separatorIndex >= 0 && separatorIndex < valueStr.length() - 1) {
                loadData.authToken = valueStr.substring(separatorIndex + 1);
                if (loadData.authToken.length() > 0) {
                    // Store when we received the token for expiration checking
                    loadData.tokenReceivedTime = millis();
                    LOG_INFO("Load command received with auth token (length: %d)", loadData.authToken.length());
                    loadData.loadRequested = true;
                    processLoadCommand();
                } else {
                    LOG_WARNING("Load command has empty auth token. Format: LOAD|authToken");
                    updateLoadStatusCharacteristic("Error: Load command must include auth token (LOAD|token)");
                }
            } else {
                LOG_WARNING("Load command missing auth token. Format: LOAD|authToken");
                updateLoadStatusCharacteristic("Error: Load command must include auth token (LOAD|token)");
            }
        } else {
            LOG_WARNING("Unknown command: %s", valueStr.c_str());
            updateLoadStatusCharacteristic("Error: Unknown command. Use LOAD|authToken");
        }
    }
}

void BLEMachineLoader::processLoadCommand() {
    // Validate all data is present
    if (loadData.userId.length() == 0) {
        loadData.errorMessage = "User ID not set";
        updateLoadStatusCharacteristic("Error: " + loadData.errorMessage);
        LOG_ERROR("Load failed: %s", loadData.errorMessage.c_str());
        return;
    }
    
    if (loadData.userName.length() == 0) {
        loadData.errorMessage = "User name not set";
        updateLoadStatusCharacteristic("Error: " + loadData.errorMessage);
        LOG_ERROR("Load failed: %s", loadData.errorMessage.c_str());
        return;
    }
    
    if (loadData.tokens <= 0) {
        loadData.errorMessage = "Invalid token count";
        updateLoadStatusCharacteristic("Error: " + loadData.errorMessage);
        LOG_ERROR("Load failed: %s", loadData.errorMessage.c_str());
        return;
    }
    
    // Validate authorization token
    if (loadData.authToken.length() == 0) {
        loadData.errorMessage = "Authorization token not set";
        updateLoadStatusCharacteristic("Error: " + loadData.errorMessage);
        LOG_ERROR("Load failed: %s", loadData.errorMessage.c_str());
        return;
    }
    
    if (!validateAuthToken(loadData.authToken, loadData.userId, machineId, loadData.tokens)) {
        loadData.errorMessage = "Invalid or expired authorization token";
        updateLoadStatusCharacteristic("Error: " + loadData.errorMessage);
        LOG_ERROR("Load failed: Authorization token validation failed");
        return;
    }
    
    // Check if machine is FREE
    if (controller->getCurrentState() != STATE_FREE) {
        loadData.errorMessage = "Machine is not available";
        updateLoadStatusCharacteristic("Error: " + loadData.errorMessage);
        LOG_ERROR("Load failed: Machine is not FREE");
        return;
    }
    
    // Load the machine directly (simulate MQTT INIT message)
    LOG_INFO("Loading machine via BLE: user=%s, tokens=%d", loadData.userId.c_str(), loadData.tokens);
    
    // Create a session ID
    char sessionIdBuffer[50];
    sprintf(sessionIdBuffer, "ble_%lu", millis());
    
    // Simulate MQTT message by directly calling the controller's load logic
    // We'll need to add a method to CarWashController to load via BLE
    // For now, use a simulated MQTT message
    StaticJsonDocument<512> doc;
    doc["session_id"] = String(sessionIdBuffer);
    doc["user_id"] = loadData.userId;
    doc["user_name"] = loadData.userName;
    doc["tokens"] = loadData.tokens;
    doc["timestamp"] = "";  // Will be set by controller
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    // Call handleMqttMessage with INIT topic to load the machine
    // Note: This is a bit of a hack - ideally we'd have a dedicated loadMachine() method
    extern String INIT_TOPIC;
    controller->handleMqttMessage(INIT_TOPIC.c_str(), (const uint8_t*)jsonString.c_str(), jsonString.length());
    
    loadData.loadComplete = true;
    updateLoadStatusCharacteristic("Success: Machine loaded");
    LOG_INFO("Machine loaded successfully via BLE");
    
    // Stop advertising since machine is now loaded
    stopAdvertising();
}

void BLEMachineLoader::updateLoadStatusCharacteristic(const String& status) {
    if (pLoadStatusCharacteristic && deviceConnected) {
        pLoadStatusCharacteristic->setValue(status.c_str());
        pLoadStatusCharacteristic->notify();
        LOG_DEBUG("Load status updated: %s", status.c_str());
    }
}

void BLEMachineLoader::updateMachineStateCharacteristic(const String& state) {
    if (pMachineStateCharacteristic && bleInitialized) {
        pMachineStateCharacteristic->setValue(state.c_str());
        if (deviceConnected) {
            pMachineStateCharacteristic->notify();
        }
        LOG_DEBUG("Machine state updated: %s", state.c_str());
    }
}

void BLEMachineLoader::resetLoadData() {
    loadData.userId = "";
    loadData.userName = "";
    loadData.tokens = 0;
    loadData.authToken = "";
    loadData.tokenReceivedTime = 0;
    loadData.loadRequested = false;
    loadData.loadComplete = false;
    loadData.errorMessage = "";
}

bool BLEMachineLoader::validateAuthToken(const String& token, const String& userId, const String& machineId, int tokens) {
    // Token format: userId|machineId|tokens|timestamp|signature
    // Split by |
    int separatorIndices[4];
    int separatorCount = 0;
    
    for (int i = 0; i < token.length() && separatorCount < 4; i++) {
        if (token.charAt(i) == '|') {
            separatorIndices[separatorCount] = i;
            separatorCount++;
        }
    }
    
    if (separatorCount != 4) {
        LOG_ERROR("Invalid token format: expected 4 separators, got %d", separatorCount);
        return false;
    }
    
    // Extract parts
    String tokenUserId = token.substring(0, separatorIndices[0]);
    String tokenMachineId = token.substring(separatorIndices[0] + 1, separatorIndices[1]);
    String tokenTokensStr = token.substring(separatorIndices[1] + 1, separatorIndices[2]);
    String tokenTimestampStr = token.substring(separatorIndices[2] + 1, separatorIndices[3]);
    String tokenSignature = token.substring(separatorIndices[3] + 1);
    
    // Validate extracted values match
    if (tokenUserId != userId) {
        LOG_ERROR("Token userId mismatch: expected %s, got %s", userId.c_str(), tokenUserId.c_str());
        return false;
    }
    
    if (tokenMachineId != machineId) {
        LOG_ERROR("Token machineId mismatch: expected %s, got %s", machineId.c_str(), tokenMachineId.c_str());
        return false;
    }
    
    int tokenTokens = tokenTokensStr.toInt();
    if (tokenTokens != tokens) {
        LOG_ERROR("Token tokens mismatch: expected %d, got %d", tokens, tokenTokens);
        return false;
    }
    
    // Check token age - validate it was received recently (within 5 minutes)
    // We use relative time since receipt rather than absolute Unix timestamps
    // since the ESP32 doesn't have synchronized time
    unsigned long timeSinceReceipt = (millis() - loadData.tokenReceivedTime) / 1000;  // seconds
    if (timeSinceReceipt > 300) {  // 5 minutes = 300 seconds
        LOG_ERROR("Token expired: received %lu seconds ago (max 300)", timeSinceReceipt);
        return false;
    }
    
    // Also validate the token timestamp is reasonable (not too old from backend perspective)
    // This is a sanity check - token should be from last 10 minutes max
    unsigned long tokenTimestamp = tokenTimestampStr.toInt();
    // We can check it's a reasonable Unix timestamp
    // (after year 2020 = 1577836800, before year 2100 = 4102444800)
    if (tokenTimestamp < 1577836800 || tokenTimestamp > 4102444800) {
        LOG_ERROR("Token timestamp out of reasonable range: %lu", tokenTimestamp);
        return false;
    }
    
    // Reconstruct payload for signature verification
    String payload = tokenUserId + "|" + tokenMachineId + "|" + tokenTokensStr + "|" + tokenTimestampStr;
    
    // Compute HMAC-SHA256
    unsigned char hmacResult[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
    
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char*)BLE_AUTH_SECRET, strlen(BLE_AUTH_SECRET));
    mbedtls_md_hmac_update(&ctx, (const unsigned char*)payload.c_str(), payload.length());
    mbedtls_md_hmac_finish(&ctx, hmacResult);
    mbedtls_md_free(&ctx);
    
    // Convert to hex string
    char computedSignature[65];
    for (int i = 0; i < 32; i++) {
        sprintf(computedSignature + (i * 2), "%02x", hmacResult[i]);
    }
    computedSignature[64] = '\0';
    
    // Compare signatures (constant-time comparison would be better, but this is minimal security)
    if (tokenSignature != String(computedSignature)) {
        LOG_ERROR("Token signature mismatch");
        return false;
    }
    
    LOG_INFO("Authorization token validated successfully");
    return true;
}

bool BLEMachineLoader::isConnected() {
    return deviceConnected;
}

bool BLEMachineLoader::isInitialized() {
    return bleInitialized;
}

void BLEMachineLoader::deinit() {
    if (!bleInitialized) {
        LOG_DEBUG("BLE Machine Loader already deinitialized");
        return;
    }
    
    LOG_INFO("Deinitializing BLE Machine Loader...");
    
    // Stop advertising
    stopAdvertising();
    
    // Deinitialize BLE device
    BLEDevice::deinit(true);
    LOG_INFO("BLE Machine Loader deinitialized");
    
    // Clear pointers
    pServer = nullptr;
    pService = nullptr;
    pUserIdCharacteristic = nullptr;
    pUserNameCharacteristic = nullptr;
    pTokensCharacteristic = nullptr;
    pLoadCommandCharacteristic = nullptr;
    pLoadStatusCharacteristic = nullptr;
    pMachineStateCharacteristic = nullptr;
    
    bleInitialized = false;
    deviceConnected = false;
    resetLoadData();
}

void BLEMachineLoader::update() {
    // Update machine state characteristic
    if (controller && bleInitialized) {
        MachineState state = controller->getCurrentState();
        String stateStr;
        
        switch(state) {
            case STATE_FREE:
                stateStr = "FREE";
                break;
            case STATE_IDLE:
                stateStr = "IDLE";
                break;
            case STATE_RUNNING:
                stateStr = "RUNNING";
                break;
            case STATE_PAUSED:
                stateStr = "PAUSED";
                break;
            default:
                stateStr = "UNKNOWN";
                break;
        }
        
        updateMachineStateCharacteristic(stateStr);
    }
}

bool BLEMachineLoader::isLoadComplete() {
    return loadData.loadComplete;
}

MachineLoadData BLEMachineLoader::getLoadData() {
    return loadData;
}

