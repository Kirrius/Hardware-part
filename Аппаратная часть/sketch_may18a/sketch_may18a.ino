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
String phoneIP = "192.168.1.102";

// --- ускорение отправки: уменьшён интервал ---
const unsigned long sendInterval = 1000; // отправлять раз в 1 секунду
unsigned long lastSendTime = 0;
volatile bool sendingInProgress = false; // флаг — идёт отправка

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
        if (!silent) {
            Serial.println("\nОшибка подключения к WiFi");
        }
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

    server.on("/sensorData", HTTP_POST, []() {
        if (phoneIP.isEmpty()) {
            server.send(400, "application/json", "{\"error\":\"IP телефона не установлен\"}");
            return;
        }
        sendSensorDataToPhone();
    });

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
    
    startServer(); // старт сервера (включает server.begin())
    Serial.println("HTTP сервер запущен");
}

void loop() {
    server.handleClient();
    
    static unsigned long lastCheckTime = 0;
    const unsigned long checkInterval = 1000; // Проверяем каждую секунду

    // Отправка данных на телефон (интервал уменьшён выше)
    if (millis() - lastSendTime >= sendInterval) {
        lastSendTime = millis();
        if (!phoneIP.isEmpty()) {
            // не запускаем новую отправку, если предыдущая ещё идёт
            if (!sendingInProgress) {
                sendSensorDataToPhone();
            } else {
                // можно логировать если нужно
                // Serial.println("Предыдущая отправка ещё выполняется, пропускаем.");
            }
        }
    }

    if (millis() - lastCheckTime >= checkInterval) {
        lastCheckTime = millis();
        
        if (WiFi.status() == WL_CONNECTED && !isWiFiConnected) {
            isWiFiConnected = true;
            Serial.println("WiFi подключен, можно закрывать точку доступа");
            WiFi.softAPdisconnect(true);
            saveLocalIP();
            readAndPrintSensorData();
        }
        
        if (isWiFiConnected) {
            readAndPrintSensorData();
        }
    }
}

float convertToLux(int rawValue) {
    rawValue = 4096 - rawValue;
    float voltage = rawValue * (3.3 / 4096.0);
    float resistance = 10000.0 * (3.3 - voltage) / voltage;
    float lux = 12518931.0 * pow(resistance, -1.405);
    return lux;
}

void readAndPrintSensorData() {
    // Читаем один раз и печатаем
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

void sendSensorDataToPhone() {
    if (phoneIP.isEmpty()) {
        Serial.println("IP телефона не установлен");
        return;
    }

    // Пометка, что отправка началась
    sendingInProgress = true;

    // Читаем датчики один раз (чтобы быстро собрать JSON)
    int rawLight = gpio.analogRead(0);
    int rawSoil = gpio.analogRead(1);
    float temp = sht.getTem();
    float hum = sht.getHum();
    float lux = convertToLux(rawLight);
    int soilMoisture = map(rawSoil, MIN, MAX, 0, 100);

    WiFiClient client;
    HTTPClient http;

    String url = "http://" + phoneIP + ":8080/sensorData";
    http.begin(client, url);

    // Сделаем небольшой таймаут, чтобы при проблемах не висеть слишком долго
    http.setTimeout(1500); // миллисекунды

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Device-ID", "ESP32_PlantMonitor");

    DynamicJsonDocument doc(512);
    doc["device"] = "ESP32_PlantMonitor";
    doc["timestamp"] = millis();
    doc["sensors"]["light"] = lux;
    doc["sensors"]["soil"] = soilMoisture;
    doc["sensors"]["temp"] = temp;
    doc["sensors"]["humidity"] = hum;
    doc["sensors"]["pump"] = pumpState;

    String jsonData;
    serializeJson(doc, jsonData);

    int httpCode = http.POST(jsonData);

    if (httpCode > 0) {
        String response = http.getString();
        Serial.printf("Данные отправлены. Код: %d, Ответ: %s\n", httpCode, response.c_str());
    } else {
        Serial.printf("Ошибка отправки: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end();
    sendingInProgress = false;
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
            // оставляем только необходимые записи, удаляем сохранённые креды
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


bool makeHttpRequest(const String &email) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Нет подключения к WiFi");
        serverError = "Нет подключения к интернету";
        return false;
    }

    HTTPClient http;
    http.begin("http://plants.alt255.tech/");
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> doc;
    doc["mac"] = WiFi.macAddress();
    doc["email"] = email;
    
    String requestBody;
    serializeJson(doc, requestBody);

    int httpCode = http.POST(requestBody);
    
    if (httpCode > 0) {
        Serial.printf("Код ответа: %d\n", httpCode);
        
        if (httpCode == 200) {
            Serial.println("Успешный запрос к серверу");
            return true;
        } else if (httpCode == 403) {
            serverError = "Доступ запрещён (403)";
        } else if (httpCode == 404) {
            serverError = "Страница не найдена (404)";
        } else if (httpCode == 500) {
            serverError = "Ошибка сервера (500)";
        } else {
            serverError = "Ошибка сервера: " + String(httpCode);
        }
    } else {
        String errorMsg = http.errorToString(httpCode);
        Serial.printf("Ошибка запроса: %s\n", errorMsg.c_str());
        serverError = "Сервер не отвечает: " + errorMsg;
    }
    
    http.end();
    return false;
}
