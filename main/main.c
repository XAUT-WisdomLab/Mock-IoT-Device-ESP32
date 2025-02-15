#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include <sys/socket.h>
#include "esp_eth.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "mqtt_client.h"
#include "cJSON.h"

// 要连接的WIFI
#define ESP_WIFI_STA_SSID ""
#define ESP_WIFI_STA_PASSWD ""

// 连接阿里云的配置信息
#define MQTT_CLIENT_ID ""
#define MQTT_USERNAME ""
#define MQTT_PASSWD ""
#define MQTT_HOST_URL ""
#define MQTT_REPORT_TOPIC ""
#define MQTT_CONTROL_TOPIC ""

static const char *TAG = "app";

// 定义传感器数据结构
struct Data
{
	float temperature;				 // 当前环境温度，单位为摄氏度
	float humidity;						 // 当前环境湿度，单位为百分比
	float pressure;						 // 当前环境气压，单位为帕斯卡
	float lightIntensity;			 // 当前光照强度，单位为勒克斯
	float smokeDensity;				 // 当前烟雾浓度，单位为PPM
	char relayStatus[4];			 // 继电器的当前状态（开或关）
	char circuitBreakerStatus; // 断路器的当前状态（开或关）
};

// 继电器状态(全局变量)
bool relay_state[4] = {0, 0, 0, 0};

// 断路器状态(全局变量)
bool circuit_breaker_state = 0; // 初始状态为关闭

// MQTT 客户端句柄（全局变量）
esp_mqtt_client_handle_t mqtt_client;

// 生成随机浮点数
float random_float(float min, float max)
{
	return min + (esp_random() % 1000) / 1000.0f * (max - min);
}

void send_json(struct Data *data)
{
	// 创建 cJSON 根对象
	cJSON *root = cJSON_CreateObject();
	if (root == NULL)
	{
		ESP_LOGE(TAG, "Failed to create JSON object");
		return;
	}

	// 创建 "params" 子对象
	cJSON *params = cJSON_CreateObject();
	if (params == NULL)
	{
		ESP_LOGE(TAG, "Failed to create params object");
		cJSON_Delete(root);
		return;
	}

	// 格式化浮点数并添加到 "params" 对象（保留两位小数）
	cJSON_AddNumberToObject(params, "temperature", roundf(data->temperature * 100) / 100);
	cJSON_AddNumberToObject(params, "humidity", roundf(data->humidity * 100) / 100);
	cJSON_AddNumberToObject(params, "pressure", roundf(data->pressure * 100) / 100);
	cJSON_AddNumberToObject(params, "lightIntensity", roundf(data->lightIntensity * 100) / 100);
	cJSON_AddNumberToObject(params, "smokeDensity", roundf(data->smokeDensity * 100) / 100);

	// 将继电器状态分开添加为 relayStatus_1, relayStatus_2, ...
	for (int i = 0; i < 4; ++i)
	{
		char relayKey[16]; // 用于生成 "relayStatus_1", "relayStatus_2" 等
		sprintf(relayKey, "relayStatus_%d", i + 1);
		cJSON_AddNumberToObject(params, relayKey, relay_state[i]);
	}

	// 将断路器状态添加为数字类型（0 或 1）
	cJSON_AddNumberToObject(params, "circuitBreakerStatus", data->circuitBreakerStatus ? 1 : 0);

	// 将 "params" 添加到根对象中
	cJSON_AddItemToObject(root, "params", params);

	// 将 JSON 对象转换为字符串
	char *json_str = cJSON_Print(root);
	if (json_str == NULL)
	{
		ESP_LOGE(TAG, "Failed to print JSON object");
		cJSON_Delete(root);
		return;
	}

	// 示例：打印 JSON 字符串
	ESP_LOGI(TAG, "JSON: %s", json_str);

	// 发送 JSON 字符串到 MQTT
	esp_mqtt_client_publish(mqtt_client, MQTT_REPORT_TOPIC, json_str, 0, 1, 0);

	// 释放资源
	cJSON_Delete(root);
	free(json_str);
}

static void log_error_if_nonzero(const char *message, int error_code)
{
	if (error_code != 0)
	{
		ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
	}
}

void parse_and_print_json(const char *json_str)
{
	// 解析 JSON 字符串
	cJSON *json = cJSON_Parse(json_str);
	if (json == NULL)
	{
		const char *error_ptr = cJSON_GetErrorPtr();
		if (error_ptr != NULL)
		{
			fprintf(stderr, "Error before: %s\n", error_ptr);
		}
		return;
	}

	// 处理继电器状态
	for (int i = 0; i < 4; i++)
	{
		char key[10];
		snprintf(key, sizeof(key), "dalay_%d", i + 1); // 构造键名 "dalay_1" ~ "dalay_4"

		cJSON *dalay = cJSON_GetObjectItemCaseSensitive(json, key);
		if (dalay && cJSON_IsNumber(dalay))
		{
			relay_state[i] = dalay->valueint;
			ESP_LOGI(TAG, "Relay %d 被控制，当前状态: %s", i + 1, relay_state[i] ? "开" : "关");
		}
	}

	// 处理断路器状态
	cJSON *circuit_breaker = cJSON_GetObjectItemCaseSensitive(json, "circuit_breaker");
	if (circuit_breaker && cJSON_IsNumber(circuit_breaker))
	{
		circuit_breaker_state = circuit_breaker->valueint;
		ESP_LOGI(TAG, "断路器被控制，当前状态: %s", circuit_breaker_state ? "开" : "关");
	}

	// 释放 JSON 解析对象
	cJSON_Delete(json);
}

// MQTT客户端事件处理
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
	ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
	esp_mqtt_event_handle_t event = event_data;
	esp_mqtt_client_handle_t client = event->client;
	int msg_id;
	switch ((esp_mqtt_event_id_t)event_id)
	{
	// MQTT连接成功
	case MQTT_EVENT_CONNECTED:
		ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
		// 订阅消息(订阅控制主题)
		msg_id = esp_mqtt_client_subscribe(client, MQTT_CONTROL_TOPIC, 0);
		ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
		break;
	// MQTT连接断开
	case MQTT_EVENT_DISCONNECTED:
		ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
		break;
	// MQTT订阅成功
	case MQTT_EVENT_SUBSCRIBED:
		ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
		break;
	// MQTT取消订阅成功
	case MQTT_EVENT_UNSUBSCRIBED:
		ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
		break;
	// MQTT发布成功
	case MQTT_EVENT_PUBLISHED:
		ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
		break;
	// MQTT收到数据
	case MQTT_EVENT_DATA:
		ESP_LOGI(TAG, "MQTT_EVENT_DATA");
		printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
		printf("DATA=%.*s\r\n", event->data_len, event->data);
		// 确保数据是以null终止的字符串
		char *json_data = (char *)malloc(event->data_len + 1);
		if (json_data == NULL)
		{
			ESP_LOGE(TAG, "Failed to allocate memory for JSON data");
			break;
		}
		memcpy(json_data, event->data, event->data_len);
		json_data[event->data_len] = '\0'; // 添加字符串终止符
		parse_and_print_json(json_data);
		break;
	// MQTT错误
	case MQTT_EVENT_ERROR:
		ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
		if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
		{
			log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
			log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
			log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
			ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
		}
		break;
	default:
		ESP_LOGI(TAG, "Other event id:%d", event->event_id);
		break;
	}
}

// MQTT客户端
static void mqtt_app_start(void)
{
	esp_mqtt_client_config_t mqtt_cfg = {
			.broker.address.uri = MQTT_HOST_URL,
			.credentials.username = MQTT_USERNAME,
			.credentials.client_id = MQTT_CLIENT_ID,
			.credentials.authentication.password = MQTT_PASSWD,
	};

	mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
	// 注册事件处理函数
	esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
	// 启动MQTT客户端
	esp_mqtt_client_start(mqtt_client);
}

void WIFI_CallBack(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	static uint8_t connect_count = 0;
	// WIFI 启动成功
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
	{
		ESP_LOGI("WIFI_EVENT", "WIFI_EVENT_STA_START");
		ESP_ERROR_CHECK(esp_wifi_connect());
	}
	// WIFI 连接失败
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
	{
		ESP_LOGI("WIFI_EVENT", "WIFI_EVENT_STA_DISCONNECTED");
		connect_count++;
		if (connect_count < 6)
		{
			vTaskDelay(1000 / portTICK_PERIOD_MS);
			ESP_ERROR_CHECK(esp_wifi_connect());
		}
		else
		{
			ESP_LOGI("WIFI_EVENT", "WIFI_EVENT_STA_DISCONNECTED 10 times");
		}
	}
	// WIFI 连接成功(获取到了IP)
	if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
	{
		ESP_LOGI("WIFI_EVENT", "WIFI_EVENT_STA_GOT_IP");
		ip_event_got_ip_t *info = (ip_event_got_ip_t *)event_data;
		ESP_LOGI("WIFI_EVENT", "got ip:" IPSTR "", IP2STR(&info->ip_info.ip));
		// 创建MQTT客户端
		mqtt_app_start();
	}
}

// wifi初始化
static void wifi_sta_init(void)
{
	ESP_ERROR_CHECK(esp_netif_init());

	// 注册事件(wifi启动成功)
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_START, WIFI_CallBack, NULL, NULL));
	// 注册事件(wifi连接失败)
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, WIFI_CallBack, NULL, NULL));
	// 注册事件(wifi连接失败)
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, WIFI_CallBack, NULL, NULL));

	// 初始化STA设备
	esp_netif_create_default_wifi_sta();

	/*Initialize WiFi */
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	// STA详细配置
	wifi_config_t sta_config = {
			.sta = {
					.ssid = ESP_WIFI_STA_SSID,
					.password = ESP_WIFI_STA_PASSWD,
					.bssid_set = false,
			},
	};
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));

	// 启动WIFI
	ESP_ERROR_CHECK(esp_wifi_start());

	// 配置省电模式
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void app_main(void)
{
	ESP_LOGI(TAG, " Startup..");

	// 初始化 NVS
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	// 创建默认事件循环
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	// 配置启动WIFI
	wifi_sta_init();

	// 初始化结构体实例
	struct Data systemData = {
			.temperature = 25.0,
			.humidity = 50.0,
			.pressure = 101325.0,
			.lightIntensity = 300,
			.smokeDensity = 0,
			.relayStatus = {0, 0, 0, 0},
			.circuitBreakerStatus = circuit_breaker_state};

	// 示例打印初始化后的结构体属性
	ESP_LOGI(TAG, "Temperature: %.2f", systemData.temperature);
	ESP_LOGI(TAG, "Humidity: %.2f", systemData.humidity);
	ESP_LOGI(TAG, "Pressure: %.2f", systemData.pressure);
	ESP_LOGI(TAG, "Light Intensity: %.2f", systemData.lightIntensity);
	ESP_LOGI(TAG, "Smoke Density: %.2f", systemData.smokeDensity);
	ESP_LOGI(TAG, "Relay Status: %s", systemData.relayStatus);
	ESP_LOGI(TAG, "Circuit Breaker Status: %c", systemData.circuitBreakerStatus);

	// 模拟传感器初始化
	ESP_LOGI(TAG, "MQ2 传感器初始化");
	ESP_LOGI(TAG, "BH1750 传感器初始化");
	ESP_LOGI(TAG, "BME280 传感器初始化");
	ESP_LOGI(TAG, "继电器初始化");
	ESP_LOGI(TAG, "断路器初始化");

	// 采集数据并上传
	vTaskDelay(pdMS_TO_TICKS(3000));
	for (;;)
	{
		// 采集数据
		vTaskDelay(pdMS_TO_TICKS(10000));

		// 生成随机传感器数据
		systemData.temperature = random_float(10.0f, 35.0f);		 // 温度范围 10°C ~ 35°C
		systemData.humidity = random_float(30.0f, 90.0f);				 // 湿度范围 30% ~ 90%
		systemData.pressure = random_float(950.0f, 1050.0f);		 // 气压范围 950 hPa ~ 1050 hPa
		systemData.lightIntensity = random_float(0.0f, 1000.0f); // 光照强度范围 0 ~ 1000 lux
		systemData.smokeDensity = random_float(0.0f, 100.0f);		 // 烟雾浓度范围 0 ~ 100 PPM

		// 更新断路器状态
		systemData.circuitBreakerStatus = circuit_breaker_state;

		// 打包数据并发送
		send_json(&systemData);
	}
}
