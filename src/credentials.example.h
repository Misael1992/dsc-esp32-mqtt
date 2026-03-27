// credentials.example.h
// Archivo de ejemplo - Copiar a credentials.h y completar con tus datos

#ifndef CREDENTIALS_H
#define CREDENTIALS_H

// ---------- CONFIGURACION MQTT ----------
const char* MQTT_HOST     = "tu_broker.emqxsl.com";
const int   MQTT_PORT     = 8883;
const char* MQTT_PASS     = "tu_mqtt_password";

// ---------- CONFIGURACION DSC ----------
const char* ACCESS_CODE   = "1234";

// ---------- CERTIFICADO CA ----------
// Copiar aquí el certificado CA de tu broker MQTT
const char* MQTT_CA_CERT =
"-----BEGIN CERTIFICATE-----\n" \
"PEGA_AQUI_TU_CERTIFICADO_CA\n" \
"-----END CERTIFICATE-----\n";

#endif // CREDENTIALS_H