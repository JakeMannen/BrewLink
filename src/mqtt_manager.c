/**
 * @file mqtt_manager.c
 * @brief MQTT Client Subsystem Implementation for BrewLink Gateway
 */

#include "mqtt_manager.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config.h"
#include "config_store.h"

static const char *TAG = "MQTT_MGR";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_connected = false;
static bool s_should_run = false;
static mqtt_inbound_cmd_cb_t s_cmd_callback = NULL;
static mqtt_conn_status_cb_t s_conn_callback = NULL;
static SemaphoreHandle_t s_mqtt_mutex = NULL;

static char s_gateway_id[GATEWAY_ID_MAX_LEN] = {0};
static char s_lwt_topic[160] = {0};
static char s_client_id[32] = {0};
static char s_default_username[32] = {0};

static void clean_mac_string(const char *in_mac, char out_mac[13]) {
    if (in_mac == NULL || out_mac == NULL) {
        if (out_mac) out_mac[0] = '\0';
        return;
    }
    int out_idx = 0;
    for (int i = 0; in_mac[i] != '\0' && out_idx < 12; i++) {
        if (isxdigit((unsigned char)in_mac[i])) {
            out_mac[out_idx++] = (char)toupper((unsigned char)in_mac[i]);
        }
    }
    out_mac[out_idx] = '\0';
}

static void ensure_gateway_id_initialized(void) {
    if (s_gateway_id[0] == '\0') {
        uint8_t base_mac[6];
        if (esp_read_mac(base_mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            snprintf(s_gateway_id, sizeof(s_gateway_id), "%02X%02X%02X%02X%02X%02X",
                     base_mac[0], base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
        } else {
            strncpy(s_gateway_id, "BREWLINK0000", sizeof(s_gateway_id) - 1);
        }
    }
}

// Compute effective topic prefix incorporating paired mqtt_topic_key when paired
static void get_effective_prefix(const brewlink_config_t *cfg, char *out_prefix, size_t max_len) {
    const char *base = (cfg->mqtt_topic_prefix[0] != '\0') ? cfg->mqtt_topic_prefix : MQTT_TOPIC_GATEWAY_PREFIX;
    if (cfg->client_paired && cfg->mqtt_topic_key[0] != '\0') {
        size_t base_len = strlen(base);
        bool has_slash = (base_len > 0 && base[base_len - 1] == '/');
        snprintf(out_prefix, max_len, "%s%s%s/", base, has_slash ? "" : "/", cfg->mqtt_topic_key);
    } else {
        snprintf(out_prefix, max_len, "%s", base);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "MQTT connected to broker (Gateway ID: %s)", s_gateway_id);
            s_connected = true;

            brewlink_config_t cfg;
            config_store_get(&cfg);
            const char *base_prefix = (cfg.mqtt_topic_prefix[0] != '\0') ? cfg.mqtt_topic_prefix : MQTT_TOPIC_GATEWAY_PREFIX;
            char eff_prefix[128];
            get_effective_prefix(&cfg, eff_prefix, sizeof(eff_prefix));

            char sub_topic[192];
            // Subscribe to paired topic namespace: brewlink/{topic_key}/{gw_id}/equipment/+/cmd
            snprintf(sub_topic, sizeof(sub_topic), "%s%s/equipment/+/cmd", eff_prefix, s_gateway_id);
            esp_mqtt_client_subscribe(s_mqtt_client, sub_topic, MQTT_QOS_COMMAND);
            ESP_LOGI(TAG, "Subscribed to: %s", sub_topic);

            snprintf(sub_topic, sizeof(sub_topic), "%s%s/cmd", eff_prefix, s_gateway_id);
            esp_mqtt_client_subscribe(s_mqtt_client, sub_topic, MQTT_QOS_COMMAND);

            // Also subscribe to base prefix if different (HMAC verification still protects /cmd)
            if (strcmp(eff_prefix, base_prefix) != 0) {
                snprintf(sub_topic, sizeof(sub_topic), "%s%s/equipment/+/cmd", base_prefix, s_gateway_id);
                esp_mqtt_client_subscribe(s_mqtt_client, sub_topic, MQTT_QOS_COMMAND);
                snprintf(sub_topic, sizeof(sub_topic), "%s%s/cmd", base_prefix, s_gateway_id);
                esp_mqtt_client_subscribe(s_mqtt_client, sub_topic, MQTT_QOS_COMMAND);
            }

            // Backward compatibility subscribe
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_CMD_WILDCARD, MQTT_QOS_COMMAND);
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_GATEWAY_CMD, MQTT_QOS_COMMAND);

            if (s_conn_callback != NULL) {
                s_conn_callback(true);
            }
            break;
        }

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected from broker");
            s_connected = false;
            if (s_conn_callback != NULL) {
                s_conn_callback(false);
            }
            break;

        case MQTT_EVENT_DATA: {
            char topic_buf[192];
            int topic_len = event->topic_len < (int)(sizeof(topic_buf) - 1) ? event->topic_len : (int)(sizeof(topic_buf) - 1);
            memcpy(topic_buf, event->topic, topic_len);
            topic_buf[topic_len] = '\0';

            if (s_cmd_callback != NULL) {
                s_cmd_callback(topic_buf, event->data, event->data_len);
            }
            break;
        }

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT event error");
            break;

        default:
            break;
    }
}

static esp_err_t create_client_instance(const brewlink_config_t *cfg) {
    if (cfg->mqtt_broker_uri[0] == '\0') {
        ESP_LOGW(TAG, "No MQTT Broker URI configured");
        return ESP_ERR_INVALID_STATE;
    }

    ensure_gateway_id_initialized();

    char eff_prefix[128];
    get_effective_prefix(cfg, eff_prefix, sizeof(eff_prefix));

    snprintf(s_lwt_topic, sizeof(s_lwt_topic), "%s%s%s", eff_prefix, s_gateway_id, MQTT_TOPIC_STATUS_SUFFIX);
    snprintf(s_client_id, sizeof(s_client_id), "brewlink_%s", s_gateway_id);
    snprintf(s_default_username, sizeof(s_default_username), "brewlink_%s", s_gateway_id);

    ESP_LOGI(TAG, "Configuring MQTT client: URI=%s, ClientID=%s, LWT=%s",
             cfg->mqtt_broker_uri, s_client_id, s_lwt_topic);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = cfg->mqtt_broker_uri,
        .credentials.client_id = s_client_id,
        .session.last_will = {
            .topic = s_lwt_topic,
            .msg = "{\"status\":\"offline\",\"reason\":\"connection_lost\"}",
            .msg_len = 0,
            .qos = MQTT_QOS_STATUS,
            .retain = 1,
        },
    };

    if (cfg->mqtt_username[0] != '\0') {
        mqtt_cfg.credentials.username = cfg->mqtt_username;
    } else {
        mqtt_cfg.credentials.username = s_default_username;
    }

    if (cfg->mqtt_password[0] != '\0') {
        mqtt_cfg.credentials.authentication.password = cfg->mqtt_password;
    }

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client handle");
        return ESP_FAIL;
    }

    return esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
}

esp_err_t mqtt_manager_init(const char *gateway_id,
                            mqtt_inbound_cmd_cb_t cmd_cb,
                            mqtt_conn_status_cb_t conn_cb) {
    if (gateway_id != NULL && strlen(gateway_id) > 0) {
        strncpy(s_gateway_id, gateway_id, sizeof(s_gateway_id) - 1);
        s_gateway_id[sizeof(s_gateway_id) - 1] = '\0';
    } else {
        ensure_gateway_id_initialized();
    }

    s_cmd_callback = cmd_cb;
    s_conn_callback = conn_cb;

    if (s_mqtt_mutex == NULL) {
        s_mqtt_mutex = xSemaphoreCreateMutex();
        if (s_mqtt_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    brewlink_config_t cfg;
    config_store_get(&cfg);

    if (cfg.mqtt_broker_uri[0] != '\0') {
        return create_client_instance(&cfg);
    }

    return ESP_OK;
}

esp_err_t mqtt_manager_start(void) {
    if (xSemaphoreTake(s_mqtt_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_should_run = true;
    esp_err_t err = ESP_OK;

    if (s_mqtt_client != NULL) {
        err = esp_mqtt_client_start(s_mqtt_client);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "MQTT client started");
        } else {
            ESP_LOGE(TAG, "esp_mqtt_client_start failed: 0x%x", err);
        }
    } else {
        brewlink_config_t cfg;
        config_store_get(&cfg);
        if (cfg.mqtt_broker_uri[0] != '\0') {
            err = create_client_instance(&cfg);
            if (err == ESP_OK && s_mqtt_client != NULL) {
                err = esp_mqtt_client_start(s_mqtt_client);
            }
        }
    }

    xSemaphoreGive(s_mqtt_mutex);
    return err;
}

esp_err_t mqtt_manager_stop(void) {
    if (xSemaphoreTake(s_mqtt_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_should_run = false;
    s_connected = false;
    esp_err_t err = ESP_OK;

    if (s_mqtt_client != NULL) {
        err = esp_mqtt_client_stop(s_mqtt_client);
        ESP_LOGI(TAG, "MQTT client stopped");
    }

    xSemaphoreGive(s_mqtt_mutex);
    return err;
}

esp_err_t mqtt_manager_reconfigure(const char *uri, const char *username,
                                   const char *password, const char *prefix) {
    if (uri == NULL || strlen(uri) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Reconfiguring MQTT broker to '%s'...", uri);
    config_store_save_mqtt(uri, username, password, prefix);

    if (xSemaphoreTake(s_mqtt_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_mqtt_client != NULL) {
        esp_mqtt_client_stop(s_mqtt_client);
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        s_connected = false;
    }

    brewlink_config_t cfg;
    config_store_get(&cfg);

    esp_err_t err = create_client_instance(&cfg);
    if (err == ESP_OK && s_should_run && s_mqtt_client != NULL) {
        err = esp_mqtt_client_start(s_mqtt_client);
    }

    xSemaphoreGive(s_mqtt_mutex);
    return err;
}

bool mqtt_manager_is_connected(void) {
    return s_connected;
}

const char *mqtt_manager_get_gateway_id(void) {
    ensure_gateway_id_initialized();
    return s_gateway_id;
}

esp_err_t mqtt_manager_publish(const char *topic, const char *data, int len, int qos, int retain) {
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, data, len, qos, retain);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_manager_publish_gateway_status(const char *status_json) {
    if (status_json == NULL) return ESP_ERR_INVALID_ARG;

    ensure_gateway_id_initialized();
    brewlink_config_t cfg;
    config_store_get(&cfg);
    char eff_prefix[128];
    get_effective_prefix(&cfg, eff_prefix, sizeof(eff_prefix));

    char topic[192];
    snprintf(topic, sizeof(topic), "%s%s%s", eff_prefix, s_gateway_id, MQTT_TOPIC_STATUS_SUFFIX);
    return mqtt_manager_publish(topic, status_json, 0, MQTT_QOS_STATUS, 1);
}

esp_err_t mqtt_manager_publish_equipment_announce(const char *eq_mac, const char *manifest_json) {
    if (eq_mac == NULL || manifest_json == NULL) return ESP_ERR_INVALID_ARG;

    ensure_gateway_id_initialized();
    char clean_mac[13];
    clean_mac_string(eq_mac, clean_mac);

    brewlink_config_t cfg;
    config_store_get(&cfg);
    char eff_prefix[128];
    get_effective_prefix(&cfg, eff_prefix, sizeof(eff_prefix));

    char topic[192];
    snprintf(topic, sizeof(topic), "%s%s/equipment/%s%s",
             eff_prefix, s_gateway_id, clean_mac, MQTT_TOPIC_ANNOUNCE_SUFFIX);

    return mqtt_manager_publish(topic, manifest_json, 0, MQTT_QOS_ANNOUNCE, 1);
}

esp_err_t mqtt_manager_publish_equipment_list(const char *list_json) {
    if (list_json == NULL) return ESP_ERR_INVALID_ARG;

    ensure_gateway_id_initialized();
    brewlink_config_t cfg;
    config_store_get(&cfg);
    char eff_prefix[128];
    get_effective_prefix(&cfg, eff_prefix, sizeof(eff_prefix));

    char topic[192];
    snprintf(topic, sizeof(topic), "%s%s/equipment", eff_prefix, s_gateway_id);
    return mqtt_manager_publish(topic, list_json, 0, MQTT_QOS_ANNOUNCE, 1);
}

esp_err_t mqtt_manager_publish_equipment_telemetry(const char *eq_mac, const char *telemetry_json) {
    if (eq_mac == NULL || telemetry_json == NULL) return ESP_ERR_INVALID_ARG;

    ensure_gateway_id_initialized();
    char clean_mac[13];
    clean_mac_string(eq_mac, clean_mac);

    brewlink_config_t cfg;
    config_store_get(&cfg);
    char eff_prefix[128];
    get_effective_prefix(&cfg, eff_prefix, sizeof(eff_prefix));

    char topic[192];
    snprintf(topic, sizeof(topic), "%s%s/equipment/%s%s",
             eff_prefix, s_gateway_id, clean_mac, MQTT_TOPIC_TELEMETRY_SUFFIX);

    return mqtt_manager_publish(topic, telemetry_json, 0, MQTT_QOS_TELEMETRY, 0);
}

esp_err_t mqtt_manager_publish_equipment_snapshot(const char *eq_mac, const char *snapshot_json) {
    if (eq_mac == NULL || snapshot_json == NULL) return ESP_ERR_INVALID_ARG;

    ensure_gateway_id_initialized();
    char clean_mac[13];
    clean_mac_string(eq_mac, clean_mac);

    brewlink_config_t cfg;
    config_store_get(&cfg);
    char eff_prefix[128];
    get_effective_prefix(&cfg, eff_prefix, sizeof(eff_prefix));

    char topic[192];
    snprintf(topic, sizeof(topic), "%s%s/equipment/%s%s",
             eff_prefix, s_gateway_id, clean_mac, MQTT_TOPIC_SNAPSHOT_SUFFIX);

    return mqtt_manager_publish(topic, snapshot_json, 0, MQTT_QOS_SNAPSHOT, 0);
}

esp_err_t mqtt_manager_publish_cmd_ack(const char *eq_mac, const char *req_id,
                                       const char *action, const char *status,
                                       int error_code, uint32_t duration_ms) {
    if (action == NULL || status == NULL) return ESP_ERR_INVALID_ARG;

    ensure_gateway_id_initialized();
    char clean_mac[13] = {0};
    if (eq_mac != NULL && strlen(eq_mac) > 0) {
        clean_mac_string(eq_mac, clean_mac);
    } else {
        strncpy(clean_mac, "GATEWAY", sizeof(clean_mac) - 1);
    }

    const char *rid = (req_id != NULL && strlen(req_id) > 0) ? req_id : "unknown";

    brewlink_config_t cfg;
    config_store_get(&cfg);
    char eff_prefix[128];
    get_effective_prefix(&cfg, eff_prefix, sizeof(eff_prefix));

    char topic[192];
    snprintf(topic, sizeof(topic), "%s%s/equipment/%s%s",
             eff_prefix, s_gateway_id, clean_mac, MQTT_TOPIC_CMD_ACK_SUFFIX);

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"event\":\"cmd_ack\",\"req_id\":\"%s\",\"mac\":\"%s\",\"action\":\"%s\",\"status\":\"%s\","
             "\"error_code\":%d,\"duration_ms\":%lu,\"timestamp_ms\":%lu}",
             rid, clean_mac, action, status,
             error_code, (unsigned long)duration_ms, (unsigned long)esp_log_timestamp());

    return mqtt_manager_publish(topic, payload, 0, MQTT_QOS_COMMAND, 0);
}
