#ifndef BREWLINK_SECRETS_EXAMPLE_H
#define BREWLINK_SECRETS_EXAMPLE_H

// Copy this file to "secrets.h" and insert your real credentials.
// "secrets.h" is gitignored and will never be committed to version control.

#define WIFI_SSID           "Your_WiFi_SSID"
#define WIFI_PASSWORD       "Your_WiFi_Password"

#define MQTT_BROKER_URI     "mqtt://192.168.1.100:1883"
#define MQTT_USERNAME       "brewlink_user"
#define MQTT_PASSWORD       "brewlink_secret_password"

#define HTTP_RELAY_ENDPOINT "http://192.168.1.100:8080/api/telemetry"

// Optional static API token for external BeerXML/WebSocket/HTTP/MQTT client pairing.
// Leave empty ("") to auto-generate a cryptographic token in NVS during PIN pairing.
#define BREWLINK_API_TOKEN  ""

#endif // BREWLINK_SECRETS_EXAMPLE_H
