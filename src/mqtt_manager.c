/**
 * @file mqtt_manager.c
 * @brief MQTT Client Subsystem Implementation
 */

#include "mqtt_manager.h"
#include <string.h>
#include "esp_log.h"
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
static SemaphoreHandle_t s_mqtt_mutex = NULL;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            s_connected = true;
            esp_mqtt_client_publish(s_mqtt_client, MQTT_TOPIC_STATUS, "{\"status\":\"online\"}", 0, MQTT_QOS_STATUS, 1);
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_GATEWAY_CMD, MQTT_QOS_COMMAND);
            esp_mqtt_client_subscribe(s_mqtt_client, MQTT_TOPIC_CMD_WILDCARD, MQTT_QOS_COMMAND);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected from broker");
            s_connected = false;
            break;

        case MQTT_EVENT_DATA: {
            char topic_buf[128];
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

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = cfg->mqtt_broker_uri,
        .session.last_will = {
            .topic = MQTT_TOPIC_STATUS,
            .msg = "{\"status\":\"offline\"}",
            .msg_len = 20,
            .qos = MQTT_QOS_STATUS,
            .retain = 1,
        },
    };

    if (cfg->mqtt_username[0] != '\0') {
        mqtt_cfg.credentials.username = cfg->mqtt_username;
    }
    if (cfg->mqtt_password[0] != '\0') {
        mqtt_cfg.credentials.authentication.password = cfg->mqtt_password;
    }

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }

    return esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
}

esp_err_t mqtt_manager_init(mqtt_inbound_cmd_cb_t cmd_cb) {
    s_cmd_callback = cmd_cb;

    s_mqtt_mutex = xSemaphoreCreateMutex();
    if (s_mqtt_mutex == NULL) {
        return ESP_ERR_NO_MEM;
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

    // Save to NVS
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

esp_err_t mqtt_manager_publish(const char *topic, const char *data, int len, int qos, int retain) {
    if (!s_connected || s_mqtt_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, data, len, qos, retain);
    return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}
