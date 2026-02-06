#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md.h>
#include <time.h>
#include <Adafruit_NeoPixel.h>

// ========== НАЛАШТУВАННЯ AWS SQS ==========
const char* AWS_REGION = "eu-north-1";
const char* AWS_ACCESS_KEY_ID = "";
const char* AWS_SECRET_ACCESS_KEY = "+SxsMcSKdzw1dtv8fEe/uhe";
const char* SQS_QUEUE_URL = "https://sqs.eu-north-1.amazonaws.com/714378673150/yab-ping";
// ==========================================

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

const char* ap_ssid = "Yet-Another-Bot-Setup";
const int RESET_BUTTON = 9;

// WS2812 RGB LED для ESP32-C3-MINI
const int RGB_LED_PIN = 10;
const int NUM_PIXELS = 1;
Adafruit_NeoPixel pixel(NUM_PIXELS, RGB_LED_PIN, NEO_RGB + NEO_KHZ800);

String ssid = "";
String password = "";
String deviceName = "";

// Для керування миганням
unsigned long previousMillis = 0;
bool ledState = false;

// Режими блимання
enum BlinkMode {
  BLINK_SETUP,      // Синій - AP режим
  BLINK_CONNECTING, // Червоний - підключення
  BLINK_CONNECTED   // Зелений - працює
};
BlinkMode currentBlinkMode = BLINK_SETUP;

// Для обробки кнопки reset
unsigned long buttonPressStart = 0;
bool buttonPressed = false;
const unsigned long RESET_HOLD_TIME = 3000;

// Для синхронізації часу
bool timeSync = false;

// Таймери (неблокуючі)
unsigned long lastPingMillis = 0;
unsigned long lastReconnectMillis = 0;
unsigned long lastTimeSyncMillis = 0;

const unsigned long PING_INTERVAL = 30000;       // Пінг кожні 60 сек
const unsigned long RECONNECT_INTERVAL = 15000;   // Реконект кожні 15 сек
const unsigned long TIME_SYNC_INTERVAL = 30000;    // Спроба синхронізації часу кожні 30 сек
const unsigned long NTP_TIMEOUT = 8000;            // Таймаут NTP 8 сек

// Стан підключення WiFi (неблокуючий)
enum WiFiState {
  WIFI_STATE_IDLE,
  WIFI_STATE_CONNECTING,
  WIFI_STATE_CONNECTED,
  WIFI_STATE_AP
};
WiFiState wifiState = WIFI_STATE_IDLE;
unsigned long wifiConnectStart = 0;
const unsigned long WIFI_CONNECT_TIMEOUT = 15000;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;
const int daylightOffset_sec = 0;

// ========== LED функції ==========

void setLEDColor(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

void ledOff() {
  setLEDColor(0, 0, 0);
}

void updateLED() {
  unsigned long currentMillis = millis();
  unsigned int interval;

  switch (currentBlinkMode) {
    case BLINK_SETUP:
      interval = 500;
      if (currentMillis - previousMillis >= interval) {
        previousMillis = currentMillis;
        ledState = !ledState;
        if (ledState) setLEDColor(0, 0, 255);
        else ledOff();
      }
      break;

    case BLINK_CONNECTING:
      interval = 200;
      if (currentMillis - previousMillis >= interval) {
        previousMillis = currentMillis;
        ledState = !ledState;
        if (ledState) setLEDColor(255, 0, 0);
        else ledOff();
      }
      break;

    case BLINK_CONNECTED:
      interval = 2000;
      if (currentMillis - previousMillis >= interval) {
        previousMillis = currentMillis;
        ledState = !ledState;
        if (ledState) setLEDColor(0, 255, 0);
        else ledOff();
      }
      break;
  }
}

// ========== Кнопка Reset ==========

void checkResetButton() {
  int buttonState = digitalRead(RESET_BUTTON);

  if (buttonState == LOW) {
    if (!buttonPressed) {
      buttonPressed = true;
      buttonPressStart = millis();
      Serial.println("Reset button pressed...");
    } else {
      unsigned long pressDuration = millis() - buttonPressStart;

      if (pressDuration < RESET_HOLD_TIME) {
        bool pulse = (millis() / 100) % 2;
        setLEDColor(pulse ? 255 : 0, pulse ? 255 : 0, pulse ? 255 : 0);
      } else {
        Serial.println("Reset button held for 3 seconds - resetting config!");
        for (int i = 0; i < 5; i++) {
          setLEDColor(255, 0, 0);
          delay(100);
          ledOff();
          delay(100);
        }
        preferences.clear();
        Serial.println("Config cleared, rebooting...");
        delay(500);
        ESP.restart();
      }
    }
  } else {
    if (buttonPressed) {
      unsigned long pressDuration = millis() - buttonPressStart;
      Serial.print("Button released after ");
      Serial.print(pressDuration);
      Serial.println("ms");
      buttonPressed = false;
    }
  }
}

// ========== Крипто функції (бінарні) ==========

void hmacSHA256Raw(const uint8_t* key, size_t keyLen,
                   const uint8_t* data, size_t dataLen,
                   uint8_t* out) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, key, keyLen);
  mbedtls_md_hmac_update(&ctx, data, dataLen);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

void sha256Raw(const uint8_t* data, size_t dataLen, uint8_t* out) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, data, dataLen);
  mbedtls_md_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

String toHex(const uint8_t* data, size_t len) {
  String result;
  result.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    char hex[3];
    sprintf(hex, "%02x", data[i]);
    result += hex;
  }
  return result;
}

String sha256Hash(const String& data) {
  uint8_t hash[32];
  sha256Raw((const uint8_t*)data.c_str(), data.length(), hash);
  return toHex(hash, 32);
}

// ========== URL Encode ==========

String urlEncode(const String& str) {
  String encoded;
  encoded.reserve(str.length() * 2);
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == ' ') {
      encoded += "%20";
    } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      char hex[4];
      sprintf(hex, "%%%02X", (uint8_t)c);
      encoded += hex;
    }
  }
  return encoded;
}

// ========== NTP синхронізація ==========

void syncTime() {
  Serial.println("Synchronizing time with NTP...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer, "time.nist.gov", "time.google.com");

  unsigned long startAttempt = millis();
  time_t now = 0;

  while (now < 1000000000 && (millis() - startAttempt < NTP_TIMEOUT)) {
    time(&now);
    delay(100);
  }

  if (now > 1000000000) {
    timeSync = true;
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &timeinfo);
    Serial.print("Time synchronized: ");
    Serial.println(buffer);
  } else {
    timeSync = false;
    Serial.println("Failed to synchronize time!");
  }
}

// ========== WiFi підключення (неблокуюче) ==========

void startWiFiConnect() {
  Serial.println("Connecting to: " + ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false); // Керуємо реконектом самі
  WiFi.begin(ssid.c_str(), password.c_str());
  wifiState = WIFI_STATE_CONNECTING;
  wifiConnectStart = millis();
  currentBlinkMode = BLINK_CONNECTING;
}

void handleWiFiConnecting() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("Device ID: " + deviceName);

    wifiState = WIFI_STATE_CONNECTED;
    currentBlinkMode = BLINK_CONNECTED;
    timeSync = false; // Пересинхронізуємо час

    syncTime();
    if (timeSync) {
      sendToSQS();
      lastPingMillis = millis();
    }
    return;
  }

  if (millis() - wifiConnectStart > WIFI_CONNECT_TIMEOUT) {
    Serial.println("\nWiFi connect timeout");
    WiFi.disconnect(true);
    wifiState = WIFI_STATE_IDLE;
    lastReconnectMillis = millis();
  }
}

// ========== AP Mode ==========

void startAPMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid);

  Serial.println("AP Started!");
  Serial.print("Connect to WiFi: ");
  Serial.println(ap_ssid);
  Serial.print("Go to: http://");
  Serial.println(WiFi.softAPIP());

  wifiState = WIFI_STATE_AP;
  currentBlinkMode = BLINK_SETUP;

  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<style>body{font-family:Arial;padding:20px;max-width:400px;margin:auto;background:#f5f5f5;}";
    html += ".container{background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);}";
    html += "input,button{width:100%;padding:12px;margin:8px 0;font-size:16px;box-sizing:border-box;border:1px solid #ddd;border-radius:4px;}";
    html += "button{background:#0088cc;color:white;border:none;cursor:pointer;font-weight:bold;}";
    html += "button:hover{background:#006699;}";
    html += "label{font-weight:bold;color:#333;display:block;margin-top:10px;}";
    html += "h2{color:#0088cc;margin-top:0;}</style></head><body>";
    html += "<div class='container'>";
    html += "<h2>🤖 Yet Another Bot Setup</h2>";
    html += "<form action='/save' method='POST'>";
    html += "<label>📱 Назва пристрою (Device ID):</label><input name='deviceName' placeholder='my-device-001' required>";
    html += "<label>📶 WiFi SSID:</label><input name='ssid' required>";
    html += "<label>🔒 WiFi Password:</label><input name='pass' type='password' required>";
    html += "<button type='submit'>💾 Зберегти і підключитись</button>";
    html += "</form></div></body></html>";
    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, []() {
    deviceName = server.arg("deviceName");
    ssid = server.arg("ssid");
    password = server.arg("pass");

    preferences.putString("deviceName", deviceName);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);

    String html = "<html><head><meta charset='utf-8'></head><body style='font-family:Arial;padding:20px;text-align:center;'>";
    html += "<h2>✅ Збережено!</h2>";
    html += "<p>Device ID: <strong>" + deviceName + "</strong></p>";
    html += "<p>Підключаюсь до WiFi...</p>";
    html += "<p>Пристрій перезавантажиться зараз</p></body></html>";
    server.send(200, "text/html", html);

    delay(2000);
    ESP.restart();
  });

  server.begin();
  Serial.println("Config server started");
}

// ========== SQS ==========

void sendToSQS() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping SQS send");
    return;
  }

  if (!timeSync) {
    Serial.println("Time not synchronized, skipping SQS send");
    return;
  }

  time_t now;
  time(&now);

  if (now < 1000000000) {
    Serial.println("Invalid time, resynchronizing...");
    timeSync = false;
    return;
  }

  Serial.println("Sending message to SQS...");
  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());

  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);

  char dateStamp[9];
  char amzDate[17];
  strftime(dateStamp, sizeof(dateStamp), "%Y%m%d", &timeinfo);
  strftime(amzDate, sizeof(amzDate), "%Y%m%dT%H%M%SZ", &timeinfo);

  Serial.print("Timestamp: ");
  Serial.println(amzDate);

  // Парсимо URL
  String queueUrl = String(SQS_QUEUE_URL);
  int hostStart = queueUrl.indexOf("://") + 3;
  int pathStart = queueUrl.indexOf("/", hostStart);
  String host = queueUrl.substring(hostStart, pathStart);
  String path = queueUrl.substring(pathStart);

  // Тіло повідомлення
  String jsonMessage = "{\"deviceId\":\"" + deviceName + "\"}";
  String payload = "Action=SendMessage&MessageBody=" + urlEncode(jsonMessage) + "&Version=2012-11-05";
  String payloadHash = sha256Hash(payload);

  // Canonical Request
  String canonicalRequest = "POST\n";
  canonicalRequest += path + "\n";
  canonicalRequest += "\n";
  canonicalRequest += "host:" + host + "\n";
  canonicalRequest += "x-amz-date:" + String(amzDate) + "\n";
  canonicalRequest += "\n";
  canonicalRequest += "host;x-amz-date\n";
  canonicalRequest += payloadHash;

  String canonicalRequestHash = sha256Hash(canonicalRequest);

  // String to Sign
  String credentialScope = String(dateStamp) + "/" + AWS_REGION + "/sqs/aws4_request";
  String stringToSign = "AWS4-HMAC-SHA256\n";
  stringToSign += String(amzDate) + "\n";
  stringToSign += credentialScope + "\n";
  stringToSign += canonicalRequestHash;

  // Signing Key (бінарний ланцюжок)
  String keyStr = "AWS4" + String(AWS_SECRET_ACCESS_KEY);
  uint8_t kDate[32], kRegion[32], kService[32], kSigning[32], sig[32];

  hmacSHA256Raw((const uint8_t*)keyStr.c_str(), keyStr.length(),
                (const uint8_t*)dateStamp, strlen(dateStamp), kDate);
  hmacSHA256Raw(kDate, 32,
                (const uint8_t*)AWS_REGION, strlen(AWS_REGION), kRegion);
  hmacSHA256Raw(kRegion, 32,
                (const uint8_t*)"sqs", 3, kService);
  hmacSHA256Raw(kService, 32,
                (const uint8_t*)"aws4_request", 12, kSigning);
  hmacSHA256Raw(kSigning, 32,
                (const uint8_t*)stringToSign.c_str(), stringToSign.length(), sig);

  String signature = toHex(sig, 32);

  // Authorization header
  String authorization = "AWS4-HMAC-SHA256 Credential=" + String(AWS_ACCESS_KEY_ID) + "/" + credentialScope;
  authorization += ", SignedHeaders=host;x-amz-date";
  authorization += ", Signature=" + signature;

  // HTTP запит (локальний клієнт — звільниться після виходу з функції)
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  secureClient.setTimeout(10); // 10 секунд таймаут

  Serial.println("Connecting to " + host + "...");
  if (!secureClient.connect(host.c_str(), 443)) {
    Serial.println("✗ TLS connection failed");
    return;
  }

  Serial.println("Connected! Sending request...");
  secureClient.print("POST " + path + " HTTP/1.1\r\n");
  secureClient.print("Host: " + host + "\r\n");
  secureClient.print("X-Amz-Date: " + String(amzDate) + "\r\n");
  secureClient.print("Authorization: " + authorization + "\r\n");
  secureClient.print("Content-Type: application/x-www-form-urlencoded\r\n");
  secureClient.print("Content-Length: " + String(payload.length()) + "\r\n");
  secureClient.print("Connection: close\r\n");
  secureClient.print("\r\n");
  secureClient.print(payload);

  // Читаємо відповідь
  unsigned long timeout = millis();
  while (secureClient.available() == 0) {
    if (millis() - timeout > 10000) {
      Serial.println("✗ Timeout waiting for response");
      secureClient.stop();
      return;
    }
    delay(10);
  }

  // Парсимо статус і тіло
  String statusLine = "";
  bool headersEnded = false;
  String responseBody = "";
  bool success = false;

  while (secureClient.available()) {
    String line = secureClient.readStringUntil('\n');
    line.trim();

    if (statusLine.isEmpty()) {
      statusLine = line;
      if (statusLine.indexOf("200") > 0) {
        success = true;
      }
    }

    if (line.isEmpty()) {
      headersEnded = true;
      continue;
    }

    if (headersEnded) {
      responseBody += line + "\n";
    }
  }

  secureClient.stop();

  if (success) {
    Serial.println("✓ Message sent to SQS successfully!");
  } else {
    Serial.println("✗ SQS request failed");
    Serial.println("Status: " + statusLine);
    // Показуємо тіло помилки (обмежено для економії RAM)
    if (responseBody.length() > 500) {
      responseBody = responseBody.substring(0, 500) + "...";
    }
    Serial.println("Response: " + responseBody);
  }

  Serial.print("Free heap after SQS: ");
  Serial.println(ESP.getFreeHeap());
}

// ========== Serial команди ==========

void handleSerialCommands() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd == "reset") {
    Serial.println("Resetting config...");
    preferences.clear();
    ESP.restart();
  } else if (cmd == "send") {
    sendToSQS();
  } else if (cmd == "synctime") {
    syncTime();
  } else if (cmd == "heap") {
    Serial.print("Free heap: ");
    Serial.println(ESP.getFreeHeap());
  } else if (cmd == "status") {
    Serial.println("=== Status ===");
    Serial.println("Device ID: " + deviceName);
    Serial.println("WiFi: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("RSSI: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
    }
    Serial.println("Time Sync: " + String(timeSync ? "Yes" : "No"));
    Serial.print("Free heap: ");
    Serial.println(ESP.getFreeHeap());
    Serial.print("Uptime: ");
    Serial.print(millis() / 1000);
    Serial.println("s");
    Serial.print("Blink Mode: ");
    switch (currentBlinkMode) {
      case BLINK_SETUP: Serial.println("SETUP (Blue)"); break;
      case BLINK_CONNECTING: Serial.println("CONNECTING (Red)"); break;
      case BLINK_CONNECTED: Serial.println("CONNECTED (Green)"); break;
    }
  } else if (cmd == "led") {
    Serial.println("Testing LED...");
    setLEDColor(255, 0, 0); delay(1000);
    setLEDColor(0, 255, 0); delay(1000);
    setLEDColor(0, 0, 255); delay(1000);
    ledOff();
  } else if (cmd == "help") {
    Serial.println("Commands: reset, send, synctime, status, heap, led, help");
  }
}

// ========== Setup ==========

void setup() {
  pinMode(RESET_BUTTON, INPUT_PULLUP);

  pixel.begin();
  pixel.setBrightness(50);
  ledOff();

  Serial.begin(115200);
  delay(100);
  Serial.println("\n\nESP32-C3-MINI starting...");

  preferences.begin("wifi-config", false);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  deviceName = preferences.getString("deviceName", "");

  if (ssid.length() > 0) {
    Serial.println("Found saved WiFi config, connecting...");
    startWiFiConnect();
  } else {
    Serial.println("No WiFi config - starting AP mode");
    startAPMode();
  }
}

// ========== Loop (повністю неблокуючий) ==========

void loop() {
  // Кнопка reset — завжди
  checkResetButton();

  // LED — завжди (якщо кнопка не натиснута)
  if (!buttonPressed) {
    updateLED();
  }

  // Serial команди — завжди
  handleSerialCommands();

  // Залежно від стану WiFi
  switch (wifiState) {
    case WIFI_STATE_AP:
      // AP режим — обробляємо DNS і веб-сервер
      dnsServer.processNextRequest();
      server.handleClient();
      break;

    case WIFI_STATE_CONNECTING:
      // Чекаємо підключення (неблокуюче)
      handleWiFiConnecting();
      break;

    case WIFI_STATE_CONNECTED:
      // Перевіряємо чи ще підключені
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi lost!");
        wifiState = WIFI_STATE_IDLE;
        currentBlinkMode = BLINK_CONNECTING;
        timeSync = false;
        lastReconnectMillis = millis(); // Не реконектимось одразу
        break;
      }

      // Синхронізація часу (якщо не синхронізовано)
      if (!timeSync && (millis() - lastTimeSyncMillis > TIME_SYNC_INTERVAL)) {
        syncTime();
        lastTimeSyncMillis = millis();
      }

      // Пінг SQS
      if (timeSync && (millis() - lastPingMillis >= PING_INTERVAL)) {
        sendToSQS();
        lastPingMillis = millis();
      }
      break;

    case WIFI_STATE_IDLE:
      // WiFi не підключений — пробуємо реконект з інтервалом
      currentBlinkMode = BLINK_CONNECTING;
      if (millis() - lastReconnectMillis >= RECONNECT_INTERVAL) {
        Serial.println("Attempting WiFi reconnect...");
        WiFi.disconnect(true);
        delay(100); // Мінімальна пауза для WiFi стека
        startWiFiConnect();
        lastReconnectMillis = millis();
      }
      break;
  }

  delay(10);
}
