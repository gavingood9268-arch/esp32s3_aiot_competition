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
const uint8_t BUZZER_PWM_CHANNEL = 0;
const uint8_t BUZZER_PWM_RESOLUTION = 8;
const uint32_t BUZZER_PWM_DUTY = 24;
const unsigned int BUZZER_STARTUP_HZ = 2200;

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
int currentPage = 0;  // 0 overview, 1 AI status, 2 QR code
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
bool buzzerOutputOn = true;
unsigned int buzzerToneHz = 0;
String cloudStatus = "OFF";
String aiStatus = "WAIT";
String aiRisk = "--";
String aiSummary = "NO AI";
String aiAdvice = "--";

bool isTempAlarm();
bool isHumiAlarm();
bool isLightAlarm();
int currentAlarmMask();
void updateAiStatusPageValues();

void setLed(bool on) {
    digitalWrite(LED_PIN, (on == LED_ACTIVE_HIGH) ? HIGH : LOW);
}

void setBuzzerOutput(bool on, unsigned int frequencyHz = BUZZER_STARTUP_HZ) {
    if (on == buzzerOutputOn && (!on || frequencyHz == buzzerToneHz)) {
        return;
    }
    if (on) {
        ledcWriteTone(BUZZER_PWM_CHANNEL, frequencyHz);
        ledcWrite(BUZZER_PWM_CHANNEL, BUZZER_PWM_DUTY);
        buzzerToneHz = frequencyHz;
    } else {
        ledcWriteTone(BUZZER_PWM_CHANNEL, 0);
        ledcWrite(BUZZER_PWM_CHANNEL, 0);
        digitalWrite(BUZZER_PIN, LOW);
        buzzerToneHz = 0;
    }
    buzzerOutputOn = on;
}

void testBuzzerAtStartup() {
    setBuzzerOutput(true, 1800);
    delay(140);
    setBuzzerOutput(false);
    delay(120);
    setBuzzerOutput(true, 2600);
    delay(100);
    setBuzzerOutput(false);
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

String extractJsonString(const String& body, const String& key, const String& fallback) {
    int idx = body.indexOf("\"" + key + "\":");
    if (idx < 0) return fallback;
    idx = body.indexOf("\"", idx + key.length() + 3);
    if (idx < 0) return fallback;
    idx++;
    String value = "";
    bool escaping = false;
    while (idx < body.length()) {
        char c = body[idx++];
        if (escaping) {
            value += c;
            escaping = false;
            continue;
        }
        if (c == '\\') {
            escaping = true;
            continue;
        }
        if (c == '"') break;
        value += c;
    }
    return value.length() ? value : fallback;
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
        aiStatus = extractJsonString(body, "status", aiStatus);
        aiRisk = extractJsonString(body, "risk", aiRisk);
        aiSummary = extractJsonString(body, "summary", aiSummary);
        aiAdvice = extractJsonString(body, "advice", aiAdvice);
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
    if (cloudManualAlarm && mask == 0) mask = 7;
    return mask;
}

bool patternOutputAt(unsigned long elapsed, const unsigned int* pattern, int length) {
    unsigned long cursor = 0;
    for (int i = 0; i < length; i++) {
        cursor += pattern[i];
        if (elapsed < cursor) {
            return (i % 2) == 0;
        }
    }
    return false;
}

bool alarmOutputAt(int mask, unsigned long elapsed) {
    static const unsigned int tempOnly[] = {260};
    static const unsigned int humiOnly[] = {120, 160, 120};
    static const unsigned int lightOnly[] = {85, 110, 85, 110, 85};
    static const unsigned int tempHumi[] = {260, 220, 120};
    static const unsigned int tempLight[] = {260, 220, 85, 110, 85, 110, 85};
    static const unsigned int humiLight[] = {120, 150, 120, 230, 260};
    static const unsigned int allAlarm[] = {90, 110, 90, 110, 90, 240, 260};

    switch (mask & 7) {
        case 1:
            return patternOutputAt(elapsed, tempOnly, sizeof(tempOnly) / sizeof(tempOnly[0]));
        case 2:
            return patternOutputAt(elapsed, humiOnly, sizeof(humiOnly) / sizeof(humiOnly[0]));
        case 3:
            return patternOutputAt(elapsed, tempHumi, sizeof(tempHumi) / sizeof(tempHumi[0]));
        case 4:
            return patternOutputAt(elapsed, lightOnly, sizeof(lightOnly) / sizeof(lightOnly[0]));
        case 5:
            return patternOutputAt(elapsed, tempLight, sizeof(tempLight) / sizeof(tempLight[0]));
        case 6:
            return patternOutputAt(elapsed, humiLight, sizeof(humiLight) / sizeof(humiLight[0]));
        case 7:
            return patternOutputAt(elapsed, allAlarm, sizeof(allAlarm) / sizeof(allAlarm[0]));
        default:
            return false;
    }
}

unsigned int alarmFrequencyForMask(int mask) {
    switch (mask & 7) {
        case 1:
            return 1500;
        case 2:
            return 2200;
        case 3:
            return 1800;
        case 4:
            return 3100;
        case 5:
            return 2600;
        case 6:
            return 2800;
        case 7:
            return 2400;
        default:
            return BUZZER_STARTUP_HZ;
    }
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
    setBuzzerOutput(alarmPulseOn, alarmFrequencyForMask(buzzerAlarmMask));
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

String aiShortStatus() {
    if (aiStatus == "OK") return "OK";
    if (aiStatus == "ERROR") return "ERR";
    if (aiStatus == "STUB") return "RULE";
    if (aiStatus == "RUNNING") return "RUN";
    if (aiStatus == "NO_DATA") return "NO";
    if (aiStatus == "NOT_RUN") return "WAIT";
    return aiStatus.length() > 4 ? aiStatus.substring(0, 4) : aiStatus;
}

String aiRiskLabel() {
    if (aiRisk.indexOf("危险") >= 0) return "DANGER";
    if (aiRisk.indexOf("预警") >= 0) return "WARN";
    if (aiRisk.indexOf("关注") >= 0) return "WATCH";
    if (aiRisk.indexOf("正常") >= 0) return "NORMAL";
    if (aiRisk.length() == 0 || aiRisk == "未分析") return "--";
    return aiRisk.length() > 8 ? aiRisk.substring(0, 8) : aiRisk;
}

uint16_t aiRiskColor() {
    String label = aiRiskLabel();
    if (label == "DANGER") return ST77XX_RED;
    if (label == "WARN") return ST77XX_ORANGE;
    if (label == "WATCH") return ST77XX_YELLOW;
    if (label == "NORMAL") return ST77XX_GREEN;
    return ST77XX_CYAN;
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
    tft.print(" AI:");
    tft.print(aiShortStatus());
}

void drawAiStatusPage() {
    tft.fillScreen(0x0841);
    tft.drawRoundRect(8, 8, 224, 224, 8, ST77XX_MAGENTA);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_MAGENTA);
    tft.setCursor(72, 18);
    tft.print("CLOUD AI");

    tft.setTextSize(1);
    tft.setTextColor(0xC618);
    tft.setCursor(22, 52);
    tft.print("NET:");
    tft.setCursor(122, 52);
    tft.print("AI:");

    tft.fillRoundRect(20, 74, 200, 52, 6, ST77XX_WHITE);
    tft.drawRoundRect(20, 74, 200, 52, 6, ST77XX_CYAN);
    tft.setTextSize(1);
    tft.setTextColor(0x4208);
    tft.setCursor(32, 84);
    tft.print("RISK");

    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(22, 142);
    tft.print("AUTO AI: ON");
    tft.setCursor(32, 158);
    tft.print("T:");
    tft.setCursor(120, 158);
    tft.print("H:");
    tft.setCursor(32, 174);
    tft.print("L:");
    tft.setCursor(120, 174);
    tft.print("ALM:");
    tft.setCursor(22, 202);
    tft.setTextColor(ST77XX_CYAN);
    tft.print("QR: WEB   BTN: NEXT");
    updateAiStatusPageValues();
}

void updateAiStatusPageValues() {
    tft.setTextSize(1);
    tft.fillRect(58, 52, 58, 10, 0x0841);
    tft.setCursor(58, 52);
    tft.setTextColor(cloudStatus == "OK" ? ST77XX_GREEN : ST77XX_RED);
    tft.print(cloudStatus);

    tft.fillRect(146, 52, 50, 10, 0x0841);
    tft.setCursor(146, 52);
    tft.setTextColor(aiStatus == "OK" ? ST77XX_GREEN : ST77XX_YELLOW);
    tft.print(aiShortStatus());

    tft.drawRoundRect(20, 74, 200, 52, 6, aiRiskColor());
    tft.fillRect(22, 96, 196, 26, ST77XX_WHITE);
    tft.setTextSize(2);
    tft.setTextColor(aiRiskColor());
    tft.setCursor(32, 102);
    tft.print(aiRiskLabel());

    tft.setTextSize(1);
    tft.fillRect(50, 158, 50, 10, 0x0841);
    tft.setCursor(50, 158);
    tft.setTextColor(isTempAlarm() ? ST77XX_RED : ST77XX_GREEN);
    tft.print(isTempAlarm() ? "HIGH" : "OK");

    tft.fillRect(140, 158, 50, 10, 0x0841);
    tft.setCursor(140, 158);
    tft.setTextColor(isHumiAlarm() ? ST77XX_RED : ST77XX_GREEN);
    tft.print(isHumiAlarm() ? "HIGH" : "OK");

    tft.fillRect(50, 174, 50, 10, 0x0841);
    tft.setCursor(50, 174);
    tft.setTextColor(isLightAlarm() ? ST77XX_RED : ST77XX_GREEN);
    tft.print(isLightAlarm() ? "HIGH" : "OK");

    tft.fillRect(150, 174, 40, 10, 0x0841);
    tft.setCursor(150, 174);
    tft.setTextColor(isAlarming ? ST77XX_RED : ST77XX_GREEN);
    tft.print(isAlarming ? "ON" : "OFF");
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
    tft.print(" AI:");
    tft.print(aiShortStatus());
}

void drawPage() {
    if (currentPage == 2) {
        drawQrCodePage();
    } else if (currentPage == 1) {
        drawAiStatusPage();
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
                currentPage = (currentPage + 1) % 3;
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
    html += "<p>云端状态: " + cloudStatus + "</p>";
    html += "<p>AI状态: " + aiStatus + "</p>";
    html += "<p>AI风险: " + aiRisk + "</p>";
    html += "<p>AI说明: " + aiSummary + "</p>";
    html += "<p>AI建议: " + aiAdvice + "</p>";
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
    ledcSetup(BUZZER_PWM_CHANNEL, BUZZER_STARTUP_HZ, BUZZER_PWM_RESOLUTION);
    ledcAttachPin(BUZZER_PIN, BUZZER_PWM_CHANNEL);
    setBuzzerOutput(false);
    testBuzzerAtStartup();
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
        } else if (currentPage == 1) {
            updateAiStatusPageValues();
        }
    }

    if (now - lastCloudSync > CLOUD_SYNC_INTERVAL_MS) {
        lastCloudSync = now;
        syncCloud();
        if (currentPage == 1) {
            updateAiStatusPageValues();
        } else if (currentPage == 0) {
            updateDataPageValues();
        }
    }

    delay(50);
}
