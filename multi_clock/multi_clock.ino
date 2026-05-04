#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT20.h>
#include <IRremote.hpp>
#include <Preferences.h>
#include <Arduino_MQTT_Client.h>
#include <ThingsBoard.h>
#include <Server_Side_RPC.h>
#include <esp_task_wdt.h>
#include <time.h>

// --- Pin Assignments ---
#define IR_RECEIVE_PIN 25
#define IR_SEND_PIN 26
#define LED_PIN 2          // Kept for status indication if needed
#define SDA_PIN GPIO_NUM_21
#define SCL_PIN GPIO_NUM_22

const int LEARN_BTN_1_PIN = 16;  // Learn Signal 1
const int LEARN_BTN_2_PIN = 17;  // Learn Signal 2
const int SEND_BTN_PIN = 15;     // Send alternating signals

// --- ThingsBoard & Network Config ---
constexpr char WIFI_SSID[] = "NHA 138";
constexpr char WIFI_PASSWORD[] = "138nguyentrai";
constexpr char TOKEN[] = "7fd15ugptp5cs2yil8ad";
constexpr char THINGSBOARD_SERVER[] = "app.coreiot.io";
constexpr uint16_t THINGSBOARD_PORT = 1883U;

constexpr uint32_t MAX_MESSAGE_SIZE = 1024U;
constexpr uint32_t SERIAL_DEBUG_BAUD = 115200U;
constexpr int16_t telemetrySendInterval = 10000U;

WiFiClient wifiClient;
Arduino_MQTT_Client tbAdapter(wifiClient);

// --- NEW V0.14+ ARCHITECTURE ---
// 1. Create an instance of the RPC module
Server_Side_RPC<2U> rpc;

// 2. Add the RPC module to an API array
const std::array<IAPI_Implementation*, 1> apis = { &rpc };

// 3. Pass it to the constructor
ThingsBoard tb(tbAdapter, MAX_MESSAGE_SIZE, MAX_MESSAGE_SIZE, Default_Max_Stack_Size, apis);

SemaphoreHandle_t tbMutex;

// --- Global LCD State Tracking ---
volatile bool isLcdBacklightOn = true;
volatile bool reqBacklightToggle = false;

// --- Hardware Objects ---
LiquidCrystal_I2C lcd(0x27, 20, 4);
DHT20 dht20;

// --- FreeRTOS Definitions ---
enum Command {
  CMD_NONE,
  CMD_START_LEARN_1,
  CMD_START_LEARN_2,
  CMD_SEND_TOGGLE
};

QueueHandle_t commandQueue; 
QueueHandle_t sensorQueue;
TaskHandle_t TimeSyncTaskHandle = NULL;

struct SensorData {
  float temperature;
  float humidity;
};

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = -25200; 
const int   daylightOffset_sec = 3600;

// --- Forward Declarations ---
void task_button(void *pvParameters);
void task_IRcontrol(void *pvParameters);
void task_sensor(void *pvParameters);
void task_lcd(void *pvParameters);
void task_network(void *pvParameters);
void task_getTime(void *pvParameters);

// --- ThingsBoard RPC Callback ---
void switchDevice(const JsonVariantConst &data, JsonDocument &response) {
    Serial.println("\n[Network] Dashboard Button Clicked! Firing IR...");

    Command cmd = CMD_SEND_TOGGLE;
    xQueueSend(commandQueue, &cmd, 0);
    
    response["toggleDevice"] = "Success";
}

void setLcdBacklight(const JsonVariantConst &data, JsonDocument &response) {
  Serial.println("\n[Network] LCD Backlight Switch Toggled!");
  
  // Extract state and sync with global tracker
  bool newState = data.as<bool>();
  isLcdBacklightOn = newState;
  
  if (newState) {
    lcd.backlight();
    Serial.println("LCD Backlight ON");
  } else {
    lcd.noBacklight();
    Serial.println("LCD Backlight OFF");
  }
  
  response["setLcdBacklightValue"] = newState;
}

const std::array<RPC_Callback, 2U> callbacks = {
  RPC_Callback{ "toggleDevice", switchDevice },
  RPC_Callback{ "setLcdBacklightValue", setLcdBacklight }
};

// --- WiFi Initialization ---
void InitWiFi() {
  Serial.println("Connecting to AP ...");
  WiFi.disconnect(); 
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    vTaskDelay(pdMS_TO_TICKS(500)); 
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to AP");
  } else {
    Serial.println("\nFailed to connect to AP. Will retry later.");
  }
}

void setup() {
  Serial.begin(SERIAL_DEBUG_BAUD);
  while (!Serial);

  // Configure hardware pins
  pinMode(LEARN_BTN_1_PIN, INPUT_PULLUP);
  pinMode(LEARN_BTN_2_PIN, INPUT_PULLUP);
  pinMode(SEND_BTN_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setTimeOut(3000);
  
  dht20.begin();

  // Watchdog setup
  esp_task_wdt_config_t twdt_config = {
      .timeout_ms = 30000,
      .idle_core_mask = (1 << 0), 
      .trigger_panic = true,
  };
  esp_task_wdt_init(&twdt_config);

  // Initialize Queues & Mutex
  commandQueue = xQueueCreate(5, sizeof(Command));
  sensorQueue = xQueueCreate(1, sizeof(SensorData)); 
  tbMutex = xSemaphoreCreateMutex();

  if (commandQueue == NULL || sensorQueue == NULL) {
    Serial.println("Error creating the Queues!");
    return;
  }

  wifiClient.setTimeout(5);
  InitWiFi();

  // CORE 1: Timing-sensitive hardware IO
  xTaskCreatePinnedToCore(task_button, "task_button", 2048, NULL, 2, NULL, 1); 
  xTaskCreatePinnedToCore(task_IRcontrol, "task_IRcontrol", 4096, NULL, 2, NULL, 1); 
  
  // CORE 0: Network processing and background displays
  xTaskCreatePinnedToCore(task_network, "Network", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(task_sensor, "Sensors", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(task_lcd, "LCD", 2048, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(task_getTime, "NTP_Sync", 4096, NULL, 1, &TimeSyncTaskHandle, 0);
  
  Serial.println("FreeRTOS Universal Remote + IoT Ready!");
  
  vTaskDelete(NULL); 
}

void loop() {
}

// ==========================================
// TASK: Handle User Input (Buttons) [Core 1]
// ==========================================
void task_button(void *pvParameters) {
  bool lastLearn1State = HIGH;
  bool lastLearn2State = HIGH;
  bool lastSendState = HIGH;

  uint32_t sendBtnPressTime = 0;
  bool isSendBtnTracking = false;

  for (;;) {
    bool currentLearn1State = digitalRead(LEARN_BTN_1_PIN);
    bool currentLearn2State = digitalRead(LEARN_BTN_2_PIN);
    bool currentSendState = digitalRead(SEND_BTN_PIN);

    // Learn Buttons
    if (lastLearn1State == HIGH && currentLearn1State == LOW) {
      Command cmd = CMD_START_LEARN_1;
      xQueueSend(commandQueue, &cmd, 0); 
    }
    if (lastLearn2State == HIGH && currentLearn2State == LOW) {
      Command cmd = CMD_START_LEARN_2;
      xQueueSend(commandQueue, &cmd, 0); 
    }
    
    // --- SEND_BTN: Short vs Long Press Logic ---
    if (lastSendState == HIGH && currentSendState == LOW) {
      // Button just pressed down
      sendBtnPressTime = millis();
      isSendBtnTracking = true;
    }
    
    // Check if held for >= 3 seconds (Triggers dynamically without waiting for release)
    if (isSendBtnTracking && currentSendState == LOW) {
      if (millis() - sendBtnPressTime >= 3000) {
        reqBacklightToggle = true;     // Signal Core 0 to safely toggle LCD Backlight
        isSendBtnTracking = false;     // Disable tracking so release ignores the short press
      }
    }

    // Button released
    if (lastSendState == LOW && currentSendState == HIGH) {
      if (isSendBtnTracking) {
        // Released before 3 seconds, measure duration for short press debounce
        uint32_t pressDuration = millis() - sendBtnPressTime;
        if (pressDuration >= 50) { 
          Command cmd = CMD_SEND_TOGGLE;
          xQueueSend(commandQueue, &cmd, 0);
        }
        isSendBtnTracking = false;
      }
    }

    lastLearn1State = currentLearn1State;
    lastLearn2State = currentLearn2State;
    lastSendState = currentSendState;

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ==========================================
// TASK: Handle IR Logic and Flash [Core 1]
// ==========================================
void task_IRcontrol(void *pvParameters) {
  Preferences preferences;
  IRData storedIRData[2];
  bool hasStoredSignal[2] = {false, false};
  int learningIndex = -1; 
  bool previous_send = false; 

  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);
  IrSender.begin(IR_SEND_PIN, ENABLE_LED_FEEDBACK);

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
        if (learningIndex == -1) { 
          if (previous_send == true) {
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

    if (learningIndex != -1) { 
      if (IrReceiver.decode()) {
        if (IrReceiver.decodedIRData.protocol != UNKNOWN) {
          storedIRData[learningIndex] = IrReceiver.decodedIRData; 
          hasStoredSignal[learningIndex] = true;
          
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

// ==========================================
// TASK: LCD Updates [Core 0]
// ==========================================
char currentScreen[4][20];
char nextScreen[4][20];

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

void pushBufferToLCD() {
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
}

void forceFullRedraw() {
  lcd.init();
  
  // Sync hardware with current global state
  if (isLcdBacklightOn) {
    lcd.backlight();
  } else {
    lcd.noBacklight();
  }
  
  memset(currentScreen, '\0', sizeof(currentScreen)); 
}

const char* getOrdinalSuffix(int day) {
  if (day >= 11 && day <= 13) return "th"; 
  switch (day % 10) {
    case 1:  return "st";
    case 2:  return "nd";
    case 3:  return "rd";
    default: return "th";
  }
}

void task_lcd(void *pvParameters) {
  SensorData receivedData; 
  receivedData.temperature = 0; 
  receivedData.humidity = 0;
  bool hasValidData = false;

  const TickType_t resetInterval = pdMS_TO_TICKS(3600000UL); 
  TickType_t lastResetTime = xTaskGetTickCount();

  initLCDBuffers();
  forceFullRedraw();

  for(;;) {
    // --- Check for Hardware Backlight Toggles securely on Core 0 ---
    if (reqBacklightToggle) {
        isLcdBacklightOn = !isLcdBacklightOn;
        if (isLcdBacklightOn) {
            lcd.backlight();
        } else {
            lcd.noBacklight();
        }
        reqBacklightToggle = false;
    }

    if (xQueueReceive(sensorQueue, &receivedData, pdMS_TO_TICKS(1000)) == pdTRUE) {
      hasValidData = true;
    }

    if ((xTaskGetTickCount() - lastResetTime) >= resetInterval) {
      forceFullRedraw();
      lastResetTime = xTaskGetTickCount();
    }

    clearNextScreen(); 

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) { 
      char timeStr[21]; 
      char dayName[4];  
      char monthName[4]; 

      strftime(dayName, sizeof(dayName), "%a", &timeinfo);
      strftime(monthName, sizeof(monthName), "%b", &timeinfo);

      snprintf(timeStr, sizeof(timeStr), "%02d:%02d %s, %d%s %s", 
               timeinfo.tm_hour, timeinfo.tm_min, 
               dayName, 
               timeinfo.tm_mday, getOrdinalSuffix(timeinfo.tm_mday), 
               monthName);

      int cursorPos = (20 - strlen(timeStr)) / 2;
      writeToBuffer(cursorPos, 0, timeStr);
    } else {
      const char* syncMsg = "Syncing...";
      int cursorPos = (20 - strlen(syncMsg)) / 2;
      writeToBuffer(cursorPos, 0, syncMsg);
    }

    if (hasValidData) {
      writeToBuffer(0, 1, "Condition:");
      if (receivedData.temperature > 30.0) {
        writeToBuffer(11, 1, "Hot");
      } 
      else if (receivedData.temperature >= 20.0 && receivedData.temperature <= 30.0) {
        writeToBuffer(11, 1, "Good");
      } 
      else {
        writeToBuffer(11, 1, "Cool");
      }

      writeToBuffer(0, 2, "Temp:");
      char tempStr[10];
      snprintf(tempStr, sizeof(tempStr), "%.1f C", receivedData.temperature);
      writeToBuffer(6, 2, tempStr);

      writeToBuffer(0, 3, "Humid:");
      char humStr[10];
      snprintf(humStr, sizeof(humStr), "%.1f %%", receivedData.humidity);
      writeToBuffer(7, 3, humStr);
    }

    pushBufferToLCD();
  }
}

// ==========================================
// TASK: DHT Read & Queue/Cloud [Core 0]
// ==========================================
void task_sensor(void *pvParameters) {
  SensorData data; 

  for(;;) {
    dht20.read();
    data.temperature = dht20.getTemperature();
    data.humidity = dht20.getHumidity();

    if (!isnan(data.temperature) && !isnan(data.humidity)) {
       xQueueOverwrite(sensorQueue, &data);

       if (xSemaphoreTake(tbMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
           if (tb.connected()) {
              tb.sendTelemetryData("temperature", data.temperature);
              tb.sendTelemetryData("humidity", data.humidity);
              
              tb.sendAttributeData("rssi", WiFi.RSSI());
              tb.sendAttributeData("localIp", WiFi.localIP().toString().c_str());
           }
           xSemaphoreGive(tbMutex); 
       } else {
           Serial.println("[Warn] Network busy/offline. Skipped cloud upload, updated LCD only.");
       }
    } else {
       Serial.println("Failed to read from DHT20 sensor!");
    }

    vTaskDelay(pdMS_TO_TICKS(telemetrySendInterval));
  }
}

// ==========================================
// TASK: Get Time
// ==========================================
void task_getTime(void *pvParameters) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    struct tm timeinfo;
    for (;;) {
        if (getLocalTime(&timeinfo)) {
            Serial.printf("System Time: %02d:%02d:%02d\n", 
                          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        } else {
            Serial.println("Time Task: Failed to obtain time");
        }

        vTaskDelay(pdMS_TO_TICKS(3600000)); 
    }
}

// ==========================================
// TASK: Network & MQTT Logic [Core 0]
// ==========================================
void task_network(void *pvParameters) {
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      InitWiFi();
    }

    if (xSemaphoreTake(tbMutex, portMAX_DELAY) == pdTRUE) {
        if (!tb.connected()) {
            Serial.print("Connecting to: ");
            Serial.print(THINGSBOARD_SERVER);
            Serial.print(" with token ");
            Serial.println(TOKEN);
            
            if (!tb.connect(THINGSBOARD_SERVER, TOKEN, THINGSBOARD_PORT)) {
                Serial.println("Failed to connect");
                xSemaphoreGive(tbMutex); 
                vTaskDelay(pdMS_TO_TICKS(3000)); 
                continue; 
            } else {
                tb.sendAttributeData("macAddress", WiFi.macAddress().c_str());
                Serial.println("Subscribing for RPC...");
                
                if (!rpc.RPC_Subscribe(callbacks.cbegin(), callbacks.cend())) {
                    Serial.println("Failed to subscribe for RPC");
                } else {
                    Serial.println("Subscribe done");
                }
            }
        }

        if (tb.connected()) {
            tb.loop();
        }

        xSemaphoreGive(tbMutex); 
    }

    vTaskDelay(pdMS_TO_TICKS(10)); 
  }
}