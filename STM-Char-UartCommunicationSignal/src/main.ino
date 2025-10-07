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
const char* flaskHost = "172.19.2.151"; // REPLACE with your laptop's local IP address
const int flaskPort = 5000;
const char* addPersonEndpoint = "/add_person";
const char* recognizePersonEndpoint = "/recognize_person";

// --- Camera Configuration ---
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

// --- UART (to STM) configuration ---
// WARNING: verify these pins are NOT used by the camera on your board.
// Example chosen pins (change if they conflict): RX = 13, TX = 12
const int UART_RX_PIN = 13;
const int UART_TX_PIN = 14;  // can be good  14 or 15
HardwareSerial SerialUART(1); // Serial1

// --- Custom Logic Configuration ---
const String FIXED_CAR_ID = "car123";      // The car ID will be fixed
const int MAX_USERS_PER_CAR = 5;         // Maximum users allowed per car ID
String current_full_person_name = "";    // To store assigned or matched user, e.g., "car123/user01"

// --- Status tracking for requests (for 'S' command) ---
bool add_in_progress = false;
bool recognize_in_progress = false;
bool last_add_success = false;
bool last_recognize_success = false;
String last_add_message = "";
String last_recognize_message = "";

// --- NEW: name tracking variables ---
// These store both raw fields from Flask and a single display name for add.
String last_add_assigned = "";    // assigned_user returned when a new user was created
String last_add_suggested = "";   // suggested_user returned when duplicate detected
String last_added_person = "";    // chosen display name for the last add (assigned or suggested)
String last_recognized_person = ""; // name of the last successfully recognized person

// --- Global HTTP Client (for persistent connection if needed, otherwise local) ---
WiFiClient client;
HTTPClient http;

// --- Function Prototypes ---
void handleSerialInput();
void handleUARTInput();
void sendStatusOverUART();
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

  // init UART to STM
  SerialUART.begin(115200, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  Serial.println("Serial1 (UART) started for STM comms.");
  Serial.printf("UART RX pin: %d, UART TX pin: %d\n", UART_RX_PIN, UART_TX_PIN);

  connectToWiFi();
  // Camera is NOT initialized here anymore! It will be initialized on demand.

  Serial.println("Type 'add' to register a new face, 'recognize' to identify a face (USB), or send A/R/S over UART from STM.");
  Serial.printf("Initial Free Heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("Initial Min Free Heap: %u bytes\n", ESP.getMinFreeHeap());
  Serial.printf("Initial Free PSRAM: %u bytes\n", ESP.getFreePsram()); // Monitor PSRAM
}

// --- Main Loop ---
void loop() {
  handleSerialInput(); // keep old USB Serial-based interface (for debugging/manual)
  handleUARTInput();   // handle commands coming from STM via UART
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

// --- Handle USB Serial Monitor Input (keeps original behavior) ---
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

// --- Handle UART input from STM (single char commands A, R, S) ---
void handleUARTInput() {
  while (SerialUART.available()) {
    char c = (char)SerialUART.read();
    // ignore newline/carriage
    if (c == '\r' || c == '\n') continue;

    Serial.printf("UART Received: '%c'\n", c);
    // Echo the single character back to STM immediately so it knows the message was received
    //delay(500);
    SerialUART.write(c);

    if (c == 'A') {
      // Start add person flow
      add_in_progress = true;
      last_add_success = false;
      last_add_message = "Started";
      // reset add-name fields for new attempt
      last_add_assigned = "";
      last_add_suggested = "";
      last_added_person = "";
      sendAddPersonRequest();
      c = '\0';
    } else if (c == 'R') {
      recognize_in_progress = true;
      last_recognize_success = false;
      last_recognize_message = "Started";
      // reset recognized name until result
      last_recognized_person = "";
      sendRecognizePersonRequest();
      c = '\0';
    } else if (c == 'S') {
      sendStatusOverUART();
      c = '\0';
    } else {
      // unknown
    }
  }
}

// --- Send overview status over UART to STM ---
void sendStatusOverUART() {
  char recFlag;
  char addFlag;

  if (last_recognize_success) {
      recFlag = 'T';
  } else {
      recFlag = 'F';
  }

  if (last_add_success) {
      addFlag = 'T';
  } else {
      addFlag= 'F';
  }
  // Header to make parsing deterministic on STM side
  SerialUART.write('!'); 
  SerialUART.write('R');     // start marker
  SerialUART.write(recFlag);
  SerialUART.write('A'); // recognition/rec flag
  SerialUART.write(addFlag);  // add success debug flag
  SerialUART.write('\n');     // newline terminator
  Serial.println("Sent STATUS over UART to STM"); // debug locally over USB
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
  add_in_progress = true;
  last_add_success = false;
  last_add_message = "Starting";

  // Initialize camera before capture
  if (!initCamera()) {
    Serial.println("Camera initialization failed. Aborting sendAddPersonRequest."); // DIAGNOSTIC PRINT
    last_add_message = "Camera init failed";
    add_in_progress = false;
    return; // Stop if camera failed to initialize
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Reconnecting...");
    connectToWiFi();
    // Re-check WiFi status after reconnect attempt
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi failed to reconnect. Aborting request.");
      last_add_message = "WiFi reconnect failed";
      deinitCamera(); 
      add_in_progress = false;
      return;
    }
  }

  String photo_b64 = captureAndEncodePhoto();
  if (photo_b64 == "") {
    Serial.println("Photo capture/encoding failed. Aborting sendAddPersonRequest."); // DIAGNOSTIC PRINT
    last_add_message = "Capture/encode failed";
    deinitCamera(); 
    add_in_progress = false;
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
  //Serial.println(payload); // avoid huge print in production

  Serial.println("Beginning HTTP connection..."); // DIAGNOSTIC PRINT
  bool httpBeginSuccess = http.begin(client, flaskHost, flaskPort, addPersonEndpoint);
  Serial.printf("HTTP begin status: %s\n", httpBeginSuccess ? "SUCCESS" : "FAILED"); // DIAGNOSTIC PRINT
  if (!httpBeginSuccess) {
    Serial.println("HTTPClient begin failed. Aborting request.");
    http.end();
    deinitCamera();
    last_add_message = "HTTP begin failed";
    add_in_progress = false;
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
      last_add_message = message;

      // NEW: store raw assigned/suggested fields
      last_add_assigned = assignedUser;
      last_add_suggested = suggestedUser;

      if (status == "success") {
        Serial.println("✅ Add Person SUCCESS!");
        last_add_success = true;
        // Update chosen last_added_person to assigned
        if (assignedUser != "") {
          last_added_person = assignedUser;
          current_full_person_name = assignedUser; // keep compatibility
          Serial.printf("Assigned/Updated user by Flask: %s\n", last_added_person.c_str());
        } else {
          // fallback: use personName if assigned missing
          last_added_person = personName;
          current_full_person_name = personName;
          Serial.printf("User added/updated, assigned_user missing. Person name: %s\n", personName.c_str());
        }
      } else if (status == "error" && suggestedUser != "") {
        Serial.println("❌ Add Person FAILED: Face already registered for this Car ID.");
        Serial.printf("Server suggests using: %s\n", suggestedUser.c_str());
        last_add_success = false;
        // For duplicate, choose suggested as the last_added_person (per your requirement)
        last_added_person = suggestedUser;
        last_add_suggested = suggestedUser;
        current_full_person_name = suggestedUser; // keep compatibility
        last_add_message = String("Already registered; suggestion: ") + suggestedUser;
      } else {
        Serial.printf("❌ Add Person FAILED: %s\n", message.c_str()); 
        last_add_success = false;
        // clear last_added_person if add failed with no suggestion
        last_added_person = "";
      }
    } else {
      last_add_success = false;
      last_add_message = "JSON parse failed";
      last_added_person = "";
    }
  } else {
    Serial.printf("Error: %s\n", http.errorToString(httpResponseCode).c_str());
    last_add_success = false;
    last_add_message = String("HTTP error: ") + http.errorToString(httpResponseCode).c_str();
    last_added_person = "";
  }
  http.end();
  deinitCamera(); 
  add_in_progress = false;
  Serial.println("Type 'add' or 'recognize' for next action.");
}

// --- Send Recognize Person Request ---
void sendRecognizePersonRequest() {
  Serial.println("Entering sendRecognizePersonRequest()..."); // DIAGNOSTIC PRINT
  recognize_in_progress = true;
  last_recognize_success = false;
  last_recognize_message = "Starting";

  // Initialize camera before capture
  if (!initCamera()) {
    Serial.println("Camera initialization failed. Aborting sendRecognizePersonRequest."); // DIAGNOSTIC PRINT
    last_recognize_message = "Camera init failed";
    recognize_in_progress = false;
    return; // Stop if camera failed to initialize
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Reconnecting...");
    connectToWiFi();
    // Re-check WiFi status after reconnect attempt
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi failed to reconnect. Aborting request.");
      last_recognize_message = "WiFi reconnect failed";
      deinitCamera(); 
      recognize_in_progress = false;
      return;
    }
  }

  String photo_b64 = captureAndEncodePhoto();
  if (photo_b64 == "") {
    Serial.println("Photo capture/encoding failed. Aborting sendRecognizePersonRequest."); // DIAGNOSTIC PRINT
    last_recognize_message = "Capture/encode failed";
    deinitCamera(); 
    recognize_in_progress = false;
    return; 
  }

  Serial.println("Preparing RECOGNIZE request...");
  String payload;
  DynamicJsonDocument doc(64000); 
  doc["photo"] = photo_b64;
  serializeJson(doc, payload);

  Serial.println("Beginning HTTP connection..."); // DIAGNOSTIC PRINT
  bool httpBeginSuccess = http.begin(client, flaskHost, flaskPort, recognizePersonEndpoint);
  Serial.printf("HTTP begin status: %s\n", httpBeginSuccess ? "SUCCESS" : "FAILED"); // DIAGNOSTIC PRINT
  if (!httpBeginSuccess) {
    Serial.println("HTTPClient begin failed. Aborting request.");
    http.end();
    deinitCamera();
    last_recognize_message = "HTTP begin failed";
    recognize_in_progress = false;
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
      last_recognize_message = message;
      if (status == "recognized") {
        Serial.printf("✅ Recognized: %s (%s)\n", personName.c_str(), message.c_str());
        last_recognize_success = true;
        last_recognized_person = personName; // NEW: store last recognized
        current_full_person_name = personName; // compatibility
      } else if (status == "not_recognized") {
        Serial.printf("❌ Not Recognized: %s. %s\n", personName.c_str(), message.c_str());
        last_recognized_person = "";
        current_full_person_name = ""; // Clear if not recognized
        last_recognize_success = false;
      } else if (status == "no_face_detected") {
        Serial.println("⚠️ No Face Detected in photo.");
        last_recognized_person = "";
        current_full_person_name = "";
        last_recognize_success = false;
      } else if (status == "no_database") {
        Serial.println("⚠️ Database is empty. Add persons first.");
        last_recognized_person = "";
        current_full_person_name = "";
        last_recognize_success = false;
      } else {
        Serial.printf("❌ Recognition FAILED: %s\n", message.c_str()); 
        last_recognized_person = "";
        current_full_person_name = "";
        last_recognize_success = false;
      }
    } else {
      last_recognize_success = false;
      last_recognize_message = "JSON parse failed";
      last_recognized_person = "";
    }
  } else {
    Serial.printf("Error: %s\n", http.errorToString(httpResponseCode).c_str());
    last_recognize_success = false;
    last_recognize_message = String("HTTP error: ") + http.errorToString(httpResponseCode).c_str();
    last_recognized_person = "";
  }
  http.end();
  deinitCamera(); 
  recognize_in_progress = false;
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
