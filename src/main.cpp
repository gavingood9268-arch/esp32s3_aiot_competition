#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <qrcode.h>
#include <math.h>
#include "secrets.h"

#define TFT_CS     44
#define TFT_DC     43
#define TFT_RST    45
#define TFT_SCLK   21
#define TFT_MOSI   47
#define BUTTON_PIN 6
#define BUZZER_PIN 3
#define LED_PIN    9
#define LIGHT_PIN  8

const unsigned long SENSOR_REFRESH_INTERVAL_MS = 3000;
const unsigned long CLOUD_SYNC_INTERVAL_MS = 3000;
const unsigned long LED_HEARTBEAT_INTERVAL_MS = 1000;
const int LIGHT_SAMPLE_COUNT = 8;
const uint8_t DISPLAY_ROTATION = 2;
const bool LED_ACTIVE_HIGH = true;
const bool BUZZER_ACTIVE_HIGH = true;

WebServer server(80);

// Global state
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
Adafruit_SHT31 sht30 = Adafruit_SHT31();
float temp = 0, humi = 0;
int light = 0;
int lightLevel = 0;
float tempThreshold = 30.0, humiThreshold = 80.0;
int lightThreshold = 3950;
bool isAlarming = false;
bool cloudManualAlarm = false;
int currentPage = 0;  // 0 overview, 1 QR code
String ipAddr = "";
bool wifiConnected = false;
unsigned long lastSensorRefresh = 0;
unsigned long lastCloudSync = 0;
unsigned long lastLedToggle = 0;
bool ledHeartbeatOn = false;
unsigned long buzzerPatternStart = 0;
unsigned long lastBuzzerPattern = 0;
int buzzerAlarmMask = 0;
bool alarmPulseOn = false;
bool buzzerOutputOn = false;
String cloudStatus = "OFF";

bool isTempAlarm();
bool isHumiAlarm();
bool isLightAlarm();
int currentAlarmMask();

void setLed(bool on) {
    digitalWrite(LED_PIN, (on == LED_ACTIVE_HIGH) ? HIGH : LOW);
}

void setBuzzerOutput(bool on) {
    if (on == buzzerOutputOn) {
        return;
    }
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, (on == BUZZER_ACTIVE_HIGH) ? HIGH : LOW);
    buzzerOutputOn = on;
}

int readLightRaw() {
    long sum = 0;
    for (int i = 0; i < LIGHT_SAMPLE_COUNT; i++) {
        sum += analogRead(LIGHT_PIN);
        delay(2);
    }
    return sum / LIGHT_SAMPLE_COUNT;
}

int calculateLightLevel(int raw) {
    if (raw < 1000) {
        return map(raw, 0, 1000, 0, 15);
    }
    if (raw < 3600) {
        return map(raw, 1000, 3600, 15, 70);
    }
    return constrain(map(raw, 3600, 4095, 70, 100), 70, 100);
}

void readSensors() {
    temp = sht30.readTemperature();
    humi = sht30.readHumidity();
    light = readLightRaw();
    lightLevel = calculateLightLevel(light);
}

void checkAlarm() {
    isAlarming = cloudManualAlarm || temp > tempThreshold || humi > humiThreshold || light > lightThreshold;
}

bool cloudEnabled() {
    return strlen(cloud_base_url) > 0;
}

String dashboardUrl() {
    if (cloudEnabled()) return String(cloud_base_url);
    if (wifiConnected) return "http://" + ipAddr;
    return "http://NO-WIFI";
}

float extractJsonFloat(const String& body, const String& key, float fallback) {
    int idx = body.indexOf("\"" + key + "\":");
    if (idx < 0) return fallback;
    idx += key.length() + 3;
    int end = idx;
    while (end < body.length()) {
        char c = body[end];
        if (!((c >= '0' && c <= '9') || c == '.' || c == '-')) break;
        end++;
    }
    return body.substring(idx, end).toFloat();
}

bool extractJsonBool(const String& body, const String& key, bool fallback) {
    int idx = body.indexOf("\"" + key + "\":");
    if (idx < 0) return fallback;
    idx += key.length() + 3;
    if (body.substring(idx, idx + 4) == "true") return true;
    if (body.substring(idx, idx + 5) == "false") return false;
    return fallback;
}

void syncCloud() {
    if (!wifiConnected || !cloudEnabled()) {
        cloudStatus = cloudEnabled() ? "NO WIFI" : "OFF";
        return;
    }

    HTTPClient http;
    String url = String(cloud_base_url) + "/api/update";
    String payload = "{";
    payload += "\"device_id\":\"esp32s3-env-001\",";
    payload += "\"temperature\":" + String(temp, 1) + ",";
    payload += "\"humidity\":" + String(humi, 1) + ",";
    payload += "\"light_level\":" + String(lightLevel) + ",";
    payload += "\"light_raw\":" + String(light) + ",";
    payload += "\"local_alarm\":" + String(isAlarming ? "true" : "false");
    payload += "}";

    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(payload);
    String body = http.getString();
    http.end();

    if (code >= 200 && code < 300) {
        cloudStatus = "OK";
        tempThreshold = extractJsonFloat(body, "temp_threshold", tempThreshold);
        humiThreshold = extractJsonFloat(body, "humi_threshold", humiThreshold);
        lightThreshold = (int)extractJsonFloat(body, "light_threshold", lightThreshold);
        cloudManualAlarm = extractJsonBool(body, "manual_alarm", false);
        checkAlarm();
    } else {
        cloudStatus = "ERR";
    }
}

void updateLedStatus() {
    if (currentAlarmMask() != 0) {
        setLed(alarmPulseOn);
        return;
    }

    unsigned long now = millis();
    if (now - lastLedToggle >= LED_HEARTBEAT_INTERVAL_MS) {
        lastLedToggle = now;
        ledHeartbeatOn = !ledHeartbeatOn;
        setLed(ledHeartbeatOn);
    }
}

int currentAlarmMask() {
    int mask = 0;
    if (isTempAlarm()) mask |= 1;
    if (isHumiAlarm()) mask |= 2;
    if (isLightAlarm()) mask |= 4;
    if (cloudManualAlarm && mask == 0) mask = 1;
    return mask;
}

int alarmCountForType(int type) {
    if (type == 1) {
        return 1;
    }
    if (type == 2) {
        return 2;
    }
    return 3;
}

bool alarmOutputAt(int mask, unsigned long elapsed) {
    const unsigned long beepOnMs = 180;
    const unsigned long beepCycleMs = 420;
    const unsigned long segmentGapMs = 900;
    unsigned long cursor = 0;

    for (int type = 1; type <= 3; type++) {
        int bit = (type == 1) ? 1 : (type == 2 ? 2 : 4);
        if (!(mask & bit)) continue;

        int count = alarmCountForType(type);
        unsigned long segmentMs = count * beepCycleMs;

        if (elapsed >= cursor && elapsed < cursor + segmentMs) {
            unsigned long local = elapsed - cursor;
            return (local % beepCycleMs) < beepOnMs;
        }

        cursor += segmentMs + segmentGapMs;
    }

    return false;
}

void updateBuzzerStatus() {
    int mask = currentAlarmMask();
    unsigned long now = millis();
    alarmPulseOn = false;

    if (mask == 0) {
        setBuzzerOutput(false);
        buzzerAlarmMask = 0;
        return;
    }

    if (mask != buzzerAlarmMask || now - lastBuzzerPattern >= 6000) {
        buzzerAlarmMask = mask;
        buzzerPatternStart = now;
        lastBuzzerPattern = now;
    }

    unsigned long elapsed = now - buzzerPatternStart;
    alarmPulseOn = alarmOutputAt(buzzerAlarmMask, elapsed);
    setBuzzerOutput(alarmPulseOn);
}

void blinkLedAtStartup() {
    for (int i = 0; i < 3; i++) {
        setLed(true);
        delay(150);
        setLed(false);
        delay(150);
    }
}

bool isTempAlarm() {
    return temp > tempThreshold;
}

bool isHumiAlarm() {
    return humi > humiThreshold;
}

bool isLightAlarm() {
    return light > lightThreshold;
}

String riskText() {
    if (!isAlarming) return "OK";
    return "WARN";
}

String riskDetailText() {
    if (!isAlarming) return "ALL NORMAL";
    if (cloudManualAlarm) return "MANUAL";
    String text = "";
    if (isTempAlarm()) text += "TEMP";
    if (isHumiAlarm()) {
        if (text.length()) text += " ";
        text += "HUMI";
    }
    if (isLightAlarm()) {
        if (text.length()) text += " ";
        text += "LIGHT";
    }
    if (text.length()) return text;
    return "CHECK";
}

uint16_t riskColor() {
    if (isAlarming) return ST77XX_RED;
    return ST77XX_GREEN;
}

void drawThermometerIcon(int x, int y, uint16_t color) {
    tft.drawRoundRect(x + 12, y, 12, 42, 6, color);
    tft.fillCircle(x + 18, y + 42, 13, color);
    tft.fillRoundRect(x + 16, y + 8, 4, 30, 2, color);
    tft.fillCircle(x + 18, y + 42, 7, ST77XX_WHITE);
}

void drawDropIcon(int x, int y, uint16_t color) {
    tft.fillCircle(x + 18, y + 35, 15, color);
    tft.fillTriangle(x + 18, y, x + 5, y + 32, x + 31, y + 32, color);
    tft.fillCircle(x + 12, y + 34, 4, ST77XX_WHITE);
}

void drawSunIcon(int x, int y, uint16_t color) {
    tft.fillCircle(x + 20, y + 25, 13, color);
    for (int i = 0; i < 8; i++) {
        float angle = i * 0.785398f;
        int x1 = x + 20 + cos(angle) * 20;
        int y1 = y + 25 + sin(angle) * 20;
        int x2 = x + 20 + cos(angle) * 28;
        int y2 = y + 25 + sin(angle) * 28;
        tft.drawLine(x1, y1, x2, y2, color);
    }
}

void drawQrCodePage() {
    tft.fillScreen(0x0841);
    tft.fillRoundRect(15, 12, 210, 216, 12, 0x2104);
    tft.drawRoundRect(15, 12, 210, 216, 12, ST77XX_BLUE);
    tft.setTextColor(ST77XX_CYAN);
    tft.setTextSize(2);
    tft.setCursor(67, 25);
    tft.print("WEB QR");

    String url = dashboardUrl();
    QRCode qrcode;
    uint8_t qrcodeData[106];
    qrcode_initText(&qrcode, qrcodeData, 3, ECC_LOW, url.c_str());

    int scale = 4;
    int qrSize = qrcode.size * scale;
    int x0 = (240 - qrSize) / 2;
    int y0 = 60;
    tft.fillRect(x0 - 6, y0 - 6, qrSize + 12, qrSize + 12, ST77XX_WHITE);
    for (uint8_t y = 0; y < qrcode.size; y++) {
        for (uint8_t x = 0; x < qrcode.size; x++) {
            if (qrcode_getModule(&qrcode, x, y)) {
                tft.fillRect(x0 + x * scale, y0 + y * scale, scale, scale, ST77XX_BLACK);
            }
        }
    }
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(48, 200);
    tft.print(url);
}

void drawMetricBox(int x, int y, const char* label, const String& value, uint16_t color, bool alarm) {
    uint16_t border = alarm ? ST77XX_RED : color;
    uint16_t fill = ST77XX_WHITE;
    tft.fillRoundRect(x, y, 100, 64, 6, fill);
    tft.drawRoundRect(x, y, 100, 64, 6, border);
    if (alarm) {
        tft.drawRoundRect(x + 2, y + 2, 96, 60, 5, ST77XX_RED);
        tft.setTextSize(1);
        tft.setTextColor(ST77XX_RED);
        tft.setCursor(x + 86, y + 8);
        tft.print("!");
    }
    tft.setTextSize(1);
    tft.setTextColor(alarm ? ST77XX_RED : 0x4208);
    tft.setCursor(x + 8, y + 8);
    tft.print(label);
    tft.setTextSize(strcmp(label, "RISK") == 0 ? 1 : 2);
    tft.setTextColor(border);
    tft.setCursor(x + 8, strcmp(label, "RISK") == 0 ? y + 34 : y + 30);
    tft.print(value);
}

void drawRiskBox(int x, int y) {
    uint16_t border = isAlarming ? ST77XX_RED : ST77XX_GREEN;
    uint16_t fill = ST77XX_WHITE;
    tft.fillRoundRect(x, y, 100, 64, 6, fill);
    tft.drawRoundRect(x, y, 100, 64, 6, border);
    if (isAlarming) {
        tft.drawRoundRect(x + 2, y + 2, 96, 60, 5, ST77XX_RED);
        tft.setTextSize(1);
        tft.setTextColor(ST77XX_RED);
        tft.setCursor(x + 86, y + 8);
        tft.print("!");
    }
    tft.setTextSize(1);
    tft.setTextColor(isAlarming ? ST77XX_RED : 0x4208);
    tft.setCursor(x + 8, y + 8);
    tft.print("RISK");
    tft.setTextSize(2);
    tft.setTextColor(border);
    tft.setCursor(x + 8, y + 24);
    tft.print(riskText());
    tft.setTextSize(1);
    tft.setCursor(x + 5, y + 48);
    tft.print(riskDetailText());
}

void drawOverviewValues() {
    drawMetricBox(15, 44, "TEMP", String(temp, 1) + "C", ST77XX_ORANGE, isTempAlarm());
    drawMetricBox(125, 44, "HUMI", String(humi, 1) + "%", ST77XX_CYAN, isHumiAlarm());
    drawMetricBox(15, 116, "LIGHT", String(lightLevel) + "%", ST77XX_YELLOW, isLightAlarm());
    drawRiskBox(125, 116);
}

void drawOverviewPage() {
    tft.fillScreen(0x0841);
    tft.drawRoundRect(8, 8, 224, 224, 8, ST77XX_CYAN);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_CYAN);
    tft.setCursor(42, 19);
    tft.print("AIoT ENV");
    drawOverviewValues();
    tft.setTextSize(1);
    tft.setTextColor(0xC618);
    tft.setCursor(23, 196);
    tft.print(wifiConnected ? "IP: " : "WiFi: ");
    tft.print(wifiConnected ? ipAddr : "CONNECTING");
    tft.setCursor(23, 212);
    tft.setTextColor(isAlarming ? ST77XX_RED : ST77XX_GREEN);
    tft.print(isAlarming ? "ALARM " : "NORMAL ");
    tft.print("CLOUD:");
    tft.print(cloudStatus);
}

void updateDataPageValues() {
    tft.fillRect(0, 38, 240, 146, 0x0841);
    drawOverviewValues();
    tft.fillRect(0, 208, 240, 18, 0x0841);
    tft.setTextSize(1);
    tft.setCursor(23, 212);
    tft.setTextColor(isAlarming ? ST77XX_RED : ST77XX_GREEN);
    tft.print(isAlarming ? "ALARM " : "NORMAL ");
    tft.print("CLOUD:");
    tft.print(cloudStatus);
}

void drawPage() {
    if (currentPage == 1) {
        drawQrCodePage();
    } else {
        drawOverviewPage();
    }
}

void handleButton() {
    static unsigned long pressStart = 0;
    static bool pressing = false;

    if (digitalRead(BUTTON_PIN) == LOW) {
        if (!pressing) {
            pressing = true;
            pressStart = millis();
        }
    } else {
        if (pressing) {
            pressing = false;
            if (millis() - pressStart > 50) {
                currentPage = (currentPage + 1) % 2;
                drawPage();
                lastSensorRefresh = millis();
            }
        }
    }
}

void handleRoot() {
    String html = "<html><head><meta charset='UTF-8'></head><body>";
    html += "<h1>环境监测系统</h1>";
    html += "<p>Wi-Fi: " + String(wifiConnected ? "已连接" : "未连接") + "</p>";
    html += "<p>IP: " + ipAddr + "</p>";
    html += "<p>温度: " + String(temp) + " °C</p>";
    html += "<p>湿度: " + String(humi) + " %</p>";
    html += "<p>光照指数: " + String(lightLevel) + " %</p>";
    html += "<p>光照ADC原始值: " + String(light) + " / 4095</p>";
    html += "<form action='/set'>";
    html += "温度阈值: <input name='temp' value='" + String(tempThreshold) + "'><br>";
    html += "湿度阈值: <input name='humi' value='" + String(humiThreshold) + "'><br>";
    html += "光照阈值: <input name='light' value='" + String(lightThreshold) + "'><br>";
    html += "<input type='submit' value='保存'>";
    html += "</form>";
    html += "<p>报警状态: " + String(isAlarming ? "报警中" : "正常") + "</p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
}

void handleSet() {
    if (server.hasArg("temp")) tempThreshold = server.arg("temp").toFloat();
    if (server.hasArg("humi")) humiThreshold = server.arg("humi").toFloat();
    if (server.hasArg("light")) lightThreshold = server.arg("light").toInt();
    server.send(200, "text/plain", "OK");
}

void setup() {
    Serial.begin(115200);

    tft.init(240, 240);
    tft.setRotation(DISPLAY_ROTATION);
    tft.invertDisplay(false);
    tft.fillScreen(ST77XX_BLACK);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    setBuzzerOutput(false);
    setLed(false);
    blinkLedAtStartup();
    pinMode(LIGHT_PIN, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(LIGHT_PIN, ADC_11db);

    Wire.begin(4, 5);
    if (!sht30.begin(0x44)) {
        tft.setCursor(20, 100);
        tft.print("SHT30 Error");
        while (1) {
            delay(100);
        }
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(sta_ssid, sta_password);
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_CYAN);
    tft.setTextSize(2);
    tft.setCursor(25, 90);
    tft.print("WiFi Connecting");
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
        delay(300);
        tft.print(".");
    }
    wifiConnected = WiFi.status() == WL_CONNECTED;
    ipAddr = wifiConnected ? WiFi.localIP().toString() : "NO WIFI";

    server.on("/", handleRoot);
    server.on("/set", handleSet);
    server.begin();

    readSensors();
    drawPage();
    lastSensorRefresh = millis();
}

void loop() {
    unsigned long now = millis();

    server.handleClient();
    handleButton();
    updateBuzzerStatus();
    updateLedStatus();

    if (now - lastSensorRefresh > SENSOR_REFRESH_INTERVAL_MS) {
        lastSensorRefresh = now;
        readSensors();
        checkAlarm();
        if (currentPage == 0) {
            updateDataPageValues();
        }
    }

    if (now - lastCloudSync > CLOUD_SYNC_INTERVAL_MS) {
        lastCloudSync = now;
        syncCloud();
    }

    delay(50);
}
