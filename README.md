# DSC Alarm to MQTT Bridge for ESP32

[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32-orange)](https://platformio.org/)
[![ESP32](https://img.shields.io/badge/ESP32-2.0.17-blue)](https://www.espressif.com/)
[![MQTT](https://img.shields.io/badge/MQTT-EMQX-green)](https://www.emqx.com/)

Sistema para conectar paneles de alarma DSC (PowerSeries) a MQTT usando ESP32. Permite monitorear y controlar tu sistema de alarma desde cualquier plataforma compatible con MQTT (Home Assistant, Node-RED, etc.).

## 🚀 Características

- ✅ **Configuración WiFi fácil** - Portal captivo para configurar WiFi sin programar
- ✅ **Botón de reset WiFi** - Mantén presionado GPIO0 por 3 segundos para reconfigurar
- ✅ **Actualizaciones OTA** - Sube nuevo firmware sin cables USB
- ✅ **Timestamp real** - Sincronización NTP con zona horaria configurable
- ✅ **MQTT seguro** - Conexión TLS/SSL con EMQX Cloud
- ✅ **Tópicos dinámicos** - Cada dispositivo tiene tópicos únicos por MAC address
- ✅ **Eventos en tiempo real** - Notificaciones inmediatas de cambios importantes
- ✅ **Estado agrupado** - Optimización de mensajes para reducir tráfico de red
- ✅ **Debug completo** - Logs detallados en monitor serial

## 📋 Requisitos

### Hardware
- ESP32 Dev Board (cualquier modelo)
- DSC Keybus Interface (pines 18, 19, 21)
- Panel DSC PowerSeries (PC1832, PC1864, etc.)
- Botón externo (opcional, para GPIO0)

### Software
- PlatformIO IDE (recomendado) o Arduino IDE
- EMQX Cloud (u otro broker MQTT con TLS)
- Cuenta en EMQX Cloud (gratuita)

## 🔌 Conexiones

| ESP32 Pin |                   Conexión                        |
|-----------|---------------------------------------------------|
| GPIO 18   | DSC Clock                                         |
| GPIO 19   | DSC Read                                          |
| GPIO 21   | DSC Write                                         |
| GPIO 0    | Botón Reset WiFi (opcional, con pull-up interno)  |
| GND       | GND común                                         |


