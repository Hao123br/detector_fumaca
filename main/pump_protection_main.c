#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "driver/adc_common.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "mqtt.h"

#define DEFAULT_VREF    1100  //Use adc2_vref_to_gpio() to obtain a better estimate
#define NO_OF_SAMPLES   64    //Multisampling
#define ADC_UNIT    1

#define DEFAULT_SSID "wifi"
#define DEFAULT_PWD "senha"

#define DEFAULT_SCAN_METHOD WIFI_FAST_SCAN
#define DEFAULT_SORT_METHOD WIFI_CONNECT_AP_BY_SIGNAL
#define DEFAULT_AUTHMODE WIFI_AUTH_WPA2_PSK
#define DEFAULT_RSSI -100

//output gpios
#define STATUS_LED_GPIO  2
#define BUZZER_GPIO    16
#define GPIO_OUTPUT_PIN_SEL BIT(STATUS_LED_GPIO) | BIT(BUZZER_GPIO)

struct NetworkBuffer {
	// handle to messages sent from the app to the controller
	RingbufHandle_t host_handle;
	// handle to messages sent from the controller to the app
	RingbufHandle_t remote_handle;
};

struct MqttMessage {
	char* topic;
	char* message;
};

//ca certificate. Used to authenticate the firmware update server.
extern const uint8_t ca_cert_start[] asm("_binary_ca_crt_start");
extern const uint8_t ca_cert_end[] asm("_binary_ca_crt_end");

// tags for log messages
static const char *WIFI_TAG = "scan";
static const char *SOCK_TAG = "server";
static const char *OTA_TAG = "ota";

// ADC parameters 
static const uint8_t width = 11;
static const adc_atten_t atten = ADC_ATTEN_DB_6;
static const uint8_t reference_ldr_channel = 4;
static const uint8_t sense_ldr_channel = 5;

// socket server port
static const uint16_t listen_port = 42032;

// ringbuffer parameters
static const uint16_t rx_ring_size = 256;
static const uint16_t tx_ring_size = 8192;

TaskHandle_t status_led_handle = NULL;

// default firmware update url
static const char *ota_url = "https://betahalo.ddns.net:42024/build/pump-protection.bin";

static bool socket_connected = false;
struct NetworkBuffer net_buf;
static bool restart_pending = false;

void socket_send(const char* str, const size_t str_len);

static void print_chip_info(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU cores, WiFi%s%s, ",
            CONFIG_IDF_TARGET,
            chip_info.cores,
            (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
            (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

    printf("silicon revision %d, ", chip_info.revision);

    printf("%dMB %s flash\n", spi_flash_get_chip_size() / (1024 * 1024),
            (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    printf("Free heap: %d\n", esp_get_free_heap_size());

	return;
}

static void event_handler(void* arg, esp_event_base_t event_base,
		                                int32_t event_id, void* event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		esp_wifi_connect();
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
		ESP_LOGI(WIFI_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
	}
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(OTA_TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(OTA_TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(OTA_TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(OTA_TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(OTA_TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(OTA_TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(OTA_TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    }
    return ESP_OK;
}

/* Initialize Wi-Fi as sta and set scan method */
static void fast_scan(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    // Initialize default station as network interface instance (esp-netif)
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    // Initialize and start WiFi
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = DEFAULT_SSID,
            .password = DEFAULT_PWD,
            .scan_method = DEFAULT_SCAN_METHOD,
            .sort_method = DEFAULT_SORT_METHOD,
            .threshold.rssi = DEFAULT_RSSI,
            .threshold.authmode = DEFAULT_AUTHMODE,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(60)); //15dbm
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static esp_adc_cal_characteristics_t* configure_adc()
{
	static esp_adc_cal_characteristics_t *adc_chars;
	//Configure ADC
	adc1_config_width(width);
	adc1_config_channel_atten(reference_ldr_channel, atten);
	adc1_config_channel_atten(sense_ldr_channel, atten);

    //Characterize ADC
    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_characterize(ADC_UNIT, atten, width, DEFAULT_VREF, adc_chars);

	return adc_chars;
}

static void configure_gpio()
{
	gpio_config_t io_conf;

	//turn led off
	gpio_set_level(STATUS_LED_GPIO, 0);
    //disable interrupt
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = false;
    //disable pull-up mode
    io_conf.pull_up_en = false;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-down mode
    io_conf.pull_down_en = true;
    gpio_config(&io_conf);
}

void socket_send(const char* str, const size_t str_len)
{
	if(socket_connected)
		xRingbufferSend(net_buf.remote_handle, str, str_len, pdMS_TO_TICKS(100));
}

static void do_comms(const int sock, struct NetworkBuffer *net_buf)
{
	int len;
	char rx_buffer[128];
	size_t item_size;
	char *tx_item;
	int written = 1;

	do {
		len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, MSG_DONTWAIT);
		if (len < 0) {
			if (errno != EAGAIN)
				ESP_LOGE(SOCK_TAG, "Error occurred during receiving: errno %d", errno);
		} else if (len == 0) {
			ESP_LOGW(SOCK_TAG, "Connection closed");
			xRingbufferSend(net_buf->host_handle, "n", 2, pdMS_TO_TICKS(100));
		} else {
			rx_buffer[len] = 0; // Null-terminate whatever is received and treat it like a string
			ESP_LOGI(SOCK_TAG, "Received %d bytes: %s", len, rx_buffer);
			xRingbufferSend(net_buf->host_handle, rx_buffer, len+1, pdMS_TO_TICKS(100));
		}

		while(1) {
			tx_item = xRingbufferReceive(net_buf->remote_handle, &item_size, pdMS_TO_TICKS(100));
			if (tx_item == NULL)
				break;

			if (strcmp(tx_item, "close") == 0)
				return;

			if (written > 0 && (len > 0 || errno == EAGAIN)) {
				// send() can return less bytes than supplied length.
				// Walk-around for robust implementation. 
				int to_write = item_size;
				while (to_write > 0) {
					written = send(sock, tx_item + (item_size - to_write), to_write, 0);
					if (written < 0) {
						ESP_LOGE(SOCK_TAG, "Error occurred during sending: errno %d", errno);
						break;
					}
					to_write -= written;
				}
			}
			vRingbufferReturnItem(net_buf->remote_handle, tx_item);
		}
    } while (len > 0 || errno == EAGAIN);
}

static void tcp_server_task(void *pvParameters)
{
    char addr_str[128];
    struct NetworkBuffer *net_buf = (struct NetworkBuffer *) pvParameters;
    int ip_protocol = 0;
    struct sockaddr_in6 dest_addr;

	struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
	dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
	dest_addr_ip4->sin_family = AF_INET;
	dest_addr_ip4->sin_port = htons(listen_port);
	ip_protocol = IPPROTO_IP;

    int listen_sock = socket(AF_INET, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(SOCK_TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(SOCK_TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(SOCK_TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(SOCK_TAG, "IPPROTO: %d", AF_INET);
        goto CLEAN_UP;
    }
    ESP_LOGI(SOCK_TAG, "Socket bound, port %d", listen_port);

    err = listen(listen_sock, 1); //Blocking operation
    if (err != 0) {
        ESP_LOGE(SOCK_TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    while (1) {

        ESP_LOGI(SOCK_TAG, "Socket listening");

        struct sockaddr_in6 source_addr; // Large enough for both IPv4 or IPv6
        uint addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(SOCK_TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        // Convert ip address to string
        if (source_addr.sin6_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
        } else if (source_addr.sin6_family == PF_INET6) {
            inet6_ntoa_r(source_addr.sin6_addr, addr_str, sizeof(addr_str) - 1);
        }
        ESP_LOGI(SOCK_TAG, "Socket accepted ip address: %s", addr_str);

		socket_connected = true;
        do_comms(sock, net_buf);

        shutdown(sock, 0);
        close(sock);
		socket_connected = false;
    }

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
}

static void blink_led()
{
	while(1) {
        /* Blink on (output high) */
        gpio_set_level(STATUS_LED_GPIO, 1);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        /* Blink off (output low) */
        gpio_set_level(STATUS_LED_GPIO, 0);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
	}
}

static void status_led_task(void* arg)
{
	blink_led();
}

void ota_task(void *pvParameter) {
	const char *error_msg = "Firmware upgrade failed\n";
	const char *success_msg = "Firmware upgrade successful, restart scheduled\n";
	const char *url = (const char *) pvParameter;
	ESP_LOGI(OTA_TAG, "Starting OTA upgrade");

	esp_http_client_config_t config = {
		.url = url,
		.cert_pem = (char *)ca_cert_start,
		.event_handler = _http_event_handler,
		.keep_alive_enable = true,
	};

	//start blinking the status led
	xTaskCreate(status_led_task, "led_task", 1024, NULL, 5, &status_led_handle);
	//download the new firmware
	esp_err_t ret = esp_https_ota(&config);
	//stop blinking the status led
	vTaskDelete(status_led_handle);
	if (ret == ESP_OK) {
		//and turn it on
		gpio_set_level(STATUS_LED_GPIO, 1);
		restart_pending = true;
		socket_send(success_msg, strlen(success_msg));
		mqtt_publisher("esp32", success_msg);
	} else {
		//and turn it off
		gpio_set_level(STATUS_LED_GPIO, 0);
		ESP_LOGE(OTA_TAG, "Firmware upgrade failed");
		socket_send(error_msg, strlen(error_msg));
		mqtt_publisher("esp32", error_msg);
    }
	vTaskDelete(NULL);
}

static void send_version() {
	const esp_partition_t *running = esp_ota_get_running_partition();
	char hex_chars[3];
	char hash[21] = "";
	char msg[100];
	size_t msg_len;
	esp_app_desc_t running_app_info;

	if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
		for(size_t i = 0; i < 10; ++i) {
			sprintf(hex_chars, "%.2x", running_app_info.app_elf_sha256[i]);
			strcat(hash, hex_chars);
		}
		msg_len = sprintf(msg, "%s %s\n", running_app_info.version, hash);
	} else {
		msg_len = sprintf(msg, "%s\n", "could not get app info");
	}

	socket_send(msg, msg_len);
	mqtt_publisher("esp32", msg);
}

static int ends_with(char *str, char *end) {
	size_t str_size = strlen(str);
	size_t end_size = strlen(end);

	if (str[str_size-1] == '\n')
		str_size--;

	for(size_t i = 1; i <= end_size; ++i) {
		if (str[str_size-i] != end[end_size-i])
			return 0;
	}

	return 1;
}

static void parse_command(char *command_str, size_t str_size, char *mode) {
	if (str_size <= 3) {
		*mode = command_str[0];
	} else if (strstr(command_str, "upgrade") != NULL) {
		strtok(command_str, " \n"); //discard first token
		command_str = strtok(NULL, " \n");
		if (command_str == NULL)
			xTaskCreate(&ota_task, "ota_task", 8192, (void *) ota_url, 5, NULL);
		else
			xTaskCreate(&ota_task, "ota_task", 8192, (void *) command_str, 5, NULL); //second token is url
	} else if (strstr(command_str, "version") != NULL) {
		send_version();
	}
}

void app_main(void)
{
	const unsigned int interval = 1000; //in ms
	const char *close_msg = "close";
	const char *restart_msg = "restarting...\n";
	TickType_t last_wake_time;
	char mode = 'n';
	char log_str[150];
	char* command_str;
	size_t str_size;
	static esp_adc_cal_characteristics_t *adc_chars;

	esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);

    /* Print chip information */
	print_chip_info();

	//scan wifi networks and connect
	fast_scan();

	adc_chars = configure_adc();
	configure_gpio();

	net_buf.host_handle = xRingbufferCreate(rx_ring_size, RINGBUF_TYPE_NOSPLIT);
	net_buf.remote_handle = xRingbufferCreate(tx_ring_size, RINGBUF_TYPE_NOSPLIT);
	if(net_buf.host_handle != NULL && net_buf.remote_handle != NULL) {
		xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*) &net_buf, 5, NULL);
	} else {
		printf("Failed to create ring buffers\n");
		net_buf.host_handle = NULL;
	}

	mqtt_start(net_buf.host_handle);

	//the app is unlikely to crash after this point, so we mark it as valid
	esp_ota_mark_app_valid_cancel_rollback();

	last_wake_time = xTaskGetTickCount();
	while (1) {
		vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(interval));

		if (net_buf.host_handle != NULL) {
			command_str = (char *)xRingbufferReceive(net_buf.host_handle, &str_size, pdMS_TO_TICKS(20));
			if (command_str != NULL) {
				parse_command(command_str, str_size, &mode);
				vRingbufferReturnItem(net_buf.host_handle, (void *) command_str);
			}
		}

		if(restart_pending) {
			socket_send(restart_msg, strlen(restart_msg)+1);
			mqtt_publisher("esp32", restart_msg);
			//wait for packets to be sent
			vTaskDelay(pdMS_TO_TICKS(250));
			//send command to close socket
			socket_send(close_msg, strlen(close_msg)+1);
			//wait for socket to close
			vTaskDelay(pdMS_TO_TICKS(250));
			esp_restart();
		}

		uint32_t reference_ldr_reading = 0;
		uint32_t sense_ldr_reading = 0;
        //Multisampling
        for (int i = 0; i < NO_OF_SAMPLES; i++) {
			reference_ldr_reading += adc1_get_raw((adc1_channel_t)reference_ldr_channel);
			sense_ldr_reading += adc1_get_raw((adc1_channel_t)sense_ldr_channel);
        }
        reference_ldr_reading /= NO_OF_SAMPLES;
        sense_ldr_reading /= NO_OF_SAMPLES;
        //Convert adc_reading to voltage in mV
        uint32_t reference_ldr_voltage = esp_adc_cal_raw_to_voltage(reference_ldr_reading, adc_chars);
        uint32_t sense_ldr_voltage = esp_adc_cal_raw_to_voltage(sense_ldr_reading, adc_chars);
        printf("Reference | Raw: %d\tVoltage: %dmV\n", reference_ldr_reading, reference_ldr_voltage);
        printf("Sense     | Raw: %d\tVoltage: %dmV\n", sense_ldr_reading, sense_ldr_voltage);

		socket_send(log_str, str_size);
		mqtt_publisher_qos("esp32", log_str, 0);
	}
}
