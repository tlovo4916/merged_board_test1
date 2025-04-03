/*
 * ESP32-S3 WebSocket客户端配置文件
 */
#ifndef _BOARD_H_
#define _BOARD_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_websocket_client.h"
#include "cJSON.h"

/* WiFi配置 */
#define WIFI_SSID               "supieTP"
#define WIFI_PASS               "supie1818"
#define WIFI_MAXIMUM_RETRY      5

/* WebSocket服务器配置 */
#define WS_SERVER_HOST          "192.168.0.23"
#define WS_SERVER_PORT          8084
#define WS_SERVER_PATH          "/robws"

/* ESP32-S3设备ID配置 */
#define DEVICE_CLIENT_ID        "esp32s3_device"

/* FreeRTOS事件组位 */
#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1

/* 函数声明 */
// WiFi相关函数
void wifi_init_sta(void);
void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

// WebSocket相关函数
void websocket_app_start(void);
void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
esp_err_t process_server_command(const char *data, int len);

// 设备控制功能
esp_err_t restart_device(void);

#endif /* _BOARD_H_ */ 