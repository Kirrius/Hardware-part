#include <WiFi.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
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


const char* serverHost = "192.168.1.107";   
const int serverPort = 5000;                 // порт Flask
const String apiKey = "esp32_secret_key_123"; // должен совпадать с ESP32_API_KEY на сервере
const String deviceId = "ESP32_PlantMonitor"; // идентификатор этого устройства

const unsigned long sendInterval = 60000; // отправлять раз в минуту
unsigned long lastSendTime = 0;
volatile bool sendingInProgress = false;

float sumLight = 0;
float sumSoil = 0;
float sumTemp = 0;
float sumHum = 0;
int readCount = 0;               // количество выполненных измерений за текущую минуту
const unsigned long readInterval = 1000; // читаем датчики каждую секунду
unsigned long lastReadTime = 0;

// Датчики
#define SHT_I2C_ADDRESS 0x11
#define FLASH_I2C_ADDRESS 0x09
#define MIN 2270
#define MAX 1989
iarduino_I2C_Expander gpio(FLASH_I2C_ADDRESS);
iarduino_I2C_SHT sht(SHT_I2C_ADDRESS); 

// Насос
#define MODULE_ADDR   0x10
#define PUMP_CHANNEL  1
iarduino_I2C_Relay pump(MODULE_ADDR);
bool pumpState = false;

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
            Serial.print("IP адрес STA: "); 
            Serial.println(WiFi.localIP());
            Serial.print("IP адрес AP: ");
            Serial.println(WiFi.softAPIP());
            if (!MDNS.begin("esp32")) {
                Serial.println("Error setting up MDNS responder!");
            } else {
                Serial.println("mDNS responder started. Access via esp32.local");
            }
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

void setup() {
    Serial.begin(115200);
    WiFi.persistent(false);
    preferences.begin("WiFiCreds", false);

    Wire.begin(21, 22);
    if (!gpio.begin(&Wire)) {
        Serial.println("Ошибка инициализации расширителя!");
    }
    if (!sht.begin(&Wire)) {
        Serial.println("Ошибка инициализации датчика температуры и влажности!");
    }

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

    // ------------------- ЭНДПОИНТЫ ДЛЯ УПРАВЛЕНИЯ ОТ СЕРВЕРА -------------------
    server.on("/togglePump", HTTP_POST, []() {
        pumpState = !pumpState;
        pump.digitalWrite(PUMP_CHANNEL, pumpState ? HIGH : LOW);
        Serial.printf("Pump toggled, new state: %s\n", pumpState ? "ON" : "OFF");
        String res = "{\"pump\":";
        res += (pumpState ? "true" : "false");
        res += "}";
        server.send(200, "application/json", res);
    });

    server.on("/pumpStatus", HTTP_GET, []() {
        String res = "{\"pump\":";
        res += (pumpState ? "true" : "false");
        res += "}";
        server.send(200, "application/json", res);
    });

server.on("/pumpOn", HTTP_POST, []() {
    pump.digitalWrite(PUMP_CHANNEL, HIGH);
    pumpState = true;
    Serial.println("Pump turned ON");
    String res = "{\"pump\":true}";
    server.send(200, "application/json", res);
});

server.on("/pumpOff", HTTP_POST, []() {
    pump.digitalWrite(PUMP_CHANNEL, LOW);
    pumpState = false;
    Serial.println("Pump turned OFF");
    String res = "{\"pump\":false}";
    server.send(200, "application/json", res);
});

    // -------------------------------------------------------------------------
    
    startServer(); // запускаем веб-сервер (включая страницу настройки)
    Serial.println("HTTP сервер запущен");
    
    // ИНИЦИАЛИЗАЦИЯ НАКОПИТЕЛЕЙ
    sumLight = 0;
    sumSoil = 0;
    sumTemp = 0;
    sumHum = 0;
    readCount = 0;
    lastReadTime = millis();
}

void loop() {
    server.handleClient();
    
    // ДОБАВЛЕНО: сбор данных каждую секунду
    if (millis() - lastReadTime >= readInterval) {
        lastReadTime = millis();
        readAndAccumulateSensorData();  // читаем и добавляем в суммы
    }
    
    // Отправка данных раз в минуту (интервал теперь 60000 мс)
    if (millis() - lastSendTime >= sendInterval) {
        lastSendTime = millis();
        if (WiFi.status() == WL_CONNECTED && !sendingInProgress) {
            sendAveragedDataToServer();  // отправляем усреднённые данные
        }
    }

    static unsigned long lastCheckTime = 0;
    const unsigned long checkInterval = 10000; // время вывода в монитор порта
    if (millis() - lastCheckTime >= checkInterval) {
        lastCheckTime = millis();
        if (WiFi.status() == WL_CONNECTED && !isWiFiConnected) {
            isWiFiConnected = true;
            WiFi.softAPdisconnect(true); // отключаем точку доступа после подключения к роутеру
            saveLocalIP();
            readAndPrintSensorData();
        }
        if (isWiFiConnected) {
            readAndPrintSensorData();
        }
    }
}

// ДОБАВЛЕНО: функция для чтения датчиков и накопления сумм
void readAndAccumulateSensorData() {
    int rawLight = gpio.analogRead(0);
    int rawSoil = gpio.analogRead(1);
    float temp = sht.getTem();
    float hum = sht.getHum();

    float lux = convertToLux(rawLight);
    int soilMoisture = map(rawSoil, MIN, MAX, 0, 100);

    // Добавляем к суммам
    sumLight += lux;
    sumSoil += soilMoisture;
    sumTemp += temp;
    sumHum += hum;
    readCount++;
}

float convertToLux(int rawValue) {
    rawValue = 4096 - rawValue;
    float voltage = rawValue * (3.3 / 4096.0);
    float resistance = 10000.0 * (3.3 - voltage) / voltage;
    float lux = 12518931.0 * pow(resistance, -1.405);
    return lux;
}

void readAndPrintSensorData() {
    int rawLight = gpio.analogRead(0);
    int rawSoil = gpio.analogRead(1);
    float temp = sht.getTem();
    float hum = sht.getHum();

    float LUM_result = convertToLux(rawLight);
    int soilMoisture = map(rawSoil, MIN, MAX, 0, 100);
    
    Serial.println("\n--- Показания датчиков ---");
    Serial.print("Освещённость: "); Serial.print(LUM_result, 1); Serial.println("лк");
    Serial.print("Влажность почвы: "); Serial.print(soilMoisture); Serial.println("%");
    Serial.print("Температура воздуха: "); Serial.print(temp); Serial.println(" °C");
    Serial.print("Влажность воздуха: "); Serial.print(hum); Serial.println("%");
    Serial.print("Насос: "); Serial.println(pumpState ? "ВКЛ" : "ВЫКЛ");
    Serial.println("--------------------------");
}

// ИЗМЕНЕНО: функция отправки данных теперь использует накопленные средние значения
void sendAveragedDataToServer() {
    // Если не накоплено ни одного измерения – пропускаем отправку
    if (readCount == 0) {
        Serial.println("Нет данных для отправки (readCount = 0)");
        return;
    }

    sendingInProgress = true;

    // Вычисляем средние значения
    float avgLight = sumLight / readCount;
    float avgSoil = sumSoil / readCount;
    float avgTemp = sumTemp / readCount;
    float avgHum = sumHum / readCount;

    WiFiClient client;
    HTTPClient http;

    String url = "http://" + String(serverHost) + ":" + String(serverPort) + "/api/device/data";
    http.begin(client, url);
    http.setTimeout(1500);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", apiKey);

    DynamicJsonDocument doc(512);
    doc["device_id"] = deviceId;            // идентификатор устройства
    doc["timestamp"] = millis();
    doc["sensors"]["light"] = avgLight;
    doc["sensors"]["soil"] = avgSoil;
    doc["sensors"]["temp"] = avgTemp;
    doc["sensors"]["humidity"] = avgHum;
    doc["sensors"]["pump"] = pumpState;     // текущее состояние насоса (не усредняем)

    String jsonData;
    serializeJson(doc, jsonData);

    int httpCode = http.POST(jsonData);

    if (httpCode > 0) {
        String response = http.getString();
        Serial.printf("Усреднённые данные отправлены на сервер. Код: %d, Ответ: %s\n", httpCode, response.c_str());
        Serial.printf("Усреднено за минуту: свет=%.1f лк, почва=%.0f%%, темп=%.1f C, влажн=%.0f%%, измерений=%d\n",
                      avgLight, avgSoil, avgTemp, avgHum, readCount);
    } else {
        Serial.printf("Ошибка отправки на сервер: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end();

    // Сбрасываем накопители для следующей минуты
    sumLight = 0;
    sumSoil = 0;
    sumTemp = 0;
    sumHum = 0;
    readCount = 0;

    sendingInProgress = false;
}
// ----------------------------------------------------------------

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

// ------------------- ПОЛНЫЙ ВЕБ-ИНТЕРФЕЙС (ВОССТАНОВЛЕН) -------------------
void startServer() {
    // Главная страница
    server.on("/", HTTP_GET, []() {
        String html = R"=====( 
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Настройка контроллера</title>
    <style>
        * {
            box-sizing: border-box;
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
        }
        body {
            background-color: #f5f5f5;
            margin: 0;
            padding: 20px;
            display: flex;
            justify-content: center;
            align-items: center;
            min-height: 100vh;
        }
        .container {
            background-color: white;
            border-radius: 10px;
            box-shadow: 0 4px 12px rgba(0, 0, 0, 0.1);
            padding: 30px;
            width: 100%;
            max-width: 500px;
        }
        h1 {
            color: #2c3e50;
            text-align: center;
            margin-bottom: 25px;
        }
        .error {
            background-color: #ffebee;
            color: #c62828;
            padding: 15px;
            border-radius: 5px;
            margin-bottom: 20px;
            border-left: 4px solid #c62828;
        }
        form {
            display: flex;
            flex-direction: column;
            gap: 15px;
        }
        input {
            padding: 12px 15px;
            border: 1px solid #ddd;
            border-radius: 5px;
            font-size: 16px;
        }
        input[type="submit"] {
            background-color: #3498db;
            color: white;
            border: none;
            cursor: pointer;
            font-weight: bold;
            padding: 14px;
        }
    </style>
</head>
<body>
<div class="container">
    <h1>Настройка контроллера</h1>
)=====";

        if (wifiConnectionFailed || !serverError.isEmpty()) {
            html += "<meta http-equiv='refresh' content='10;url=/'>";
        }

        if (!serverError.isEmpty()) {
            html += "<div class='error'>Ошибка сервера: " + serverError + "</div>";
            serverError = "";
        }
        else if (wifiConnectionFailed && showReturnMessage) {
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

    // Сохранение настроек
    server.on("/save", HTTP_POST, []() {
        String ssid     = server.arg("ssid");
        String password = server.arg("password");
        String email    = server.arg("email");

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
    <p>SSID: )=====";

        response += ssid;
        response += R"=====( </p>
    <p>Email: )=====";
        response += email;
        response += R"=====( </p>
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