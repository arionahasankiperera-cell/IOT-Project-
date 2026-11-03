/*
  ESP32 #1 - pH + TDS
  Keeps the existing Firebase paths and JSON keys so the current backend/app
  does not need to be changed.

  Pins retained from the supplied project:
    pH  -> GPIO 34
    TDS -> GPIO 35

  IMPORTANT:
  - The pH conversion below preserves the project's existing approximate
    formula. Calibrate it with known pH buffer solutions before relying on it.
  - The TDS formula assumes a compatible analog TDS module. Calibrate against
    a reference solution/meter.
  - This code cannot determine whether a probe is out of water unless that
    sensor/module provides a defined electrical indication of that condition.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <math.h>

const char* ssid = "shahdhi";
const char* password = "12345678";

const char* firebaseURL =
  "https://ai-water-quality-monitor-f6dc4-default-rtdb.asia-southeast1.firebasedatabase.app/sensors/esp32_1.json";
const char* temperatureURL =
  "https://ai-water-quality-monitor-f6dc4-default-rtdb.asia-southeast1.firebasedatabase.app/sensors/esp32_2/Temperature.json";

#define PH_PIN 34
#define TDS_PIN 35

constexpr int SAMPLE_COUNT = 9; // median filter sample count
const unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
const unsigned long UPLOAD_INTERVAL_MS = 5000;
unsigned long lastWifiRetry = 0;
unsigned long lastUpload = 0;

float smoothedPH = NAN;
float smoothedTDS = NAN;
const float FILTER_ALPHA = 0.25f; // lower = smoother, but slower to respond

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiRetry < WIFI_RETRY_INTERVAL_MS) return;
  lastWifiRetry = millis();
  Serial.print("Connecting to Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
}

void sortFloats(float* values, int n) {
  for (int i = 1; i < n; i++) {
    float key = values[i];
    int j = i - 1;
    while (j >= 0 && values[j] > key) {
      values[j + 1] = values[j];
      j--;
    }
    values[j + 1] = key;
  }
}

// Median of calibrated ADC millivolt samples reduces isolated spikes.
float readMedianVoltage(int pin, int* medianRaw = nullptr) {
  float mv[SAMPLE_COUNT];
  int raw[SAMPLE_COUNT];
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    raw[i] = analogRead(pin);
    mv[i] = (float)analogReadMilliVolts(pin);
    delay(8);
  }
  sortFloats(mv, SAMPLE_COUNT);
  // Sort raw values separately for the reported median raw ADC reading.
  for (int i = 1; i < SAMPLE_COUNT; i++) {
    int key = raw[i];
    int j = i - 1;
    while (j >= 0 && raw[j] > key) {
      raw[j + 1] = raw[j];
      j--;
    }
    raw[j + 1] = key;
  }
  if (medianRaw) *medianRaw = raw[SAMPLE_COUNT / 2];
  return mv[SAMPLE_COUNT / 2] / 1000.0f;
}

float smoothValue(float current, float &previous) {
  if (!isfinite(previous)) previous = current;
  else previous = FILTER_ALPHA * current + (1.0f - FILTER_ALPHA) * previous;
  return previous;
}

bool isAnalogReadingPlausible(int raw, float voltage) {
  // Only catches ADC rails/open-circuit extremes. It cannot prove that a probe
  // is immersed in water; the sensor's output can be valid electrically but
  // unsuitable for a water measurement while dry.
  return raw > 8 && raw < 4087 && isfinite(voltage) && voltage >= 0.0f && voltage <= 3.30f;
}

bool getTemperatureFromFirebase(float &temperature) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setTimeout(2500);
  if (!http.begin(temperatureURL)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Temperature Firebase GET failed: %d\n", code);
    http.end();
    return false;
  }

  String response = http.getString();
  http.end();
  response.trim();
  if (response.length() == 0 || response == "null") return false;

  char* endPtr = nullptr;
  float parsed = strtof(response.c_str(), &endPtr);
  if (endPtr == response.c_str() || !isfinite(parsed) || parsed < -20.0f || parsed > 80.0f) {
    return false;
  }
  temperature = parsed;
  return true;
}

void uploadReadings(float pH, float tds) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi unavailable; readings not uploaded this cycle.");
    return;
  }

  HTTPClient http;
  http.setTimeout(4000);
  if (!http.begin(firebaseURL)) {
    Serial.println("Could not start Firebase request.");
    return;
  }
  http.addHeader("Content-Type", "application/json");

  // Keep the same Firebase fields used by the existing backend/application.
  String json = "{\"pH\":" + String(pH, 2) +
                ",\"TDS\":" + String(tds, 2) +
                ",\"source_type\":\"ESP32_1\"," 
                "\"source_description\":\"pH and TDS Sensors\"}";

  int code = http.PUT(json);
  Serial.printf("Firebase PUT HTTP code: %d\n", code);
  if (code == HTTP_CODE_OK) Serial.println("ESP32 #1 upload successful.");
  else Serial.println(http.getString());
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  analogReadResolution(12);
  analogSetPinAttenuation(PH_PIN, ADC_11db);
  analogSetPinAttenuation(TDS_PIN, ADC_11db);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  Serial.println("\nESP32 #1 - pH + TDS (filtered readings)");
  Serial.println("Pins: pH GPIO34, TDS GPIO35");
}

void loop() {
  connectWiFi();

  int phRaw = 0;
  int tdsRaw = 0;
  float phVoltage = readMedianVoltage(PH_PIN, &phRaw);
  float tdsVoltage = readMedianVoltage(TDS_PIN, &tdsRaw);

  bool phPlausible = isAnalogReadingPlausible(phRaw, phVoltage);
  bool tdsPlausible = isAnalogReadingPlausible(tdsRaw, tdsVoltage);

  // Preserve the original project's pH conversion. Calibration is required.
  float pH = NAN;
  if (phPlausible) {
    float rawPH = 7.0f + ((2.50f - phVoltage) / 0.18f);
    if (rawPH >= 0.0f && rawPH <= 14.0f) pH = smoothValue(rawPH, smoothedPH);
  }

  float temperature = 25.0f;
  bool tempAvailable = getTemperatureFromFirebase(temperature);
  if (!tempAvailable) {
    // Use 25 C only as a fallback for the TDS temperature compensation.
    // This avoids blocking pH/TDS uploads when ESP32 #2 is briefly offline.
    temperature = 25.0f;
    Serial.println("Temperature unavailable; TDS compensation assumes 25 C.");
  }

  float tds = NAN;
  if (tdsPlausible) {
    float coefficient = 1.0f + 0.02f * (temperature - 25.0f);
    float compensatedVoltage = tdsVoltage / coefficient;
    float rawTDS = (133.42f * powf(compensatedVoltage, 3) -
                    255.86f * powf(compensatedVoltage, 2) +
                    857.39f * compensatedVoltage) * 0.5f;
    if (isfinite(rawTDS) && rawTDS >= 0.0f && rawTDS <= 5000.0f) {
      tds = smoothValue(rawTDS, smoothedTDS);
    }
  }

  Serial.println("----------------------------------------");
  Serial.printf("pH raw ADC=%d, voltage=%.3f V, pH=", phRaw, phVoltage);
  if (isfinite(pH)) Serial.println(pH, 2); else Serial.println("INVALID (check probe/wiring)");
  Serial.printf("TDS raw ADC=%d, voltage=%.3f V, T=%.2f C, TDS=", tdsRaw, tdsVoltage, temperature);
  if (isfinite(tds)) Serial.printf("%.2f ppm\n", tds); else Serial.println("INVALID (check probe/wiring)");

  // Do not write made-up zero values to Firebase. Keep the existing schema and
  // upload only when both measurements pass basic electrical/range checks.
  if (millis() - lastUpload >= UPLOAD_INTERVAL_MS) {
    lastUpload = millis();
    if (isfinite(pH) && isfinite(tds)) uploadReadings(pH, tds);
    else Serial.println("Upload skipped: one or more analog readings are invalid.");
  }
  delay(150);
}
