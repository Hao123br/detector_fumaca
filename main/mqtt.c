/* MQTT over SSL Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/param.h>

#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#include "freertos/ringbuf.h"

#include "mqtt.h"

static const char *TAG = "MQTTS";
esp_mqtt_client_handle_t client;
RingbufHandle_t command_handle;
bool connected = false;

//mqtt client certificate
extern const uint8_t mqtt_client_cert_pem_start[] asm("_binary_esp32_crt_start");
extern const uint8_t mqtt_client_cert_pem_end[] asm("_binary_esp32_crt_end");

//mqtt client key
extern const uint8_t mqtt_client_key_pem_start[] asm("_binary_esp32_key_start");
extern const uint8_t mqtt_client_key_pem_end[] asm("_binary_esp32_key_end");

//mqtt ca certificate. Will be used to verify the server certificate.
extern const uint8_t mqtt_ca_cert_pem_start[]   asm("_binary_ca_crt_start");
extern const uint8_t mqtt_ca_cert_pem_end[]   asm("_binary_ca_crt_end");

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    int msg_id;
    char *data;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
			connected = true;
            msg_id = esp_mqtt_client_subscribe(client, "commands", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            // msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
            // ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            // msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
            // ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
			connected = false;
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            // msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
            // ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            data = (char *) malloc(event->data_len+1);
            memcpy(data, event->data, event->data_len);
            data[event->data_len] = '\0';
            xRingbufferSend(command_handle, data, event->data_len+1, pdMS_TO_TICKS(100));
            free(data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            // if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            //     ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            //     ESP_LOGI(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            //     ESP_LOGI(TAG, "Last captured errno : %d (%s)",  event->error_handle->esp_transport_sock_errno,
            //                                                     strerror(event->error_handle->esp_transport_sock_errno));
            // } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            //     ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
            // } else {
            //     ESP_LOGW(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
            // }
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}

void mqtt_start(RingbufHandle_t host_handle)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .host = "betahalo.ddns.net",
        .port = 42083,
        .transport = MQTT_TRANSPORT_OVER_SSL,
        .client_cert_pem = (const char *)mqtt_client_cert_pem_start,
        .client_key_pem = (const char *)mqtt_client_key_pem_start,
        .cert_pem = (const char *)mqtt_ca_cert_pem_start,
    };
    esp_log_level_set(TAG, ESP_LOG_VERBOSE);
    command_handle = host_handle;
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

void mqtt_publisher(const char * topic, const char * message)
{
	mqtt_publisher_qos(topic, message, 2);
}

void mqtt_publisher_qos(const char * topic, const char * message, const int qos)
{
	int message_id;
	if (connected) {
		message_id = esp_mqtt_client_publish(client, topic, message, 0, qos, 0);
		ESP_LOGI(TAG, "Mensagem enviada, ID: %d", message_id);
	}
}
