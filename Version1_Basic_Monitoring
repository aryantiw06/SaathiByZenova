#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"

MAX30105 particleSensor;

#define MAX_BRIGHTNESS 255

uint32_t irBuffer[100];
uint32_t redBuffer[100];

int32_t bufferLength;
int32_t spo2;
int8_t validSPO2;

int32_t heartRate;
int8_t validHeartRate;

void setup()
{
  Serial.begin(115200);

  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD))
  {
    Serial.println("MAX30102 not found");

    while (1);
  }

  Serial.println("Place finger on sensor");

  particleSensor.setup();

  particleSensor.setPulseAmplitudeRed(0x1F);
  particleSensor.setPulseAmplitudeIR(0x1F);
}

void loop()
{
  bufferLength = 100;

  /******** COLLECT 100 SAMPLES ********/

  for (byte i = 0; i < bufferLength; i++)
  {
    while (particleSensor.available() == false)
      particleSensor.check();

    redBuffer[i] = particleSensor.getRed();
    irBuffer[i] = particleSensor.getIR();

    particleSensor.nextSample();
  }

  /******** CALCULATE REAL SPO2 + BPM ********/

  maxim_heart_rate_and_oxygen_saturation(
      irBuffer,
      bufferLength,
      redBuffer,
      &spo2,
      &validSPO2,
      &heartRate,
      &validHeartRate);

  /******** NO FINGER ********/

  if (irBuffer[99] < 50000)
  {
    Serial.println("Place Finger");

    delay(1000);

    return;
  }

  /******** DISPLAY ********/

  if (validHeartRate && validSPO2)
  {
    Serial.print("BPM: ");
    Serial.print(heartRate);

    Serial.print("   SpO2: ");
    Serial.print(spo2);

    Serial.println("%");
  }
  else
  {
    Serial.println("Calculating...");
  }

  delay(1000);
}
