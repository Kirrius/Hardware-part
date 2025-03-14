#include <Wire.h>
#include <iarduino_I2C_Expander.h>
#include <iarduino_I2C_SHT.h> 

#define FLASH_I2C_ADDRESS 0x09
#define MIN 2270              
#define MAX 1989                 

iarduino_I2C_Expander gpio;
iarduino_I2C_SHT sht;

void setup() {
    Serial.begin(115200);
    Wire.begin(21, 22); 

    // Инициализация расширителя
    if (!gpio.begin(&Wire)) {
        Serial.println("Ошибка инициализации расширителя!");
    }

    // Инициализация датчика температуры и влажности
    if (!sht.begin(&Wire)) {
        Serial.println("Ошибка инициализации датчика температуры и влажности!");
    }
}

void loop() {
    Wire.beginTransmission(0x70);
    Wire.write(0x01);        
    Wire.endTransmission();

    int LUM_result = 4096 - gpio.analogRead(0); 
    Serial.print("Illumination = ");
    Serial.println(LUM_result); 

    int sensor = gpio.analogRead(1); 
    sensor = map(sensor, MIN, MAX, 0, 100); // Адаптируем значения от 0 до 100
    Serial.print("Текущая влажность почвы: ");
    Serial.print(sensor); // Выводим текущее значение влажности
    Serial.println("%");   // Выводим символ процента на той же строке

    // Пример активации канала на I2C Hub для температуры и влажности
    Wire.beginTransmission(0x70); // Адрес вашего I2C Hub
    Wire.write(0x03);              // Активируем третий канал для температуры и влажности
    Wire.endTransmission();

    // Чтение данных с датчика температуры и влажности
    Serial.print("Температура = ");
    Serial.print(sht.getTem());
    Serial.println(" °C");
    Serial.print("Влажность воздуха = ");
    Serial.print(sht.getHum());
    Serial.println(" %");

    delay(5000);
}