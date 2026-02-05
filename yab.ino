#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md.h>
#include <time.h>

// ========== НАЛАШТУВАННЯ AWS SQS ==========
const char* AWS_REGION = "eu-north-1"; // ← Змініть на вашу region
const char* AWS_ACCESS_KEY_ID = ""; // ← Ваш Access Key
const char* AWS_SECRET_ACCESS_KEY = ""; // ← Ваш Secret Key
const char* SQS_QUEUE_URL = ""; // ← URL вашої черги
// ==========================================

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

const char* ap_ssid = "Yet-Another-Bot-Setup";
const int RESET_BUTTON = 0;
const int LED_PIN = 2;

String ssid = "";
String password = "";
String deviceName = "";

// Для керування миганням
unsigned long previousMillis = 0;
unsigned long lastPingMillis = 0;
const unsigned long pingInterval = 20000; // 20 секунд
int ledState = LOW;
int blinkInterval = 0;

// Для обробки кнопки reset
unsigned long buttonPressStart = 0;
bool buttonPressed = false;
const unsigned long RESET_HOLD_TIME = 3000; // 3 секунди

// Для синхронізації часу
bool timeSync = false;

WiFiClientSecure client;

// NTP сервер для отримання часу
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;
const int daylightOffset_sec = 0;

void setup() {
  pinMode(RESET_BUTTON, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial.begin(115200);
  Serial.println("ESP32 starting...");
  
  preferences.begin("wifi-config", false);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  deviceName = preferences.getString("deviceName", "");
  
  if (ssid.length() > 0) {
    Serial.println("Found saved WiFi config");
    blinkInterval = 0; // Горить постійно під час підключення
    digitalWrite(LED_PIN, HIGH);
    connectToWiFi();
  } else {
    Serial.println("No WiFi config - starting AP mode");
    blinkInterval = 200; // Швидке мигання в режимі конфігурації
    startAPMode();
  }
}

void startAPMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid);
  
  Serial.println("AP Started!");
  Serial.println("Connect to WiFi: ");
  Serial.println(ap_ssid);
  Serial.println("Go to: http://");
  Serial.println(WiFi.softAPIP());
  
  blinkInterval = 200; // Швидке мигання
  
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

void syncTime() {
  Serial.println("Synchronizing time with NTP...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer, "time.nist.gov", "time.google.com");
  
  int attempts = 0;
  time_t now = 0;
  
  while (now < 1000000000 && attempts < 30) {
    time(&now);
    Serial.print(".");
    delay(500);
    attempts++;
  }
  
  Serial.println();
  
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

void connectToWiFi() {
  Serial.println("Connecting to: " + ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  
  blinkInterval = 0; // Горить постійно під час підключення
  digitalWrite(LED_PIN, HIGH);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println(); 
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("Device ID: " + deviceName);
    blinkInterval = 2000; // Повільне мигання коли підключено
    
    client.setInsecure();
    
    // Синхронізуємо час для AWS підпису
    syncTime();
    
    if (timeSync) {
      // Відправляємо перше повідомлення при підключенні
      sendToSQS();
      lastPingMillis = millis();
    } else {
      Serial.println("Cannot send to SQS without time sync. Will retry...");
    }
  } else {
    Serial.println("Connection failed - will retry...");
    blinkInterval = 0; // Продовжуємо горіти
    digitalWrite(LED_PIN, HIGH);
    delay(5000); 
  }
}

String hmacSHA256Hex(const String& key, const String& data) {
  byte hmac[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
  
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key.c_str(), key.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)data.c_str(), data.length());
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx);
  
  String result = "";
  for (int i = 0; i < 32; i++) {
    char hex[3];
    sprintf(hex, "%02x", hmac[i]);
    result += hex;
  }
  return result;
}

String hmacSHA256Binary(const String& keyHex, const String& data) {
  // Конвертуємо hex key назад в binary
  byte keyBinary[32];
  for (int i = 0; i < 32; i++) {
    String byteString = keyHex.substring(i*2, i*2 + 2);
    keyBinary[i] = (byte)strtol(byteString.c_str(), NULL, 16);
  }
  
  byte hmac[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
  
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_hmac_starts(&ctx, keyBinary, 32);
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)data.c_str(), data.length());
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx);
  
  String result = "";
  for (int i = 0; i < 32; i++) {
    char hex[3];
    sprintf(hex, "%02x", hmac[i]);
    result += hex;
  }
  return result;
}

String sha256Hash(const String& data) {
  byte hash[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
  
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char*)data.c_str(), data.length());
  mbedtls_md_finish(&ctx, hash);
  mbedtls_md_free(&ctx);
  
  String result = "";
  for (int i = 0; i < 32; i++) {
    char hex[3];
    sprintf(hex, "%02x", hash[i]);
    result += hex;
  }
  return result;
}

void sendToSQS() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return;
  }

  if (!timeSync) {
    Serial.println("Time not synchronized, retrying...");
    syncTime();
    if (!timeSync) {
      Serial.println("Still no time sync, skipping SQS send");
      return;
    }
  }

  Serial.println("Sending message to SQS...");
  
  // Отримуємо поточний час
  time_t now;
  time(&now);
  
  if (now < 1000000000) {
    Serial.println("Invalid time, resynchronizing...");
    syncTime();
    time(&now);
    if (now < 1000000000) {
      Serial.println("Time sync failed, aborting send");
      return;
    }
  }
  
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  
  char dateStamp[9];
  char amzDate[17];
  strftime(dateStamp, sizeof(dateStamp), "%Y%m%d", &timeinfo);
  strftime(amzDate, sizeof(amzDate), "%Y%m%dT%H%M%SZ", &timeinfo);
  
  Serial.print("Using timestamp: ");
  Serial.println(amzDate);
  
  // Парсимо URL черги
  String queueUrl = String(SQS_QUEUE_URL);
  int hostStart = queueUrl.indexOf("://") + 3;
  int pathStart = queueUrl.indexOf("/", hostStart);
  String host = queueUrl.substring(hostStart, pathStart);
  String path = queueUrl.substring(pathStart);
  
  // Формуємо JSON повідомлення
  String jsonMessage = "{\"deviceId\":\"" + deviceName + "\"}";
  Serial.println("Message: " + jsonMessage);
  
  // Формуємо тіло запиту
  String payload = "Action=SendMessage&MessageBody=" + urlEncode(jsonMessage) + "&Version=2012-11-05";
  String payloadHash = sha256Hash(payload);
  
  // Canonical request
  String canonicalRequest = "POST\n";
  canonicalRequest += path + "\n";
  canonicalRequest += "\n";
  canonicalRequest += "host:" + host + "\n";
  canonicalRequest += "x-amz-date:" + String(amzDate) + "\n";
  canonicalRequest += "\n";
  canonicalRequest += "host;x-amz-date\n";
  canonicalRequest += payloadHash;
  
  String canonicalRequestHash = sha256Hash(canonicalRequest);
  
  // String to sign
  String credentialScope = String(dateStamp) + "/" + AWS_REGION + "/sqs/aws4_request";
  String stringToSign = "AWS4-HMAC-SHA256\n";
  stringToSign += String(amzDate) + "\n";
  stringToSign += credentialScope + "\n";
  stringToSign += canonicalRequestHash;
  
  // Signing key
  String kDate = hmacSHA256Hex("AWS4" + String(AWS_SECRET_ACCESS_KEY), dateStamp);
  String kRegion = hmacSHA256Binary(kDate, AWS_REGION);
  String kService = hmacSHA256Binary(kRegion, "sqs");
  String kSigning = hmacSHA256Binary(kService, "aws4_request");
  
  // Signature
  String signature = hmacSHA256Binary(kSigning, stringToSign);
  
  // Authorization header
  String authorization = "AWS4-HMAC-SHA256 Credential=" + String(AWS_ACCESS_KEY_ID) + "/" + credentialScope;
  authorization += ", SignedHeaders=host;x-amz-date";
  authorization += ", Signature=" + signature;
  
  // Відправляємо запит
  Serial.println("Connecting to " + host + "...");
  if (client.connect(host.c_str(), 443)) {
    Serial.println("Connected! Sending request...");
    client.println("POST " + path + " HTTP/1.1");
    client.println("Host: " + host);
    client.println("X-Amz-Date: " + String(amzDate));
    client.println("Authorization: " + authorization);
    client.println("Content-Type: application/x-www-form-urlencoded");
    client.println("Content-Length: " + String(payload.length()));
    client.println("Connection: close");
    client.println();
    client.println(payload);
    
    unsigned long timeout = millis();
    while (client.available() == 0) {
      if (millis() - timeout > 10000) {
        Serial.println("Timeout waiting for response!");
        client.stop();
        return;
      }
      delay(10);
    }
    
    // Читаємо відповідь
    bool success = false;
    while (client.available()) {
      String line = client.readStringUntil('\n');
      if (line.indexOf("200 OK") > 0 || line.indexOf("MessageId") > 0) {
        success = true;
      }
    }
    
    if (success) {
      Serial.println("✓ Message sent to SQS successfully!");
    } else {
      Serial.println("✗ Failed to send message to SQS");
    }
    
    client.stop();
  } else {
    Serial.println("✗ Connection to SQS failed");
  }
}

String urlEncode(String str) {
  String encoded = "";
  char c;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encoded += "%20";
    } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      encoded += '%';
      char hex[3];
      sprintf(hex, "%02X", c);
      encoded += hex;
    }
  }
  return encoded;
}

void updateLED() {
  if (blinkInterval == 0) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= blinkInterval) {
      previousMillis = currentMillis;
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
    }
  }
}

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
        digitalWrite(LED_PIN, (millis() / 100) % 2);
      } else {
        Serial.println("Reset button held for 3 seconds - resetting config!");
        
        for (int i = 0; i < 5; i++) {
          digitalWrite(LED_PIN, HIGH);
          delay(100);
          digitalWrite(LED_PIN, LOW);
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
      
      if (pressDuration < RESET_HOLD_TIME) {
        Serial.println("Not held long enough for reset");
      }
      
      buttonPressed = false;
    }
  }
}

void loop() {
  checkResetButton();
  
  if (!buttonPressed) {
    updateLED();
  }
  
  if (WiFi.getMode() == WIFI_AP) {
    // В режимі налаштування - обробляємо веб-сервер
    dnsServer.processNextRequest();
    server.handleClient();
  } else if (WiFi.status() == WL_CONNECTED) {
    // Підключені до WiFi - тільки відправка в SQS, БЕЗ веб-сервера
    blinkInterval = 2000;
    
    // Перевіряємо синхронізацію часу періодично
    if (!timeSync) {
      static unsigned long lastTimeSyncAttempt = 0;
      if (millis() - lastTimeSyncAttempt > 30000) {
        syncTime();
        lastTimeSyncAttempt = millis();
      }
    }
    
    // Відправляємо повідомлення кожні 20 секунд
    if (timeSync && (millis() - lastPingMillis >= pingInterval)) {
      sendToSQS();
      lastPingMillis = millis();
    }
  } else {
    Serial.println("WiFi lost, reconnecting...");
    blinkInterval = 0;
    digitalWrite(LED_PIN, HIGH);
    timeSync = false;
    connectToWiFi();
  }
  
  // Serial команди
  if (Serial.available()) {
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
    } else if (cmd == "status") {
      Serial.println("=== Status ===");
      Serial.println("Device ID: " + deviceName);
      Serial.println("WiFi: " + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected"));
      Serial.println("Time Sync: " + String(timeSync ? "Yes" : "No"));
    }
  }
  
  delay(10);
}
