#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <TinyGPS++.h>
#include <math.h>


/**************************************************************
 *  SAATHI — Elderly Health Monitoring System
 *
 *  Hardware:
 *    - ESP32
 *    - MAX30102  (Heart Rate + SpO2)
 *    - MPU6050   (Fall Detection)
 *    - LM35      (Body Temperature)
 *    - Neo-6M    (GPS Location)
 *    - 16x2 I2C LCD
 *    - Buzzer
 *    - SOS Push Button
 *
 *  Features:
 *    - Real-time BPM + SpO2 monitoring
 *    - Body temperature monitoring
 *    - Fall detection with 10s cancellation countdown
 *    - SOS emergency button
 *    - Telegram alerts for all emergencies
 *    - On-demand health report via "HealthReport" command
 *    - Health Score (0-100) with smart alert levels
 *    - Abnormality persistence (3 consecutive bad readings)
 *    - Predictive rising BPM alert
 *    - Medication reminder every 4 hours
 *    - Periodic health summary
 *    - WiFi auto-reconnect with alert queue
 *    - Live GPS location in all alerts
 *
 *  Setup:
 *    1. Fill in your credentials in the USER CONFIGURATION
 *       section below
 *    2. Install required libraries via Arduino Library Manager:
 *       - MAX30105 by SparkFun
 *       - LiquidCrystal I2C by Frank de Brabander
 *       - TinyGPSPlus by Mikal Hart
 *    3. Flash to ESP32
 *
 *  Telegram Setup:
 *    - Create a bot via @BotFather → copy the Bot Token
 *    - Get your Chat ID via @userinfobot
 *
 **************************************************************/


/**************************************************/
// ⚙️  USER CONFIGURATION — fill these in
/**************************************************/

const char* ssid       = "YOUR_WIFI_SSID";
const char* password   = "YOUR_WIFI_PASSWORD";

const String BOT_TOKEN = "YOUR_TELEGRAM_BOT_TOKEN";   // from @BotFather
const String CHAT_ID   = "YOUR_TELEGRAM_CHAT_ID";     // from @userinfobot

// JSONBin (https://jsonbin.io) — for dashboard
const String BIN_ID    = "YOUR_JSONBIN_BIN_ID";
const String BIN_KEY   = "YOUR_JSONBIN_MASTER_KEY";


/**************************************************/
// END OF USER CONFIGURATION
/**************************************************/


const String BIN_URL = "https://api.jsonbin.io/v3/b/" + BIN_ID;


/******** GPS ********/

TinyGPSPlus gps;
HardwareSerial gpsSerial(2);   // RX=16, TX=17


/******** LCD ********/

LiquidCrystal_I2C lcd(0x27, 16, 2);


/******** MAX30102 ********/

MAX30105 particleSensor;

uint32_t irBuffer[100];
uint32_t redBuffer[100];

int32_t bufferLength;
int32_t spo2;
int8_t  validSPO2;
int32_t heartRate;
int8_t  validHeartRate;


/******** PINS ********/

#define BUZZER   25
#define LM35     34
#define BUTTON   14
#define MPU_ADDR 0x68


/******** FALL DETECTION ********/

#define FREEFALL_THRESHOLD  6553
#define IMPACT_THRESHOLD   40960
#define IMPACT_WINDOW        600


/******** TREND — last 10 readings ********/

#define TREND_SIZE 10
int   bpmHistory[TREND_SIZE];
int   spo2History[TREND_SIZE];
float tempHistory[TREND_SIZE];
int   historyIndex = 0;
int   historyCount = 0;


/******** ABNORMALITY PERSISTENCE ********/

int highBpmCount  = 0;
int lowSpo2Count  = 0;
int highTempCount = 0;


/******** FINGER REMOVED — LCD ONLY ********/

bool fingerWasRemoved = false;


/******** WIFI ALERT QUEUE ********/

String pendingTelegramMsg = "";
bool   alertPending       = false;


/******** SUMMARY TIMER ********/

unsigned long lastSummaryTime = 0;
// Change to 86400000UL for real 24-hour summary
#define SUMMARY_INTERVAL_MS  300000UL

int   sumBpm      = 0;
int   minBpm      = 9999;
int   maxBpm      = 0;
int   sumSpo2     = 0;
float sumTemp     = 0.0;
int   readingCount = 0;


/******** MEDICATION REMINDER ********/

unsigned long lastMedReminder = 0;
#define MED_INTERVAL_MS  14400000UL   // 4 hours


/******** TELEGRAM POLLING ********/

unsigned long lastTelegramPoll = 0;
#define TELEGRAM_POLL_MS  3000UL
long lastUpdateId = 0;


/******** LAST KNOWN VITALS ********/

int    lastBpm    = 0;
int    lastSpo2   = 0;
float  lastTemp   = 0.0;
int    lastScore  = 0;
String lastStatus = "Unknown";
String lastTrend  = "Unknown";


/**************************************************/
// FORWARD DECLARATIONS
/**************************************************/

String getLocationString();
void   sendTelegramAlert(String message);
void   sendToWebsite(int bpm, int spo2Val, float temp, bool fall, bool sos,
                     int score, String status, String trend);
int    calcHealthScore(int bpm, int spo2Val, float temp);
String healthStatus(int score);
String alertEmoji(int score);
String detectTrend();


/**************************************************/
// WIFI RECONNECT
/**************************************************/

void ensureWiFi()
{
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("WiFi lost — reconnecting...");
  WiFi.disconnect();
  WiFi.begin(ssid, password);

  int a = 0;
  while (WiFi.status() != WL_CONNECTED && a < 20)
  { delay(500); Serial.print("."); a++; }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\nWiFi reconnected!");
    if (alertPending && pendingTelegramMsg != "")
    {
      sendTelegramAlert(pendingTelegramMsg);
      pendingTelegramMsg = "";
      alertPending       = false;
    }
  }
  else
  {
    Serial.println("\nReconnect failed.");
  }
}


/**************************************************/
// LIVE GPS LOCATION STRING
/**************************************************/

String getLocationString()
{
  // Feed any available GPS data
  while (gpsSerial.available() > 0)
    gps.encode(gpsSerial.read());

  if (gps.location.isValid() && gps.location.isUpdated())
  {
    String loc = "\n📍 *Location:*";
    loc += "\nCoordinates: "
           + String(gps.location.lat(), 6)
           + ", "
           + String(gps.location.lng(), 6);
    loc += "\nGoogle Maps: https://maps.google.com/?q="
           + String(gps.location.lat(), 6)
           + ","
           + String(gps.location.lng(), 6);
    return loc;
  }

  return "\n📍 Location: Acquiring GPS fix...";
}


/**************************************************/
// TELEGRAM SEND
/**************************************************/

void sendTelegramAlert(String message)
{
  ensureWiFi();

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi unavailable — queuing alert.");
    pendingTelegramMsg = message;
    alertPending       = true;
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);

  HTTPClient http;
  http.setTimeout(5000);

  String url = "https://api.telegram.org/bot" + BOT_TOKEN + "/sendMessage";
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  message.replace("\"", "\\\"");

  String payload =
      "{\"chat_id\":\"" + CHAT_ID +
      "\",\"text\":\"" + message +
      "\",\"parse_mode\":\"Markdown\"}";

  int code = http.POST(payload);
  Serial.print("Telegram Code: "); Serial.println(code);
  http.end();
}


/**************************************************/
// TELEGRAM POLL — HealthReport command only
/**************************************************/

void checkTelegramCommands()
{
  if (millis() - lastTelegramPoll < TELEGRAM_POLL_MS) return;
  lastTelegramPoll = millis();

  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(3);

  HTTPClient http;
  http.setTimeout(3000);

  String url = "https://api.telegram.org/bot" + BOT_TOKEN +
               "/getUpdates?offset=" + String(lastUpdateId + 1) +
               "&limit=5&timeout=0";

  http.begin(client, url);
  int code = http.GET();
  if (code != 200) { http.end(); return; }

  String response = http.getString();
  http.end();

  // Advance update_id
  int uidIdx = response.indexOf("\"update_id\":");
  if (uidIdx != -1)
  {
    int start = uidIdx + 12;
    int end   = response.indexOf(",", start);
    lastUpdateId = response.substring(start, end).toInt();
  }

  // Only respond to HealthReport
  if (response.indexOf("HealthReport") == -1 &&
      response.indexOf("healthreport") == -1) return;

  String emoji  = alertEmoji(lastScore);
  String report = "📋 *SAATHI Health Report*\n";
  report += "━━━━━━━━━━━━━━━━\n";
  report += "❤️  BPM:   "     + String(lastBpm)      + "\n";
  report += "🩸 SpO2:  "      + String(lastSpo2)     + "%\n";
  report += "🌡️  Temp:  "     + String(lastTemp, 1)  + " C\n";
  report += "━━━━━━━━━━━━━━━━\n";
  report += emoji + " Score: " + String(lastScore)   + "/100\n";
  report += "📊 Status: "     + lastStatus            + "\n";
  report += "📈 Trend:  "     + lastTrend             + "\n";
  report += "━━━━━━━━━━━━━━━━";
  report += getLocationString();
  sendTelegramAlert(report);
  Serial.println("Health report sent.");
}


/**************************************************/
// JSONBIN
/**************************************************/

void sendToWebsite(int bpm, int spo2Val, float temp, bool fall, bool sos,
                   int score, String status, String trend)
{
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return;

  // Get latest GPS coordinates
  while (gpsSerial.available() > 0)
    gps.encode(gpsSerial.read());

  String latStr = "null";
  String lngStr = "null";
  if (gps.location.isValid())
  {
    latStr = String(gps.location.lat(), 6);
    lngStr = String(gps.location.lng(), 6);
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);

  HTTPClient http;
  http.setTimeout(5000);
  http.begin(client, BIN_URL);
  http.addHeader("Content-Type",     "application/json");
  http.addHeader("X-Master-Key",     BIN_KEY);
  http.addHeader("X-Bin-Versioning", "false");

  // History arrays
  String bpmArr = "[", spo2Arr = "[", tempArr = "[";
  int count = min(historyCount, TREND_SIZE);
  for (int i = 0; i < count; i++)
  {
    int idx = (historyIndex - count + i + TREND_SIZE) % TREND_SIZE;
    bpmArr  += String(bpmHistory[idx]);
    spo2Arr += String(spo2History[idx]);
    tempArr += String(tempHistory[idx], 1);
    if (i < count - 1) { bpmArr += ","; spo2Arr += ","; tempArr += ","; }
  }
  bpmArr += "]"; spo2Arr += "]"; tempArr += "]";

  String payload = "{";
  payload += "\"bpm\":"         + String(bpm)                      + ",";
  payload += "\"spo2\":"        + String(spo2Val)                  + ",";
  payload += "\"temperature\":" + String(temp, 1)                  + ",";
  payload += "\"fall\":"        + String(fall ? "true" : "false")  + ",";
  payload += "\"sos\":"         + String(sos  ? "true" : "false")  + ",";
  payload += "\"battery\":100,";
  payload += "\"latitude\":"    + latStr                           + ",";
  payload += "\"longitude\":"   + lngStr                          + ",";
  payload += "\"healthScore\":" + String(score)                    + ",";
  payload += "\"status\":\""    + status                           + "\",";
  payload += "\"trend\":\""     + trend                            + "\",";
  payload += "\"bpmHistory\":"  + bpmArr                           + ",";
  payload += "\"spo2History\":" + spo2Arr                          + ",";
  payload += "\"tempHistory\":" + tempArr;
  payload += "}";

  Serial.println("JSONBin → " + payload);
  int httpCode = http.PUT(payload);
  Serial.print("JSONBin Code: "); Serial.println(httpCode);
  http.end();
}


/**************************************************/
// HEALTH SCORE
/**************************************************/

int calcHealthScore(int bpm, int spo2Val, float temp)
{
  int score = 100;
  if      (bpm > 120)    score -= 20;
  else if (bpm > 110)    score -= 10;
  if      (spo2Val < 90) score -= 30;
  else if (spo2Val < 94) score -= 15;
  if      (temp > 39)    score -= 20;
  else if (temp > 38)    score -= 10;
  return max(score, 0);
}

String healthStatus(int score)
{
  if (score >= 85) return "Normal";
  if (score >= 60) return "Warning";
  return "Critical";
}

String alertEmoji(int score)
{
  if (score >= 85) return "🟢";
  if (score >= 60) return "🟡";
  return "🔴";
}


/**************************************************/
// TREND
/**************************************************/

void pushHistory(int bpm, int spo2Val, float temp)
{
  bpmHistory[historyIndex]  = bpm;
  spo2History[historyIndex] = spo2Val;
  tempHistory[historyIndex] = temp;
  historyIndex = (historyIndex + 1) % TREND_SIZE;
  if (historyCount < TREND_SIZE) historyCount++;
}

String detectTrend()
{
  if (historyCount < 4) return "Collecting data";

  int rising = 0, falling = 0;
  for (int i = 1; i < 4; i++)
  {
    int prev = (historyIndex - i - 1 + TREND_SIZE) % TREND_SIZE;
    int curr = (historyIndex - i     + TREND_SIZE) % TREND_SIZE;
    if (bpmHistory[curr]  > bpmHistory[prev])  rising++;
    if (spo2History[curr] < spo2History[prev]) falling++;
  }
  if (rising  == 3) return "Rising Heart Rate";
  if (falling == 3) return "Falling SpO2";
  return "Stable";
}


/**************************************************/
// MPU6050
/**************************************************/

long readMagnitude()
{
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6, true);
  if (Wire.available() < 6) return -1;

  int16_t ax = (Wire.read() << 8) | Wire.read();
  int16_t ay = (Wire.read() << 8) | Wire.read();
  int16_t az = (Wire.read() << 8) | Wire.read();

  return (long)sqrt((float)ax*ax + (float)ay*ay + (float)az*az);
}


/**************************************************/
// FALL DETECTION
/**************************************************/

void checkFall()
{
  long mag = readMagnitude();
  if (mag < 0) return;

  if (mag < FREEFALL_THRESHOLD)
  {
    unsigned long t = millis();
    while (millis() - t < IMPACT_WINDOW)
    {
      if (readMagnitude() > IMPACT_THRESHOLD)
      {
        // 10-second cancellation countdown
        bool cancelled = false;
        for (int count = 10; count > 0; count--)
        {
          lcd.clear();
          lcd.setCursor(0, 0); lcd.print("FALL DETECTED!");
          lcd.setCursor(0, 1); lcd.print("Cancel: "); lcd.print(count); lcd.print("s  ");
          ledcWriteTone(BUZZER, 1000); delay(100);
          ledcWriteTone(BUZZER, 0);
          if (digitalRead(BUTTON) == LOW) { cancelled = true; break; }
          delay(900);
        }

        if (cancelled)
        {
          lcd.clear();
          lcd.setCursor(0, 0); lcd.print("Alert Cancelled");
          lcd.setCursor(0, 1); lcd.print("Stay Safe!");
          delay(2000);
          return;
        }

        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("FALL DETECTED!");
        lcd.setCursor(0, 1); lcd.print("HELP COMING!");
        for (int i = 0; i < 5; i++)
        {
          ledcWriteTone(BUZZER, 2500); delay(200);
          ledcWriteTone(BUZZER, 0);   delay(200);
        }

        int score = calcHealthScore(lastBpm, lastSpo2, lastTemp);
        String msg = "🚨 *FALL DETECTED*\n";
        msg += alertEmoji(score) + " Severity: " + healthStatus(score) + "\n";
        msg += "Patient may have fallen!\n";
        msg += "Please check immediately.\n";
        msg += "⚠️ Avoid sudden movement.";
        msg += getLocationString();
        sendTelegramAlert(msg);
        sendToWebsite(lastBpm, lastSpo2, lastTemp, true, false,
                      score, "Fall", "Fall Detected");

        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("Alert Sent!");
        lcd.setCursor(0, 1); lcd.print("Help Coming");
        delay(3000);
        return;
      }
      delay(10);
    }
  }
}


/**************************************************/
// DAILY SUMMARY
/**************************************************/

void sendDailySummary()
{
  if (readingCount == 0) return;

  String msg = "📊 *Health Summary*\n";
  msg += "━━━━━━━━━━━━━━━━\n";
  msg += "Avg BPM: "  + String(sumBpm  / readingCount)    + "\n";
  msg += "Max BPM: "  + String(maxBpm)                    + "\n";
  msg += "Min BPM: "  + String(minBpm)                    + "\n";
  msg += "Avg SpO2: " + String(sumSpo2 / readingCount)    + "%\n";
  msg += "Avg Temp: " + String(sumTemp / readingCount, 1) + " C\n";
  msg += "Readings: " + String(readingCount);
  sendTelegramAlert(msg);

  sumBpm = 0; minBpm = 9999; maxBpm = 0;
  sumSpo2 = 0; sumTemp = 0.0; readingCount = 0;
}


/**************************************************/
// SETUP
/**************************************************/

void setup()
{
  Serial.begin(115200);
  delay(500);

  ledcAttach(BUZZER, 2000, 8);
  pinMode(BUTTON, INPUT_PULLUP);

  Wire.begin(21, 22);
  Wire.setClock(400000);

  // Wake MPU6050
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); Wire.write(0x00);
  Wire.endTransmission(true);

  // Set ±2g range
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1C); Wire.write(0x00);
  Wire.endTransmission(true);

  // GPS
  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);
  Serial.println("GPS initializing...");

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("SAATHI");
  lcd.setCursor(0, 1); lcd.print("Initializing...");

  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20)
  { delay(500); Serial.print("."); attempts++; }

  if (WiFi.status() == WL_CONNECTED)
    Serial.println("\nWiFi: " + WiFi.localIP().toString());
  else
    Serial.println("\nWiFi FAILED.");

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST))
  {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("MAX30102 Error!");
    Serial.println("MAX30102 not found");
    while (1);
  }
  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x1F);
  particleSensor.setPulseAmplitudeIR(0x1F);

  lastSummaryTime  = millis();
  lastMedReminder  = millis();
  lastTelegramPoll = millis();

  delay(2000);
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Place Finger");
  lcd.setCursor(0, 1); lcd.print("On Sensor...");
}


/**************************************************/
// LOOP
/**************************************************/

void loop()
{
  // Feed GPS continuously
  while (gpsSerial.available() > 0)
    gps.encode(gpsSerial.read());

  ensureWiFi();
  checkFall();
  checkTelegramCommands();


  /******** MEDICATION REMINDER ********/

  if (millis() - lastMedReminder >= MED_INTERVAL_MS)
  {
    lastMedReminder = millis();
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Medicine Time!");
    lcd.setCursor(0, 1); lcd.print("Take your dose");
    for (int i = 0; i < 3; i++)
    { ledcWriteTone(BUZZER,1500); delay(300); ledcWriteTone(BUZZER,0); delay(300); }
    sendTelegramAlert("💊 *Medication Reminder*\nTime for the patient's medicine.");
    delay(3000);
  }


  /******** SUMMARY ********/

  if (millis() - lastSummaryTime >= SUMMARY_INTERVAL_MS)
  { lastSummaryTime = millis(); sendDailySummary(); }


  /******** SOS BUTTON ********/

  if (digitalRead(BUTTON) == LOW)
  {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("EMERGENCY!");
    lcd.setCursor(0, 1); lcd.print("HELP NEEDED");
    for (int i = 0; i < 5; i++)
    { ledcWriteTone(BUZZER,2500); delay(200); ledcWriteTone(BUZZER,0); delay(200); }

    String msg = "🆘 *SOS BUTTON PRESSED*\n";
    msg += "🔴 Patient manually triggered emergency!\n";
    msg += "Please respond immediately.";
    msg += getLocationString();
    sendTelegramAlert(msg);

    int score = calcHealthScore(lastBpm, lastSpo2, lastTemp);
    sendToWebsite(lastBpm, lastSpo2, lastTemp, false, true,
                  score, "SOS", "SOS Triggered");

    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Alert Sent!");
    lcd.setCursor(0, 1); lcd.print("Help Coming");
    delay(3000);
    return;
  }


  /******** TEMPERATURE ********/

  int sensorValue    = analogRead(LM35);
  float voltage      = sensorValue * (3.3 / 4095.0);
  float temperatureC = voltage * 100;


  /******** COLLECT SAMPLES ********/

  bufferLength = 100;

  for (byte i = 0; i < bufferLength; i++)
  {
    // Feed GPS during sampling too
    while (gpsSerial.available() > 0)
      gps.encode(gpsSerial.read());

    checkFall();

    if (digitalRead(BUTTON) == LOW)
    {
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("EMERGENCY!");
      lcd.setCursor(0, 1); lcd.print("HELP NEEDED");
      for (int j = 0; j < 5; j++)
      { ledcWriteTone(BUZZER,2500); delay(200); ledcWriteTone(BUZZER,0); delay(200); }
      String msg = "🆘 *SOS BUTTON PRESSED*\n";
      msg += "🔴 Patient manually triggered emergency!\n";
      msg += "Please respond immediately.";
      msg += getLocationString();
      sendTelegramAlert(msg);
      int score = calcHealthScore(lastBpm, lastSpo2, lastTemp);
      sendToWebsite(lastBpm, lastSpo2, lastTemp, false, true,
                    score, "SOS", "SOS Triggered");
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("Alert Sent!");
      lcd.setCursor(0, 1); lcd.print("Help Coming");
      delay(3000);
      return;
    }

    while (particleSensor.available() == false)
      particleSensor.check();

    redBuffer[i] = particleSensor.getRed();
    irBuffer[i]  = particleSensor.getIR();
    particleSensor.nextSample();
  }


  /******** NO FINGER — LCD only, zero Telegram ********/

  if (irBuffer[99] < 50000)
  {
    fingerWasRemoved = true;
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Place Finger");
    lcd.setCursor(0, 1); lcd.print("On Sensor...");
    ledcWriteTone(BUZZER, 0);
    delay(500);
    return;
  }
  else
  {
    fingerWasRemoved = false;
  }


  /******** CALCULATE ********/

  maxim_heart_rate_and_oxygen_saturation(
      irBuffer, bufferLength, redBuffer,
      &spo2, &validSPO2, &heartRate, &validHeartRate);

  if (!validHeartRate || !validSPO2)
  {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Calculating...");
    lcd.setCursor(0, 1); lcd.print("Keep still...");
    delay(1000);
    return;
  }


  /******** HEALTH SCORE + TREND ********/

  int    score  = calcHealthScore((int)heartRate, (int)spo2, temperatureC);
  String status = healthStatus(score);
  String emoji  = alertEmoji(score);

  pushHistory((int)heartRate, (int)spo2, temperatureC);
  String trend = detectTrend();

  lastBpm    = (int)heartRate;
  lastSpo2   = (int)spo2;
  lastTemp   = temperatureC;
  lastScore  = score;
  lastStatus = status;
  lastTrend  = trend;

  sumBpm  += (int)heartRate;
  sumSpo2 += (int)spo2;
  sumTemp += temperatureC;
  if ((int)heartRate > maxBpm) maxBpm = (int)heartRate;
  if ((int)heartRate < minBpm) minBpm = (int)heartRate;
  readingCount++;

  Serial.print("BPM:"); Serial.print(heartRate);
  Serial.print(" SpO2:"); Serial.print(spo2);
  Serial.print("% Temp:"); Serial.print(temperatureC);
  Serial.print("C Score:"); Serial.print(score);
  Serial.print(" "); Serial.println(status);


  /******** SCREEN 1 — BPM + SpO2 ********/

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("BPM : "); lcd.print(heartRate);
  lcd.setCursor(0, 1); lcd.print("SpO2: "); lcd.print(spo2); lcd.print("%");
  delay(2000);


  /******** SCREEN 2 — Temperature ********/

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Temp: "); lcd.print(temperatureC, 1); lcd.print(" C");
  lcd.setCursor(0, 1); lcd.print("Saathi Active");
  delay(2000);


  /******** ABNORMALITY PERSISTENCE ********/

  if (heartRate > 120) highBpmCount++; else highBpmCount = 0;
  if (highBpmCount >= 3)
  {
    highBpmCount = 0;
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("HIGH BPM!");
    lcd.setCursor(0, 1); lcd.print("Calm Down");
    for (int i = 0; i < 3; i++)
    { ledcWriteTone(BUZZER,2500); delay(200); ledcWriteTone(BUZZER,0); delay(200); }
    String msg = emoji + " *HIGH HEART RATE*\n";
    msg += "BPM: " + String(heartRate) + " (3 consecutive)\n";
    msg += "Score: " + String(score) + "/100 — " + status + "\n";
    msg += "Trend: " + trend + "\n";
    msg += "Patient should calm down and rest.";
    msg += getLocationString();
    sendTelegramAlert(msg);
  }

  if (spo2 < 94) lowSpo2Count++; else lowSpo2Count = 0;
  if (lowSpo2Count >= 3)
  {
    lowSpo2Count = 0;
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("LOW OXYGEN!");
    lcd.setCursor(0, 1); lcd.print("Take Rest");
    for (int i = 0; i < 3; i++)
    { ledcWriteTone(BUZZER,2500); delay(200); ledcWriteTone(BUZZER,0); delay(200); }
    String msg = emoji + " *LOW OXYGEN*\n";
    msg += "SpO2: " + String(spo2) + "% (3 consecutive)\n";
    msg += "Score: " + String(score) + "/100 — " + status + "\n";
    msg += "Trend: " + trend + "\n";
    msg += "Patient should rest and breathe deeply.";
    msg += getLocationString();
    sendTelegramAlert(msg);
  }

  if (temperatureC > 38) highTempCount++; else highTempCount = 0;
  if (highTempCount >= 3)
  {
    highTempCount = 0;
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("HIGH TEMP!");
    lcd.setCursor(0, 1); lcd.print("Drink Water");
    for (int i = 0; i < 3; i++)
    { ledcWriteTone(BUZZER,2500); delay(200); ledcWriteTone(BUZZER,0); delay(200); }
    String msg = emoji + " *HIGH TEMPERATURE*\n";
    msg += "Temp: " + String(temperatureC, 1) + " C (3 consecutive)\n";
    msg += "Score: " + String(score) + "/100 — " + status + "\n";
    msg += "Patient should drink water and rest.";
    msg += getLocationString();
    sendTelegramAlert(msg);
  }

  if (trend == "Rising Heart Rate" && heartRate > 100 && heartRate <= 120)
  {
    String msg = "🟡 *PREDICTIVE ALERT*\n";
    msg += "BPM trending upward: " + String(heartRate) + "\n";
    msg += "May reach dangerous levels soon.\n";
    msg += "Score: " + String(score) + "/100";
    sendTelegramAlert(msg);
  }


  /******** SEND TO JSONBIN ********/

  sendToWebsite((int)heartRate, (int)spo2, temperatureC, false, false,
                score, status, trend);
}
