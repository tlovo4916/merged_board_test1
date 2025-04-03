/**
 * @file main.c
 * @brief ESP32-S3 开发板主程序
 * 
 * 实现功能：
 * 1. 上电后检查所有协议（I2C, I2S），确认各芯片正常工作
 * 2. 自动进入配网流程，如果已经配置过WiFi，则直接连接
 * 3. 配网并连接到WiFi后，重启后第二次连接到WiFi时，开启WebSocket与服务器建立实时通信
 * 4. 服务器可以发送事件，开启录音笔功能
 * 5. 模块化的事件处理系统，方便后续和服务器同步事件
 */

#include "board.h"
#include <inttypes.h>

static const char *TAG = "MAIN";

// 录音缓冲区
static uint8_t *s_audio_buffer = NULL;
static size_t s_audio_buffer_size = BOARD_AUDIO_BUFFER_SIZE;

// I2S通道句柄
static i2s_chan_handle_t s_tx_handle = NULL; // 播放
static i2s_chan_handle_t s_rx_handle = NULL; // 录音

// WebSocket客户端句柄
static esp_websocket_client_handle_t s_ws_client = NULL;

// 引用嵌入的PCM文件
extern const uint8_t pcm_1_pcm_start[] asm("_binary_1_pcm_start");
extern const uint8_t pcm_1_pcm_end[] asm("_binary_1_pcm_end");
extern const uint8_t pcm_2_pcm_start[] asm("_binary_2_pcm_start");
extern const uint8_t pcm_2_pcm_end[]   asm("_binary_2_pcm_end");
extern const uint8_t pcm_3_pcm_start[] asm("_binary_3_pcm_start");
extern const uint8_t pcm_3_pcm_end[]   asm("_binary_3_pcm_end");
extern const uint8_t pcm_4_pcm_start[] asm("_binary_4_pcm_start");
extern const uint8_t pcm_4_pcm_end[]   asm("_binary_4_pcm_end");

// 系统状态
typedef enum {
    SYSTEM_STATE_INIT,           // 初始化
    SYSTEM_STATE_WIFI_CONFIG,    // WiFi配网模式
    SYSTEM_STATE_WIFI_CONNECTING,// 连接WiFi中
    SYSTEM_STATE_WIFI_CONNECTED, // WiFi已连接
    SYSTEM_STATE_WS_CONNECTED,   // WebSocket已连接
    SYSTEM_STATE_RECORDING,      // 录音中
    SYSTEM_STATE_PLAYING,        // 播放中
    SYSTEM_STATE_ERROR,          // 错误状态
} system_state_t;

static system_state_t s_system_state = SYSTEM_STATE_INIT;

// 添加到文件顶部的变量声明部分
static bool first_connection = true;

// 添加重置连接标志的定时器回调函数
static void reset_connection_timer_cb(TimerHandle_t timer)
{
    // 重置first_connection标志，这样下次连接时会再次播放提示音
    first_connection = true;
    ESP_LOGI(TAG, "长时间断开连接，重置首次连接标志");
}

// 函数声明
static void play_recorded_audio(size_t bytes_recorded);
static void play_default_audio(void);
static esp_err_t play_pcm_by_id(int pcm_id);

/**
 * @brief 根据ID播放PCM文件
 * @param pcm_id PCM文件ID (1-4)
 * @return ESP_OK成功，其他失败
 * @note 当前仅实现了1.pcm，需要播放其他PCM文件，请在CMakeLists.txt中添加对应文件
 *       并添加相应的extern声明
 */
static esp_err_t play_pcm_by_id(int pcm_id)
{
    esp_err_t ret;
    const uint8_t *pcm_start = NULL;
    const uint8_t *pcm_end = NULL;
    
    // 根据ID选择PCM文件
    switch (pcm_id) {
        case 1:
            pcm_start = pcm_1_pcm_start;
            pcm_end = pcm_1_pcm_end;
            break;
        case 2: 
            pcm_start = pcm_2_pcm_start;
            pcm_end = pcm_2_pcm_end;
            break;
        case 3:
            pcm_start = pcm_3_pcm_start;
            pcm_end = pcm_3_pcm_end;
            break;
        case 4:
            pcm_start = pcm_4_pcm_start;
            pcm_end = pcm_4_pcm_end;
            break;
        default:
            ESP_LOGE(TAG, "无效的PCM ID: %d", pcm_id);
            return ESP_ERR_INVALID_ARG;
    }
    
    // 检查是否获取到了PCM文件
    if (pcm_start == NULL || pcm_end == NULL) {
        ESP_LOGE(TAG, "PCM文件 %d 未找到或未嵌入", pcm_id);
        return ESP_ERR_NOT_FOUND;
    }
    
    // 初始化播放设备 (如果未初始化)
    if (s_tx_handle == NULL) {
        ret = board_audio_playback_init(&s_tx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "初始化播放设备失败: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    
    // 更新系统状态
    s_system_state = SYSTEM_STATE_PLAYING;
    
    // 计算PCM文件大小
    size_t pcm_size = pcm_end - pcm_start;
    
    // 开始播放
    ESP_LOGI(TAG, "开始播放PCM %d，数据大小: %u 字节", pcm_id, (unsigned int)pcm_size);
    
    ret = board_audio_play(s_tx_handle, pcm_start, pcm_size);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "播放失败: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "播放完成");
        
        // 添加额外的延迟，确保所有音频数据都已经输出到功放
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // 恢复系统状态
    s_system_state = SYSTEM_STATE_INIT;
    
    return ret;
}

/**
 * @brief 播放默认的音频文件(1.pcm)
 */
static void play_default_audio(void)
{
    play_pcm_by_id(1);
}

/**
 * @brief 处理恢复出厂设置事件
 */
static void handle_factory_reset_event(void)
{
    ESP_LOGW(TAG, "检测到恢复出厂设置请求");
    
    // 播放提示音
    play_default_audio();
    
    // 创建一个任务来执行恢复出厂设置
    xTaskCreate(board_factory_reset_task, 
                "factory_reset", 
                4096,  // 增加栈大小
                NULL, 
                5, 
                NULL);
    
    // 不会执行到这里
}

/**
 * @brief 启动录音功能
 */
static void start_audio_recording(int seconds)
{
    esp_err_t ret;
    
    if (s_system_state == SYSTEM_STATE_RECORDING) {
        ESP_LOGW(TAG, "录音已经在进行中");
        return;
    }
    
    // 检查是否有足够的录音缓冲区
    if (s_audio_buffer == NULL) {
        s_audio_buffer = heap_caps_malloc(s_audio_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_audio_buffer == NULL) {
            // 尝试使用内部内存
            ESP_LOGW(TAG, "PSRAM分配失败，尝试使用内部内存");
            s_audio_buffer_size = s_audio_buffer_size / 2; // 减小缓冲区大小
            s_audio_buffer = heap_caps_malloc(s_audio_buffer_size, MALLOC_CAP_8BIT);
            
            if (s_audio_buffer == NULL) {
                ESP_LOGE(TAG, "分配录音缓冲区失败");
                return;
            }
        }
    }
    
    // 初始化录音设备 (如果未初始化)
    if (s_rx_handle == NULL) {
        ret = board_audio_record_init(&s_rx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "初始化录音设备失败: %s", esp_err_to_name(ret));
            return;
        }
    }
    
    // 更新系统状态
    s_system_state = SYSTEM_STATE_RECORDING;
    
    // 开始录音
    ESP_LOGI(TAG, "开始录音, 时长: %d 秒", seconds);
    
    size_t bytes_read = 0;
    ret = board_audio_record(s_rx_handle, s_audio_buffer, s_audio_buffer_size, &bytes_read, seconds * 1000);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "录音失败: %s", esp_err_to_name(ret));
        s_system_state = SYSTEM_STATE_WIFI_CONNECTED;
        return;
    }
    
    ESP_LOGI(TAG, "录音完成，共录制 %u 字节数据", (unsigned int)bytes_read);
    
    // 发送录音完成通知给服务器
    if (s_ws_client != NULL && esp_websocket_client_is_connected(s_ws_client)) {
        char response[128];
        snprintf(response, sizeof(response), 
                 "{\"event\":\"record_complete\",\"size\":%u,\"duration\":%d}", 
                 (unsigned int)bytes_read, seconds);
        esp_websocket_client_send_text(s_ws_client, response, strlen(response), portMAX_DELAY);
    }
    
    // 可选：播放录音内容进行测试
    if (bytes_read > 0) {
        play_recorded_audio(bytes_read);
    }
    
    // 恢复系统状态
    s_system_state = SYSTEM_STATE_WIFI_CONNECTED;
}

/**
 * @brief 播放录制的音频
 */
static void play_recorded_audio(size_t bytes_recorded)
{
    esp_err_t ret;
    
    if (s_audio_buffer == NULL || bytes_recorded == 0) {
        ESP_LOGE(TAG, "没有可播放的录音数据");
        return;
    }
    
    // 初始化播放设备 (如果未初始化)
    if (s_tx_handle == NULL) {
        ret = board_audio_playback_init(&s_tx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "初始化播放设备失败: %s", esp_err_to_name(ret));
            return;
        }
    }
    
    // 更新系统状态
    s_system_state = SYSTEM_STATE_PLAYING;
    
    // 开始播放
    ESP_LOGI(TAG, "开始播放录音，数据大小: %u 字节", (unsigned int)bytes_recorded);
    
    ret = board_audio_play(s_tx_handle, s_audio_buffer, bytes_recorded);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "播放失败: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "播放完成");
    }
    
    // 恢复系统状态
    s_system_state = SYSTEM_STATE_WIFI_CONNECTED;
}

/**
 * @brief WebSocket事件处理函数
 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base, 
                                  int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket 已连接");
            
            // 使用全局变量跟踪是否是首次连接
            if (first_connection) {
                // 播放连接成功提示音
                play_pcm_by_id(4);
                first_connection = false;
            } else {
                ESP_LOGI(TAG, "WebSocket 重新连接成功，跳过提示音播放");
            }

            // 设置WebSocket连接事件位
            xEventGroupSetBits(board_event_group, WEBSOCKET_CONNECTED_BIT);
            
            // 连接后发送客户端ID消息
            char connect_msg[128];
            snprintf(connect_msg, sizeof(connect_msg), 
                    "{\"event\":\"device_connected\",\"data\":{\"clientId\":\"%s\",\"type\":\"esp32s3\"}}", 
                    BOARD_WS_DEVICE_CLIENT_ID);
            esp_websocket_client_send_text(s_ws_client, connect_msg, strlen(connect_msg), portMAX_DELAY);
            
            // 更新系统状态
            s_system_state = SYSTEM_STATE_WS_CONNECTED;
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WebSocket 已断开连接");
            
            // 创建一个定时器，如果断开超过一定时间（例如30秒），则重置首次连接标志
            static TimerHandle_t reset_timer = NULL;
            if (reset_timer == NULL) {
                reset_timer = xTimerCreate(
                    "reset_connection",
                    pdMS_TO_TICKS(30000), // 30秒
                    pdFALSE,              // 不自动重载
                    NULL,
                    reset_connection_timer_cb
                );
            }
            
            // 启动或重新启动定时器
            if (reset_timer != NULL) {
                if (xTimerIsTimerActive(reset_timer)) {
                    xTimerReset(reset_timer, 0);
                } else {
                    xTimerStart(reset_timer, 0);
                }
            }
            
            // 清除WebSocket连接事件位
            xEventGroupClearBits(board_event_group, WEBSOCKET_CONNECTED_BIT);
            xEventGroupSetBits(board_event_group, WEBSOCKET_DISCONNECTED_BIT);
            
            // 恢复系统状态
            s_system_state = SYSTEM_STATE_WIFI_CONNECTED;
            break;
            
        case WEBSOCKET_EVENT_DATA:
            // 处理收到的数据
            if (data->data_len > 0) {
                ESP_LOGI(TAG, "收到数据: %.*s", data->data_len, (char *)data->data_ptr);
                
                // 使用cJSON解析
                cJSON *root = cJSON_Parse(data->data_ptr);
                
                if (root) {
                    // 获取event字段
                    cJSON *event = cJSON_GetObjectItem(root, "event");
                    // 获取data字段
                    cJSON *data_obj = cJSON_GetObjectItem(root, "data");
                    
                    if (cJSON_IsString(event) && event->valuestring != NULL) {
                        ESP_LOGI(TAG, "收到事件: %s", event->valuestring);
                        
                        // 处理录音事件
                        if (strcmp(event->valuestring, "start_recording") == 0) {
                            // 默认录音时长为5秒
                            int duration = 5;
                            
                            // 从data字段获取参数
                            if (data_obj && cJSON_IsObject(data_obj)) {
                                cJSON *duration_obj = cJSON_GetObjectItem(data_obj, "duration");
                                if (cJSON_IsNumber(duration_obj)) {
                                    duration = duration_obj->valueint;
                                    if (duration < 1) duration = 1;
                                    if (duration > 60) duration = 60; // 限制最大时长
                                }
                            }
                            
                            ESP_LOGI(TAG, "开始录音，时长: %d秒", duration);
                            start_audio_recording(duration);
                            
                            // 发送确认消息
                            char response[128];
                            snprintf(response, sizeof(response), 
                                    "{\"event\":\"recording_started\",\"data\":{\"duration\":%d}}", 
                                    duration);
                            esp_websocket_client_send_text(s_ws_client, response, strlen(response), portMAX_DELAY);
                        }
                        // 处理重启事件
                        else if (strcmp(event->valuestring, "restart") == 0) {
                            ESP_LOGW(TAG, "收到重启命令，设备将在3秒后重启");
                            
                            // 发送确认消息
                            esp_websocket_client_send_text(s_ws_client, 
                                "{\"event\":\"restart_ack\",\"data\":{\"status\":\"ok\"}}", 
                                -1, portMAX_DELAY);
                            vTaskDelay(pdMS_TO_TICKS(3000));
                            esp_restart();
                        }
                        // 处理播放PCM文件事件
                        else if (strcmp(event->valuestring, "play_pcm") == 0) {
                            // 默认播放1.pcm
                            int pcm_id = 1;
                            
                            // 从data字段获取参数
                            if (data_obj && cJSON_IsObject(data_obj)) {
                                cJSON *id_obj = cJSON_GetObjectItem(data_obj, "id");
                                if (cJSON_IsNumber(id_obj)) {
                                    pcm_id = id_obj->valueint;
                                }
                            }
                            
                            ESP_LOGI(TAG, "收到播放PCM命令，ID: %d", pcm_id);
                            
                            // 播放指定PCM文件
                            esp_err_t ret = play_pcm_by_id(pcm_id);
                            
                            // 发送播放结果
                            char response[128];
                            snprintf(response, sizeof(response), 
                                    "{\"event\":\"play_pcm_result\",\"data\":{\"id\":%d,\"status\":\"%s\"}}", 
                                    pcm_id, (ret == ESP_OK) ? "ok" : "fail");
                            esp_websocket_client_send_text(s_ws_client, response, strlen(response), portMAX_DELAY);
                        }
                        // 处理其他事件...
                    } else {
                        ESP_LOGW(TAG, "收到的JSON数据中没有有效的event字段");
                    }
                    
                    cJSON_Delete(root);
                } else {
                    ESP_LOGW(TAG, "收到无效的JSON格式数据");
                }
            }
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket 发生错误");
            break;
            
        default:
            break;
    }
}

/**
 * @brief 工厂重置监听任务
 */
static void factory_reset_monitor_task(void *arg)
{
    while (1) {
        // 检查是否触发了恢复出厂设置事件
        EventBits_t bits = xEventGroupWaitBits(
            board_event_group,      // 事件组句柄
            FACTORY_RESET_BIT,      // 等待的位
            pdTRUE,                 // 清除位
            pdFALSE,                // 任意一位满足即可返回
            pdMS_TO_TICKS(1000)     // 阻塞时间(1秒)
        );
        
        if (bits & FACTORY_RESET_BIT) {
            handle_factory_reset_event();
            // 不会执行到这里，因为设备会重启
        }
        
        // 简单空闲时的保持活动
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief 初始化WebSocket连接
 */
static void init_websocket_connection(void)
{
    // 检查WiFi连接状态
    if (!(xEventGroupGetBits(board_event_group) & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi未连接，无法初始化WebSocket连接");
        return;
    }
    
    // 检查是否已初始化
    if (s_ws_client != NULL) {
        ESP_LOGI(TAG, "WebSocket客户端已初始化");
        return;
    }
    
    ESP_LOGI(TAG, "初始化WebSocket客户端...");
    
    // 初始化WebSocket客户端
    esp_err_t ret = board_websocket_init(&s_ws_client, websocket_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化WebSocket客户端失败: %s", esp_err_to_name(ret));
        return;
    }
    
    // 启动WebSocket连接
    ret = board_websocket_start(s_ws_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动WebSocket连接失败: %s", esp_err_to_name(ret));
        return;
    }
}

/**
 * @brief 主函数
 */
void app_main(void)
{
    esp_err_t ret;
    
    ESP_LOGI(TAG, "=== ESP32-S3 开发板启动 ===");
    ESP_LOGI(TAG, "IDF版本: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "可用内存: %" PRIu32 " 字节", esp_get_free_heap_size());
    ESP_LOGI(TAG, "===========================");
    
    // 初始化板载硬件
    ret = board_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "板载硬件初始化失败: %s", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
        return;
    }
    
    // I2C总线进行额外稳定等待
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 首先进行音频系统初始化和测试
    // 初始化ES8311用于播放
    ESP_LOGI(TAG, "初始化音频播放系统...");
    ret = board_audio_playback_init(&s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "音频播放系统初始化失败: %s", esp_err_to_name(ret));
        // 不退出，继续尝试初始化录音部分
    } else {
        ESP_LOGI(TAG, "音频播放系统初始化成功");
    }
    
    /*
    // 初始化ES7210用于录音
    ESP_LOGI(TAG, "初始化音频录音系统...");
    ret = board_audio_record_init(&s_rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "音频录音系统初始化失败: %s", esp_err_to_name(ret));
        // 不退出，继续初始化其他系统
    } else {
        ESP_LOGI(TAG, "音频录音系统初始化成功");
    }
    */
    
    // 检查芯片状态
    ret = board_check_chip_status();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "部分硬件检测失败，但将继续运行: %s", esp_err_to_name(ret));
    }
    
    // 创建工厂重置监听任务
    xTaskCreate(factory_reset_monitor_task, "factory_reset_monitor", 
                2048, NULL, 3, NULL);
    
    // 初始化WiFi
    ret = board_wifi_sta_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi初始化失败: %s", esp_err_to_name(ret));
        goto config_mode;
    }
    
    // 检查是否有有效的WiFi配置
    char ssid[33] = {0};
    bool has_wifi_config = board_wifi_has_valid_config(ssid, NULL);
    
    // 如果没有WiFi配置，表示首次上电或恢复出厂设置后，播放默认音频
    if (!has_wifi_config && s_tx_handle != NULL) {
        ESP_LOGI(TAG, "首次启动或恢复出厂设置后，播放欢迎音频");
        play_default_audio();
    }
    
    if (has_wifi_config) {
        // 有配置，等待WiFi连接
        ESP_LOGI(TAG, "发现WiFi配置，SSID: %s", ssid);
        s_system_state = SYSTEM_STATE_WIFI_CONNECTING;
        
        // 等待WiFi连接结果(30秒超时)
        ret = board_wifi_sta_wait_connected(30000);
        
        if (ret == ESP_OK) {
            // 连接成功
            char ip_addr[16] = {0};
            int8_t rssi = 0;
            
            // 获取WiFi连接信息
            if (board_wifi_sta_get_info(ip_addr, NULL, &rssi) == ESP_OK) {
                ESP_LOGI(TAG, "WiFi连接成功: IP=%s, 信号=%d dBm", ip_addr, rssi);
            }
            
            // 更新系统状态
            s_system_state = SYSTEM_STATE_WIFI_CONNECTED;
            
            // 初始化WebSocket连接
            init_websocket_connection();
        } else {
            // 连接失败，进入配网模式
            ESP_LOGW(TAG, "WiFi连接失败: %s", esp_err_to_name(ret));
            goto config_mode;
        }
    } else {
        // 没有配置，启动配网模式
config_mode:
        ESP_LOGI(TAG, "启动WiFi配网模式");
        
        // 播放进入配网模式提示音
        if (s_tx_handle != NULL) { // 确保播放设备已初始化
            play_pcm_by_id(2);
        }
        
        s_system_state = SYSTEM_STATE_WIFI_CONFIG;
        
        // 启动SoftAP和配网服务器
        ret = board_wifi_softap_start();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "启动配网模式失败: %s", esp_err_to_name(ret));
        }
        
        // 等待配网完成事件
        while (1) {
            EventBits_t bits = xEventGroupWaitBits(
                board_event_group,        // 事件组句柄
                WIFI_CONFIG_SAVED_BIT,    // 等待的位
                pdFALSE,                  // 不清除位
                pdFALSE,                  // 任意一位满足即可返回
                pdMS_TO_TICKS(1000)       // 阻塞时间(1秒)
            );
            
            if (bits & WIFI_CONFIG_SAVED_BIT) {
                // 配网完成，播放成功提示音
                ESP_LOGI(TAG, "配网信息已保存");
                if (s_tx_handle != NULL) { // 确保播放设备已初始化
                     play_pcm_by_id(3);
                }

                // 重启设备
                ESP_LOGI(TAG, "配网完成，设备将在3秒后重启...");
                vTaskDelay(pdMS_TO_TICKS(3000));
                esp_restart();
                break;
            }
            
            // 简单保持活动
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    
    // 主循环
    while (1) {
        // 根据当前系统状态执行不同的任务
        switch (s_system_state) {
            case SYSTEM_STATE_WIFI_CONNECTED:
            case SYSTEM_STATE_WS_CONNECTED:
                // 检查WebSocket连接状态
                if (s_ws_client != NULL && !esp_websocket_client_is_connected(s_ws_client)) {
                    ESP_LOGW(TAG, "WebSocket连接已断开，尝试重连");
                    
                    // 重新初始化WebSocket连接
                    if (esp_websocket_client_destroy(s_ws_client) == ESP_OK) {
                        s_ws_client = NULL;
                        vTaskDelay(pdMS_TO_TICKS(1000)); // 等待1秒
                        init_websocket_connection();
                    }
                }
                break;
                
            default:
                break;
        }
        
        // 每秒检查一次系统状态
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
} 