#include <WiFi.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>

Preferences preferences;

// Настройки точки доступа
const char* apName = "ESP32_AP";
const char* apPassword = "123456789";

WebServer server;

void setup() {
    Serial.begin(115200);
    preferences.begin("WiFiCreds", false);

    // Получаем данные Wi-Fi
    String ssid = preferences.getString("SSID", "");
    String password = preferences.getString("PASSWORD", "");

    // Если нет сохраненных данных, создаем точку доступа
    if (ssid.isEmpty() || password.isEmpty()) {
        Serial.println("Нет настроек WiFi, создаем точку доступа...");
        WiFi.softAP(apName, apPassword);
        startServer(); // Запускаем веб-сервер
    } else {
        connectToWiFi(ssid.c_str(), password.c_str());
    }
}

void startServer() {
    // Настраиваем обработчик для корневого URL
    server.on("/", HTTP_GET, []() {
        // Генерация HTML-кода для формы
        String html = "<html><body><h1>WiFi Setup</h1>";
        html += "<form action=\"/save\" method=\"POST\">";
        html += "SSID: <input type=\"text\" name=\"ssid\"><br>";
        html += "Password: <input type=\"password\" name=\"password\"><br>";
        html += "<input type=\"submit\" value=\"Save\">";
        html += "</form></body></html>";
        server.send(200, "text/html", html);
    });

    // Обработчик для сохранения данных
    server.on("/save", HTTP_POST, []() {
        String ssid = server.arg("ssid");
        String password = server.arg("password");
        preferences.putString("SSID", ssid);
        preferences.putString("PASSWORD", password);
        server.send(200, "text/html", "Настройки сохранены. Перезапуск ESP32...");
        delay(2000);
        ESP.restart(); // Перезагрузка для применения новых настроек
    });

    server.begin();
    Serial.println("HTTP server started.");
}

void connectToWiFi(const char* ssid, const char* password) {
    WiFi.begin(ssid, password);
    Serial.println("Подключение к WiFi...");
    
    for (int attempt = 0; attempt < 10 && WiFi.status() != WL_CONNECTED; ++attempt) {
        delay(1000);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Успешное подключение к Wi-Fi!");
        makeHttpRequest();
    } else {
        Serial.println("Ошибка подключения к Wi-Fi.");
        // Если не удалось подключиться к Wi-Fi, создаем точку доступа
        WiFi.softAP(apName, apPassword);
        Serial.println("Создаем точку доступа повторно для ввода данных WiFi...");
        startServer(); // Запускаем веб-сервер
    }
}

void makeHttpRequest() {
    HTTPClient http;
    http.begin("http://plantsystem.ru/set_new_controller");

    int httpCode = http.GET();
    if (httpCode > 0) {
        // Уменьшаем размер статического документа
        StaticJsonDocument<256> doc; // Используем меньший размер

        if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
            // Обработка данных из `doc`
            Serial.println("Данные успешно десериализованы.");
        } else {
            Serial.println("Ошибка при десериализации JSON.");
        }
    } else {
        Serial.println("Ошибка при обращении к серверу: " + String(httpCode));
    }
    
    http.end();
}

void loop() {
    server.handleClient(); // Обработка HTTP-запросов
}
