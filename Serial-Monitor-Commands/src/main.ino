#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h> // For HTTPS, if your Flask is HTTPS. For HTTP, just WiFiClient.
#include <HTTPClient.h>
#include <ArduinoJson.h> // For JSON parsing and creation
#include "base64.h" // For Base64 encoding
#include <esp_heap_caps.h> // For heap memory monitoring

// --- WiFi Credentials ---
const char* ssid = "MATGR-OFFICE";         // REPLACE with your WiFi SSID
const char* password = "oPPPS@13"; // REPLACE with your WiFi password

//======================================== Static IP Configuration
IPAddress local_IP(172, 19, 3, 222);
IPAddress gateway(172, 19, 0, 1);
IPAddress subnet(255, 255, 252, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);
//========================================

// --- Flask Server Details ---
const char* flaskHost = "172.19.2.151"; // REPLACE with your laptop's local IP address (e.g., 192.168.1.100)
const int flaskPort = 5000;
const char* addPersonEndpoint = "/add_person";
const char* recognizePersonEndpoint = "/recognize_person";

// --- Camera Configuration ---
// Adjust according to your ESP32-CAM model (e.g., AI-Thinker)
#define CAMERA_MODEL_AI_THINKER
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#define LED_GPIO_NUM       4 // Flash LED, if applicable

// --- Custom Logic Configuration ---
const String FIXED_CAR_ID = "car123";      // The car ID will be fixed
const int MAX_USERS_PER_CAR = 5;         // Maximum users allowed per car ID
String current_full_person_name = "";    // To store assigned or matched user, e.g., "car123/user01"

// --- Global HTTP Client (for persistent connection if needed, otherwise local) ---
WiFiClient client;
HTTPClient http;

// --- Function Prototypes ---
void handleSerialInput();
void connectToWiFi();
bool initCamera(); // Now returns bool to indicate success/failure
void sendAddPersonRequest();
void sendRecognizePersonRequest();
bool parseAndHandleFlaskResponse(String payload, String& status, String& message, String& personName, String& assignedUser, String& suggestedUser);
void deinitCamera(); // Explicitly declare deinitCamera

// --- Setup ---
void setup() {
  Serial.begin(115200);
  Serial.setTxBufferSize(1024); // Increase TX buffer for long base64 strings
  Serial.println("Booting ESP32-CAM...");

  connectToWiFi();
  // Camera is NOT initialized here anymore! It will be initialized on demand.

  Serial.println("Type 'add' to register a new face, 'recognize' to identify a face.");
  Serial.printf("Initial Free Heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Initial Min Free Heap: %u bytes\n", ESP.getMinFreeHeap());
  Serial.printf("Initial Free PSRAM: %u bytes\n", ESP.getFreePsram()); // Monitor PSRAM
}

// --- Main Loop ---
void loop() {
  handleSerialInput();
}

// --- WiFi Connection ---
void connectToWiFi() {
  Serial.println("Configuring static IP...");
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("STA Failed to configure");
  } else {
    Serial.print("Attempting to connect with static IP: ");
    Serial.println(local_IP);
  }

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi ");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

// --- Camera Initialization (now on demand) ---
bool initCamera() {
  Serial.println("Initializing Camera...");
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  config.frame_size = FRAMESIZE_SVGA; // Keeping CIF for stability based on previous issues //VGA
  config.jpeg_quality = 18;           // Good compression for CIF

  // *** Explicitly use PSRAM for the frame buffer ***
  config.fb_count = 1; // Only one frame buffer needed if immediately copying
  config.fb_location = CAMERA_FB_IN_PSRAM; 

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false; // Indicate failure
  }
  Serial.println("Camera initialized successfully!");
  Serial.printf("Free Heap after Camera Init: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Free PSRAM after Camera Init: %u bytes\n", ESP.getFreePsram());
  return true; // Indicate success
}

// --- De-initialize Camera (to free resources) ---
void deinitCamera() {
  Serial.println("De-initializing Camera...");
  esp_camera_deinit();
  Serial.println("Camera de-initialized.");
  Serial.printf("Free Heap after Camera De-init: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Free PSRAM after Camera De-init: %u bytes\n", ESP.getFreePsram());
}


// --- Handle Serial Monitor Input ---
void handleSerialInput() {
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim(); 
    Serial.printf("Received command: '%s'\n", command.c_str()); // DIAGNOSTIC PRINT

    Serial.printf("Min Free Heap before command: %u bytes\n", ESP.getMinFreeHeap());

    if (command == "add") {
      sendAddPersonRequest();
    } else if (command == "recognize") {
      sendRecognizePersonRequest();
    } else {
      Serial.println("Invalid command. Type 'add' or 'recognize'.");
    }
    Serial.printf("Min Free Heap after command: %u bytes\n", ESP.getMinFreeHeap());
  }
}

// --- Capture Photo and Encode to Base64 (with buffer copy) ---
String captureAndEncodePhoto() {
  Serial.println("Entering captureAndEncodePhoto()..."); // DIAGNOSTIC PRINT
  camera_fb_t * fb = NULL;
  Serial.println("Capturing photo...");
  fb = esp_camera_fb_get(); 
  if (!fb) {
    Serial.println("Camera capture failed");
    return "";
  }

  // Allocate memory on heap for copy, preferring PSRAM
  uint8_t *temp_buf = (uint8_t *) heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); 
  if (temp_buf == NULL) {
    Serial.println("Failed to allocate memory for photo copy! Trying internal RAM...");
    temp_buf = (uint8_t *) malloc(fb->len); // Fallback to internal RAM
    if (temp_buf == NULL) {
      Serial.println("Failed to allocate memory for photo copy in internal RAM too!");
      esp_camera_fb_return(fb); 
      return "";
    }
  }
  memcpy(temp_buf, fb->buf, fb->len); 
  size_t temp_len = fb->len; 

  esp_camera_fb_return(fb); // *** IMMEDIATELY RETURN THE CAMERA'S FRAME BUFFER ***

  Serial.printf("Free Heap after frame return and copy: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Free PSRAM after frame return and copy: %u bytes\n", ESP.getFreePsram());


  // Now, encode from the copied buffer
  String photo_base64 = base64::encode(temp_buf, temp_len);
  heap_caps_free(temp_buf); // Use heap_caps_free for memory allocated with heap_caps_malloc or free for malloc

  Serial.println("Photo captured and encoded.");
  return photo_base64;
}

// --- Send Add Person Request ---
void sendAddPersonRequest() {
  Serial.println("Entering sendAddPersonRequest()..."); // DIAGNOSTIC PRINT
  // Initialize camera before capture
  if (!initCamera()) {
    Serial.println("Camera initialization failed. Aborting sendAddPersonRequest."); // DIAGNOSTIC PRINT
    return; // Stop if camera failed to initialize
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Reconnecting...");
    connectToWiFi();
    // Re-check WiFi status after reconnect attempt
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi failed to reconnect. Aborting request.");
      deinitCamera(); 
      return;
    }
  }

  String photo_b64 = captureAndEncodePhoto();
  if (photo_b64 == "") {
    Serial.println("Photo capture/encoding failed. Aborting sendAddPersonRequest."); // DIAGNOSTIC PRINT
    deinitCamera(); 
    return; 
  }

  Serial.println("Preparing ADD request...");
  String payload;
  DynamicJsonDocument doc(64000); // Keep increased size for larger Base64 strings!

  // Always send only the FIXED_CAR_ID for add requests
  String nameToSend = FIXED_CAR_ID; 
  Serial.printf("Sending to Flask: Attempting to add/update user for car ID '%s'\n", nameToSend.c_str());
  
  doc["name"] = nameToSend; // Flask will now assign/suggest user ID
  JsonArray photosArray = doc.createNestedArray("photos");
  photosArray.add(photo_b64);
  serializeJson(doc, payload);

  Serial.println("JSON Payload being sent:"); // DIAGNOSTIC PRINT
  Serial.println(payload); // DIAGNOSTIC PRINT

  Serial.println("Beginning HTTP connection..."); // DIAGNOSTIC PRINT
  bool httpBeginSuccess = http.begin(client, flaskHost, flaskPort, addPersonEndpoint);
  Serial.printf("HTTP begin status: %s\n", httpBeginSuccess ? "SUCCESS" : "FAILED"); // DIAGNOSTIC PRINT
  if (!httpBeginSuccess) {
    Serial.println("HTTPClient begin failed. Aborting request.");
    http.end();
    deinitCamera();
    return;
  }

  http.addHeader("Content-Type", "application/json");

  long startTime = millis();
  int httpResponseCode = http.POST(payload);
  long endTime = millis();

  Serial.printf("HTTP POST to ADD took %ld ms\n", endTime - startTime);
  Serial.printf("HTTP Response code: %d\n", httpResponseCode);

  if (httpResponseCode > 0) {
    String responsePayload = http.getString();
    Serial.print("Server Response: ");
    Serial.println(responsePayload);

    String status, message, personName, assignedUser, suggestedUser;
    if (parseAndHandleFlaskResponse(responsePayload, status, message, personName, assignedUser, suggestedUser)) {
      if (status == "success") {
        Serial.println("✅ Add Person SUCCESS!");
        // Update current_full_person_name from Flask's assigned_user
        if (assignedUser != "") {
          current_full_person_name = assignedUser;
          Serial.printf("Assigned/Updated user by Flask: %s\n", current_full_person_name.c_str());
        } else {
          // Fallback if assignedUser is unexpectedly empty, though Flask should send it on success
          Serial.printf("User added/updated, but assignedUser not received. Person name: %s\n", personName.c_str());
        }
      } else if (status == "error" && suggestedUser != "") {
        Serial.println("❌ Add Person FAILED: Face already registered for this Car ID.");
        Serial.printf("Server suggests using: %s\n", suggestedUser.c_str());
        current_full_person_name = suggestedUser; // Update for future actions if needed
        Serial.println("Please use 'add' again if you meant to add this person as new, or simply 'recognize'.");
      } else {
        Serial.printf("❌ Add Person FAILED: %s\n", message.c_str()); 
      }
    }
  } else {
    Serial.printf("Error: %s\n", http.errorToString(httpResponseCode).c_str());
  }
  http.end();
  deinitCamera(); 
  Serial.println("Type 'add' or 'recognize' for next action.");
}

// --- Send Recognize Person Request ---
void sendRecognizePersonRequest() {
  Serial.println("Entering sendRecognizePersonRequest()..."); // DIAGNOSTIC PRINT
  // Initialize camera before capture
  if (!initCamera()) {
    Serial.println("Camera initialization failed. Aborting sendRecognizePersonRequest."); // DIAGNOSTIC PRINT
    return; // Stop if camera failed to initialize
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Reconnecting...");
    connectToWiFi();
    // Re-check WiFi status after reconnect attempt
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi failed to reconnect. Aborting request.");
      deinitCamera(); 
      return;
    }
  }

  String photo_b64 = captureAndEncodePhoto();
  if (photo_b64 == "") {
    Serial.println("Photo capture/encoding failed. Aborting sendRecognizePersonRequest."); // DIAGNOSTIC PRINT
    deinitCamera(); 
    return; 
  }

  Serial.println("Preparing RECOGNIZE request...");
  String payload;
  DynamicJsonDocument doc(64000); 
  doc["photo"] = photo_b64;
  serializeJson(doc, payload);

  Serial.println("JSON Payload being sent:"); // DIAGNOSTIC PRINT
  Serial.println(payload); // DIAGNOSTIC PRINT

  Serial.println("Beginning HTTP connection..."); // DIAGNOSTIC PRINT
  bool httpBeginSuccess = http.begin(client, flaskHost, flaskPort, recognizePersonEndpoint);
  Serial.printf("HTTP begin status: %s\n", httpBeginSuccess ? "SUCCESS" : "FAILED"); // DIAGNOSTIC PRINT
  if (!httpBeginSuccess) {
    Serial.println("HTTPClient begin failed. Aborting request.");
    http.end();
    deinitCamera();
    return;
  }

  http.addHeader("Content-Type", "application/json");

  long startTime = millis();
  int httpResponseCode = http.POST(payload);
  long endTime = millis();

  Serial.printf("HTTP POST to RECOGNIZE took %ld ms\n", endTime - startTime);
  Serial.printf("HTTP Response code: %d\n", httpResponseCode);

  if (httpResponseCode > 0) {
    String responsePayload = http.getString();
    Serial.print("Server Response: ");
    Serial.println(responsePayload);

    String status, message, personName, assignedUser, suggestedUser;
    if (parseAndHandleFlaskResponse(responsePayload, status, message, personName, assignedUser, suggestedUser)) {
      if (status == "recognized") {
        Serial.printf("✅ Recognized: %s (%s)\n", personName.c_str(), message.c_str());
        current_full_person_name = personName; // Update with the recognized person
      } else if (status == "not_recognized") {
        Serial.printf("❌ Not Recognized: %s. %s\n", personName.c_str(), message.c_str());
        current_full_person_name = ""; // Clear if not recognized
      } else if (status == "no_face_detected") {
        Serial.println("⚠️ No Face Detected in photo.");
        current_full_person_name = "";
      } else if (status == "no_database") {
        Serial.println("⚠️ Database is empty. Add persons first.");
        current_full_person_name = "";
      } else {
        Serial.printf("❌ Recognition FAILED: %s\n", message.c_str()); 
        current_full_person_name = "";
      }
    }
  } else {
    Serial.printf("Error: %s\n", http.errorToString(httpResponseCode).c_str());
  }
  http.end();
  deinitCamera(); 
  Serial.println("Type 'add' or 'recognize' for next action.");
}

// --- Parse Flask JSON Response ---
bool parseAndHandleFlaskResponse(String payload, String& status, String& message, String& personName, String& assignedUser, String& suggestedUser) {
  DynamicJsonDocument doc(16384); 
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return false;
  }

  status = doc["status"].as<String>();
  message = doc["message"].as<String>();
  personName = doc["person_name"].as<String>();
  assignedUser = doc.containsKey("assigned_user") ? doc["assigned_user"].as<String>() : ""; 
  suggestedUser = doc.containsKey("suggested_user") ? doc["suggested_user"].as<String>() : ""; 

  return true;
}
