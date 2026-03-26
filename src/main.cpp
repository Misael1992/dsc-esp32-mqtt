#include <Arduino.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <dscKeybusInterface.h>
#include <ArduinoJson.h>
#include <time.h>
#include <ArduinoOTA.h>

// ---------- INCLUIR CREDENCIALES ----------
#include "credentials.h"

// ---------- CONFIGURACION DINAMICA ----------
// deviceId se genera automáticamente desde la MAC
String deviceId = "dsc_" + String((uint32_t)ESP.getEfuseMac(), HEX);
const char* MQTT_USER = deviceId.c_str();  // Username = deviceId

// ---------- TOPICOS DINAMICOS CON DEVICEID ----------
String TOPIC_STATE;      // dsc/{deviceId}/state
String TOPIC_EVENT;      // dsc/{deviceId}/event

// ---------- PINES KEYBUS ----------
#define DSC_CLOCK_PIN 18
#define DSC_READ_PIN  19
#define DSC_WRITE_PIN 21

// ---------- TIMERS PARA AGRUPAR MENSAJES ----------
const unsigned long PUBLISH_INTERVAL = 200;  // 200ms de intervalo para agrupar cambios
unsigned long lastPublishTime = 0;
bool pendingUpdate = false;

// ---------- CONFIGURACION NTP AVANZADA ----------
// Zona horaria para Ecuador (UTC-5)
// Formato: "EST5" para UTC-5 sin horario de verano
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = -18000;  // UTC-5 para Ecuador
const int   DAYLIGHT_OFFSET_SEC = 0;  // Ecuador NO tiene horario de verano

unsigned long lastNTPSync = 0;
const unsigned long NTP_SYNC_INTERVAL = 3600000; // Sincronizar cada hora
bool timeInitialized = false;

// ---------- OBJETOS ----------
dscKeybusInterface dsc(DSC_CLOCK_PIN, DSC_READ_PIN, DSC_WRITE_PIN);
WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);
StaticJsonDocument<512> jsonDoc;
unsigned long lastReconnectAttempt = 0;

// Variables para detectar cambios específicos
struct StateChange {
  bool partitionChanged[4];
  bool zonesChanged;
  bool keybusChanged;
  
  StateChange() {
    reset();
  }
  
  void reset() {
    memset(partitionChanged, 0, sizeof(partitionChanged));
    zonesChanged = false;
    keybusChanged = false;
  }
};

StateChange stateChange;

// Variables para debug
unsigned long lastDebugPrint = 0;
const unsigned long DEBUG_PRINT_INTERVAL = 30000; // Debug cada 30 segundos

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

// ===============================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=========================================");
  Serial.println("Iniciando sistema DSC MQTT");
  Serial.println("=========================================");

  // Configurar tópicos con deviceId único
  setupTopics();

  // Conexión WiFi
  Serial.println("\n[WiFi] Conectando a red...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Conectado exitosamente!");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WiFi] MAC: ");
    Serial.println(WiFi.macAddress());
  } else {
    Serial.println("\n[WiFi] Error: No se pudo conectar");
  }

  // Inicializar NTP (versión mejorada)
  initNTP();

  // Configuración MQTT
  Serial.println("\n[MQTT] Configurando cliente...");
  secureClient.setCACert(MQTT_CA_CERT);
  secureClient.setHandshakeTimeout(30);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);

  // Configurar OTA
  Serial.println("\n[OTA] Configurando actualizaciones Over-The-Air...");
  ArduinoOTA.setHostname(deviceId.c_str());
  ArduinoOTA.setPassword("1234");
  
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {
      type = "filesystem";
    }
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
  Serial.println("[OTA] Listo - Esperando actualizaciones");

  // Iniciar comunicación con DSC
  Serial.println("\n[DSC] Iniciando comunicación con Keybus...");
  dsc.begin();
  Serial.println("[DSC] Sistema listo");
  
  Serial.println("\n=========================================");
  Serial.println("Sistema iniciado correctamente!");
  Serial.println("=========================================\n");
}

// ===============================
void initNTP() {
  Serial.println("\n[NTP] Inicializando cliente de tiempo...");
  Serial.println("[NTP] Zona horaria: Ecuador (UTC-5)");
  
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  
  // Esperar hasta 10 segundos para obtener la hora
  int attempts = 0;
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo, 1000) && attempts < 10) {
    Serial.print(".");
    attempts++;
    delay(1000);
  }
  
  if (attempts < 10) {
    timeInitialized = true;
    Serial.println("\n[NTP] Tiempo sincronizado correctamente!");
    char timeString[64];
    strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
    Serial.print("[NTP] Fecha/Hora: ");
    Serial.println(timeString);
  } else {
    timeInitialized = false;
    Serial.println("\n[NTP] Advertencia: No se pudo sincronizar, usando millis()");
  }
}

// ===============================
String getFormattedTime() {
  if (!timeInitialized) {
    return String(millis());
  }
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return String(millis());
  }
  
  char timeString[64];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeString);
}

// ===============================
unsigned long getUnixTimestamp() {
  if (!timeInitialized) {
    return millis() / 1000;
  }
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return millis() / 1000;
  }
  
  time_t t = mktime(&timeinfo);
  return (unsigned long)t;
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
void loop() {
  // Manejar OTA
  ArduinoOTA.handle();
  
  // Sincronizar NTP periódicamente
  if (millis() - lastNTPSync >= NTP_SYNC_INTERVAL) {
    lastNTPSync = millis();
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 2000)) {
      timeInitialized = true;
      Serial.println("[NTP] Tiempo resincronizado correctamente");
    } else {
      Serial.println("[NTP] Error al resincronizar");
    }
  }
  
  // Debug periódico
  if (millis() - lastDebugPrint >= DEBUG_PRINT_INTERVAL) {
    lastDebugPrint = millis();
    printDebugInfo();
  }
  
  // Manejo MQTT
  if (!mqtt.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      if (mqttConnect()) {
        publishState();
        Serial.println("[MQTT] Reconectado exitosamente");
      }
    }
  } else {
    mqtt.loop();
  }

  dsc.loop();

  // Detectar cambios y marcar para actualización
  if (dsc.statusChanged) {
    dsc.statusChanged = false;
    stateChange.keybusChanged = true;
    pendingUpdate = true;
    Serial.println("[DSC] Cambio detectado: Estado del Keybus");
  }

  if (dsc.accessCodePrompt) {
    dsc.accessCodePrompt = false;
    dsc.write(ACCESS_CODE);
    Serial.println("[DSC] Enviando código de acceso al panel");
  }

  // Detectar cambios en particiones
  for (byte p = 0; p < 4; p++) {
    if (dsc.disabled[p]) continue;

    if (dsc.armedChanged[p] || dsc.alarmChanged[p] || dsc.readyChanged[p]) {
      stateChange.partitionChanged[p] = true;
      pendingUpdate = true;
      
      if (dsc.armedChanged[p]) {
        const char* eventType = dsc.armed[p] ? "armed" : "disarmed";
        publishEvent(eventType, 0, p);
        Serial.printf("[DSC] Evento: Partición %d %s\n", p + 1, eventType);
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

  // Detectar cambios en zonas
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
        Serial.printf("[DSC] Evento: Zona %d %s\n", zone, open ? "abierta" : "cerrada");
      }
    }
  }

  // Publicar estado agrupado si hay cambios pendientes
  if (pendingUpdate && (millis() - lastPublishTime >= PUBLISH_INTERVAL)) {
    publishState();
    pendingUpdate = false;
    stateChange.reset();
    lastPublishTime = millis();
    Serial.println("[MQTT] Estado publicado (cambios agrupados)");
  }
}

// ===============================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_EVENT.c_str()) != 0) return;
  
  StaticJsonDocument<128> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.println("[MQTT] Error al parsear comando JSON");
    return;
  }
  
  const char* cmd = doc["cmd"];
  byte partition = doc["partition"] | 1;
  
  if (!cmd) {
    Serial.println("[MQTT] Comando sin campo 'cmd'");
    return;
  }
  
  partition--;
  if (partition > 3) {
    Serial.println("[MQTT] Partición inválida");
    return;
  }
  
  Serial.printf("[MQTT] Comando recibido: %s para partición %d\n", cmd, partition + 1);
  
  if (strcmp(cmd, "arm") == 0 && dsc.ready[partition]) {
    dsc.writePartition = partition + 1;
    dsc.write('w');
    publishEvent("arming", 0, partition);
    Serial.printf("[DSC] Ejecutando ARM en partición %d\n", partition + 1);
  }
  else if (strcmp(cmd, "stay") == 0 && dsc.ready[partition]) {
    dsc.writePartition = partition + 1;
    dsc.write('s');
    publishEvent("stay_arming", 0, partition);
    Serial.printf("[DSC] Ejecutando STAY ARM en partición %d\n", partition + 1);
  }
  else if (strcmp(cmd, "disarm") == 0 && (dsc.armed[partition] || dsc.alarm[partition])) {
    dsc.writePartition = partition + 1;
    dsc.write(ACCESS_CODE);
    publishEvent("disarming", 0, partition);
    Serial.printf("[DSC] Ejecutando DISARM en partición %d\n", partition + 1);
  }
  else {
    Serial.printf("[MQTT] Comando '%s' ignorado (condiciones no cumplidas)\n", cmd);
  }
}

// ===============================
void publishState() {
  if (!mqtt.connected()) {
    Serial.println("[MQTT] No se puede publicar estado: desconectado");
    return;
  }
  
  jsonDoc.clear();
  updateStateJson();
  
  char buffer[512];
  size_t len = serializeJson(jsonDoc, buffer);
  
  if (mqtt.publish(TOPIC_STATE.c_str(), (const uint8_t*)buffer, len, true)) {
    // Serial.println("[MQTT] Estado publicado correctamente");
  } else {
    Serial.println("[MQTT] Error al publicar estado");
  }
}

// ===============================
void publishEvent(const char* eventType, byte zone, byte partition) {
  if (!mqtt.connected()) {
    Serial.println("[MQTT] No se puede publicar evento: desconectado");
    return;
  }
  
  jsonDoc.clear();
  jsonDoc["event"] = eventType;
  jsonDoc["timestamp"] = getUnixTimestamp();  // Usar timestamp Unix real
  
  if (zone > 0) jsonDoc["zone"] = zone;
  if (partition > 0) jsonDoc["partition"] = partition + 1;
  
  char buffer[256];
  size_t len = serializeJson(jsonDoc, buffer);
  
  if (mqtt.publish(TOPIC_EVENT.c_str(), (const uint8_t*)buffer, len, true)) {
    // Serial.printf("[MQTT] Evento '%s' publicado\n", eventType);
  } else {
    Serial.printf("[MQTT] Error al publicar evento '%s'\n", eventType);
  }
}

// ===============================
void updateStateJson() {
  jsonDoc["device_id"] = deviceId;
  jsonDoc["timestamp"] = getUnixTimestamp();  // Usar timestamp Unix real
  jsonDoc["keybus"] = dsc.keybusConnected ? 1 : 0;
  
  JsonArray partitions = jsonDoc.createNestedArray("partitions");
  for (byte p = 0; p < 4; p++) {
    if (dsc.disabled[p]) {
      JsonObject emptyPart = partitions.createNestedObject();
      emptyPart["disabled"] = true;
      continue;
    }
    
    JsonObject part = partitions.createNestedObject();
    
    if (dsc.alarm[p]) {
      part["state"] = "alarm";
    } else if (!dsc.armed[p]) {
      part["state"] = "disarmed";
    } else if (dsc.armedAway[p]) {
      part["state"] = "armed_away";
    } else if (dsc.armedStay[p]) {
      part["state"] = "armed_home";
    }
    
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
  const char* lwtMessage = "{\"keybus\":0}";
  
  Serial.printf("[MQTT] Intentando conectar a %s:%d como %s\n", MQTT_HOST, MQTT_PORT, deviceId.c_str());
  
  if (mqtt.connect(deviceId.c_str(), MQTT_USER, MQTT_PASS,
                   TOPIC_STATE.c_str(), 0, true, lwtMessage)) {
    
    Serial.println("[MQTT] Conexión exitosa!");
    publishState();
    mqtt.subscribe(TOPIC_EVENT.c_str());
    Serial.printf("[MQTT] Suscrito a: %s\n", TOPIC_EVENT.c_str());
    return true;
  }
  
  Serial.printf("[MQTT] Error de conexión, rc=%d\n", mqtt.state());
  return false;
}

// ===============================
void printDebugInfo() {
  Serial.println("\n========== DEBUG INFO ==========");
  Serial.printf("Uptime: %lu segundos\n", millis() / 1000);
  Serial.printf("WiFi: %s (RSSI: %d dBm)\n", WiFi.status() == WL_CONNECTED ? "Conectado" : "Desconectado", WiFi.RSSI());
  Serial.printf("MQTT: %s\n", mqtt.connected() ? "Conectado" : "Desconectado");
  Serial.printf("NTP: %s\n", timeInitialized ? "Sincronizado" : "No sincronizado");
  if (timeInitialized) {
    Serial.printf("Hora: %s\n", getFormattedTime().c_str());
  }
  Serial.printf("Keybus: %s\n", dsc.keybusConnected ? "Online" : "Offline");
  Serial.printf("Memoria libre: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Memoria PSRAM: %d bytes\n", ESP.getPsramSize());
  Serial.println("================================\n");
}