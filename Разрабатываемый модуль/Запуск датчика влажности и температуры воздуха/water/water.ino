#define  SENSOR      15               

void setup(){
  Serial.begin(115200);                    
}
void loop(){
  int sensor = analogRead(SENSOR);    
  Serial.println(sensor);                  
  delay(5000);                             
}