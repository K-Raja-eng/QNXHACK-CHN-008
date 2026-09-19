#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

// DHT11
#define DHT11_PIN 6
DHT dht11(DHT11_PIN, DHT11);

// Analog Pins
const byte IR_PIN   = A0;   // Replaced LDR
const byte GAS_PIN  = A1;
const byte MIC1_PIN = A2;
const byte MIC2_PIN = A3;

// Output Pins
const byte SOUND_LED = 4;
const byte IR_LED    = 5;

// Thresholds
const int MIC_THRESHOLD = 40;
const int IR_THRESHOLD  = 500;   // Adjust if needed

unsigned long lastScreen = 0;
byte screen = 0;

// Keep the last valid DHT reading so "nan" never reaches the QNX JSON path.
float lastTemp = 28.0;
float lastHum  = 60.0;

void setup() {
  pinMode(SOUND_LED, OUTPUT);
  pinMode(IR_LED, OUTPUT);

  lcd.init();
  lcd.backlight();

  dht11.begin();

  Serial.begin(9600);

  lcd.setCursor(0,0);
  lcd.print("City Twin Node");
  delay(1500);
  lcd.clear();
}

void loop() {

  int mic1 = analogRead(MIC1_PIN);
  int mic2 = analogRead(MIC2_PIN);
  int gas  = analogRead(GAS_PIN);
  int ir   = analogRead(IR_PIN);

  float temp = dht11.readTemperature();
  float hum  = dht11.readHumidity();

  if (!isnan(temp)) lastTemp = temp;
  if (!isnan(hum))  lastHum  = hum;

  // Always send finite values to the laptop/QNX bridge.
  temp = lastTemp;
  hum  = lastHum;

  // -------- Microphones --------
  if (mic1 > MIC_THRESHOLD && mic2 > MIC_THRESHOLD)
    digitalWrite(SOUND_LED, HIGH);
  else
    digitalWrite(SOUND_LED, LOW);

  // -------- IR Sensor --------
  if (ir < IR_THRESHOLD)      // Object detected
    digitalWrite(IR_LED, HIGH);
  else
    digitalWrite(IR_LED, LOW);

  // -------- LCD Screen Change --------
  if (millis() - lastScreen > 3000) {
    lastScreen = millis();
    screen = (screen + 1) % 3;
    lcd.clear();
  }

  switch(screen){

    case 0:
      lcd.setCursor(0,0);
      lcd.print("DHT11");
      lcd.setCursor(0,1);
      if(!isnan(temp)){
        lcd.print("T:");
        lcd.print(temp,1);
        lcd.print(" H:");
        lcd.print(hum,0);
      }else{
        lcd.print("Read Error");
      }
      break;

    case 1:
      lcd.setCursor(0,0);
      lcd.print("Gas:");
      lcd.print(gas);

      lcd.setCursor(0,1);

      if(gas < 200)
        lcd.print("Air Normal     ");
      else if(gas < 400)
        lcd.print("Smoke Detect   ");
      else if(gas < 600)
        lcd.print("LPG Possible   ");
      else if(gas < 800)
        lcd.print("Methane High   ");
      else
        lcd.print("Gas Leak Alert ");
      break;

    case 2:
      lcd.setCursor(0,0);
      lcd.print("M1:");
      lcd.print(mic1);

      lcd.setCursor(9,0);
      lcd.print("M2:");
      lcd.print(mic2);

      lcd.setCursor(0,1);
      if(ir < IR_THRESHOLD)
        lcd.print("Object Detect  ");
      else
        lcd.print("No Object      ");
      break;
  }

  // -------- Serial Monitor --------
  Serial.print("Mic1:");
  Serial.print(mic1);
  Serial.print(" Mic2:");
  Serial.print(mic2);
  Serial.print(" Gas:");
  Serial.print(gas);
  Serial.print(" IR:");
  Serial.print(ir);
  Serial.print(" Temp:");
  Serial.print(temp);
  Serial.print(" Hum:");
  Serial.println(hum);

  delay(100);
}