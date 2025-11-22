#include <Wire.h>
#include <iarduino_I2C_Relay.h>

#define SDA_PIN       21
#define SCL_PIN       22

#define I2C_HUB_ADDR  0x09   // адрес i2c Hub (информативно)
#define MODULE_ADDR   0x10   // адрес силового ключа
#define PUMP_CHANNEL  1      // канал K1

iarduino_I2C_Relay pump(MODULE_ADDR);

void setup() {
  Serial.begin(115200);
  delay(200);

  Wire.begin(SDA_PIN, SCL_PIN);
  delay(50);

  pump.begin();  // инициализация модуля

  Serial.println("Type 1 = ON, 0 = OFF");
  Serial.print("i2C Hub address: 0x");
  Serial.println(I2C_HUB_ADDR, HEX);
  Serial.print("Module address: 0x");
  Serial.println(MODULE_ADDR, HEX);
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();

    if (c == '1') {
      pump.digitalWrite(PUMP_CHANNEL, HIGH);   // включить K1
      Serial.println("K1 ON");
    }

    if (c == '0') {
      pump.digitalWrite(PUMP_CHANNEL, LOW);    // выключить K1
      Serial.println("K1 OFF");
    }
  }
}