/*
 * ESP32-S3 WebSocket客户端应用测试
 */
#include "board.h"
#include <inttypes.h>

static const char *TAG = "ESP32_WEBSOCKET_CLIENT";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static esp_websocket_client_handle_t client = NULL;

/* WiFi事件处理函数 */
void wifi_event_handler(void *arg, esp_event_base_t event_base,
                         int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "重试连接WiFi...");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "无法连接到WiFi网络");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "获取到IP地址: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* WiFi站点模式初始化 */
void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta 完成");

    /* 等待WiFi连接或者连接失败 */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "已连接到WiFi SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "无法连接到WiFi SSID:%s, 密码:%s", WIFI_SSID, WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "意外事件");
    }
}

/* 处理服务器命令 */
esp_err_t process_server_command(const char *data, int len)
{
    if (data == NULL || len <= 0) {
        ESP_LOGE(TAG, "无效的数据");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    cJSON *root = cJSON_Parse(data);
    
    if (root == NULL) {
        ESP_LOGE(TAG, "JSON解析错误: %s", data);
        return ESP_FAIL;
    }
    
    // 检查是否有event字段
    cJSON *event = cJSON_GetObjectItem(root, "event");
    if (cJSON_IsString(event) && (event->valuestring != NULL)) {
        ESP_LOGI(TAG, "收到事件: %s", event->valuestring);
        
        // 处理重启命令
        if (strcmp(event->valuestring, "restart") == 0) {
            ESP_LOGI(TAG, "收到重启命令");
            ret = restart_device();
        } 
        // 可以添加更多命令处理
    } else {
        ESP_LOGW(TAG, "未找到event字段或event不是字符串类型");
    }
    
    cJSON_Delete(root);
    return ret;
}

/* 重启设备 */
esp_err_t restart_device(void)
{
    ESP_LOGI(TAG, "设备将在3秒后重启...");
    // 发送确认消息给服务器
    if (client != NULL && esp_websocket_client_is_connected(client)) {
        char response[100];
        snprintf(response, sizeof(response), "{\"status\":\"ok\",\"message\":\"device restarting\",\"clientId\":\"%s\"}", DEVICE_CLIENT_ID);
        esp_websocket_client_send_text(client, response, strlen(response), portMAX_DELAY);
    }
    
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    esp_restart();
    return ESP_OK;
}

/* WebSocket事件处理函数 */
void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket已连接");
        // 连接后发送客户端ID消息
        char connect_msg[100];
        snprintf(connect_msg, sizeof(connect_msg), "{\"clientId\":\"%s\",\"status\":\"connected\"}", DEVICE_CLIENT_ID);
        esp_websocket_client_send_text(client, connect_msg, strlen(connect_msg), portMAX_DELAY);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket已断开连接");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->data_len > 0) {
            ESP_LOGI(TAG, "收到数据: %.*s", data->data_len, (char *)data->data_ptr);
            // 处理服务器命令
            process_server_command(data->data_ptr, data->data_len);
        } else {
            ESP_LOGW(TAG, "收到空数据");
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket错误");
        break;
    }
}

/* WebSocket应用启动 */
void websocket_app_start(void)
{
    char ws_uri[128];
    snprintf(ws_uri, sizeof(ws_uri), "ws://%s:%d%s/%s", 
             WS_SERVER_HOST, WS_SERVER_PORT, WS_SERVER_PATH, DEVICE_CLIENT_ID);
    
    ESP_LOGI(TAG, "正在连接到WebSocket服务器: %s", ws_uri);
    
    esp_websocket_client_config_t websocket_cfg = {
        .uri = ws_uri,
        .disable_auto_reconnect = false,
        .reconnect_timeout_ms = 10000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 10,
    };

    client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);

    esp_websocket_client_start(client);
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] 启动中...");
    ESP_LOGI(TAG, "[APP] 可用内存: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF版本: %s", esp_get_idf_version());
    
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化并连接WiFi
    ESP_LOGI(TAG, "ESP32-S3 WiFi连接启动");
    wifi_init_sta();
    
    // 启动WebSocket客户端
    websocket_app_start();
} 