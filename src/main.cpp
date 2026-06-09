#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <dscKeybusInterface.h>
#include <ArduinoJson.h>
#include <time.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <Preferences.h>  // ← NUEVO

// ---------- INCLUIR CREDENCIALES ----------
#include "credentials.h"

// ---------- CONFIGURACION DINAMICA ----------
String deviceId = "dsc_" + String((uint32_t)ESP.getEfuseMac(), HEX);

// ---------- NUEVO: GESTION DE CONFIGURACION ----------
Preferences preferences;


// ---------- TOPICOS DINAMICOS ----------
String TOPIC_STATE;
String TOPIC_EVENT;

// ---------- PINES KEYBUS ----------
#define DSC_CLOCK_PIN 18
#define DSC_READ_PIN  19
#define DSC_WRITE_PIN 21

// ---------- BOTON PARA RESET WIFI ----------
#define WIFI_RESET_BUTTON_PIN 0
#define BUTTON_PRESS_TIME 5000
unsigned long buttonPressStart = 0;
bool buttonPressed = false;
bool resetWiFiConfig = false;

// ---------- TIMERS ----------
const unsigned long PUBLISH_INTERVAL = 200;
unsigned long lastPublishTime = 0;
bool pendingUpdate = false;

// ---------- NTP ----------
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -18000;
const int   DAYLIGHT_OFFSET_SEC = 0;
unsigned long lastNTPSync = 0;
const unsigned long NTP_SYNC_INTERVAL = 3600000;
bool timeInitialized = false;

// ---------- OBJETOS ----------
dscKeybusInterface dsc(DSC_CLOCK_PIN, DSC_READ_PIN, DSC_WRITE_PIN);
WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);
StaticJsonDocument<512> jsonDoc;
unsigned long lastReconnectAttempt = 0;
unsigned long lastWiFiCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 60000;

// ---------- ESTRUCTURA DE CAMBIOS ----------
struct StateChange {
  bool partitionChanged[4];
  bool zonesChanged;
  bool keybusChanged;
  
  void reset() {
    memset(partitionChanged, 0, sizeof(partitionChanged));
    zonesChanged = false;
    keybusChanged = false;
  }
};

StateChange stateChange;
unsigned long lastDebugPrint = 0;
const unsigned long DEBUG_PRINT_INTERVAL = 30000;

// ---------- PROTOTIPOS ----------
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishState();
void publishEvent(const char* eventType, byte zone = 0, byte partition = 0);
bool mqttConnect();
void updateStateJson();
void setupTopics();
void printDebugInfo();
void initNTP();
String getFormattedTime();
unsigned long getUnixTimestamp();
bool setupWiFi();
void checkWiFiConnection();
void checkResetButton();
void saveConfig();   // NUEVO
void loadConfig();   // NUEVO

// ===============================
// NUEVAS FUNCIONES PARA CONFIGURACION
// ===============================
void saveConfig() {
  preferences.begin("dsc_config", false);
  preferences.putString("access_code", customAccessCode);
  preferences.end();
  Serial.println("[Config] Código guardado en flash");
}

void loadConfig() {
  preferences.begin("dsc_config", true);
  String savedCode = preferences.getString("access_code", "1234");
  strcpy(customAccessCode, savedCode.c_str());
  preferences.end();
  Serial.printf("[Config] Código cargado: %s\n", customAccessCode);
}

// ===============================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=========================================");
  Serial.println("Iniciando sistema DSC MQTT");
  Serial.println("=========================================");

  // Cargar código guardado
  loadConfig();

  pinMode(WIFI_RESET_BUTTON_PIN, INPUT_PULLUP);
  Serial.println("[Botón] Mantener 5 segundos para reset completo");

  if (digitalRead(WIFI_RESET_BUTTON_PIN) == LOW) {
    Serial.println("[Botón] Reset completo activado");
    preferences.begin("dsc_config", false);
    preferences.clear();
    preferences.end();
    resetWiFiConfig = true;
  }

  setupTopics();

  if (!setupWiFi()) {
    Serial.println("[WiFi] Error crítico, reiniciando...");
    delay(3000);
    ESP.restart();
  }

  initNTP();

  Serial.println("\n[MQTT] Configurando cliente...");
  secureClient.setCACert(MQTT_CA_CERT);
  secureClient.setHandshakeTimeout(30);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);

  Serial.println("\n[OTA] Configurando actualizaciones...");
  ArduinoOTA.setHostname(deviceId.c_str());
  ArduinoOTA.setPassword("1234");
  
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) type = "sketch";
    else type = "filesystem";
    Serial.println("\n[OTA] Iniciando actualización de " + type);
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] Actualización completada!");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] Progreso: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("\n[OTA] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Error de autenticación");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Error al iniciar");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Error de conexión");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Error al recibir");
    else if (error == OTA_END_ERROR) Serial.println("Error al finalizar");
  });
  
  ArduinoOTA.begin();
  Serial.println("[OTA] Listo");

  Serial.println("\n[DSC] Iniciando comunicación con Keybus...");
  dsc.begin();
  Serial.println("[DSC] Sistema listo");
  
  Serial.println("\n=========================================");
  Serial.println("Sistema iniciado correctamente!");
  Serial.printf("Código de acceso: %s\n", customAccessCode);
  Serial.println("=========================================\n");
  
  publishEvent("system_start", 0, 0);
}

// ===============================
bool setupWiFi() {
  Serial.println("\n[WiFi] Configurando WiFi Manager...");
  
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(300);
  
  String apName = "DSC_" + deviceId.substring(deviceId.length() - 6);
  
  // Parámetro para el código DSC
  char codeHint[50];
  snprintf(codeHint, sizeof(codeHint), "Código DSC (4-6 dígitos) - Actual: %s", customAccessCode);
  WiFiManagerParameter dsc_code_param("dsc_code", codeHint, customAccessCode, 7);
  wifiManager.addParameter(&dsc_code_param);
  
  Serial.print("[WiFi] AP Name: ");
  Serial.println(apName);
  
  wifiManager.setSaveParamsCallback([&]() {
    Serial.println("[WiFiManager] Configuración recibida, guardando código...");
    strcpy(customAccessCode, dsc_code_param.getValue());
    saveConfig();
    Serial.printf("[WiFiManager] Nuevo código: %s\n", customAccessCode);
  });
  
  if (resetWiFiConfig) {
    Serial.println("[WiFi] Reseteando configuración guardada...");
    wifiManager.resetSettings();
  }
  
  if (wifiManager.autoConnect(apName.c_str(), "dsc12345")) {
    Serial.println("[WiFi] Conectado exitosamente!");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("[WiFi] Error: No se pudo conectar");
    return false;
  }
}

// ===============================
void checkResetButton() {
  bool buttonState = (digitalRead(WIFI_RESET_BUTTON_PIN) == LOW);
  
  if (buttonState && !buttonPressed) {
    buttonPressed = true;
    buttonPressStart = millis();
    Serial.println("[Botón] Mantenga 5 segundos para reset completo...");
  } 
  else if (!buttonState && buttonPressed) {
    if (millis() - buttonPressStart < BUTTON_PRESS_TIME) {
      Serial.println("[Botón] Reset cancelado");
    }
    buttonPressed = false;
  }
  
  if (buttonPressed && (millis() - buttonPressStart >= BUTTON_PRESS_TIME)) {
    Serial.println("\n[Botón] RESET COMPLETO!");
    publishEvent("factory_reset", 0, 0);
    delay(500);
    
    preferences.begin("dsc_config", false);
    preferences.clear();
    preferences.end();
    
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    
    delay(2000);
    ESP.restart();
  }
}

// ===============================
void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Conexión perdida, reconectando...");
    WiFi.reconnect();
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      attempts++;
      Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n[WiFi] Reconectado!");
      publishEvent("wifi_reconnected", 0, 0);
    } else {
      Serial.println("\n[WiFi] Error: No se pudo reconectar");
    }
  }
}

// ===============================
void setupTopics() {
  TOPIC_STATE = "dsc/" + deviceId + "/state";
  TOPIC_EVENT = "dsc/" + deviceId + "/event";
  
  Serial.println("=== Topics configurados ===");
  Serial.print("State: "); Serial.println(TOPIC_STATE);
  Serial.print("Event: "); Serial.println(TOPIC_EVENT);
  Serial.print("Device ID: "); Serial.println(deviceId);
}

// ===============================
void initNTP() {
  Serial.println("\n[NTP] Inicializando...");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  
  int attempts = 0;
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo, 1000) && attempts < 10) {
    Serial.print(".");
    attempts++;
    delay(1000);
  }
  
  if (attempts < 10) {
    timeInitialized = true;
    Serial.println("\n[NTP] Sincronizado!");
  } else {
    Serial.println("\n[NTP] No sincronizado, usando uptime");
  }
}

// ===============================
String getFormattedTime() {
  if (!timeInitialized) return String(millis());
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return String(millis());
  char timeString[64];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeString);
}

// ===============================
unsigned long getUnixTimestamp() {
  if (!timeInitialized) return millis() / 1000;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return millis() / 1000;
  time_t t = mktime(&timeinfo);
  return (unsigned long)t;
}

// ===============================
void loop() {
  checkResetButton();
  
  if (millis() - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
    lastWiFiCheck = millis();
    checkWiFiConnection();
  }
  
  ArduinoOTA.handle();
  
  if (millis() - lastNTPSync >= NTP_SYNC_INTERVAL) {
    lastNTPSync = millis();
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 2000)) {
      timeInitialized = true;
    }
  }
  
  if (millis() - lastDebugPrint >= DEBUG_PRINT_INTERVAL) {
    lastDebugPrint = millis();
    printDebugInfo();
  }
  
  if (!mqtt.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      if (mqttConnect()) {
        publishState();
      }
    }
  } else {
    mqtt.loop();
  }

  dsc.loop();

  if (dsc.statusChanged) {
    dsc.statusChanged = false;
    stateChange.keybusChanged = true;
    pendingUpdate = true;
  }

  if (dsc.accessCodePrompt) {
    dsc.accessCodePrompt = false;
    dsc.write(customAccessCode);  // ← SOLO CAMBIO: antes era ACCESS_CODE
    Serial.println("[DSC] Enviando código de acceso");
  }

  for (byte p = 0; p < 8; p++) {  // ✅ CAMBIADO: 4 -> 8
    if (dsc.disabled[p]) continue;

    if (dsc.armedChanged[p] || dsc.alarmChanged[p] || dsc.readyChanged[p]) {
      stateChange.partitionChanged[p] = true;
      pendingUpdate = true;
      
      if (dsc.armedChanged[p]) {
        if (dsc.armed[p]) {
          // Armando - diferenciar AWAY/STAY
          const char* eventType = dsc.armedAway[p] ? "armed" : "stay_armed";
          publishEvent(eventType, 0, p);
          Serial.printf("[DSC] Evento: Partición %d %s\n", p + 1, eventType);
        } else {
          // Desarmando
          publishEvent("disarmed", 0, p);
          Serial.printf("[DSC] Evento: Partición %d DESARMADA\n", p + 1);
        }
      }
      
      if (dsc.alarmChanged[p] && dsc.alarm[p]) {
        publishEvent("alarm", 0, p);
        Serial.printf("[DSC] Evento: ALARMA en partición %d\n", p + 1);
      }
      
      dsc.armedChanged[p] = false;
      dsc.alarmChanged[p] = false;
      dsc.readyChanged[p] = false;
    }
  }

  if (dsc.openZonesStatusChanged) {
    dsc.openZonesStatusChanged = false;
    stateChange.zonesChanged = true;
    pendingUpdate = true;
    
    for (byte group = 0; group < 4; group++) {
      for (byte bit = 0; bit < 8; bit++) {
        if (!bitRead(dsc.openZonesChanged[group], bit)) continue;
        bitWrite(dsc.openZonesChanged[group], bit, 0);
        byte zone = group * 8 + bit + 1;
        if (zone > 30) continue;
        
        bool open = bitRead(dsc.openZones[group], bit);
        const char* eventType = open ? "zone_open" : "zone_closed";
        publishEvent(eventType, zone, 0);
      }
    }
  }

  if (pendingUpdate && (millis() - lastPublishTime >= PUBLISH_INTERVAL)) {
    publishState();
    pendingUpdate = false;
    stateChange.reset();
    lastPublishTime = millis();
  }
}

// ===============================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_EVENT.c_str()) != 0) return;
  
  StaticJsonDocument<128> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.println("[MQTT] Error parsing command");
    return;
  }
  
  const char* cmd = doc["cmd"];
  byte partition = doc["partition"] | 1;
  
  if (!cmd) return;
  partition--;
  if (partition > 3) return;
  
  Serial.printf("[MQTT] Comando: %s para partición %d\n", cmd, partition + 1);
  
  if (strcmp(cmd, "arm") == 0 && dsc.ready[partition]) {
    dsc.writePartition = partition + 1;
    dsc.write('w');  // ← IGUAL QUE ORIGINAL
    publishEvent("arming", 0, partition);
  }
  else if (strcmp(cmd, "stay") == 0 && dsc.ready[partition]) {
    dsc.writePartition = partition + 1;
    dsc.write('s');  // ← IGUAL QUE ORIGINAL
    publishEvent("stay_arming", 0, partition);
  }
  else if (strcmp(cmd, "disarm") == 0 && (dsc.armed[partition] || dsc.alarm[partition])) {
    dsc.writePartition = partition + 1;
    dsc.write(customAccessCode);  // ← SOLO CAMBIO: antes era ACCESS_CODE
    publishEvent("disarming", 0, partition);
  }
  else if (strcmp(cmd, "reset_wifi") == 0) {
    Serial.println("[MQTT] Comando reset_wifi recibido");
    publishEvent("wifi_reset_remote", 0, 0);
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    delay(1000);
    ESP.restart();
  }
}

// ===============================
void publishState() {
  if (!mqtt.connected()) return;
  
  jsonDoc.clear();
  updateStateJson();
  
  char buffer[512];
  size_t len = serializeJson(jsonDoc, buffer);
  mqtt.publish(TOPIC_STATE.c_str(), (const uint8_t*)buffer, len, true);
}

// ===============================
void publishEvent(const char* eventType, byte zone, byte partition) {
  if (!mqtt.connected()) return;
  
  jsonDoc.clear();
  jsonDoc["event"] = eventType;
  jsonDoc["timestamp"] = getUnixTimestamp();
  
  if (zone > 0) jsonDoc["zone"] = zone;
  
  // ✅ Incluir partición (0 = partición 1, 1 = partición 2, etc.)
  if (partition <= 7) {
    jsonDoc["partition"] = partition + 1;
  }
  
  char buffer[256];
  size_t len = serializeJson(jsonDoc, buffer);
  mqtt.publish(TOPIC_EVENT.c_str(), (const uint8_t*)buffer, len, true);
}

// ===============================
void updateStateJson() {
  jsonDoc["device_id"] = deviceId;
  jsonDoc["timestamp"] = getUnixTimestamp();
  jsonDoc["keybus"] = dsc.keybusConnected ? 1 : 0;
  jsonDoc["wifi_ssid"] = WiFi.SSID();
  jsonDoc["rssi"] = WiFi.RSSI();
  
  JsonArray partitions = jsonDoc.createNestedArray("partitions");
  for (byte p = 0; p < 4; p++) {
    if (dsc.disabled[p]) {
      JsonObject emptyPart = partitions.createNestedObject();
      emptyPart["disabled"] = true;
      continue;
    }
    
    JsonObject part = partitions.createNestedObject();
    
    if (dsc.alarm[p]) part["state"] = "alarm";
    else if (!dsc.armed[p]) part["state"] = "disarmed";
    else if (dsc.armedAway[p]) part["state"] = "armed_away";
    else if (dsc.armedStay[p]) part["state"] = "armed_home";
    
    part["ready"] = dsc.ready[p] ? 1 : 0;
  }
  
  JsonArray openZones = jsonDoc.createNestedArray("open_zones");
  for (byte zone = 1; zone <= 30; zone++) {
    byte group = (zone - 1) / 8;
    byte bit   = (zone - 1) % 8;
    
    if (bitRead(dsc.openZones[group], bit)) {
      openZones.add(zone);
    }
  }
}

// ===============================
bool mqttConnect() {
  char lwtMessage[128];
  snprintf(lwtMessage, sizeof(lwtMessage), "{\"keybus\":0,\"timestamp\":%lu}", getUnixTimestamp());
  
  const char* MQTT_USER = deviceId.c_str();
  
  Serial.printf("[MQTT] Conectando a %s:%d como %s\n", MQTT_HOST, MQTT_PORT, deviceId.c_str());
  
  if (mqtt.connect(deviceId.c_str(), MQTT_USER, MQTT_PASS,
                   TOPIC_STATE.c_str(), 0, true, lwtMessage)) {
    
    Serial.println("[MQTT] Conexión exitosa!");
    publishState();
    mqtt.subscribe(TOPIC_EVENT.c_str());
    return true;
  }
  
  Serial.printf("[MQTT] Error, rc=%d\n", mqtt.state());
  return false;
}

// ===============================
void printDebugInfo() {
  Serial.println("\n========== DEBUG INFO ==========");
  Serial.printf("Uptime: %lu segundos\n", millis() / 1000);
  Serial.printf("WiFi: %s (RSSI: %d dBm)\n", 
                WiFi.status() == WL_CONNECTED ? "Conectado" : "Desconectado", 
                WiFi.RSSI());
  Serial.printf("MQTT: %s\n", mqtt.connected() ? "Conectado" : "Desconectado");
  Serial.printf("Keybus: %s\n", dsc.keybusConnected ? "Online" : "Offline");
  Serial.printf("Código DSC: %s\n", customAccessCode);
  Serial.printf("Memoria libre: %d bytes\n", ESP.getFreeHeap());
  Serial.println("================================\n");
}