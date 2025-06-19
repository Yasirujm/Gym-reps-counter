#include "Wire.h"
#include "I2Cdev.h"
#include "MPU6050.h"
#include "DHT.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// WiFi credentials
const char* ssid = "";
const char* password = "";

// Firebase URLs
const String baseURL = "";
const String dataURL = baseURL + "data.json";
const String controlURL = baseURL + "rep_counter/control/action.json";
const String targetsURL = baseURL + "rep_counter/targets.json";
const String restStatusURL = baseURL + "rep_counter/rest_status.json";

// Pulse Sensor
#define PULSE_PIN 34
int threshold = 600;
int pulseValue = 0;
bool pulseDetected = false;
unsigned long lastBeatTime = 0;
unsigned long beatStartTime = 0;
int beatCount = 0;
int bpm = 0;

// DHT Sensor
#define DHTPIN 15
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// MPU6050
MPU6050 mpu;

// Ultrasonic
#define TRIG_PIN 27
#define ECHO_PIN 26

// Rep Logic
bool inCurl = false;
unsigned long lastRepTime = 0;
int repCount = 0;
int setCount = 0;
int targetReps = 10;
int targetSets = 3;
int lastSyncedReps = -1;
int lastSyncedSets = -1;

// Touch
#define TOUCH_PIN 4
bool isSessionActive = false;
bool lastTouchState = LOW;
unsigned long touchStartTime = 0;
bool longPressHandled = false;

// Rest logic
bool isResting = false;
unsigned long restStartTime = 0;
const unsigned long restDuration = 10000;

// Timing
unsigned long lastPrintTime = 0;
String lastCommand = "";
unsigned long lastTargetSync = 0;

void setup() {
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\nWiFi Connected.");

  dht.begin();
  Wire.begin(21, 22);
  mpu.initialize();
  if (!mpu.testConnection()) {
    Serial.println("MPU6050 failed.");
    while (1);
  }

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(TOUCH_PIN, INPUT);
}

void sendDataToFirebase(int reps, int sets, int bpm, float temp, float hum) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(dataURL);
    http.addHeader("Content-Type", "application/json");

    String payload = "{";
    payload += "\"reps\":" + String(reps) + ",";
    payload += "\"set_count\":" + String(sets) + ",";
    payload += "\"heart_rate\":" + String(bpm) + ",";
    payload += "\"temperature\":" + String(temp, 1) + ",";
    payload += "\"humidity\":" + String(hum);
    payload += "}";

    http.PUT(payload);
    http.end();
  }
}

void updateControlState(const String& state) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(controlURL);
    http.addHeader("Content-Type", "application/json");
    http.PUT("\"" + state + "\"");
    http.end();
  }
}

void setRestStatus(bool status) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(restStatusURL);
    http.addHeader("Content-Type", "application/json");
    http.PUT(status ? "true" : "false");
    http.end();
  }
}

String getControlCommand() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(controlURL);
    int code = http.GET();
    if (code == 200) {
      String result = http.getString();
      result.trim();
      result.replace("\"", "");
      http.end();
      return result;
    }
    http.end();
  }
  return "";
}

void syncTargets() {
  if (millis() - lastTargetSync < 5000) return;
  lastTargetSync = millis();

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(targetsURL);
    int code = http.GET();
    if (code == 200) {
      String res = http.getString();
      DynamicJsonDocument doc(512);
      if (deserializeJson(doc, res) == DeserializationError::Ok) {
        int newReps = doc["target_reps"] | 10;
        int newSets = doc["target_sets"] | 3;
        if (newReps != targetReps || newSets != targetSets) {
          targetReps = newReps;
          targetSets = newSets;
          Serial.print("Synced targets: Reps = ");
          Serial.print(targetReps);
          Serial.print(" Sets = ");
          Serial.println(targetSets);
        }
      }
    }
    http.end();
  }
}

void resetSession() {
  isSessionActive = false;
  isResting = false;
  setRestStatus(false);
  repCount = 0;
  setCount = 0;
  beatCount = 0;
  bpm = 0;
  inCurl = false;
  lastRepTime = 0;
  lastSyncedReps = -1;
  lastSyncedSets = -1;

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(dataURL);
    http.addHeader("Content-Type", "application/json");

    String payload = "{";
    payload += "\"reps\":0,";
    payload += "\"set_count\":0";
    payload += "}";

    http.PATCH(payload);
    http.end();
  }

  Serial.println("Session Reset");  
}

void loop() {
  syncTargets();

  String command = getControlCommand();
  if (command != "" && command != lastCommand) {
    lastCommand = command;
    if (command == "start") {
      if (setCount >= targetSets) {
        resetSession();
      }
      isSessionActive = true;
    } else if (command == "pause") {
      isSessionActive = false;
    } else if (command == "reset") {
      resetSession();
    }
  }

  bool touch = digitalRead(TOUCH_PIN);
  if (touch && !lastTouchState) {
    touchStartTime = millis();
    longPressHandled = false;
  }
  if (touch && !longPressHandled && millis() - touchStartTime > 2000) {
    resetSession();
    updateControlState("reset");
    longPressHandled = true;
  }
  if (!touch && lastTouchState && !longPressHandled) {
    if (setCount >= targetSets) {
      resetSession();
    }
    isSessionActive = !isSessionActive;
    updateControlState(isSessionActive ? "start" : "pause");
  }
  lastTouchState = touch;

  if (isResting && millis() - restStartTime >= restDuration) {
    Serial.println("Rest over. Next set starting.");
    repCount = 0;
    isResting = false;
    setRestStatus(false);
  }

  if (isSessionActive && !isResting) {
    pulseValue = analogRead(PULSE_PIN);
    if (pulseValue > threshold && !pulseDetected) {
      pulseDetected = true;
      if (millis() - lastBeatTime > 400) {
        lastBeatTime = millis();
        if (beatCount == 0) beatStartTime = millis();
        beatCount++;
        if (beatCount == 10) {
          bpm = (10 * 60000UL) / (millis() - beatStartTime);
          beatCount = 0;
        }
      }
    }
    if (pulseValue < threshold) pulseDetected = false;

    int16_t ax, ay, az, gx, gy, gz;
    mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);

    if (ax > 11000 && !inCurl) inCurl = true;
    if (ax < 9000 && inCurl && millis() - lastRepTime > 1000) {
      inCurl = false;
      lastRepTime = millis();
      repCount++;
      Serial.print("Rep: "); Serial.println(repCount);

      if (repCount >= targetReps) {
        setCount++;
        Serial.print("Set: "); Serial.println(setCount);
        if (setCount >= targetSets) {
          Serial.println("All sets complete. Pausing.");
          isSessionActive = false;
          updateControlState("pause");
        } else {
          Serial.println("Starting rest...");
          isResting = true;
          restStartTime = millis();
          setRestStatus(true);
        }
      }
    }

    if ((millis() - lastPrintTime >= 2000) || repCount != lastSyncedReps || setCount != lastSyncedSets) {
      lastPrintTime = millis();
      float hum = dht.readHumidity();
      float temp = dht.readTemperature();
      if (isnan(hum)) hum = 0;
      if (isnan(temp)) temp = 0;
      sendDataToFirebase(repCount, setCount, bpm, temp, hum);
      lastSyncedReps = repCount;
      lastSyncedSets = setCount;
    }
  }

  delay(5);
}
