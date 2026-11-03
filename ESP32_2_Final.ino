/*
  ESP32 #2 - DS18B20 temperature + turbidity + LCD
  Keeps the existing Firebase paths and JSON keys so the current backend/app
  does not need to be changed.

  Pins retained from the supplied project:
    DS18B20 data -> GPIO 4
    Turbidity AO -> GPIO 32
    LCD SDA/SCL  -> GPIO 21 / GPIO 22, address 0x27

  IMPORTANT:
  - The turbidity voltage-to-NTU equation is the same approximate equation
    from the supplied project. It is sensor-specific and must be calibrated.
  - A dry probe can produce a changing voltage; software cannot universally
    identify “out of water” from analog voltage alone. See notes provided with
    this code about the 10-ohm/20-ohm resistor arrangement.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>

const char* ssid = "shahdhi";
const char* password = "12345678";

const char* sensorFirebaseURL =
  "https://ai-water-quality-monitor-f6dc4-default-rtdb.asia-southeast1.firebasedatabase.app/sensors/esp32_2.json";
const char* latestFirebaseURL =
  "https://ai-water-quality-monitor-f6dc4-default-rtdb.asia-southeast1.firebasedatabase.app/latest.json";

LiquidCrystal_I2C lcd(0x27, 16, 2);
#define ONE_WIRE_BUS 4
#define TURBIDITY_PIN 32
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);

constexpr int SAMPLE_COUNT = 9;
const float FILTER_ALPHA = 0.25f;
const unsigned long UPLOAD_INTERVAL_MS = 5000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
unsigned long lastUpload = 0;
unsigned long lastWifiRetry = 0;
unsigned long lastLcdPageChange = 0;
int lcdPage = 0;
float smoothedTurbidity = NAN;
float lastTemperature = NAN;
float lastTurbidity = NAN;
String lastDecision = "Waiting";

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiRetry < WIFI_RETRY_INTERVAL_MS) return;
  lastWifiRetry = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("Attempting Wi-Fi reconnect...");
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

float readMedianVoltage(int pin, int* medianRaw = nullptr) {
  float mv[SAMPLE_COUNT];
  int raw[SAMPLE_COUNT];
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    raw[i] = analogRead(pin);
    mv[i] = (float)analogReadMilliVolts(pin);
    delay(8);
  }
  sortFloats(mv, SAMPLE_COUNT);
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

bool getAIDecision(String &decision) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setTimeout(3000);
  if (!http.begin(latestFirebaseURL)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  String response = http.getString();
  http.end();
  String key = "\"prediction\":\"";
  int start = response.indexOf(key);
  if (start < 0) return false;
  start += key.length();
  int end = response.indexOf('"', start);
  if (end <= start) return false;
  decision = response.substring(start, end);
  return true;
}

void uploadReadings(float temperature, float turbidityNTU) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi unavailable; not uploading this cycle.");
    return;
  }
  HTTPClient http;
  http.setTimeout(4000);
  if (!http.begin(sensorFirebaseURL)) {
    Serial.println("Could not start Firebase request.");
    return;
  }
  http.addHeader("Content-Type", "application/json");

  // Keep the same Firebase fields used by the existing backend/application.
  String json = "{\"Temperature\":" + String(temperature, 2) +
                ",\"Turbidity\":" + String(turbidityNTU, 2) +
                ",\"source_type\":\"ESP32_2\"," 
                "\"source_description\":\"Turbidity and Temperature Sensors\"}";

  int code = http.PUT(json);
  Serial.printf("Firebase PUT HTTP code: %d\n", code);
  if (code == HTTP_CODE_OK) Serial.println("ESP32 #2 upload successful.");
  else Serial.println(http.getString());
  http.end();
}

void showLCD() {
  lcd.clear();
  if (lcdPage == 0) {
    lcd.setCursor(0, 0);
    if (isfinite(lastTemperature)) {
      lcd.print("Temp: "); lcd.print(lastTemperature, 1); lcd.print((char)223); lcd.print("C");
    } else lcd.print("Temp: SENSOR ERR");
    lcd.setCursor(0, 1);
    if (isfinite(lastTurbidity)) {
      lcd.print("Turb: "); lcd.print(lastTurbidity, 0); lcd.print(" NTU");
    } else lcd.print("Turb: SENSOR ERR");
  } else {
    lcd.setCursor(0, 0); lcd.print("Water Quality:");
    lcd.setCursor(0, 1);
    if (lastDecision.length() <= 16) lcd.print(lastDecision);
    else lcd.print(lastDecision.substring(0, 16));
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  analogReadResolution(12);
  analogSetPinAttenuation(TURBIDITY_PIN, ADC_11db);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("AI Water Quality");
  lcd.setCursor(0, 1); lcd.print("Starting...");

  tempSensor.begin();
  tempSensor.setResolution(12);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  Serial.println("\nESP32 #2 - temperature + turbidity + LCD");
  Serial.println("Pins: DS18B20 GPIO4, turbidity GPIO32, LCD SDA21/SCL22");
}

void loop() {
  connectWiFi();

  tempSensor.requestTemperatures();
  float temperature = tempSensor.getTempCByIndex(0);
  bool temperatureValid = temperature != DEVICE_DISCONNECTED_C &&
                          isfinite(temperature) && temperature >= -20.0f && temperature <= 80.0f;
  if (!temperatureValid) temperature = NAN;

  int turbidityRaw = 0;
  float voltage = readMedianVoltage(TURBIDITY_PIN, &turbidityRaw);
  bool adcPlausible = turbidityRaw > 8 && turbidityRaw < 4087 &&
                      isfinite(voltage) && voltage >= 0.0f && voltage <= 3.30f;

  float turbidityNTU = NAN;
  if (adcPlausible) {
    // Same approximate conversion used by the supplied sketch.
    float rawNTU = -1120.4f * voltage * voltage + 5742.3f * voltage - 4352.9f;
    if (isfinite(rawNTU) && rawNTU >= -100.0f && rawNTU <= 4000.0f) {
      if (rawNTU < 0.0f) rawNTU = 0.0f;
      turbidityNTU = smoothValue(rawNTU, smoothedTurbidity);
    }
  }

  if (temperatureValid) lastTemperature = temperature;
  if (isfinite(turbidityNTU)) lastTurbidity = turbidityNTU;

  Serial.println("----------------------------------------");
  if (temperatureValid) Serial.printf("Temperature: %.2f C\n", temperature);
  else Serial.println("Temperature: INVALID (check DS18B20 and its pull-up resistor)");
  Serial.printf("Turbidity raw ADC: %d, voltage: %.3f V, NTU: ", turbidityRaw, voltage);
  if (isfinite(turbidityNTU)) Serial.printf("%.2f\n", turbidityNTU);
  else Serial.println("INVALID (check analog output/wiring)");

  if (millis() - lastUpload >= UPLOAD_INTERVAL_MS) {
    lastUpload = millis();
    if (temperatureValid && isfinite(turbidityNTU)) uploadReadings(temperature, turbidityNTU);
    else Serial.println("Upload skipped: temperature or turbidity reading invalid.");
  }

  String decision;
  if (getAIDecision(decision)) lastDecision = decision;
  else if (WiFi.status() != WL_CONNECTED) lastDecision = "WiFi offline";
  // Otherwise keep the most recent decision rather than flickering the LCD.

  if (millis() - lastLcdPageChange >= 3000) {
    lastLcdPageChange = millis();
    lcdPage = 1 - lcdPage;
    showLCD();
  }
  delay(150);
}
