#include <WiFi.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <iarduino_I2C_Expander.h>
#include <iarduino_I2C_SHT.h>
#include <iarduino_I2C_Relay.h>
#include <ESPmDNS.h>

Preferences preferences;

const char* apName = "ESP32_AP";
const char* apPassword = "123456789";

WebServer server(80);
bool wifiConnectionFailed = false;
bool showReturnMessage = true;
String serverError = "";
bool isWiFiConnected = false;

// ========== НАСТРОЙКИ MQTT (ПУБЛИЧНЫЙ БРОКЕР) ==========
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

const String mqtt_topic_data = "plantcare/device/ESP32_PlantMonitor/data";
const String mqtt_topic_command = "plantcare/device/ESP32_PlantMonitor/command";
const String mqtt_topic_status = "plantcare/device/ESP32_PlantMonitor/status";

WiFiClient espClient;
PubSubClient mqttClient(espClient);
bool mqttConnected = false;

// Старые настройки HTTP (оставляем для обратной совместимости)
const char* serverHost = "plant-care.up.railway.app";
const String apiKey = "esp32_secret_key_123";
const String deviceId = "ESP32_PlantMonitor";

const unsigned long sendInterval = 15000;
unsigned long lastSendTime = 0;
volatile bool sendingInProgress = false;

// Суммы и счётчики для датчиков
// Датчики освещённости (каналы 0 и 1)
float sumLight1 = 0;
int light1ReadCount = 0;
float sumLight2 = 0;
int light2ReadCount = 0;
// Датчик влажности почвы (канал 2)
float sumSoil = 0;
int soilReadCount = 0;
// SHT датчик
float sumTemp = 0;
float sumHum = 0;
int shtReadCount = 0;

const unsigned long readInterval = 1000;
unsigned long lastReadTime = 0;

// Датчики
#define SHT_I2C_ADDRESS 0x11
#define EXPANDER_I2C_ADDR 0x12
#define SOIL_DRY 2270
#define SOIL_WET 1989
iarduino_I2C_Expander gpio(EXPANDER_I2C_ADDR);
iarduino_I2C_SHT sht(SHT_I2C_ADDRESS); 

// Насос
#define MODULE_ADDR   0x10
#define PUMP_CHANNEL  1
iarduino_I2C_Relay pump(MODULE_ADDR);
bool pumpState = false;

// Флаги наличия датчиков
bool shtConnected = false;
bool light1Connected = false;
bool light2Connected = false;
bool soilConnected = false;
bool expanderConnected = false; // Флаг состояния расширителя

// Для периодического переподключения SHT
unsigned long lastSHTReconnectAttempt = 0;
const unsigned long shtReconnectInterval = 10000; // 10 секунд

// Для периодической проверки расширителя
unsigned long lastExpanderCheck = 0;
const unsigned long expanderCheckInterval = 15000; // 15 секунд

// ========== ФУНКЦИИ MQTT ==========
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (int i = 0; i < length; i++) message += (char)payload[i];
    Serial.printf("MQTT команда получена [%s]: %s\n", topic, message.c_str());

    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, message);
    if (error) {
        Serial.println("Ошибка парсинга JSON команды");
        return;
    }
    String command = doc["command"] | "";

    if (command == "toggle_pump") {
        pumpState = !pumpState;
        pump.digitalWrite(PUMP_CHANNEL, pumpState ? HIGH : LOW);
        DynamicJsonDocument response(128);
        response["success"] = true;
        response["pump_state"] = pumpState;
        response["command_id"] = doc["command_id"] | "";
        String responseStr;
        serializeJson(response, responseStr);
        mqttClient.publish(mqtt_topic_status.c_str(), responseStr.c_str());
    } else if (command == "pump_on") {
        pump.digitalWrite(PUMP_CHANNEL, HIGH);
        pumpState = true;
        DynamicJsonDocument response(128);
        response["success"] = true;
        response["pump_state"] = true;
        response["command_id"] = doc["command_id"] | "";
        String responseStr;
        serializeJson(response, responseStr);
        mqttClient.publish(mqtt_topic_status.c_str(), responseStr.c_str());
    } else if (command == "pump_off") {
        pump.digitalWrite(PUMP_CHANNEL, LOW);
        pumpState = false;
        DynamicJsonDocument response(128);
        response["success"] = true;
        response["pump_state"] = false;
        response["command_id"] = doc["command_id"] | "";
        String responseStr;
        serializeJson(response, responseStr);
        mqttClient.publish(mqtt_topic_status.c_str(), responseStr.c_str());
    } else if (command == "get_status") {
        DynamicJsonDocument response(128);
        response["pump_state"] = pumpState;
        response["device_id"] = deviceId;
        String responseStr;
        serializeJson(response, responseStr);
        mqttClient.publish(mqtt_topic_status.c_str(), responseStr.c_str());
    }
}

void reconnectMQTT() {
    while (!mqttClient.connected() && WiFi.status() == WL_CONNECTED) {
        Serial.print("Подключение к MQTT брокеру...");
        String clientId = deviceId + "-" + String(random(0xffff), HEX);
        if (mqttClient.connect(clientId.c_str())) {
            Serial.println(" подключено!");
            mqttConnected = true;
            if (mqttClient.subscribe(mqtt_topic_command.c_str())) {
                Serial.println("Подписан на топик команд: " + mqtt_topic_command);
            } else {
                Serial.println("Ошибка подписки на топик команд");
            }
            DynamicJsonDocument onlineMsg(128);
            onlineMsg["status"] = "online";
            onlineMsg["timestamp"] = millis();
            String statusStr;
            serializeJson(onlineMsg, statusStr);
            mqttClient.publish(mqtt_topic_status.c_str(), statusStr.c_str());
        } else {
            Serial.print(" ошибка, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" повтор через 5 секунд");
            delay(5000);
        }
    }
}

void sendDataViaMQTT() {
    if (!mqttClient.connected()) {
        Serial.println("MQTT не подключен, данные не отправлены");
        return;
    }
    
    DynamicJsonDocument doc(512);
    doc["device_id"] = deviceId;
    doc["timestamp"] = millis();
    JsonObject sensors = doc.createNestedObject("sensors");
    
    // Отправляем только те датчики, которые реально подключены
    if (light1Connected && light1ReadCount > 0) {
        sensors["light_1"] = sumLight1 / light1ReadCount;
    }
    if (light2Connected && light2ReadCount > 0) {
        sensors["light_2"] = sumLight2 / light2ReadCount;
    }
    if (soilConnected && soilReadCount > 0) {
        sensors["soil"] = sumSoil / soilReadCount;
    }
    if (shtConnected && shtReadCount > 0) {
        sensors["temp"] = sumTemp / shtReadCount;
        sensors["humidity"] = sumHum / shtReadCount;
    }
    sensors["pump"] = pumpState;
    
    String jsonData;
    serializeJson(doc, jsonData);
    
    if (mqttClient.publish(mqtt_topic_data.c_str(), jsonData.c_str())) {
        Serial.println("Данные отправлены через MQTT");
        if (light1Connected) Serial.printf("  Свет1: %.1f лк", sumLight1 / light1ReadCount);
        if (light2Connected) Serial.printf(", Свет2: %.1f лк", sumLight2 / light2ReadCount);
        if (soilConnected) Serial.printf(", Почва: %.0f%%", sumSoil / soilReadCount);
        if (shtConnected) Serial.printf(", Темп: %.1f C, Влаж: %.0f%%", sumTemp / shtReadCount, sumHum / shtReadCount);
        Serial.printf(", Насос: %s\n", pumpState ? "ON" : "OFF");
    } else {
        Serial.println("Ошибка отправки данных через MQTT");
    }
    
    // Сброс после отправки
    sumLight1 = 0; light1ReadCount = 0;
    sumLight2 = 0; light2ReadCount = 0;
    sumSoil = 0; soilReadCount = 0;
    sumTemp = 0; sumHum = 0; shtReadCount = 0;
}

void sendAveragedDataToServer() {
    if (light1ReadCount == 0 && light2ReadCount == 0 && soilReadCount == 0 && shtReadCount == 0) return;
    
    if (mqttConnected && mqttClient.connected()) {
        sendDataViaMQTT();
    } else {
        Serial.println("MQTT недоступен, используем HTTP fallback");
    }
}

// ========== WiFi, ВЕБ-СЕРВЕР ==========
bool connectToWiFi(const char* ssid, const char* password, bool silent = false) {
    if (!silent) Serial.println("Подключение к WiFi...");
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid, password);
    int attempts = 0;
    while (attempts < 10 && WiFi.status() != WL_CONNECTED) {
        delay(1000);
        if (!silent) Serial.print(".");
        attempts++;
        server.handleClient();
    }
    if (WiFi.status() == WL_CONNECTED) {
        if (!silent) {
            Serial.println("\nУспешное подключение!");
            Serial.print("IP адрес STA: "); Serial.println(WiFi.localIP());
            Serial.print("IP адрес AP: "); Serial.println(WiFi.softAPIP());
            if (!MDNS.begin("esp32")) Serial.println("Error setting up MDNS responder!");
            else Serial.println("mDNS responder started. Access via esp32.local");
        }
        saveLocalIP();
        isWiFiConnected = true;
        return true;
    } else {
        if (!silent) Serial.println("\nОшибка подключения к WiFi");
        wifiConnectionFailed = true;
        showReturnMessage = true;
        return false;
    }
}

void saveLocalIP() {
    String currentIP = WiFi.localIP().toString();
    String savedIP = preferences.getString("LOCAL_IP", "");
    if (currentIP != savedIP) {
        preferences.putString("LOCAL_IP", currentIP);
        Serial.println("Сохранен новый локальный IP: " + currentIP);
    }
}

// Функция проверки и переподключения расширителя
void checkExpander() {
    if (!expanderConnected) {
        unsigned long now = millis();
        if (now - lastExpanderCheck >= expanderCheckInterval) {
            lastExpanderCheck = now;
            Serial.println("Попытка переподключения расширителя gpio...");
            if (gpio.begin(&Wire)) {
                expanderConnected = true;
                Serial.println("Расширитель gpio переподключен!");
                // Сбрасываем флаги датчиков, чтобы они переопределились
                light1Connected = false;
                light2Connected = false;
                soilConnected = false;
            } else {
                Serial.println("Не удалось переподключить расширитель gpio");
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    WiFi.persistent(false);
    preferences.begin("WiFiCreds", false);

    Wire.begin(21, 22);
    
    // Сканирование I2C шины для отладки
    Serial.println("Сканирование I2C шины...");
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("Найден I2C адрес: 0x%02X (%d)\n", addr, addr);
        }
    }
    Serial.println("Сканирование завершено");
    
    // Инициализация расширителя gpio
    expanderConnected = gpio.begin(&Wire);
    if (!expanderConnected) {
        Serial.println("Ошибка инициализации расширителя gpio!");
    } else {
        Serial.println("Расширитель gpio инициализирован");
        // Проверяем чтение с аналоговых каналов
        int testLight1 = gpio.analogRead(0);
        int testLight2 = gpio.analogRead(1);
        int testSoil = gpio.analogRead(2);
        Serial.printf("Тестовое чтение канала 0 (свет1): %d\n", testLight1);
        Serial.printf("Тестовое чтение канала 1 (свет2): %d\n", testLight2);
        Serial.printf("Тестовое чтение канала 2 (почва): %d\n", testSoil);
        
        // Если все значения слишком низкие, возможно датчики не подключены
        if (testLight1 < 100 && testLight2 < 100 && testSoil < 100) {
            Serial.println("Внимание: на всех аналоговых каналах низкие значения. Проверьте подключение датчиков!");
        }
    }
    
    // Инициализация SHT
    shtConnected = sht.begin(&Wire);
    if (!shtConnected) {
        Serial.println("SHT датчик не обнаружен!");
    } else {
        Serial.println("SHT датчик инициализирован");
    }
    lastSHTReconnectAttempt = millis();
    lastExpanderCheck = millis();

    pump.begin();
    pump.digitalWrite(PUMP_CHANNEL, LOW);
    pumpState = false;

    String ssid = preferences.getString("SSID", "");
    String password = preferences.getString("PASSWORD", "");
    String email = preferences.getString("EMAIL", "");

    if (ssid.isEmpty() || password.isEmpty() || email.isEmpty()) {
        Serial.println("Не хватает настроек, создаем точку доступа...");
        createAccessPoint();
    } else {
        if (!connectToWiFi(ssid.c_str(), password.c_str())) {
            preferences.remove("SSID");
            preferences.remove("PASSWORD");
            preferences.remove("EMAIL");
            Serial.println("Сохранённые данные WiFi удалены из памяти");
            createAccessPoint();
        }
    }

    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(60);

    server.on("/togglePump", HTTP_POST, []() {
        pumpState = !pumpState;
        pump.digitalWrite(PUMP_CHANNEL, pumpState ? HIGH : LOW);
        server.send(200, "application/json", "{\"pump\":" + String(pumpState ? "true" : "false") + "}");
    });
    server.on("/pumpStatus", HTTP_GET, []() {
        server.send(200, "application/json", "{\"pump\":" + String(pumpState ? "true" : "false") + "}");
    });
    server.on("/pumpOn", HTTP_POST, []() {
        pump.digitalWrite(PUMP_CHANNEL, HIGH);
        pumpState = true;
        server.send(200, "application/json", "{\"pump\":true}");
    });
    server.on("/pumpOff", HTTP_POST, []() {
        pump.digitalWrite(PUMP_CHANNEL, LOW);
        pumpState = false;
        server.send(200, "application/json", "{\"pump\":false}");
    });
    server.on("/reset", HTTP_POST, []() {
        Serial.println("Получен запрос на сброс настроек WiFi");
        preferences.begin("WiFiCreds", false);
        preferences.clear();
        preferences.end();
        server.send(200, "text/html", "<html><body><h1>Настройки сброшены. Перезагрузка...</h1></body></html>");
        delay(3000);
        ESP.restart();
    });
    
    startServer();
    Serial.println("HTTP сервер запущен");
    
    lastReadTime = millis();
}

void loop() {
    server.handleClient();
    if (WiFi.status() == WL_CONNECTED) {
        if (!mqttClient.connected()) reconnectMQTT();
        else mqttClient.loop();
    }
    
    // Периодическая проверка расширителя
    checkExpander();
    
    if (millis() - lastReadTime >= readInterval) {
        lastReadTime = millis();
        readAndAccumulateSensorData();
    }
    if (millis() - lastSendTime >= sendInterval) {
        lastSendTime = millis();
        if (WiFi.status() == WL_CONNECTED && !sendingInProgress) {
            sendingInProgress = true;
            sendAveragedDataToServer();
            sendingInProgress = false;
        }
    }
    static unsigned long lastCheckTime = 0;
    if (millis() - lastCheckTime >= 10000) {
        lastCheckTime = millis();
        if (WiFi.status() == WL_CONNECTED && !isWiFiConnected) {
            isWiFiConnected = true;
            WiFi.softAPdisconnect(true);
            saveLocalIP();
            readAndPrintSensorData();
        }
        if (isWiFiConnected) readAndPrintSensorData();
        if (WiFi.status() == WL_CONNECTED) Serial.printf("MQTT статус: %s\n", mqttClient.connected() ? "подключен" : "отключен");
    }
}

void readAndAccumulateSensorData() {
    // Если расширитель отключён, не пытаемся читать
    if (!expanderConnected) {
        light1Connected = false;
        light2Connected = false;
        soilConnected = false;
        return;
    }
    
    int rawLight1 = gpio.analogRead(0);  // Датчик освещённости 1
    int rawLight2 = gpio.analogRead(1);  // Датчик освещённости 2
    int rawSoil = gpio.analogRead(2);    // Датчик влажности почвы
    
    // Обработка освещённости 1
    if (rawLight1 > 100 && rawLight1 < 4000) {
        float lux = convertToLux(rawLight1);
        if (lux >= 0 && lux < 9000) {
            sumLight1 += lux;
            light1ReadCount++;
            light1Connected = true;
        } else {
            light1Connected = false;
        }
    } else {
        light1Connected = false;
    }
    
    // Обработка освещённости 2
    if (rawLight2 > 100 && rawLight2 < 4000) {
        float lux = convertToLux(rawLight2);
        if (lux >= 0 && lux < 9000) {
            sumLight2 += lux;
            light2ReadCount++;
            light2Connected = true;
        } else {
            light2Connected = false;
        }
    } else {
        light2Connected = false;
    }
    
    // Обработка влажности почвы
    if (rawSoil > 1500 && rawSoil < 2800) {
        int soilMoisture = map(rawSoil, SOIL_DRY, SOIL_WET, 0, 100);
        if (soilMoisture >= 0 && soilMoisture <= 100) {
            sumSoil += soilMoisture;
            soilReadCount++;
            soilConnected = true;
        } else {
            soilConnected = false;
        }
    } else {
        soilConnected = false;
    }
    
    // Если все каналы показывают очень низкие значения, возможно расширитель отключился
    if (rawLight1 < 50 && rawLight2 < 50 && rawSoil < 50 && expanderConnected) {
        Serial.println("Подозрение на отключение расширителя gpio (все каналы показывают 0)");
        expanderConnected = false;
    }
    
    // Обработка SHT с возможностью переподключения
    if (shtConnected) {
        float temp = sht.getTem();
        float hum = sht.getHum();
        if (!isnan(temp) && !isnan(hum) && temp > -40 && temp < 85 && hum > 0 && hum <= 100) {
            sumTemp += temp;
            sumHum += hum;
            shtReadCount++;
        } else {
            shtConnected = false;
        }
    } else {
        unsigned long now = millis();
        if (now - lastSHTReconnectAttempt >= shtReconnectInterval) {
            lastSHTReconnectAttempt = now;
            if (sht.begin(&Wire)) {
                shtConnected = true;
                Serial.println("SHT датчик переподключен!");
            }
        }
    }
}

float convertToLux(int rawValue) {
    if (rawValue < 100 || rawValue > 4000) return -1;
    int directValue = 4096 - rawValue;
    if (directValue < 0) directValue = 0;
    if (directValue > 4095) directValue = 4095;
    if (directValue == 0) return 0;
    float lux = 500.0 * pow((4096.0 / (4096 - directValue)), 2);
    if (lux < 0) lux = 0;
    if (lux > 10000) lux = 10000;
    return lux;
}

void readAndPrintSensorData() {
    if (!expanderConnected) {
        Serial.println("\n--- Показания датчиков ---");
        Serial.println("Расширитель gpio не подключен!");
        Serial.println("Освещённость 1: --- (датчик недоступен)");
        Serial.println("Освещённость 2: --- (датчик недоступен)");
        Serial.println("Влажность почвы: --- (датчик недоступен)");
    } else {
        int rawLight1 = gpio.analogRead(0);
        int rawLight2 = gpio.analogRead(1);
        int rawSoil = gpio.analogRead(2);
        
        Serial.println("\n--- Показания датчиков ---");
        
        // Датчик освещённости 1
        if (rawLight1 > 100 && rawLight1 < 4000) {
            float lux = convertToLux(rawLight1);
            Serial.print("Освещённость 1: "); Serial.print(lux, 1); Serial.println("лк");
            light1Connected = true;
        } else {
            Serial.println("Освещённость 1: --- (датчик отключён)");
            light1Connected = false;
        }
        
        // Датчик освещённости 2
        if (rawLight2 > 100 && rawLight2 < 4000) {
            float lux = convertToLux(rawLight2);
            Serial.print("Освещённость 2: "); Serial.print(lux, 1); Serial.println("лк");
            light2Connected = true;
        } else {
            Serial.println("Освещённость 2: --- (датчик отключён)");
            light2Connected = false;
        }
        
        // Влажность почвы
        if (rawSoil > 1500 && rawSoil < 2800) {
            int soilMoisture = map(rawSoil, SOIL_DRY, SOIL_WET, 0, 100);
            if (soilMoisture < 0) soilMoisture = 0;
            if (soilMoisture > 100) soilMoisture = 100;
            Serial.print("Влажность почвы: "); Serial.print(soilMoisture); Serial.println("%");
            soilConnected = true;
        } else {
            Serial.println("Влажность почвы: --- (датчик отключён)");
            soilConnected = false;
        }
    }
    
    // SHT датчик
    if (shtConnected) {
        float temp = sht.getTem();
        float hum = sht.getHum();
        if (!isnan(temp) && !isnan(hum) && temp > -40 && temp < 85 && hum > 0 && hum <= 100) {
            Serial.print("Температура воздуха: "); Serial.print(temp); Serial.println(" °C");
            Serial.print("Влажность воздуха: "); Serial.print(hum); Serial.println("%");
        } else {
            Serial.println("Температура воздуха: --- (ошибка чтения)");
            Serial.println("Влажность воздуха: --- (ошибка чтения)");
            shtConnected = false;
        }
    } else {
        Serial.println("Температура воздуха: --- (датчик отключён)");
        Serial.println("Влажность воздуха: --- (датчик отключён)");
    }
    
    Serial.print("Насос: "); Serial.println(pumpState ? "ВКЛ" : "ВЫКЛ");
    Serial.println("--------------------------");
}

void createAccessPoint() {
    WiFi.mode(WIFI_AP_STA);
    delay(100);
    if (!WiFi.softAP(apName, apPassword)) {
        Serial.println("Ошибка создания точки доступа!");
        delay(1000);
        ESP.restart();
    }
    Serial.print("Точка доступа создана: "); Serial.println(apName);
    Serial.print("IP адрес: "); Serial.println(WiFi.softAPIP());
    startServer();
}

void startServer() {
    server.on("/", HTTP_GET, []() {
        String html = R"=====( 
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Настройка контроллера</title>
    <style>
        * { box-sizing: border-box; font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; }
        body { background-color: #f5f5f5; margin: 0; padding: 20px; display: flex; justify-content: center; align-items: center; min-height: 100vh; }
        .container { background-color: white; border-radius: 10px; box-shadow: 0 4px 12px rgba(0,0,0,0.1); padding: 30px; width: 100%; max-width: 500px; }
        h1 { color: #2c3e50; text-align: center; margin-bottom: 25px; }
        .error { background-color: #ffebee; color: #c62828; padding: 15px; border-radius: 5px; margin-bottom: 20px; border-left: 4px solid #c62828; }
        form { display: flex; flex-direction: column; gap: 15px; }
        input { padding: 12px 15px; border: 1px solid #ddd; border-radius: 5px; font-size: 16px; }
        input[type="submit"] { background-color: #3498db; color: white; border: none; cursor: pointer; font-weight: bold; padding: 14px; }
    </style>
</head>
<body>
<div class="container">
    <h1>Настройка контроллера</h1>
)=====";
        if (wifiConnectionFailed || !serverError.isEmpty()) html += "<meta http-equiv='refresh' content='10;url=/'>";
        if (!serverError.isEmpty()) {
            html += "<div class='error'>Ошибка сервера: " + serverError + "</div>";
            serverError = "";
        } else if (wifiConnectionFailed && showReturnMessage) {
            html += "<div class='error'>Ошибка подключения к WiFi! Проверьте SSID и пароль.</div>";
            wifiConnectionFailed = false;
            showReturnMessage = false;
        }
        html += R"=====( 
    <form action="/save" method="POST">
        <label>WiFi SSID:</label>
        <input type="text" name="ssid" required>
        <label>Пароль WiFi:</label>
        <input type="password" name="password" required>
        <label>Email:</label>
        <input type="email" name="email" required>
        <input type="submit" value="Сохранить настройки">
    </form>
</div>
</body>
</html>
)=====";
        server.send(200, "text/html", html);
    });

    server.on("/save", HTTP_POST, []() {
        String ssid = server.arg("ssid");
        String password = server.arg("password");
        String email = server.arg("email");
        if (email.indexOf('@') == -1) {
            serverError = "Неверный email";
            server.sendHeader("Location", "/");
            server.send(302);
            return;
        }
        preferences.putString("SSID", ssid);
        preferences.putString("PASSWORD", password);
        preferences.putString("EMAIL", email);
        String response = R"=====( 
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta http-equiv="refresh" content="10;url=/">
    <title>Сохранено</title>
</head>
<body>
    <h1>Настройки сохранены</h1>
    <p>SSID: )=====" + ssid + R"=====( </p>
    <p>Email: )=====" + email + R"=====( </p>
</body>
</html>
)=====";
        server.send(200, "text/html", response);
        if (!connectToWiFi(ssid.c_str(), password.c_str(), true)) {
            preferences.remove("SSID");
            preferences.remove("PASSWORD");
            preferences.remove("EMAIL");
            wifiConnectionFailed = true;
            showReturnMessage = true;
        } else {
            delay(3000);
            ESP.restart();
        }
    });
    server.begin();
    Serial.println("HTTP сервер запущен");
}