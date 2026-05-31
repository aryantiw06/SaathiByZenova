#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <TinyGPS++.h>

/******** LCD ********/

LiquidCrystal_I2C lcd(0x27, 16, 2);

/******** MAX30102 ********/

MAX30105 particleSensor;

uint32_t irBuffer[100];
uint32_t redBuffer[100];

int32_t bufferLength;

int32_t spo2;
int8_t validSPO2;

int32_t heartRate;
int8_t validHeartRate;

/******** BUZZER ********/

#define BUZZER 25

/******** LM35 ********/

#define LM35 34

/******** SOS BUTTON ********/

#define BUTTON 14

TinyGPSPlus gps;

HardwareSerial gpsSerial(2);


/**************************************************/

void setup()
{
  Serial.begin(115200);

  /******** BUZZER PWM ********/

  ledcAttach(BUZZER, 2000, 8);

  /******** SOS BUTTON ********/

  pinMode(BUTTON, INPUT_PULLUP);

  /******** I2C START ********/

  Wire.begin(21, 22);

  /******** LCD START ********/

  lcd.init();
  lcd.backlight();

  lcd.setCursor(0,0);
  lcd.print("SAATHI");

  lcd.setCursor(0,1);
  lcd.print("Initializing");

gpsSerial.begin(9600, SERIAL_8N1, 16, 17);

Serial.println("GPS Initializing...");


  /******** SENSOR START ********/

  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD))
  {
    lcd.clear();

    lcd.setCursor(0,0);
    lcd.print("MAX30102 Error");

    Serial.println("MAX30102 not found");

    while (1);
  }

  particleSensor.setup();

  particleSensor.setPulseAmplitudeRed(0x1F);
  particleSensor.setPulseAmplitudeIR(0x1F);

  delay(2000);

  lcd.clear();

  lcd.setCursor(0,0);
  lcd.print("Place Finger");
}

/**************************************************/

void loop()
{

  while (gpsSerial.available() > 0)
{
    gps.encode(gpsSerial.read());
}

  /******** SOS BUTTON ********/

  if (digitalRead(BUTTON) == LOW)
{
    Serial.println("SOS ALERT ACTIVATED");

    lcd.clear();

    lcd.setCursor(0,0);
    lcd.print("EMERGENCY!");

    lcd.setCursor(0,1);
    lcd.print("HELP NEEDED");

    /******** BUZZER ********/

    for(int i = 0; i < 5; i++)
    {
        ledcWriteTone(BUZZER, 2500);

        delay(200);

        ledcWriteTone(BUZZER, 0);

        delay(200);
    }

    /******** GPS LOCATION ********/

    Serial.println("Caretaker Alert Sent");

    if (gps.location.isValid())
    {
        Serial.print("Latitude: ");

        Serial.println(gps.location.lat(), 6);

        Serial.print("Longitude: ");

        Serial.println(gps.location.lng(), 6);

        Serial.print("Google Maps Link: ");

        Serial.print("https://maps.google.com/?q=");

        Serial.print(gps.location.lat(), 6);

        Serial.print(",");

        Serial.println(gps.location.lng(), 6);

        /******** LCD GPS SCREEN ********/

        lcd.clear();

        lcd.setCursor(0,0);
        lcd.print("Location Sent");

        lcd.setCursor(0,1);
        lcd.print("To Caretaker");
    }
    else
    {
        Serial.println("GPS NOT AVAILABLE");

        lcd.clear();

        lcd.setCursor(0,0);
        lcd.print("GPS Not Found");

        lcd.setCursor(0,1);
        lcd.print("Move Outside");
    }

    delay(3000);

    return;
}



  /******** TEMPERATURE ********/

  int sensorValue = analogRead(LM35);

  float voltage = sensorValue * (3.3 / 4095.0);

  float temperatureC = voltage * 100;

  /******** COLLECT SAMPLES ********/

  bufferLength = 100;

  for (byte i = 0; i < bufferLength; i++)
  {
    while (particleSensor.available() == false)
    {
      particleSensor.check();
    }

    redBuffer[i] = particleSensor.getRed();

    irBuffer[i] = particleSensor.getIR();

    particleSensor.nextSample();
  }

  /******** NO FINGER ********/

  if (irBuffer[99] < 50000)
  {
    lcd.clear();

    lcd.setCursor(0,0);
    lcd.print("Place Finger");

    Serial.println("Place Finger");

    ledcWriteTone(BUZZER, 0);

    delay(500);

    return;
  }

  /******** CALCULATE BPM + SPO2 ********/

  maxim_heart_rate_and_oxygen_saturation(
      irBuffer,
      bufferLength,
      redBuffer,
      &spo2,
      &validSPO2,
      &heartRate,
      &validHeartRate);

  /******** INVALID VALUES ********/

  if (!validHeartRate || !validSPO2)
  {
    lcd.clear();

    lcd.setCursor(0,0);
    lcd.print("Calculating...");

    Serial.println("Calculating...");

    delay(1000);

    return;
  }

  /******** SERIAL OUTPUT ********/

  Serial.print("BPM: ");
  Serial.print(heartRate);

  Serial.print("   SpO2: ");
  Serial.print(spo2);

  Serial.print("%");

  Serial.print("   Temp: ");
  Serial.print(temperatureC);

  Serial.println(" C");

  /******** SCREEN 1 ********/

lcd.clear();

lcd.setCursor(0,0);
lcd.print("BPM: ");
lcd.print(heartRate);

lcd.setCursor(0,1);
lcd.print("SpO2: ");
lcd.print(spo2);
lcd.print("%");

delay(2000);

/******** SCREEN 2 ********/

lcd.clear();

lcd.setCursor(0,0);
lcd.print("Temp: ");
lcd.print(temperatureC);
lcd.print(" C");

lcd.setCursor(0,1);
lcd.print("Saathi Active");

delay(2000);


  /******** HIGH BPM ALERT ********/

  if (heartRate > 120)
  {
    Serial.println("HIGH BPM ALERT");

    lcd.clear();

    lcd.setCursor(0,0);
    lcd.print("HIGH BPM");

    lcd.setCursor(0,1);
    lcd.print("CALM DOWN");

    for(int i = 0; i < 3; i++)
    {
      ledcWriteTone(BUZZER, 2500);

      delay(200);

      ledcWriteTone(BUZZER, 0);

      delay(200);
    }
  }

  /******** LOW OXYGEN ALERT ********/

  if (spo2 < 94)
  {
    Serial.println("LOW OXYGEN ALERT");

    lcd.clear();

    lcd.setCursor(0,0);
    lcd.print("LOW OXYGEN");

    lcd.setCursor(0,1);
    lcd.print("TAKE REST");

    for(int i = 0; i < 3; i++)
    {
      ledcWriteTone(BUZZER, 2500);

      delay(200);

      ledcWriteTone(BUZZER, 0);

      delay(200);
    }
  }

  /******** HIGH TEMPERATURE ALERT ********/

  if (temperatureC > 38)
  {
    Serial.println("HIGH TEMPERATURE ALERT");

    lcd.clear();

    lcd.setCursor(0,0);
    lcd.print("HIGH TEMP");

    lcd.setCursor(0,1);
    lcd.print("DRINK WATER");

    for(int i = 0; i < 3; i++)
    {
      ledcWriteTone(BUZZER, 2500);

      delay(200);

      ledcWriteTone(BUZZER, 0);

      delay(200);
    }
  }

  delay(1000);
}
 
