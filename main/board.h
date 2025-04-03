/**
 * @file board.h
 * @brief ESP32-S3 开发板 统一配置文件
 * @details 整合了音频 (ES8311, ES7210), WiFi (STA, SoftAP配网), WebSocket 客户端,
 *          以及其他板级配置 (I2C, GPIO, NVS, 恢复出厂设置).
 */

#ifndef _BOARD_H_
#define _BOARD_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4.h"
#include "es8311.h"
#include "es7210.h"
#include "esp_websocket_client.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************** 全局事件组定义 ****************************/
extern EventGroupHandle_t board_event_group; // 全局事件组句柄

/* 事件位定义 */
#define WIFI_CONNECTED_BIT          BIT0  // WiFi STA 连接成功事件位
#define WIFI_FAIL_BIT               BIT1  // WiFi STA 连接失败事件位 (达到最大重试次数)
#define WIFI_CONFIG_SAVED_BIT       BIT2  // SoftAP 配网成功保存配置事件位
#define FACTORY_RESET_BIT           BIT3  // 恢复出厂设置请求事件位
#define WEBSOCKET_CONNECTED_BIT     BIT4  // WebSocket 连接成功事件位
#define WEBSOCKET_DISCONNECTED_BIT  BIT5  // WebSocket 断开连接事件位

/**************************** 板级硬件配置 ****************************/
/* I2C 配置 */
#define BOARD_I2C_NUM             0       // I2C 控制器的通道编号
#define BOARD_I2C_SDA_IO          1       // I2C 数据引脚
#define BOARD_I2C_SCL_IO          2       // I2C 时钟引脚
#define BOARD_I2C_FREQ_HZ         100000  // I2C 时钟频率 (100kHz)

/* 恢复出厂设置按键配置 */
#define BOARD_FACTORY_RESET_GPIO  0     // BOOT 按键 GPIO
#define BOARD_FACTORY_RESET_LONG_PRESS_TIME_MS CONFIG_FACTORY_RESET_LONG_PRESS_TIME_MS  // 长按触发恢复出厂设置的时间 (毫秒)

/* 功放控制GPIO */
#define BOARD_PA_EN_GPIO          3     // 功放使能引脚 (高电平有效)

/**************************** 音频系统配置 ****************************/
/* 音频公共配置 */
#define BOARD_AUDIO_SAMPLE_RATE   CONFIG_AUDIO_SAMPLE_RATE   // 音频采样率 (Hz)
#define BOARD_AUDIO_BIT_WIDTH     CONFIG_AUDIO_BIT_WIDTH     // 音频位宽
#define BOARD_AUDIO_CHANNELS      CONFIG_AUDIO_CHANNELS      // 音频通道数
#define BOARD_AUDIO_BUFFER_SIZE   CONFIG_AUDIO_BUFFER_SIZE   // 音频缓冲区大小 (字节)
#define BOARD_AUDIO_MCLK_MULTIPLE       256     // MCLK = Sample Rate * MCLK Multiple (修改为与play_test一致)
#define BOARD_AUDIO_MCLK_FREQ_HZ        (BOARD_AUDIO_SAMPLE_RATE * BOARD_AUDIO_MCLK_MULTIPLE) // MCLK 频率 (注意: 播放时需根据16kHz重新计算)

/* ES8311 (播放) 配置 */
#define BOARD_ES8311_I2C_ADDR         ES8311_ADDRRES_0  // ES8311 I2C 地址
#define BOARD_ES8311_I2S_NUM          0       // I2S 控制器编号
#define BOARD_ES8311_MCLK_IO          38      // MCLK 引脚
#define BOARD_ES8311_BCK_IO           14      // Bit Clock (BCLK) 引脚
#define BOARD_ES8311_WS_IO            13      // Word Select (WS/LRCK) 引脚
#define BOARD_ES8311_DO_IO            45      // Data Out (DOUT) 引脚
#define BOARD_ES8311_DI_IO            -1      // Data In (DIN) 引脚 (ES8311 不使用)
#define BOARD_ES8311_VOLUME           70      // 默认播放音量 (0-100)

/* ES7210 (录音) 配置 */
#define BOARD_ES7210_I2C_ADDR         0x41    // ES7210 I2C 地址
#define BOARD_ES7210_I2S_NUM          0       // I2S 控制器编号
#define BOARD_ES7210_MCLK_IO          38      // MCLK 引脚
#define BOARD_ES7210_BCK_IO           14      // Bit Clock (BCLK) 引脚
#define BOARD_ES7210_WS_IO            13      // Word Select (WS/LRCK) 引脚
#define BOARD_ES7210_DI_IO            12      // Data In (DIN) 引脚
#define BOARD_ES7210_DO_IO            -1      // Data Out (DOUT) 引脚 (ES7210 不使用)
#define BOARD_ES7210_MIC_GAIN         ES7210_MIC_GAIN_30DB  // 麦克风增益
#define BOARD_ES7210_MIC_BIAS         ES7210_MIC_BIAS_2V87  // 麦克风偏置电压
#define BOARD_ES7210_ADC_VOLUME       0       // ADC 音量
#define BOARD_ES7210_AUDIO_FORMAT     ES7210_I2S_FMT_I2S    // 音频格式
#define BOARD_ES7210_I2S_SLOT_MASK    (I2S_TDM_SLOT0 | I2S_TDM_SLOT1) // 使用的 I2S 时隙

/* 音频缓冲区配置 */
#define BOARD_AUDIO_RECORD_CHUNK_SIZE (1024 * 2) // 每次录音读取的数据块大小

/**************************** WiFi 配置 ****************************/
/* WiFi STA 模式配置 */
#define BOARD_WIFI_MAX_RETRY        5       // STA 模式连接失败最大重试次数
#define BOARD_WIFI_NVS_NAMESPACE    "wifi_config" // NVS 命名空间
#define BOARD_WIFI_SSID_KEY         "ssid"        // NVS 中存储 SSID 的键名
#define BOARD_WIFI_PASSWORD_KEY     "password"    // NVS 中存储密码的键名
#define BOARD_WIFI_SSID             CONFIG_WIFI_SSID             // 默认WiFi SSID
#define BOARD_WIFI_PASSWORD         CONFIG_WIFI_PASSWORD         // 默认WiFi密码
#define BOARD_WIFI_CONNECT_TIMEOUT_MS CONFIG_WIFI_CONNECT_TIMEOUT_MS // WiFi连接超时时间
#define BOARD_WIFI_RECONNECT_INTERVAL_MS CONFIG_WIFI_RECONNECT_INTERVAL_MS // WiFi重连间隔

/* WiFi SoftAP 配网模式配置 */
#define BOARD_SOFTAP_SSID_PREFIX    "ESP32-S3-Config-" // SoftAP SSID 前缀 (后面会追加MAC地址后三字节)
#define BOARD_SOFTAP_PASSWORD       ""               // SoftAP 密码 (空表示开放网络)
#define BOARD_SOFTAP_CHANNEL        1                // SoftAP 信道
#define BOARD_SOFTAP_MAX_CONN       4                // SoftAP 最大连接数
#define BOARD_HTTP_SERVER_PORT      80               // 配网 HTTP 服务器端口
#define BOARD_DNS_SERVER_PORT       53               // Captive Portal DNS 服务器端口

/**************************** WebSocket 配置 ****************************/
#define BOARD_WS_SERVER_URL         CONFIG_WS_SERVER_URL         // WebSocket 服务器URL
#define BOARD_WS_DEVICE_CLIENT_ID   CONFIG_WS_DEVICE_CLIENT_ID   // 设备 WebSocket 客户端 ID
#define BOARD_WS_RECONNECT_INTERVAL_MS CONFIG_WS_RECONNECT_INTERVAL_MS // WebSocket 重连间隔
#define BOARD_WS_RECONNECT_TIMEOUT_MS 10000          // WebSocket 重连超时时间 (毫秒)
#define BOARD_WS_NETWORK_TIMEOUT_MS 10000          // WebSocket 网络超时时间 (毫秒)
#define BOARD_WS_PING_INTERVAL_SEC  10               // WebSocket PING 发送间隔 (秒)

/**************************** 函数声明 ****************************/

/**
 * @brief 初始化开发板基础硬件 (I2C, GPIO 等)
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_init(void);

/**
 * @brief 检查板载芯片状态 (如 I2C 设备)
 * @details 通过尝试与配置的 I2C 设备通信来检查其状态
 * @return esp_err_t ESP_OK 所有芯片正常, 其他表示有芯片异常
 */
esp_err_t board_check_chip_status(void);

/**
 * @brief 控制功放 (PA) 电源
 * @param enable true: 打开功放, false: 关闭功放
 */
void board_pa_power(bool enable);

/**
 * @brief 初始化 ES8311 音频播放功能
 * @param[out] tx_handle_out 返回 I2S 发送通道句柄
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_audio_playback_init(i2s_chan_handle_t *tx_handle_out);

/**
 * @brief 初始化 ES7210 音频录音功能
 * @param[out] rx_handle_out 返回 I2S 接收通道句柄
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_audio_record_init(i2s_chan_handle_t *rx_handle_out);

/**
 * @brief 播放音频数据
 * @param tx_handle I2S 发送通道句柄
 * @param buffer 存储音频数据的缓冲区
 * @param buffer_size 要播放的数据大小 (字节)
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_audio_play(i2s_chan_handle_t tx_handle, const uint8_t *buffer, size_t buffer_size);

/**
 * @brief 录制音频数据
 * @param rx_handle I2S 接收通道句柄
 * @param buffer 存储录音数据的缓冲区
 * @param buffer_size 缓冲区大小 (字节)
 * @param[out] bytes_read 实际录制的数据字节数
 * @param timeout_ms 读取超时时间 (毫秒)
 * @return esp_err_t ESP_OK 成功, ESP_ERR_TIMEOUT 超时, 其他失败
 */
esp_err_t board_audio_record(i2s_chan_handle_t rx_handle, uint8_t *buffer, size_t buffer_size, size_t *bytes_read, uint32_t timeout_ms);

/**
 * @brief 卸载音频 I2S 通道
 * @param handle 要卸载的 I2S 通道句柄
 */
void board_audio_i2s_deinit(i2s_chan_handle_t handle);

/**
 * @brief 初始化 WiFi (STA 模式)
 * @details 初始化 WiFi 栈, 事件循环, 并根据 NVS 中的配置尝试连接.
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_wifi_sta_init(void);

/**
 * @brief 等待 WiFi STA 连接结果
 * @param timeout_ms 超时时间 (毫秒), 0 表示无限等待
 * @return esp_err_t ESP_OK 连接成功, ESP_FAIL 连接失败, ESP_ERR_TIMEOUT 超时
 */
esp_err_t board_wifi_sta_wait_connected(uint32_t timeout_ms);

/**
 * @brief 获取 WiFi STA 连接信息
 * @param[out] ip_addr (可选) 输出 IP 地址字符串 (需保证足够空间, 至少 16 字节)
 * @param[out] ssid (可选) 输出 SSID 字符串 (需保证足够空间, 至少 33 字节)
 * @param[out] rssi (可选) 输出信号强度 (dBm)
 * @return esp_err_t ESP_OK 成功, 其他失败 (如未连接)
 */
esp_err_t board_wifi_sta_get_info(char *ip_addr, char *ssid, int8_t *rssi);

/**
 * @brief 检查 NVS 中是否存有有效的 WiFi 配置
 * @param[out] ssid (可选) 如果有有效配置, 输出 SSID (需保证足够空间)
 * @param[out] password (可选) 如果有有效配置, 输出密码 (需保证足够空间)
 * @return true: 有有效配置, false: 无有效配置
 */
bool board_wifi_has_valid_config(char *ssid, char *password);

/**
 * @brief 保存 WiFi 配置到 NVS
 * @param ssid 要保存的 SSID
 * @param password 要保存的密码
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_wifi_save_config(const char *ssid, const char *password);

/**
 * @brief 初始化并启动 WiFi SoftAP 配网模式
 * @details 启动 SoftAP, DNS 服务器 (用于强制门户), HTTP 服务器 (用于配网页).
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_wifi_softap_start(void);

/**
 * @brief 停止 WiFi SoftAP 配网模式
 * @details 停止 SoftAP, DNS 服务器, HTTP 服务器.
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_wifi_softap_stop(void);

/**
 * @brief 初始化恢复出厂设置按键及中断
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_factory_reset_init(void);

/**
 * @brief 执行恢复出厂设置操作
 * @details 清除 NVS 中的 WiFi 配置并重启设备.
 */
void board_factory_reset_task(void *arg);

/**
 * @brief 初始化 WebSocket 客户端
 * @param[out] client_handle_out 返回 WebSocket 客户端句柄
 * @param event_handler WebSocket 事件处理回调函数
 * @param handler_args 传递给回调函数的参数
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_websocket_init(esp_websocket_client_handle_t *client_handle_out, esp_event_handler_t event_handler, void *handler_args);

/**
 * @brief 启动 WebSocket 客户端连接
 * @param client WebSocket 客户端句柄
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_websocket_start(esp_websocket_client_handle_t client);

/**
 * @brief 停止 WebSocket 客户端连接
 * @param client WebSocket 客户端句柄
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_websocket_stop(esp_websocket_client_handle_t client);

/**
 * @brief 销毁 WebSocket 客户端
 * @param client WebSocket 客户端句柄
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_websocket_destroy(esp_websocket_client_handle_t client);

/**
 * @brief 获取设备 MAC 地址字符串
 * @param[out] mac_str 输出 MAC 地址字符串 (格式: "XX:XX:XX:XX:XX:XX", 需要至少 18 字节)
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_get_mac_address_string(char *mac_str);

/**
 * @brief 初始化 I2C 总线
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_i2c_init(void);

#ifdef __cplusplus
}
#endif

#endif /* _BOARD_H_ */ 