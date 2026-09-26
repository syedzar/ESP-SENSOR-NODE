// ESP32 wireless sensor node
//
// Two FreeRTOS tasks communicating over a queue:
//   sensorTask  - samples a BME280 over I2C on a fixed interval
//   networkTask - waits for readings on the queue, connects to Wi-Fi if
//                 needed, and POSTs each reading as JSON to an HTTP API
//
// See README.md for wiring, build, and simulation instructions.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Wire.h>

#include "config.h"

// Wokwi has no simulated BME280 part, so WOKWI_SIMULATION (set by the
// esp32dev-sim PlatformIO env) swaps in a BMP180, which has no humidity
// sensor. Real hardware always builds against the real BME280.
#ifdef WOKWI_SIMULATION
#include <Adafruit_BMP085.h>
#else
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#endif

struct SensorReading {
  float temperatureC;
  float
      humidityPct;  // NAN on the simulated BMP180, which has no humidity sensor
  float pressureHPa;
  uint32_t timestampMs;
};

static QueueHandle_t sensorQueue;
#ifdef WOKWI_SIMULATION
static Adafruit_BMP085 bme;
#else
static Adafruit_BME280 bme;
#endif

static void ensureWifiConnected();

// ---------------------------------------------------------------------
// Task 1: read the sensor on a fixed interval and push readings onto the
// queue. Never blocks on the network - if the queue is full (because the
// network task has fallen behind) the oldest-in-flight reading is simply
// dropped rather than stalling sensor sampling.
// ---------------------------------------------------------------------
static void sensorTask(void* pvParameters) {
  const TickType_t samplingInterval = pdMS_TO_TICKS(SENSOR_SAMPLE_INTERVAL_MS);

  for (;;) {
    SensorReading reading;
    reading.temperatureC = bme.readTemperature();
#ifdef WOKWI_SIMULATION
    reading.humidityPct = NAN;
#else
    reading.humidityPct = bme.readHumidity();
#endif
    reading.pressureHPa = bme.readPressure() / 100.0F;
    reading.timestampMs = millis();

#ifdef WOKWI_SIMULATION
    if (isnan(reading.temperatureC)) {
#else
    if (isnan(reading.temperatureC) || isnan(reading.humidityPct)) {
#endif
      Serial.println(
          "[sensorTask] Sensor read failed (NaN), skipping this cycle.");
    } else {
#ifdef WOKWI_SIMULATION
      Serial.printf(
          "[sensorTask] T=%.2fC P=%.2fhPa (no humidity sensor in simulation)\r\n",
          reading.temperatureC, reading.pressureHPa);
#else
      Serial.printf("[sensorTask] T=%.2fC H=%.2f%% P=%.2fhPa\r\n",
                    reading.temperatureC, reading.humidityPct,
                    reading.pressureHPa);
#endif

      if (xQueueSend(sensorQueue, &reading, 0) != pdTRUE) {
        Serial.println("[sensorTask] Queue full, dropping reading.");
      }
    }

    vTaskDelay(samplingInterval);
  }
}

// ---------------------------------------------------------------------
// Task 2: block on the queue, then ship each reading to the API over
// HTTP. Owns all Wi-Fi/HTTP state so sensorTask never has to know
// whether the network is up.
// ---------------------------------------------------------------------
static void networkTask(void* pvParameters) {
  SensorReading reading;

  for (;;) {
    if (xQueueReceive(sensorQueue, &reading, portMAX_DELAY) == pdTRUE) {
      ensureWifiConnected();

      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(API_ENDPOINT_URL);
        http.addHeader("Content-Type", "application/json");

        StaticJsonDocument<256> doc;
        doc["device_id"] = DEVICE_ID;
        doc["timestamp_ms"] = reading.timestampMs;
        doc["temperature_c"] = reading.temperatureC;
#ifndef WOKWI_SIMULATION
        doc["humidity_pct"] = reading.humidityPct;
#endif
        doc["pressure_hpa"] = reading.pressureHPa;

        String payload;
        serializeJson(doc, payload);

        int httpResponseCode = http.POST(payload);
        if (httpResponseCode > 0) {
          Serial.printf("[networkTask] POST -> HTTP %d\r\n", httpResponseCode);
        } else {
          Serial.printf("[networkTask] POST failed, error: %s\r\n",
                        http.errorToString(httpResponseCode).c_str());
        }

        http.end();
      } else {
        Serial.println("[networkTask] Wi-Fi unavailable, dropping reading.");
      }
    }
  }
}

static void ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.printf("[networkTask] Connecting to Wi-Fi SSID '%s'...\r\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t startAttemptMs = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttemptMs < WIFI_CONNECT_TIMEOUT_MS) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[networkTask] Connected, IP = %s\r\n",
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[networkTask] Wi-Fi connection attempt timed out.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

#ifdef WOKWI_SIMULATION
  if (!bme.begin()) {
    Serial.println(
        "[setup] Could not find BMP180 sensor (simulation). Check wiring.");
  }
#else
  if (!bme.begin(BME280_I2C_ADDRESS, &Wire)) {
    Serial.println(
        "[setup] Could not find BME280 sensor. Check wiring and I2C address.");
  }
#endif

  sensorQueue = xQueueCreate(SENSOR_QUEUE_LENGTH, sizeof(SensorReading));
  if (sensorQueue == NULL) {
    Serial.println("[setup] Failed to create sensor queue.");
  }

  // Pin sensorTask and networkTask to different cores so a slow/blocked
  // HTTP call can never delay sensor sampling.
  xTaskCreatePinnedToCore(sensorTask, "SensorTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(networkTask, "NetworkTask", 8192, NULL, 1, NULL, 0);
}

void loop() {
  // All work happens in the FreeRTOS tasks above; nothing to do here.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
