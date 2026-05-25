/**
 * @file multi_clock.ino
 * @brief FreeRTOS Dual-Core Universal IR Remote & Weather Station
 * 
 * Hardware Layout:
 * - Core 1: User Interactions, Buttons, IR Tx/Rx, DHT20 Sensor Sampling, Double-Buffered LCD (I2C)
 * - Core 0: WiFi Connection Maintenance, Weather API Fetching, Network Stack (lwIP)
 * 
 * Dependencies:
 * - LiquidCrystal_I2C, DHT20, IRremote, ArduinoJson, WiFi, Preferences, HTTPClient
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT20.h>
#include <IRremote.hpp>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ============================================================================
// 1. PIN ASSIGNMENTS & HARDWARE CONFIG
// ============================================================================
#define IR_RECEIVE_PIN 25
#define IR_SEND_PIN    26
#define LED_PIN        2          
#define SDA_PIN        GPIO_NUM_21
#define SCL_PIN        GPIO_NUM_22

const int LEARN_BTN_1_PIN = 16;  
const int LEARN_BTN_2_PIN = 17;  
const int SEND_BTN_PIN    = 15;     

// ============================================================================
// 2. NETWORK & WEATHER CONFIGURATION
// ============================================================================
constexpr char WIFI_SSID[]           = "NHA 138";
constexpr char WIFI_PASSWORD[]       = "138nguyentrai";
constexpr char WEATHER_API_KEY[]     = "504ca910c5a942dab9b231229262505"; // Set your WeatherAPI.com API Key here
constexpr uint32_t SERIAL_DEBUG_BAUD     = 115200U;
constexpr int16_t telemetrySendInterval  = 10000U;

// ============================================================================
// 3. SYNCHRONIZATION & QUEUE DEFINITIONS
// ============================================================================
SemaphoreHandle_t i2cMutex;

enum Command {
  CMD_NONE,
  CMD_START_LEARN_1,
  CMD_START_LEARN_2,
  CMD_SEND_TOGGLE
};

struct SensorData {
  float temperature;
  float humidity;
};

QueueHandle_t commandQueue; 
QueueHandle_t sensorQueue;

// ============================================================================
// 4. GLOBAL STATE VARIABLES
// ============================================================================
volatile bool isLcdBacklightOn       = true;
volatile bool reqBacklightToggle     = false;
volatile float outsideTemp           = 0.0;
volatile bool hasOutsideTemp         = false;

// LCD Double-Buffering Variables
char currentScreen[4][20];
char nextScreen[4][20];

// Hardware Instances
LiquidCrystal_I2C lcd(0x27, 20, 4);
DHT20 dht20;

// Forward Declarations
void task_button(void *pvParameters);
void task_IRcontrol(void *pvParameters);
void task_sensor(void *pvParameters);
void task_lcd(void *pvParameters);
void task_network(void *pvParameters);
void task_weather(void *pvParameters);

// ============================================================================
// 6. NETWORKING HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Initialize WiFi and Connect to configured AP
 */
void InitWiFi() {
  Serial.println("[System] Connecting to AP...");
  WiFi.disconnect(); 
  
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    vTaskDelay(pdMS_TO_TICKS(500)); 
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[System] WiFi Connected!");
    WiFi.setTxPower(WIFI_POWER_19_5dBm); 
  } else {
    Serial.println("\n[System] Failed to connect to AP. Will retry later.");
  }
}

// ============================================================================
// 7. MAIN ARDUINO SETUP & LOOP
// ============================================================================
void setup() {
  Serial.begin(SERIAL_DEBUG_BAUD);
  
  // Set up button GPIOs with internal pull-ups
  pinMode(LEARN_BTN_1_PIN, INPUT_PULLUP);
  pinMode(LEARN_BTN_2_PIN, INPUT_PULLUP);
  pinMode(SEND_BTN_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  
  // Initialize I2C Bus and Pins
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setTimeOut(3000);
  
  // Initialize Peripherals
  lcd.init();
  if (isLcdBacklightOn) lcd.backlight();
  dht20.begin();

  // Initialize Infrared Receiver & Transmitter
  IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);
  IrSender.begin(IR_SEND_PIN, DISABLE_LED_FEEDBACK);

  // Initialize FreeRTOS Queues and Mutexes
  commandQueue = xQueueCreate(5, sizeof(Command));
  sensorQueue  = xQueueCreate(1, sizeof(SensorData)); 
  i2cMutex     = xSemaphoreCreateMutex();

  if (commandQueue == NULL || sensorQueue == NULL || i2cMutex == NULL) {
    Serial.println("[System Error] Failed creating RTOS Queues or Mutex!");
    return;
  }

  InitWiFi();

  Serial.println("[System] Booting RTOS Tasks...");

  // ==========================================================================
  // CORE 1 (APP CPU): User Interactions, Precise Timing, & I2C Peripherals
  // ==========================================================================
  xTaskCreatePinnedToCore(task_button, "task_button", 2048, NULL, 3, NULL, 1); 
  vTaskDelay(pdMS_TO_TICKS(50));
  
  xTaskCreatePinnedToCore(task_IRcontrol, "task_IRcontrol", 4096, NULL, 3, NULL, 1); 
  vTaskDelay(pdMS_TO_TICKS(50));
  
  xTaskCreatePinnedToCore(task_sensor, "Sensors", 2048, NULL, 1, NULL, 1);
  vTaskDelay(pdMS_TO_TICKS(50));
  
  xTaskCreatePinnedToCore(task_lcd, "LCD", 3072, NULL, 1, NULL, 1);
  vTaskDelay(pdMS_TO_TICKS(50));

  // ==========================================================================
  // CORE 0 (PRO CPU): Background Network Connection & Weather Fetching
  // ==========================================================================
  xTaskCreatePinnedToCore(task_network, "Network", 4096, NULL, 1, NULL, 0);
  vTaskDelay(pdMS_TO_TICKS(50));
  
  xTaskCreatePinnedToCore(task_weather, "WeatherAPI", 6144, NULL, 1, NULL, 0);
  vTaskDelay(pdMS_TO_TICKS(50));
  
  Serial.println("[System] FreeRTOS Universal Remote + IoT Ready!");
}

void loop() {
  // The loop runs on APP_CPU (Core 1) with low priority, we simply yield
  vTaskDelay(pdMS_TO_TICKS(100)); 
}

// ============================================================================
// 8. CORE 1 TASKS (Application Logic)
// ============================================================================

/**
 * @brief Task: Debounces hardware buttons and routes commands
 */
void task_button(void *pvParameters) {
  bool lastLearn1State  = HIGH;
  bool lastLearn2State  = HIGH;
  bool lastSendState    = HIGH;
  uint32_t sendBtnPressTime = 0;
  bool isSendBtnTracking    = false;

  for (;;) {
    bool currentLearn1State = digitalRead(LEARN_BTN_1_PIN);
    bool currentLearn2State = digitalRead(LEARN_BTN_2_PIN);
    bool currentSendState   = digitalRead(SEND_BTN_PIN);

    // Learn 1 Button (Press Event)
    if (lastLearn1State == HIGH && currentLearn1State == LOW) {
      Command cmd = CMD_START_LEARN_1;
      xQueueSend(commandQueue, &cmd, 0); 
    }
    
    // Learn 2 Button (Press Event)
    if (lastLearn2State == HIGH && currentLearn2State == LOW) {
      Command cmd = CMD_START_LEARN_2;
      xQueueSend(commandQueue, &cmd, 0); 
    }
    
    // Send Button Press Event
    if (lastSendState == HIGH && currentSendState == LOW) {
      sendBtnPressTime   = millis();
      isSendBtnTracking  = true;
    }
    
    // Send Button Long Press Detection (>= 3 Seconds)
    if (isSendBtnTracking && currentSendState == LOW) {
      if (millis() - sendBtnPressTime >= 3000) {
        reqBacklightToggle = true;     
        isSendBtnTracking  = false;     
      }
    }

    // Send Button Release Event (Short Press Detection)
    if (lastSendState == LOW && currentSendState == HIGH) {
      if (isSendBtnTracking) {
        uint32_t pressDuration = millis() - sendBtnPressTime;
        if (pressDuration >= 50) { // Software debounce threshold
          Command cmd = CMD_SEND_TOGGLE;
          xQueueSend(commandQueue, &cmd, 0);
        }
        isSendBtnTracking = false;
      }
    }

    lastLearn1State = currentLearn1State;
    lastLearn2State = currentLearn2State;
    lastSendState   = currentSendState;

    vTaskDelay(pdMS_TO_TICKS(20)); // Polled debouncer running every 20ms
  }
}

/**
 * @brief Task: Manages IR capture, NVM (Preferences) storage, and transmission
 */
void task_IRcontrol(void *pvParameters) {
  Preferences preferences;
  IRData storedIRData[2];
  bool hasStoredSignal[2] = {false, false};
  int learningIndex = -1; 
  bool previous_send = false; 

  // Load previously saved IR commands from NVM
  preferences.begin("ir_store", true); 
  hasStoredSignal[0] = preferences.getBool("has_data_1", false);
  if (hasStoredSignal[0]) {
    preferences.getBytes("ir_data_1", &storedIRData[0], sizeof(IRData));
    Serial.println("[System] Found Signal 1 in flash.");
  }
  
  hasStoredSignal[1] = preferences.getBool("has_data_2", false);
  if (hasStoredSignal[1]) {
    preferences.getBytes("ir_data_2", &storedIRData[1], sizeof(IRData));
    Serial.println("[System] Found Signal 2 in flash.");
  }
  preferences.end();

  for (;;) {
    Command receivedCmd;

    // Check queue for incoming remote control actions
    if (xQueueReceive(commandQueue, &receivedCmd, 0) == pdTRUE) {
      if (receivedCmd == CMD_START_LEARN_1) {
        learningIndex = 0;
        Serial.println("\n[Mode] --- LEARNING ACTIVE (SIGNAL 1) ---");
        IrReceiver.resume();
      } 
      else if (receivedCmd == CMD_START_LEARN_2) {
        learningIndex = 1;
        Serial.println("\n[Mode] --- LEARNING ACTIVE (SIGNAL 2) ---");
        IrReceiver.resume(); 
      }
      else if (receivedCmd == CMD_SEND_TOGGLE) {
        if (learningIndex == -1) { // Block transmission while in learning state
          if (previous_send) {
            if (hasStoredSignal[1]) {
              Serial.println("\n[Tx] Sending stored Signal 2...");
              IrReceiver.stop(); 
              IrSender.write(&storedIRData[1]);
              IrReceiver.start(); 
              previous_send = false; 
            } else {
              Serial.println("\n[Error] Signal 2 not learned yet!");
            }
          } else {
            if (hasStoredSignal[0]) {
              Serial.println("\n[Tx] Sending stored Signal 1...");
              IrReceiver.stop(); 
              IrSender.write(&storedIRData[0]);
              IrReceiver.start(); 
              previous_send = true;
            } else {
              Serial.println("\n[Error] Signal 1 not learned yet!");
            }
          }
        }
      }
    }

    // Process Active IR Learning Operations
    if (learningIndex != -1) { 
      if (IrReceiver.decode()) {
        if (IrReceiver.decodedIRData.protocol != UNKNOWN) {
          storedIRData[learningIndex] = IrReceiver.decodedIRData; 
          hasStoredSignal[learningIndex] = true;
          
          // Persist learned command to Flash
          preferences.begin("ir_store", false); 
          if (learningIndex == 0) {
            preferences.putBytes("ir_data_1", &storedIRData[0], sizeof(IRData));
            preferences.putBool("has_data_1", true);
            Serial.println("[Rx] Signal 1 learned & saved!");
          } else {
            preferences.putBytes("ir_data_2", &storedIRData[1], sizeof(IRData));
            preferences.putBool("has_data_2", true);
            Serial.println("[Rx] Signal 2 learned & saved!");
          }
          preferences.end();

          IrReceiver.printIRResultShort(&Serial);
          learningIndex = -1; 
        } else {
          Serial.println("[Rx Error] Protocol UNKNOWN. Try again.");
          IrReceiver.resume(); 
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10)); 
  }
}

/**
 * @brief Task: Handles WiFi Lifecycle, MQTT Broker Connection, and RPC Events
 */
void task_network(void *pvParameters) {
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      InitWiFi();
    }
    
    
    vTaskDelay(pdMS_TO_TICKS(50)); // Yield to allow other tasks to breathe
  }
}

/**
 * @brief Task: Fetches weather data periodically from WeatherAPI.com REST API
 */
void task_weather(void *pvParameters) {
  char weatherUrl[150];
  snprintf(weatherUrl, sizeof(weatherUrl), "http://api.weatherapi.com/v1/current.json?key=%s&q=10.759197,106.678694", WEATHER_API_KEY);

  // Pause initially to allow network configuration to fully complete
  vTaskDelay(pdMS_TO_TICKS(10000)); 

  for(;;) {
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.setTimeout(3000); // 3-second network client timeout
      http.begin(weatherUrl);
      int httpCode = http.GET();

      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
          outsideTemp    = doc["current"]["temp_c"];
          hasOutsideTemp = true;
          Serial.printf("[Weather] Outside Temp updated: %.1f C\n", outsideTemp);
        } else {
          Serial.print("[Weather] JSON Parse Error: ");
          Serial.println(error.c_str());
        }
      } else {
        Serial.printf("[Weather] HTTP Request failed, code: %d\n", httpCode);
      }
      http.end();
      
      // Update weather conditions every 15 minutes
      vTaskDelay(pdMS_TO_TICKS(15 * 60 * 1000));
    } else {
      vTaskDelay(pdMS_TO_TICKS(10000));
    }
  }
}

// ============================================================================
// 9. CORE 0 TASKS (System & Peripheral Handlers)
// ============================================================================

/**
 * @brief Task: Samples the DHT20 sensor and uploads data to ThingsBoard
 */
void task_sensor(void *pvParameters) {
  SensorData data; 

  for(;;) {
    // Exclusively access I2C bus for DHT20 read
    if (xSemaphoreTake(i2cMutex, portMAX_DELAY)) {
      dht20.read();
      data.temperature = dht20.getTemperature();
      data.humidity    = dht20.getHumidity();
      xSemaphoreGive(i2cMutex);
    }

    if (!isnan(data.temperature) && !isnan(data.humidity)) {
       // Push current values to LCD renderer queue
       xQueueOverwrite(sensorQueue, &data);

    }

    vTaskDelay(pdMS_TO_TICKS(telemetrySendInterval));
  }
}

// ============================================================================
// 10. LCD RENDERING HELPERS & DOUBLE BUFFER
// ============================================================================

void initLCDBuffers() {
  memset(currentScreen, ' ', sizeof(currentScreen));
  memset(nextScreen, ' ', sizeof(nextScreen));
}

void clearNextScreen() {
  memset(nextScreen, ' ', sizeof(nextScreen));
}

void writeToBuffer(int col, int row, const char* str) {
  if (row < 0 || row >= 4) return;
  int i = 0;
  while (str[i] != '\0' && (col + i) < 20) {
    nextScreen[row][col + i] = str[i];
    i++;
  }
}

/**
 * @brief Compares and writes only modified segments to the LCD over I2C
 */
void pushBufferToLCD() {
  if (xSemaphoreTake(i2cMutex, portMAX_DELAY)) {
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 20; c++) {
        if (nextScreen[r][c] != currentScreen[r][c]) {
          lcd.setCursor(c, r);
          
          String diff = "";
          while (c < 20 && nextScreen[r][c] != currentScreen[r][c]) {
            diff += nextScreen[r][c];
            currentScreen[r][c] = nextScreen[r][c];
            c++;
          }
          lcd.print(diff);
          c--; 
        }
      }
    }
    xSemaphoreGive(i2cMutex);
  }
}

/**
 * @brief Performs a full physical LCD clearing to correct transient EMI artifacts
 */
void forceFullRedraw() {
  if (xSemaphoreTake(i2cMutex, portMAX_DELAY)) {
    lcd.clear(); 
    if (isLcdBacklightOn) {
      lcd.backlight();
    } else {
      lcd.noBacklight();
    }
    memset(currentScreen, '\0', sizeof(currentScreen)); 
    xSemaphoreGive(i2cMutex);
  }
}

/**
 * @brief Task: Double-buffered screen manager
 */
void task_lcd(void *pvParameters) {
  SensorData receivedData; 
  receivedData.temperature = 0; 
  receivedData.humidity    = 0;
  bool hasValidData        = false;

  const TickType_t resetInterval = pdMS_TO_TICKS(3600000UL); // Hard refresh hourly
  TickType_t lastResetTime       = xTaskGetTickCount();

  initLCDBuffers();
  forceFullRedraw();

  for(;;) {
    // Process physical backlight modifications
    if (reqBacklightToggle) {
        isLcdBacklightOn = !isLcdBacklightOn;
        if (xSemaphoreTake(i2cMutex, portMAX_DELAY)) {
            if (isLcdBacklightOn) lcd.backlight();
            else lcd.noBacklight();
            xSemaphoreGive(i2cMutex);
        }
        reqBacklightToggle = false;
    }

    // Wait for fresh sensor packets
    if (xQueueReceive(sensorQueue, &receivedData, pdMS_TO_TICKS(100)) == pdTRUE) {
      hasValidData = true;
    }

    // Hourly full refresh to correct physical display discrepancies
    if ((xTaskGetTickCount() - lastResetTime) >= resetInterval) {
      forceFullRedraw();
      lastResetTime = xTaskGetTickCount();
    }

    clearNextScreen(); 

    // Render weather forecasting info (Row 0)
    if (hasOutsideTemp) {
      char outTempStr[21];
      snprintf(outTempStr, sizeof(outTempStr), "Out side temp:%.1f C", outsideTemp);
      
      int cursorPos = (20 - strlen(outTempStr)) / 2;
      if (cursorPos < 0) cursorPos = 0;
      writeToBuffer(cursorPos, 0, outTempStr);
    } else {
      const char* waitStr = "Fetching Weather...";
      int cursorPos = (20 - strlen(waitStr)) / 2;
      writeToBuffer(cursorPos, 0, waitStr);
    }

    // Render sensor readouts (Rows 1, 2, 3)
    if (hasValidData) {
      writeToBuffer(0, 1, "Condition:");
      if (receivedData.temperature > 30.0) writeToBuffer(11, 1, "Hot");
      else if (receivedData.temperature >= 20.0 && receivedData.temperature <= 30.0) writeToBuffer(11, 1, "Good");
      else writeToBuffer(11, 1, "Cool");

      char tempStr[10];
      snprintf(tempStr, sizeof(tempStr), "%.1f C", receivedData.temperature);
      writeToBuffer(0, 2, "Temp:");
      writeToBuffer(6, 2, tempStr);

      char humStr[10];
      snprintf(humStr, sizeof(humStr), "%.1f %%", receivedData.humidity);
      writeToBuffer(0, 3, "Humid:");
      writeToBuffer(7, 3, humStr);
    }

    // Push logical differences over to physical LCD
    pushBufferToLCD();
  }
}
