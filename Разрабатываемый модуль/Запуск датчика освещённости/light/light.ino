#include <Arduino.h>

int LUM_result;     
int PIN_illumination = 32;                     

void setup() {
  Serial.begin(115200);                              
}

void loop() {
  LUM_result = 4095 - analogRead(PIN_illumination); 
  Serial.print("Illumination = ");              
  Serial.println(LUM_result);                 
  delay(1000);  
}