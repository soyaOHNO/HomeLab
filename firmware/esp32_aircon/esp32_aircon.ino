#include <Arduino.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <ir_Panasonic.h>

#include "secrets.h"

// 使用ピン
constexpr uint8_t kDhtPin = 25;
constexpr uint8_t kIrLedPin = 26;

// 読み取り・再接続間隔
constexpr unsigned long kSensorIntervalMs = 3000;
constexpr unsigned long kWifiRetryIntervalMs = 10000;
constexpr unsigned long kMqttRetryIntervalMs = 5000;

// MQTTトピック
constexpr char kDeviceId[] = "aircon-01";
constexpr char kTopicSet[] =
    "home/aircon/aircon-01/set";
constexpr char kTopicState[] =
    "home/aircon/aircon-01/state";
constexpr char kTopicTelemetry[] =
    "home/aircon/aircon-01/telemetry";
constexpr char kTopicAvailability[] =
    "home/aircon/aircon-01/availability";

// デバイス
DHT dht(kDhtPin, DHT11);
IRPanasonicAc ac(kIrLedPin);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// タイマー
unsigned long lastSensorRead = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastMqttAttempt = 0;

// DHT11起動直後の不安定値を捨てる
uint8_t sensorWarmupReads = 0;

// 関数宣言
void maintainConnections();
void connectMqtt();
void mqttCallback(char* topic, byte* payload, unsigned int length);

void readSensor();
void handleSerialCommand();
void sendAcState();

void publishAcState();
void publishTelemetry(float temperature, float humidity);

bool setModeFromString(const char* value);
bool setFanFromString(const char* value);
bool setSwingVerticalFromString(const char* value);

const char* modeToString(uint8_t value);
const char* fanToString(uint8_t value);
const char* swingVerticalToString(uint8_t value);

void setup() {
  Serial.begin(115200);

  dht.begin();
  ac.begin();

  // Panasonic JKEの初期状態
  ac.setModel(kPanasonicJke);
  ac.setPower(false);
  ac.setMode(kPanasonicAcCool);
  ac.setTemp(24);
  ac.setFan(kPanasonicAcFanAuto);
  ac.setSwingVertical(kPanasonicAcSwingVAuto);
  ac.setQuiet(false);
  ac.setPowerful(false);

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // MQTT
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);

  Serial.println();
  Serial.println("DHT11 + Panasonic AC + MQTT ready");
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);
  Serial.println("Serial commands:");
  Serial.println("  c: Cool 24C ON");
  Serial.println("  o: Power OFF");
}

void loop() {
  maintainConnections();
  readSensor();
  handleSerialCommand();
}

void maintainConnections() {
  const unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiAttempt >= kWifiRetryIntervalMs) {
      lastWifiAttempt = now;

      Serial.println("Retrying Wi-Fi...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }

    return;
  }

  if (!mqttClient.connected()) {
    if (now - lastMqttAttempt >= kMqttRetryIntervalMs) {
      lastMqttAttempt = now;
      connectMqtt();
    }

    return;
  }

  mqttClient.loop();
}

void connectMqtt() {
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Connecting to MQTT ");
  Serial.print(MQTT_HOST);
  Serial.print(":");
  Serial.println(MQTT_PORT);

  const bool connected = mqttClient.connect(
      kDeviceId,
      MQTT_USERNAME,
      MQTT_PASSWORD,
      kTopicAvailability,
      1,
      true,
      "offline"
  );

  if (!connected) {
    Serial.print("MQTT failed, state=");
    Serial.println(mqttClient.state());
    return;
  }

  Serial.println("MQTT connected");

  mqttClient.publish(kTopicAvailability, "online", true);
  mqttClient.subscribe(kTopicSet, 1);

  publishAcState();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, kTopicSet) != 0) {
    return;
  }

  JsonDocument command;
  const DeserializationError error =
      deserializeJson(command, payload, length);

  if (error) {
    Serial.print("Invalid MQTT JSON: ");
    Serial.println(error.c_str());
    return;
  }

  bool shouldSend = false;

  if (command["power"].is<bool>()) {
    ac.setPower(command["power"].as<bool>());
    shouldSend = true;
  }

  if (command["mode"].is<const char*>()) {
    if (setModeFromString(command["mode"])) {
      shouldSend = true;
    } else {
      Serial.println("Unknown mode");
    }
  }

  if (command["temperature"].is<int>()) {
    const int temperature = command["temperature"];

    if (temperature >= 16 && temperature <= 30) {
      ac.setTemp(static_cast<uint8_t>(temperature));
      shouldSend = true;
    } else {
      Serial.println("Temperature must be 16-30");
    }
  }

  if (command["fan"].is<const char*>()) {
    if (setFanFromString(command["fan"])) {
      shouldSend = true;
    } else {
      Serial.println("Unknown fan setting");
    }
  }

  if (command["swing_vertical"].is<const char*>()) {
    if (setSwingVerticalFromString(command["swing_vertical"])) {
      shouldSend = true;
    } else {
      Serial.println("Unknown vertical swing setting");
    }
  }

  if (command["quiet"].is<bool>()) {
    ac.setQuiet(command["quiet"].as<bool>());
    shouldSend = true;
  }

  if (command["powerful"].is<bool>()) {
    ac.setPowerful(command["powerful"].as<bool>());
    shouldSend = true;
  }

  if (!shouldSend) {
    Serial.println("MQTT command contained no supported settings");
    return;
  }

  sendAcState();
}

void sendAcState() {
  ac.send();

  Serial.print("AC sent: ");
  Serial.println(ac.toString().c_str());

  publishAcState();
}

void publishAcState() {
  if (!mqttClient.connected()) {
    return;
  }

  JsonDocument state;

  state["power"] = ac.getPower();
  state["mode"] = modeToString(ac.getMode());
  state["temperature"] = ac.getTemp();
  state["fan"] = fanToString(ac.getFan());
  state["swing_vertical"] =
      swingVerticalToString(ac.getSwingVertical());
  state["quiet"] = ac.getQuiet();
  state["powerful"] = ac.getPowerful();

  char payload[384];
  serializeJson(state, payload, sizeof(payload));

  mqttClient.publish(kTopicState, payload, true);
}

void readSensor() {
  const unsigned long now = millis();

  if (now - lastSensorRead < kSensorIntervalMs) {
    return;
  }

  lastSensorRead = now;

  const float humidity = dht.readHumidity();
  const float temperature = dht.readTemperature();

  if (isnan(humidity) ||
      isnan(temperature) ||
      humidity <= 0.0F ||
      humidity > 100.0F ||
      temperature < -20.0F ||
      temperature > 60.0F) {
    Serial.println("DHT11 read failed or invalid");
    return;
  }

  // 最初の2回は起動直後の不安定値として捨てる
  if (sensorWarmupReads < 2) {
    sensorWarmupReads++;
    Serial.println("DHT11 warming up...");
    return;
  }

  Serial.print("Temperature: ");
  Serial.print(temperature, 1);
  Serial.print(" C, Humidity: ");
  Serial.print(humidity, 1);
  Serial.println(" %");

  publishTelemetry(temperature, humidity);
}

void publishTelemetry(float temperature, float humidity) {
  if (!mqttClient.connected()) {
    return;
  }

  JsonDocument telemetry;

  telemetry["temperature"] = serialized(
      String(temperature, 1)
  );
  telemetry["humidity"] = serialized(
      String(humidity, 1)
  );
  telemetry["rssi"] = WiFi.RSSI();

  char payload[192];
  serializeJson(telemetry, payload, sizeof(payload));

  mqttClient.publish(kTopicTelemetry, payload, true);
}

void handleSerialCommand() {
  if (Serial.available() <= 0) {
    return;
  }

  const char command = Serial.read();

  if (command == 'c' || command == 'C') {
    ac.on();
    ac.setMode(kPanasonicAcCool);
    ac.setTemp(24);
    sendAcState();
  }

  if (command == 'o' || command == 'O') {
    ac.off();
    sendAcState();
  }
}

bool setModeFromString(const char* value) {
  if (strcmp(value, "auto") == 0) {
    ac.setMode(kPanasonicAcAuto);
  } else if (strcmp(value, "cool") == 0) {
    ac.setMode(kPanasonicAcCool);
  } else if (strcmp(value, "dry") == 0) {
    ac.setMode(kPanasonicAcDry);
  } else if (strcmp(value, "heat") == 0) {
    ac.setMode(kPanasonicAcHeat);
  } else if (strcmp(value, "fan") == 0) {
    ac.setMode(kPanasonicAcFan);
  } else {
    return false;
  }

  return true;
}

bool setFanFromString(const char* value) {
  if (strcmp(value, "auto") == 0) {
    ac.setFan(kPanasonicAcFanAuto);
  } else if (strcmp(value, "min") == 0) {
    ac.setFan(kPanasonicAcFanMin);
  } else if (strcmp(value, "low") == 0) {
    ac.setFan(kPanasonicAcFanLow);
  } else if (strcmp(value, "medium") == 0) {
    ac.setFan(kPanasonicAcFanMed);
  } else if (strcmp(value, "high") == 0) {
    ac.setFan(kPanasonicAcFanHigh);
  } else if (strcmp(value, "max") == 0) {
    ac.setFan(kPanasonicAcFanMax);
  } else {
    return false;
  }

  return true;
}

bool setSwingVerticalFromString(const char* value) {
  if (strcmp(value, "auto") == 0) {
    ac.setSwingVertical(kPanasonicAcSwingVAuto);
  } else if (strcmp(value, "highest") == 0) {
    ac.setSwingVertical(kPanasonicAcSwingVHighest);
  } else if (strcmp(value, "high") == 0) {
    ac.setSwingVertical(kPanasonicAcSwingVHigh);
  } else if (strcmp(value, "middle") == 0) {
    ac.setSwingVertical(kPanasonicAcSwingVMiddle);
  } else if (strcmp(value, "low") == 0) {
    ac.setSwingVertical(kPanasonicAcSwingVLow);
  } else if (strcmp(value, "lowest") == 0) {
    ac.setSwingVertical(kPanasonicAcSwingVLowest);
  } else {
    return false;
  }

  return true;
}

const char* modeToString(uint8_t value) {
  switch (value) {
    case kPanasonicAcAuto:
      return "auto";
    case kPanasonicAcCool:
      return "cool";
    case kPanasonicAcDry:
      return "dry";
    case kPanasonicAcHeat:
      return "heat";
    case kPanasonicAcFan:
      return "fan";
    default:
      return "unknown";
  }
}

const char* fanToString(uint8_t value) {
  switch (value) {
    case kPanasonicAcFanAuto:
      return "auto";
    case kPanasonicAcFanMin:
      return "min";
    case kPanasonicAcFanLow:
      return "low";
    case kPanasonicAcFanMed:
      return "medium";
    case kPanasonicAcFanHigh:
      return "high";
    case kPanasonicAcFanMax:
      return "max";
    default:
      return "unknown";
  }
}

const char* swingVerticalToString(uint8_t value) {
  switch (value) {
    case kPanasonicAcSwingVAuto:
      return "auto";
    case kPanasonicAcSwingVHighest:
      return "highest";
    case kPanasonicAcSwingVHigh:
      return "high";
    case kPanasonicAcSwingVMiddle:
      return "middle";
    case kPanasonicAcSwingVLow:
      return "low";
    case kPanasonicAcSwingVLowest:
      return "lowest";
    default:
      return "unknown";
  }
}
