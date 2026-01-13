#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_camera.h"
#include <UniversalTelegramBot.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <WebServer.h>
#include "soc/rtc_cntl_reg.h"

// ===================== UART PARA RASPBERRY =====================
#define UART_RX_PIN 14
#define UART_TX_PIN 15
HardwareSerial SerialRPI(1);

// ===================== AP WIFI & EEPROM =====================
WebServer server(80);

#define EEPROM_SIZE 512
#define EEPROM_ADDR_SERVERURL 350

// ===================== BOTÓN POR INTERRUPCIÓN =====================
#define BUTTON_PIN 13

int countOpenDoor = 0;
volatile bool requestOpenDoor = false;
volatile unsigned long lastInterruptTime = 0;
const unsigned long DEBOUNCE_TIME = 300;

// ===================== TIMERS (millis) =====================
unsigned long lastUartCheck     = 0;
unsigned long lastTelegramCheck = 0;

const unsigned long UART_INTERVAL  = 20;
const unsigned long TG_INTERVAL    = 800;

// ===================== EEPROM STRINGS =====================
String leerStringDeEEPROM(int direccion) {
  String cadena = "";
  char caracter = EEPROM.read(direccion);
  int i = 0;
  while (caracter != '\0' && i < 150) {
    cadena += caracter;
    i++;
    caracter = EEPROM.read(direccion + i);
  }
  return cadena;
}

void escribirStringEnEEPROM(int direccion, String cadena) {
  for (int i = 0; i < cadena.length(); i++)
    EEPROM.write(direccion + i, cadena[i]);
  EEPROM.write(direccion + cadena.length(), '\0');
  EEPROM.commit();
}

// ===================== WIFI CONFIG AP =====================
void handleRoot() {
  String html = "<html><body>";
  html += "<h2>Configurar WiFi ESP32-CAM</h2>";
  html += "<form method='POST' action='/wifi'>";
  html += "SSID: <input name='ssid'><br>";
  html += "Password: <input name='password' type='password'><br>";
  html += "<input type='submit' value='Conectar'>";
  html += "</form></body></html>";
  server.send(200, "text/html", html);
}

int posW = 50;

void handleWifi() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");

  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), password.c_str());

  int cnt = 0;
  while (WiFi.status() != WL_CONNECTED && cnt < 10) {
    delay(1000);
    cnt++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    String varsave = leerStringDeEEPROM(300);
    if (varsave == "a") { posW = 0; escribirStringEnEEPROM(300, "b"); }
    else               { posW = 50; escribirStringEnEEPROM(300, "a"); }

    escribirStringEnEEPROM(0 + posW, ssid);
    escribirStringEnEEPROM(100 + posW, password);
    server.send(200, "text/plain", "Conectado correctamente");
  } else {
    server.send(200, "text/plain", "No se pudo conectar");
  }
}

bool lastRed() {
  for (int psW = 0; psW <= 50; psW += 50) {
    String usu = leerStringDeEEPROM(0 + psW);
    String cla = leerStringDeEEPROM(100 + psW);
    if (usu.length() == 0) continue;

    WiFi.disconnect();
    WiFi.begin(usu.c_str(), cla.c_str());

    int cnt = 0;
    while (WiFi.status() != WL_CONNECTED && cnt < 8) {
      delay(1000);
      cnt++;
    }
    if (WiFi.status() == WL_CONNECTED) return true;
  }
  return false;
}

void initAP(const char *apSsid, const char *apPassword) {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid, apPassword);
  server.on("/", handleRoot);
  server.on("/wifi", handleWifi);
  server.begin();
}

void loopAP() { server.handleClient(); }

void intentoconexion(const char *apname, const char *appassword) {
  EEPROM.begin(EEPROM_SIZE);

  if (!lastRed()) {
    Serial.println("❌ No se pudo conectar a WiFi guardado");
    Serial.println("📡 Creando AP...");
    initAP(apname, appassword);

    while (WiFi.status() != WL_CONNECTED) {
      loopAP();
      delay(10);
    }
  }

  Serial.println("✅ WiFi conectado");
  Serial.println(WiFi.localIP());
}

// ===================== PROYECTO ESP32-CAM =====================
#define RELAY_PIN 12
#define flashLed 4

// --- Pines cámara ---
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// --- Telegram ---
String token = "7998500572:AAFCKRu1cITqLfJF_kB6neVoodaxzZmFR-c";
String idChat = "5998082378";

// --- Flask dinámico ---
String serverUrl = "http://0.0.0.0:5000/recognize";

WiFiClientSecure clientTCP;
UniversalTelegramBot bot(token, clientTCP);

bool wifiConnected = false;
bool flashEstado = LOW;

// ===================== PROTOTIPOS =====================
void iniciarCamara();
void manejarMensajes(int nuevoMensajes);
void abrirPuerta();
void cerrarPuerta();
String sendPhotoTelegram();
String sendImageToServer(const char* action);
void revisarUART_RPI();
void cargarServerUrlDesdeEEPROM();

// ===================== ISR BOTÓN =====================
void IRAM_ATTR handleButtonInterrupt() {
  unsigned long now = millis();

  if ((now - lastInterruptTime > DEBOUNCE_TIME) && (countOpenDoor == 0)) {
    requestOpenDoor = true;
    countOpenDoor = 1;
    lastInterruptTime = now;
  }
}

// ========================= SETUP =========================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonInterrupt, FALLING);
  pinMode(flashLed, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(flashLed, LOW);
  digitalWrite(RELAY_PIN, HIGH);

  Serial.begin(115200);
  SerialRPI.begin(9600, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  EEPROM.begin(EEPROM_SIZE);

  Serial.println("🚀 Iniciando ESP32-CAM");

  cargarServerUrlDesdeEEPROM();

  intentoconexion("ESP32_CAM_SETUP", "12345678");
  wifiConnected = true;

  clientTCP.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  bot.sendMessage(idChat, "✅ ESP32-CAM conectada y lista", "");

  iniciarCamara();
}

// ========================= LOOP =========================
void loop() {
  unsigned long now = millis();

  if (requestOpenDoor && (countOpenDoor == 1)) {
    requestOpenDoor = false;   // IMPORTANTE: limpiar bandera
    Serial.println("🔘 Botón presionado → Abriendo puerta");
    abrirPuerta();
  }

  if (now - lastUartCheck >= UART_INTERVAL) {
    lastUartCheck = now;
    revisarUART_RPI();
  }

  if (wifiConnected && now - lastTelegramCheck >= TG_INTERVAL) {
    lastTelegramCheck = now;
    int nuevos = bot.getUpdates(bot.last_message_received + 1);
    if (nuevos) manejarMensajes(nuevos);
  }
}

// ===================== EEPROM: URL =====================
void cargarServerUrlDesdeEEPROM() {
  String url = leerStringDeEEPROM(EEPROM_ADDR_SERVERURL);

  if (url.length() > 10 && url.startsWith("http")) {
    serverUrl = url;
    Serial.println("📥 URL Flask cargada desde EEPROM:");
    Serial.println(serverUrl);
  } else {
    Serial.println("ℹ No hay URL guardada en EEPROM, usando por defecto");
  }
}

// ===================== UART =====================
void revisarUART_RPI() {
  if (!SerialRPI.available()) return;

  String msg = SerialRPI.readStringUntil('\n');
  msg.trim();

  if (msg.startsWith("IP:")) {
    String ip = msg.substring(3);
    serverUrl = "http://" + ip + ":5000/recognize";

    escribirStringEnEEPROM(EEPROM_ADDR_SERVERURL, serverUrl);

    bot.sendMessage(idChat,
      "🔄 IP del servidor actualizada:\n" + serverUrl, "");
  }
}

// ========================= CÁMARA =========================
void iniciarCamara() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 20;
    config.fb_count = 1;
  }

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("❌ Error iniciando la cámara");
    ESP.restart();
  } else {
    Serial.println("📸 Cámara iniciada correctamente.");
  }
}

// ========================= RESTO DE FUNCIONES =========================
void manejarMensajes(int nuevoMensajes) {
  for (int i = 0; i < nuevoMensajes; i++) {
    String text = bot.messages[i].text;
    String chat_id = bot.messages[i].chat_id;

    if (chat_id != idChat) {
      bot.sendMessage(chat_id, "Acceso no autorizado.", "");
      continue;
    }

    if (text == "/start" || text == "/inicio") {
      String menu = "Control de acceso ESP32-CAM:\n";
      menu += "/foto - Tomar y enviar foto\n";
      menu += "/flash - Encender/Apagar flash\n";
      menu += "/abrir - Desbloquear cerradura\n";
      menu += "/cerrar - Bloquear cerradura\n";
      menu += "/reconocer - Enviar foto al servidor Flask\n";
      menu += "/ip - Mostrar IP/URL del servidor Flask\n";
      bot.sendMessage(chat_id, menu, "");
    }

    if (text == "/foto") {
      bot.sendMessage(chat_id, "📸 Tomando foto...", "");
      sendPhotoTelegram();
    }

    if (text == "/flash") {
      flashEstado = !flashEstado;
      digitalWrite(flashLed, flashEstado);
      bot.sendMessage(chat_id, flashEstado ? "💡 Flash encendido." : "💤 Flash apagado.", "");
    }

    if (text == "/ip") {
      String urlGuardada = leerStringDeEEPROM(EEPROM_ADDR_SERVERURL);

      if (urlGuardada.length() > 10) {
        bot.sendMessage(chat_id,
          "🌐 URL del servidor Flask guardada:\n" + urlGuardada, "");
      } else {
        bot.sendMessage(chat_id,
          "⚠️ No hay una URL guardada en la EEPROM.", "");
      }
    }

    if (text == "/abrir") abrirPuerta();
    if (text == "/cerrar") cerrarPuerta();

    if (text == "/reconocer") {
      bot.sendMessage(chat_id, "📡 Enviando imagen al servidor Flask...", "");
      String res = sendImageToServer("recognize");
      if (res.indexOf("\"authorized\":true") >= 0 || res.indexOf("\"authorized\": true") >= 0) {
        bot.sendMessage(chat_id, "✅ Rostro autorizado. Abriendo cerradura...", "");
        abrirPuerta();
      } else {
        bot.sendMessage(chat_id, "🚫 Rostro no autorizado detectado.", "");
      }
    }
  }
}

void abrirPuerta() {
  digitalWrite(RELAY_PIN, LOW);
  bot.sendMessage(idChat, "🔓 Cerradura desbloqueada.", "");
  delay(3000);
  cerrarPuerta();
}

void cerrarPuerta() {
  digitalWrite(RELAY_PIN, HIGH);
  bot.sendMessage(idChat, "🔒 Cerradura bloqueada.", "");
  countOpenDoor = 0;
}

String sendPhotoTelegram() {
  const char* host = "api.telegram.org";
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("❌ Error al capturar la imagen");
    return "Error captura";
  }

  if (clientTCP.connect(host, 443)) {
    String head = "--BOUNDARY\r\nContent-Disposition: form-data; name=\"chat_id\";\r\n\r\n" + idChat +
                  "\r\n--BOUNDARY\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"foto.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
    String tail = "\r\n--BOUNDARY--\r\n";

    clientTCP.println("POST /bot" + token + "/sendPhoto HTTP/1.1");
    clientTCP.println("Host: " + String(host));
    clientTCP.println("Content-Length: " + String(fb->len + head.length() + tail.length()));
    clientTCP.println("Content-Type: multipart/form-data; boundary=BOUNDARY");
    clientTCP.println();
    clientTCP.print(head);

    clientTCP.write(fb->buf, fb->len);
    clientTCP.print(tail);

    esp_camera_fb_return(fb);
    bot.sendMessage(idChat, "📤 Foto enviada con éxito.", "");
  } else {
    bot.sendMessage(idChat, "❌ Error al conectar con Telegram API.", "");
  }

  clientTCP.stop();
  return "OK";
}

String sendImageToServer(const char* action) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("❌ Captura fallida");
    return "{\"error\":\"capture_failed\"}";
  }

  HTTPClient http;
  http.begin(serverUrl);
  http.addHeader("Content-Type", "image/jpeg");
   http.setTimeout(15000);

  int httpCode = http.POST(fb->buf, fb->len);
  String response = http.getString();

  esp_camera_fb_return(fb);
  http.end();

  Serial.printf("📨 Código HTTP: %d\n", httpCode);
  Serial.println("📩 Respuesta del servidor: " + response);
  return response;
}

