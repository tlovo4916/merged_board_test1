/**
 * @file board.c
 * @brief ESP32-S3 开发板驱动
 * @details 整合了音频 (ES8311, ES7210), WiFi (STA, SoftAP配网), WebSocket 客户端,
 *          以及其他板级功能 (I2C, GPIO, NVS, 恢复出厂设置).
 */

#include "board.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <inttypes.h>
#include "driver/i2c.h"
#include "es7210.h"
#include "es8311.h"

/* 标记不同功能模块的日志标签 */
static const char *TAG = "BOARD";           // 通用驱动
static const char *TAG_AUDIO = "AUDIO";     // 音频系统
static const char *TAG_WIFI = "WIFI";       // WiFi 系统
static const char *TAG_CONFIG = "CONFIG";   // 配网

// 函数声明
static void factory_reset_btn_timer_cb(TimerHandle_t xTimer);
void board_factory_reset_task(void *arg);

/**************************** 全局变量 ****************************/
/* 全局事件组 */
EventGroupHandle_t board_event_group = NULL;

/* WiFi 相关 */
static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_wifi_retry_num = 0;

/* WebSocket 相关全局变量 */
static esp_websocket_client_handle_t s_websocket_client = NULL;

/* HTTP 服务器变量 (用于配网) */
static httpd_handle_t s_config_server_handle = NULL;
static esp_netif_t *s_ap_netif = NULL;

// 定义按钮事件队列句柄
static QueueHandle_t factory_reset_btn_queue = NULL;

// 定义按钮事件类型
typedef enum {
    FACTORY_RESET_BTN_PRESS,
    FACTORY_RESET_BTN_RELEASE
} factory_reset_btn_event_t;

/**
 * @brief 初始化开发板基础硬件
 */
esp_err_t board_init(void)
{
    esp_err_t ret;
    
    // 初始化 NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 分区被擦除或格式化
        ESP_LOGW(TAG, "正在擦除并重新初始化 NVS...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 创建全局事件组
    if (board_event_group == NULL) {
        board_event_group = xEventGroupCreate();
        if (board_event_group == NULL) {
            ESP_LOGE(TAG, "创建全局事件组失败");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "创建全局事件组成功");
    }
    
    // 初始化 I2C 总线
    ret = board_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 初始化恢复出厂设置按键
    ret = board_factory_reset_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "恢复出厂设置按键初始化失败: %s", esp_err_to_name(ret));
        // 继续执行，这不是致命错误
    }
    
    // 初始化功放控制引脚
    esp_rom_gpio_pad_select_gpio(BOARD_PA_EN_GPIO);
    gpio_set_direction(BOARD_PA_EN_GPIO, GPIO_MODE_OUTPUT);
    // 默认关闭功放
    gpio_set_level(BOARD_PA_EN_GPIO, 0);
    
    ESP_LOGI(TAG, "开发板基础硬件初始化完成");
    return ESP_OK;
}

/**
 * @brief 检查板载芯片状态
 */
esp_err_t board_check_chip_status(void)
{
    esp_err_t ret;
    
    // 检查 I2C 总线是否已初始化
    ret = board_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线初始化失败");
        return ret;
    }
    
    // 检查 ES7210 编解码器
    es7210_dev_handle_t es7210_handle = NULL;
    es7210_i2c_config_t es7210_i2c_conf = {
        .i2c_port = BOARD_I2C_NUM,
        .i2c_addr = BOARD_ES7210_I2C_ADDR
    };
    ret = es7210_new_codec(&es7210_i2c_conf, &es7210_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 编解码器初始化失败");
        return ret;
    }
    
    // 检查 ES8311 编解码器
    es8311_handle_t es8311_handle = es8311_create(BOARD_I2C_NUM, BOARD_ES8311_I2C_ADDR);
    if (es8311_handle == NULL) {
        ESP_LOGE(TAG, "ES8311 编解码器初始化失败");
        es7210_del_codec(es7210_handle);
        return ESP_FAIL;
    }
    
    // 释放编解码器
    es7210_del_codec(es7210_handle);
    es8311_delete(es8311_handle);
    
    return ESP_OK;
}

/**
 * @brief 初始化 I2C 总线
 * @return esp_err_t ESP_OK 成功, 其他失败
 */
esp_err_t board_i2c_init(void)
{
    static bool is_initialized = false;
    
    // 如果I2C已经初始化，直接返回成功
    if (is_initialized) {
        ESP_LOGI(TAG, "I2C已经初始化，跳过初始化");
        return ESP_OK;
    }
    
    // 先检查I2C是否已经初始化
    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_I2C_SDA_IO,
        .scl_io_num = BOARD_I2C_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE, // 启用GPIO上拉
        .scl_pullup_en = GPIO_PULLUP_ENABLE, // 启用GPIO上拉
        .master.clk_speed = BOARD_I2C_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(BOARD_I2C_NUM, &i2c_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C参数配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 尝试安装I2C驱动
    ret = i2c_driver_install(BOARD_I2C_NUM, i2c_conf.mode, 0, 0, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "I2C驱动已安装，继续执行");
        ret = ESP_OK;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C驱动安装失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    is_initialized = true;
    ESP_LOGI(TAG, "I2C接口初始化成功");
    return ESP_OK;
}

/**
 * @brief 控制功放 (PA) 电源
 */
void board_pa_power(bool enable)
{
    // 设置功放使能状态
    gpio_set_level(BOARD_PA_EN_GPIO, enable);
    ESP_LOGI(TAG_AUDIO, "功放电源: %s", enable ? "开启" : "关闭");
}

/**************************** 音频系统相关函数 ****************************/

/**
 * @brief 初始化 ES8311 音频播放功能
 */
esp_err_t board_audio_playback_init(i2s_chan_handle_t *tx_handle_out)
{
    ESP_LOGI(TAG_AUDIO, "初始化音频播放系统...");
    
    if (tx_handle_out == NULL) {
        ESP_LOGE(TAG_AUDIO, "无效参数: tx_handle_out 为空");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;
    
    // 1. 初始化I2C接口
    ret = board_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "初始化I2C失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 等待I2C总线稳定
    vTaskDelay(pdMS_TO_TICKS(20));
    
    // 2. 配置I2S通道 - 如果已经存在通道句柄，则不再重新创建
    if (*tx_handle_out == NULL) {
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_ES8311_I2S_NUM, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true; // 自动清除DMA缓冲区中的旧数据
        ESP_LOGI(TAG_AUDIO, "创建I2S发送通道");
        ret = i2s_new_channel(&chan_cfg, tx_handle_out, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG_AUDIO, "创建I2S通道失败: %s", esp_err_to_name(ret));
            return ret;
        }
    } else {
        ESP_LOGI(TAG_AUDIO, "使用已存在的I2S通道");
    }
    
    // 3. 配置I2S标准模式
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_SAMPLE_RATE), // 使用配置的采样率 (44100Hz)
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_ES8311_MCLK_IO,
            .bclk = BOARD_ES8311_BCK_IO,
            .ws = BOARD_ES8311_WS_IO,
            .dout = BOARD_ES8311_DO_IO,
            .din = BOARD_ES8311_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE;
    
    ESP_LOGI(TAG_AUDIO, "初始化I2S标准模式");
    ret = i2s_channel_init_std_mode(*tx_handle_out, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "初始化I2S标准模式失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 4. 初始化ES8311编解码器
    ESP_LOGI(TAG_AUDIO, "初始化ES8311编解码器");
    
    // 等待I2C总线稳定
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // 创建ES8311句柄
    es8311_handle_t es_handle = es8311_create(BOARD_I2C_NUM, BOARD_ES8311_I2C_ADDR);
    if (!es_handle) {
        ESP_LOGE(TAG_AUDIO, "创建ES8311句柄失败");
        return ESP_FAIL;
    }
    
    // 配置ES8311时钟 - 使用配置的采样率
    uint32_t mclk_freq_hz = BOARD_AUDIO_SAMPLE_RATE * BOARD_AUDIO_MCLK_MULTIPLE;
    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,  
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = mclk_freq_hz, // 使用基于配置采样率计算的MCLK频率
        .sample_frequency = BOARD_AUDIO_SAMPLE_RATE // 使用配置的采样率
    };
    
    // 初始化ES8311
    ret = es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "初始化ES8311失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 配置采样率
    ret = es8311_sample_frequency_config(es_handle, mclk_freq_hz, BOARD_AUDIO_SAMPLE_RATE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "配置ES8311采样率失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 设置音量
    ret = es8311_voice_volume_set(es_handle, BOARD_ES8311_VOLUME, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "设置ES8311音量失败: %s", esp_err_to_name(ret));
        // 即使音量设置失败也继续，但这是一个警告信号
        // return ret; // 不再因为音量设置失败而退出
    } else {
        ESP_LOGI(TAG_AUDIO, "ES8311音量设置成功: %d", BOARD_ES8311_VOLUME);
    }
    
    // 禁用麦克风输入(我们使用ES7210)
    ret = es8311_microphone_config(es_handle, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "配置ES8311麦克风失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 额外稳定性等待
    vTaskDelay(pdMS_TO_TICKS(20));
    
    ESP_LOGI(TAG_AUDIO, "ES8311播放接口初始化成功");
    return ESP_OK;
}

/**
 * @brief 初始化 ES7210 音频录音功能
 */
esp_err_t board_audio_record_init(i2s_chan_handle_t *rx_handle_out)
{
    ESP_LOGI(TAG_AUDIO, "初始化音频录音系统...");
    
    if (rx_handle_out == NULL) {
        ESP_LOGE(TAG_AUDIO, "无效参数: rx_handle_out 为空");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;
    
    // 1. 初始化I2C接口
    ret = board_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "初始化I2C失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 等待I2C总线稳定
    vTaskDelay(pdMS_TO_TICKS(20));
    
    // 2. 创建I2S接收通道 - 检查是否已存在
    if (*rx_handle_out == NULL) {
        ESP_LOGI(TAG_AUDIO, "创建I2S接收通道");
        i2s_chan_config_t i2s_rx_conf = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_ES7210_I2S_NUM, I2S_ROLE_MASTER);
        ret = i2s_new_channel(&i2s_rx_conf, NULL, rx_handle_out);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG_AUDIO, "创建I2S接收通道失败: %s", esp_err_to_name(ret));
            return ret;
        }
    } else {
        ESP_LOGI(TAG_AUDIO, "使用已存在的I2S通道");
    }
    
    // 3. 配置I2S TDM模式
    ESP_LOGI(TAG_AUDIO, "配置I2S TDM模式");
    i2s_tdm_config_t i2s_tdm_rx_conf = {  
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, BOARD_ES7210_I2S_SLOT_MASK),
        .clk_cfg  = {
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .sample_rate_hz = BOARD_AUDIO_SAMPLE_RATE,
            .mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE
        },
        .gpio_cfg = {
            .mclk = BOARD_ES7210_MCLK_IO,
            .bclk = BOARD_ES7210_BCK_IO,
            .ws   = BOARD_ES7210_WS_IO,
            .dout = BOARD_ES7210_DO_IO,
            .din  = BOARD_ES7210_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        },
    };
    
    ret = i2s_channel_init_tdm_mode(*rx_handle_out, &i2s_tdm_rx_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "初始化I2S TDM模式失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 4. 初始化ES7210编解码器
    ESP_LOGI(TAG_AUDIO, "初始化ES7210编解码器");
    
    // 等待I2C总线稳定
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // 创建ES7210设备句柄
    es7210_dev_handle_t es7210_handle = NULL;
    es7210_i2c_config_t es7210_i2c_conf = {
        .i2c_port = BOARD_I2C_NUM,
        .i2c_addr = BOARD_ES7210_I2C_ADDR
    };
    ret = es7210_new_codec(&es7210_i2c_conf, &es7210_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "创建ES7210句柄失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 配置ES7210参数
    es7210_codec_config_t codec_conf = {
        .i2s_format = BOARD_ES7210_AUDIO_FORMAT,
        .mclk_ratio = BOARD_AUDIO_MCLK_MULTIPLE,
        .sample_rate_hz = BOARD_AUDIO_SAMPLE_RATE,
        .bit_width = (es7210_i2s_bits_t)I2S_DATA_BIT_WIDTH_16BIT,
        .mic_bias = BOARD_ES7210_MIC_BIAS,
        .mic_gain = BOARD_ES7210_MIC_GAIN,
        .flags.tdm_enable = true
    };
    
    ret = es7210_config_codec(es7210_handle, &codec_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "配置ES7210编解码器失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = es7210_config_volume(es7210_handle, BOARD_ES7210_ADC_VOLUME);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "配置ES7210音量失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 额外稳定性等待
    vTaskDelay(pdMS_TO_TICKS(20));
    
    ESP_LOGI(TAG_AUDIO, "ES7210录音接口初始化成功");
    return ESP_OK;
}

/**
 * @brief 录制音频数据
 */
esp_err_t board_audio_record(i2s_chan_handle_t rx_handle, uint8_t *buffer, 
                            size_t buffer_size, size_t *bytes_read, uint32_t timeout_ms)
{
    if (!rx_handle || !buffer || !bytes_read) {
        ESP_LOGE(TAG_AUDIO, "无效参数");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret;
    size_t bytes_read_once = 0;
    *bytes_read = 0;  // 初始化已读取字节数
    
    // 启用I2S通道
    ESP_LOGI(TAG_AUDIO, "启动录音...");
    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "启用I2S通道失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 等待I2S通道稳定
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 开始录音
    ESP_LOGI(TAG_AUDIO, "开始录制音频，最大字节数: %u", (unsigned int)buffer_size);
    uint32_t start_time = esp_log_timestamp();
    
    // 读取数据
    ret = i2s_channel_read(rx_handle, buffer, buffer_size, &bytes_read_once, 
                          pdMS_TO_TICKS(timeout_ms));
    
    if (ret == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG_AUDIO, "读取超时");
        i2s_channel_disable(rx_handle);
        return ESP_ERR_TIMEOUT;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "读取错误: %s", esp_err_to_name(ret));
        i2s_channel_disable(rx_handle);
        return ret;
    }
    
    // 禁用I2S通道
    i2s_channel_disable(rx_handle);
    
    // 返回实际读取的字节数
    *bytes_read = bytes_read_once;
    
    ESP_LOGI(TAG_AUDIO, "录音完成，共录制 %u 字节的数据, 耗时 %u ms", 
            (unsigned int)bytes_read_once, 
            (unsigned int)(esp_log_timestamp() - start_time));
    return ESP_OK;
}

/**
 * @brief 通过ES8311播放音频数据
 */
esp_err_t board_audio_play(i2s_chan_handle_t tx_handle, const uint8_t *buffer, size_t buffer_size)
{
    if (!tx_handle || !buffer || buffer_size == 0) {
        ESP_LOGE(TAG_AUDIO, "无效参数");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret;
    size_t bytes_written = 0;
    
    // 重置I2S通道状态
    // i2s_channel_disable(tx_handle); // 移除此行，通道在此函数前不应被启用
    // vTaskDelay(pdMS_TO_TICKS(10)); // 移除此行
    
    // 预加载部分数据
    size_t preload_size = buffer_size > 1024 ? 1024 : buffer_size;
    ret = i2s_channel_preload_data(tx_handle, buffer, preload_size, &bytes_written);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "预加载数据失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG_AUDIO, "预加载了 %u 字节的音频数据", (unsigned int)bytes_written);
    
    // 打开功放
    board_pa_power(true);
    
    // 启用发送通道
    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_AUDIO, "启用I2S通道失败: %s", esp_err_to_name(ret));
        board_pa_power(false);
        return ret;
    }
    
    // 播放剩余数据
    size_t remaining = buffer_size - bytes_written;
    size_t offset = bytes_written;
    
    ESP_LOGI(TAG_AUDIO, "开始播放音频...");
    uint32_t start_time = esp_log_timestamp();
    
    while (remaining > 0) {
        ret = i2s_channel_write(tx_handle, buffer + offset, remaining, &bytes_written, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG_AUDIO, "写入I2S通道失败: %s", esp_err_to_name(ret));
            break;
        }
        
        if (bytes_written > 0) {
            remaining -= bytes_written;
            offset += bytes_written;
            
            // 每秒显示进度
            if (esp_log_timestamp() - start_time >= 1000) {
                start_time = esp_log_timestamp();
                ESP_LOGI(TAG_AUDIO, "播放进度: %.1f%%", (float)(buffer_size - remaining) * 100 / buffer_size);
            }
        }
    }
    
    // 等待所有数据播放完毕
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // 禁用I2S通道并关闭功放
    i2s_channel_disable(tx_handle);
    board_pa_power(false);
    
    ESP_LOGI(TAG_AUDIO, "音频播放完成");
    return ESP_OK;
}

/**
 * @brief 卸载音频 I2S 通道
 */
void board_audio_i2s_deinit(i2s_chan_handle_t handle)
{
    if (handle == NULL) {
        ESP_LOGW(TAG_AUDIO, "I2S 通道句柄为空，无需释放");
        return;
    }
    
    // 确保通道停止
    ESP_LOGI(TAG_AUDIO, "关闭 I2S 通道...");
    i2s_channel_disable(handle);
    
    // 删除通道
    ESP_LOGI(TAG_AUDIO, "删除 I2S 通道...");
    i2s_del_channel(handle);
    
    ESP_LOGI(TAG_AUDIO, "I2S 通道资源已释放");
}

/**************************** WiFi STA 模式相关函数 ****************************/

/**
 * @brief WiFi STA 模式事件处理回调函数
 */
static void wifi_sta_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            // WiFi启动，尝试连接AP
            ESP_LOGI(TAG_WIFI, "WiFi 启动，开始连接到 AP...");
            s_wifi_retry_num = 0;
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            // 已连接到AP，等待获取IP
            wifi_event_sta_connected_t* event = (wifi_event_sta_connected_t*) event_data;
            ESP_LOGI(TAG_WIFI, "已连接到 AP, SSID: %s, 信道: %d", 
                    (char*)event->ssid, event->channel);
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            // 连接断开，尝试重新连接
            wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
            ESP_LOGW(TAG_WIFI, "WiFi 连接断开，原因码: %d", event->reason);
            
            // 详细解释断开原因
            switch (event->reason) {
                case WIFI_REASON_AUTH_EXPIRE:
                    ESP_LOGW(TAG_WIFI, "认证过期，请检查密码");
                    break;
                case WIFI_REASON_AUTH_FAIL:
                    ESP_LOGW(TAG_WIFI, "认证失败，密码可能错误");
                    break;
                case WIFI_REASON_NO_AP_FOUND:
                    ESP_LOGW(TAG_WIFI, "未找到 AP，请检查 SSID 和路由器状态");
                    break;
                case WIFI_REASON_ASSOC_FAIL:
                    ESP_LOGW(TAG_WIFI, "关联失败，检查路由器是否允许新设备连接");
                    break;
                case WIFI_REASON_HANDSHAKE_TIMEOUT:
                    ESP_LOGW(TAG_WIFI, "握手超时，尝试重新连接");
                    break;
                default:
                    ESP_LOGW(TAG_WIFI, "其他断开原因，错误码: %d", event->reason);
                    break;
            }
            
            if (s_wifi_retry_num < BOARD_WIFI_MAX_RETRY) {
                // 增加指数回退，避免太快重试
                vTaskDelay(pdMS_TO_TICKS(500 * (1 << s_wifi_retry_num)));
                esp_wifi_connect();
                s_wifi_retry_num++;
                ESP_LOGI(TAG_WIFI, "WiFi 连接失败，正在重试... (%d/%d)", s_wifi_retry_num, BOARD_WIFI_MAX_RETRY);
            } else {
                // 超过最大重试次数，设置失败事件位
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                xEventGroupSetBits(board_event_group, WIFI_FAIL_BIT);
                ESP_LOGI(TAG_WIFI, "WiFi 连接失败，已达到最大重试次数");
            }
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            // 获取到IP地址，连接成功
            ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG_WIFI, "WiFi 连接成功! IP 地址: " IPSTR, IP2STR(&event->ip_info.ip));
            s_wifi_retry_num = 0;
            // 设置连接成功事件位
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            xEventGroupSetBits(board_event_group, WIFI_CONNECTED_BIT);
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            ESP_LOGW(TAG_WIFI, "IP 地址已丢失，等待重新获取...");
        }
    }
}

/**
 * @brief 初始化 WiFi (STA 模式)
 */
esp_err_t board_wifi_sta_init(void)
{
    ESP_LOGI(TAG_WIFI, "初始化 WiFi STA 模式...");
    
    esp_err_t ret = ESP_OK;
    char ssid[33] = {0};
    char password[65] = {0};
    
    // 创建WiFi事件组
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG_WIFI, "创建 WiFi 事件组失败");
            return ESP_FAIL;
        }
    }
    
    // 初始化底层TCP/IP适配层 (如果尚未初始化)
    ESP_LOGI(TAG_WIFI, "初始化 TCP/IP 适配层...");
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG_WIFI, "初始化 TCP/IP 适配层失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 创建默认事件循环 (如果尚未创建)
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG_WIFI, "创建事件循环失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 创建默认WiFi站点模式
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG_WIFI, "创建默认 WiFi STA 接口失败");
        return ESP_FAIL;
    }
    
    // 初始化WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "初始化 WiFi 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 注册WiFi事件处理函数
    esp_event_handler_instance_t wifi_handler_any_id;
    esp_event_handler_instance_t ip_handler_got_ip;
    
    ret = esp_event_handler_instance_register(WIFI_EVENT,
                                           ESP_EVENT_ANY_ID,
                                           &wifi_sta_event_handler,
                                           NULL,
                                           &wifi_handler_any_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "注册 WiFi 事件处理函数失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = esp_event_handler_instance_register(IP_EVENT,
                                           IP_EVENT_STA_GOT_IP,
                                           &wifi_sta_event_handler,
                                           NULL,
                                           &ip_handler_got_ip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "注册 IP 事件处理函数失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 从NVS中读取WiFi配置
    bool has_config = board_wifi_has_valid_config(ssid, password);
    if (has_config) {
        // 配置WiFi为站点模式
        wifi_config_t wifi_config = {
            .sta = {
                /* 认证模式 
                 * WIFI_AUTH_OPEN: 开放式网络
                 * WIFI_AUTH_WEP: WEP认证
                 * WIFI_AUTH_WPA_PSK: WPA个人认证
                 * WIFI_AUTH_WPA2_PSK: WPA2个人认证
                 * WIFI_AUTH_WPA_WPA2_PSK: WPA/WPA2混合个人认证
                 */
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg = {
                    .capable = true,
                    .required = false
                },
            },
        };
        // 复制配置的SSID和密码
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        
        // 设置WiFi工作模式为站点模式
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        
        // 设置WiFi配置
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        
        // 启动WiFi
        ESP_ERROR_CHECK(esp_wifi_start());
        
        ESP_LOGI(TAG_WIFI, "WiFi 初始化完成，正在连接到 AP: %s", ssid);
    } else {
        ESP_LOGI(TAG_WIFI, "未找到有效的 WiFi 配置，仅初始化 WiFi 栈");
        // 仅配置为站点模式，但不启动WiFi (将由SoftAP配网模式启动)
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }
    
    return ESP_OK;
}

/**
 * @brief 等待 WiFi STA 连接结果
 */
esp_err_t board_wifi_sta_wait_connected(uint32_t timeout_ms)
{
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG_WIFI, "WiFi 未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    
    // 设置等待的事件位
    const EventBits_t bits = WIFI_CONNECTED_BIT | WIFI_FAIL_BIT;
    
    ESP_LOGI(TAG_WIFI, "等待连接结果，超时: %lu ms", (unsigned long)timeout_ms);
    
    // 等待WiFi连接完成或失败
    TickType_t ticks_to_wait = timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t result = xEventGroupWaitBits(s_wifi_event_group, bits, pdFALSE, pdFALSE, ticks_to_wait);
    
    // 检查等待结果
    if (result & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG_WIFI, "WiFi 已成功连接到 AP");
        return ESP_OK;
    } else if (result & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG_WIFI, "WiFi 连接到 AP 失败");
        return ESP_FAIL;
    } else {
        ESP_LOGW(TAG_WIFI, "WiFi 连接超时");
        return ESP_ERR_TIMEOUT;
    }
}

/**
 * @brief 获取 WiFi STA 连接信息
 */
esp_err_t board_wifi_sta_get_info(char *ip_addr, char *ssid, int8_t *rssi)
{
    // 至少需要获取一项信息
    if (!ip_addr && !ssid && !rssi) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 检查WiFi连接状态
    wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "获取 AP 信息失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 获取SSID
    if (ssid) {
        memcpy(ssid, ap_info.ssid, sizeof(ap_info.ssid));
    }
    
    // 获取信号强度
    if (rssi) {
        *rssi = ap_info.rssi;
    }
    
    // 获取IP地址
    if (ip_addr) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            ret = esp_netif_get_ip_info(netif, &ip_info);
            if (ret == ESP_OK) {
                sprintf(ip_addr, IPSTR, IP2STR(&ip_info.ip));
            } else {
                ESP_LOGE(TAG_WIFI, "获取 IP 信息失败: %s", esp_err_to_name(ret));
                return ret;
            }
        } else {
            ESP_LOGE(TAG_WIFI, "获取网络接口失败");
            return ESP_FAIL;
        }
    }
    
    return ESP_OK;
}

/**
 * @brief 检查 NVS 中是否存有有效的 WiFi 配置
 */
bool board_wifi_has_valid_config(char *ssid, char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret;
    
    // 打开NVS命名空间
    ret = nvs_open(BOARD_WIFI_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG_WIFI, "NVS 命名空间未找到，没有存储的 WiFi 配置");
        return false;
    }
    
    // 检查是否存在SSID
    size_t ssid_len = 0;
    ret = nvs_get_str(nvs_handle, BOARD_WIFI_SSID_KEY, NULL, &ssid_len);
    if (ret != ESP_OK || ssid_len == 0 || ssid_len > 32) {
        ESP_LOGW(TAG_WIFI, "未找到有效的 SSID 配置");
        nvs_close(nvs_handle);
        return false;
    }
    
    // 检查是否存在密码
    size_t pass_len = 0;
    ret = nvs_get_str(nvs_handle, BOARD_WIFI_PASSWORD_KEY, NULL, &pass_len);
    if (ret != ESP_OK || pass_len > 64) {
        ESP_LOGW(TAG_WIFI, "未找到有效的密码配置");
        nvs_close(nvs_handle);
        return false;
    }
    
    // 如果需要返回SSID和密码
    if (ssid) {
        ret = nvs_get_str(nvs_handle, BOARD_WIFI_SSID_KEY, ssid, &ssid_len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG_WIFI, "获取 SSID 失败: %s", esp_err_to_name(ret));
            nvs_close(nvs_handle);
            return false;
        }
    }
    
    if (password) {
        ret = nvs_get_str(nvs_handle, BOARD_WIFI_PASSWORD_KEY, password, &pass_len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG_WIFI, "获取密码失败: %s", esp_err_to_name(ret));
            nvs_close(nvs_handle);
            return false;
        }
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG_WIFI, "找到有效的 WiFi 配置");
    return true;
}

/**
 * @brief 保存 WiFi 配置到 NVS
 */
esp_err_t board_wifi_save_config(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0 || strlen(ssid) > 32) {
        ESP_LOGE(TAG_WIFI, "无效的 SSID");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!password || strlen(password) > 64) {
        ESP_LOGE(TAG_WIFI, "无效的密码");
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t ret;
    
    // 打开NVS命名空间
    ret = nvs_open(BOARD_WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "打开 NVS 命名空间失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 保存SSID
    ret = nvs_set_str(nvs_handle, BOARD_WIFI_SSID_KEY, ssid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "保存 SSID 失败: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    // 保存密码
    ret = nvs_set_str(nvs_handle, BOARD_WIFI_PASSWORD_KEY, password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "保存密码失败: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    // 提交更改
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "提交 NVS 更改失败: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG_WIFI, "WiFi 配置已保存: SSID=%s", ssid);
    
    // 设置配置已保存事件位
    xEventGroupSetBits(board_event_group, WIFI_CONFIG_SAVED_BIT);
    
    return ESP_OK;
}

/**
 * @brief 获取设备 MAC 地址字符串
 */
esp_err_t board_get_mac_address_string(char *mac_str)
{
    if (mac_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "获取 MAC 地址失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 格式化MAC地址为字符串 (XX:XX:XX:XX:XX:XX)
    sprintf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X", 
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    return ESP_OK;
}

/**************************** WiFi SoftAP 配网模式相关函数 ****************************/

// DNS 服务器任务参数
typedef struct {
    uint16_t port;
    esp_netif_t *netif;
} dns_server_task_param_t;

// HTTP 服务器处理函数
static esp_err_t http_server_get_root_handler(httpd_req_t *req)
{
    extern const unsigned char index_html_start[] asm("_binary_index_html_start");
    extern const unsigned char index_html_end[] asm("_binary_index_html_end");
    const size_t index_html_size = (index_html_end - index_html_start);
    
    // 设置缓存控制头，防止缓存
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_size);
    
    return ESP_OK;
}

// 通用捕获门户处理函数，捕获所有HTTP请求并重定向到配网页面
static esp_err_t captive_portal_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_CONFIG, "捕获请求: %s", req->uri);
    
    // 检查是否有User-Agent头
    char ua_buf[128] = {0};
    size_t ua_len = httpd_req_get_hdr_value_len(req, "User-Agent");
    bool is_apple = false;
    bool is_xiaomi = false;
    
    if (ua_len > 0 && ua_len < sizeof(ua_buf)) {
        httpd_req_get_hdr_value_str(req, "User-Agent", ua_buf, sizeof(ua_buf));
        
        // 检测是否为苹果设备
        if (strstr(ua_buf, "iPhone") || strstr(ua_buf, "iPad") || strstr(ua_buf, "Mac")) {
            is_apple = true;
            ESP_LOGI(TAG_CONFIG, "检测到苹果设备: %s", ua_buf);
        }
        
        // 检测是否为小米设备
        if (strstr(ua_buf, "MiuiBrowser") || strstr(ua_buf, "XiaoMi") || strstr(ua_buf, "MI ")) {
            is_xiaomi = true;
            ESP_LOGI(TAG_CONFIG, "检测到小米设备: %s", ua_buf);
        }
    }
    
    // 获取Host头信息
    char host_buf[64] = {0};
    size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
    if (host_len > 0 && host_len < sizeof(host_buf)) {
        httpd_req_get_hdr_value_str(req, "Host", host_buf, sizeof(host_buf));
        ESP_LOGI(TAG_CONFIG, "Host: %s", host_buf);
        
        // 如果是captive.apple.com，确认是苹果设备
        if (strstr(host_buf, "captive.apple.com")) {
            is_apple = true;
        }
    }
    
    // 设置通用响应头
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    httpd_resp_set_hdr(req, "Connection", "close");
    
    // 为不同设备使用不同的重定向策略
    if (is_xiaomi) {
        // 小米设备使用302重定向
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
    } 
    else if (is_apple) {
        // 苹果设备使用HTML刷新
        const char *resp_str = 
            "<!DOCTYPE html><html><head>"
            "<meta http-equiv='refresh' content='0;url=http://192.168.4.1/'>"
            "</head><body>"
            "<h2>正在跳转到配网页面...</h2>"
            "<p><a href='http://192.168.4.1/'>点击这里</a></p>"
            "</body></html>";
        
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req, resp_str);
    }
    else {
        // 其他设备使用HTML+JavaScript双重方法
        const char *resp_str = 
            "<!DOCTYPE html><html><head>"
            "<meta http-equiv='refresh' content='0;url=http://192.168.4.1/'>"
            "<script>window.location.href='http://192.168.4.1/';</script>"
            "</head><body>"
            "<h2>正在跳转到配网页面...</h2>"
            "<p>如果没有自动跳转，请<a href='http://192.168.4.1/'>点击这里</a></p>"
            "</body></html>";
        
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req, resp_str);
    }
    
    return ESP_OK;
}

// 获取设备信息 API 处理函数
static esp_err_t http_server_get_device_info_handler(httpd_req_t *req)
{
    char resp[256];
    char mac_str[18];
    
    // 获取设备MAC地址
    board_get_mac_address_string(mac_str);
    
    // 创建JSON响应
    snprintf(resp, sizeof(resp), 
             "{\"status\":\"ok\",\"device_name\":\"ESP32-S3\",\"mac\":\"%s\",\"ip\":\"192.168.4.1\"}", 
             mac_str);
    
    // 设置响应类型
    httpd_resp_set_type(req, "application/json");
    // 设置 CORS 头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    // 发送响应
    httpd_resp_send(req, resp, strlen(resp));
    
    return ESP_OK;
}

// WiFi 配置 API 处理函数
static esp_err_t http_server_set_wifi_config_handler(httpd_req_t *req)
{
    char buf[128];
    char ssid[33] = {0};
    char password[65] = {0};
    
    // 获取POST数据
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    // 解析SSID和密码
    char *ssid_start = strstr(buf, "ssid=");
    if (ssid_start) {
        ssid_start += 5; // 跳过"ssid="
        char *ssid_end = strchr(ssid_start, '&');
        if (ssid_end) {
            // 找到结束位置 (& 或 NULL)
            strncpy(ssid, ssid_start, ssid_end - ssid_start);
            ssid[ssid_end - ssid_start] = '\0';
        } else {
            // 没有其他参数，直接到结尾
            strncpy(ssid, ssid_start, sizeof(ssid) - 1);
        }
    }
    
    char *password_start = strstr(buf, "password=");
    if (password_start) {
        password_start += 9; // 跳过"password="
        strncpy(password, password_start, sizeof(password) - 1);
    }
    
    // 处理URL编码 (简单处理)
    for (int i = 0; i < strlen(ssid); i++) {
        if (ssid[i] == '+') ssid[i] = ' ';
    }
    for (int i = 0; i < strlen(password); i++) {
        if (password[i] == '+') password[i] = ' ';
    }
    
    // 输出收到的配置信息 (注意: 生产环境中不要泄露密码)
    ESP_LOGI(TAG_CONFIG, "收到 WiFi 配置, SSID: %s, 密码长度: %d", ssid, strlen(password));
    
    // 保存WiFi配置到NVS
    esp_err_t save_ret = board_wifi_save_config(ssid, password);
    
    // 设置响应类型
    httpd_resp_set_type(req, "application/json");
    // 设置 CORS 头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    // 发送操作结果响应
    if (save_ret == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"配置已保存，设备将重启并尝试连接\"}");
    } else {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), 
                 "{\"status\":\"error\",\"message\":\"保存配置失败: %s\"}", 
                 esp_err_to_name(save_ret));
        httpd_resp_sendstr(req, err_msg);
    }
    
    return ESP_OK;
}

// 启动HTTP服务器
static esp_err_t start_http_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.server_port = BOARD_HTTP_SERVER_PORT;
    
    // 使用通配符匹配
    config.uri_match_fn = httpd_uri_match_wildcard;
    
    // URI 处理函数
    httpd_uri_t uri_get_root = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = http_server_get_root_handler,
        .user_ctx  = NULL
    };
    
    httpd_uri_t uri_get_device_info = {
        .uri       = "/api/device-info",
        .method    = HTTP_GET,
        .handler   = http_server_get_device_info_handler,
        .user_ctx  = NULL
    };
    
    httpd_uri_t uri_post_wifi_config = {
        .uri       = "/api/set-wifi",
        .method    = HTTP_POST,
        .handler   = http_server_set_wifi_config_handler,
        .user_ctx  = NULL
    };
    
    // 捕获门户通配符处理函数
    httpd_uri_t uri_captive_portal = {
        .uri       = "/*",
        .method    = HTTP_GET,
        .handler   = captive_portal_handler,
        .user_ctx  = NULL
    };
    
    // 启动HTTP服务器
    ESP_LOGI(TAG_CONFIG, "启动 HTTP 服务器，端口: %d", config.server_port);
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG_CONFIG, "启动 HTTP 服务器失败");
        return ESP_FAIL;
    }
    
    // 注册URI处理函数
    httpd_register_uri_handler(server, &uri_get_root);
    httpd_register_uri_handler(server, &uri_get_device_info);
    httpd_register_uri_handler(server, &uri_post_wifi_config);
    
    // 注册捕获门户处理函数（最后注册，因为它会捕获所有未匹配的请求）
    httpd_register_uri_handler(server, &uri_captive_portal);
    
    // 保存服务器句柄，以便后续操作
    s_config_server_handle = server;
    
    return ESP_OK;
}

// 停止HTTP服务器
static esp_err_t stop_http_server(void)
{
    if (s_config_server_handle == NULL) {
        ESP_LOGW(TAG_CONFIG, "HTTP 服务器未运行");
        return ESP_OK;
    }
    
    esp_err_t ret = httpd_stop(s_config_server_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CONFIG, "停止 HTTP 服务器失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    s_config_server_handle = NULL;
    ESP_LOGI(TAG_CONFIG, "HTTP 服务器已停止");
    return ESP_OK;
}

// DNS 服务器任务
static void dns_server_task(void *pvParameters)
{
    dns_server_task_param_t *params = (dns_server_task_param_t *)pvParameters;
    uint16_t dns_port = params->port;
    esp_netif_t *netif = params->netif;
    free(params); // 释放参数结构体
    
    int sock;
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    uint8_t rx_buffer[128] = {0};
    
    // 创建UDP套接字
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG_CONFIG, "DNS服务器创建套接字失败");
        vTaskDelete(NULL);
        return;
    }
    
    // 绑定到全部地址，DNS端口
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(dns_port);
    
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG_CONFIG, "DNS服务器绑定套接字失败");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    
    // 获取AP的IP地址
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);
    
    ESP_LOGI(TAG_CONFIG, "DNS服务器已启动，端口: %d, AP IP: " IPSTR, 
             dns_port, IP2STR(&ip_info.ip));
    
    // 固定的DNS响应模板
    uint8_t dns_reply[128] = {
        0x00, 0x00, // ID (来自请求)
        0x81, 0x80, // 标志 (响应，递归可用)
        0x00, 0x01, // 问题数: 1
        0x00, 0x01, // 回答数: 1
        0x00, 0x00, // 认证服务器数: 0
        0x00, 0x00, // 额外记录数: 0
        
        // [问题部分需复制自请求]
        
        // 回答部分会在后面补充
    };
    
    while (1) {
        // 清空接收缓冲区
        memset(rx_buffer, 0, sizeof(rx_buffer));
        
        // 接收DNS查询
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, 
                         (struct sockaddr *)&client_addr, &addr_len);
        
        if (len < 12) { // 不是有效的DNS请求
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        
        // 准备DNS响应
        // 1. 复制请求ID (前两个字节)
        dns_reply[0] = rx_buffer[0];
        dns_reply[1] = rx_buffer[1];
        
        // 2. 查找问题部分到尾部
        int question_end = 12;
        while (question_end < len && rx_buffer[question_end] != 0) {
            // 跳过标签
            question_end += rx_buffer[question_end] + 1;
        }
        // 再跳过标签结束的0字节和类型与类
        question_end += 5;

        // 提取域名用于调试
        char domain[64] = {0};
        int domain_pos = 12; // DNS查询中域名开始位置
        int i = 0;
        
        while (domain_pos < len && rx_buffer[domain_pos] != 0 && i < sizeof(domain)-1) {
            int label_len = rx_buffer[domain_pos++];
            
            if (i > 0 && i < sizeof(domain)-1) {
                domain[i++] = '.';
            }
            
            for (int j = 0; j < label_len && i < sizeof(domain)-1; j++) {
                domain[i++] = rx_buffer[domain_pos++];
            }
        }
        domain[i] = '\0';
        
        ESP_LOGD(TAG_CONFIG, "DNS查询: %s, 返回IP: " IPSTR, domain, IP2STR(&ip_info.ip));
        
        // 确保有足够空间复制问题部分
        uint8_t response[128];
        if (question_end <= len && question_end <= 64) {
            // 复制固定头部
            memcpy(response, dns_reply, 12);
            
            // 复制问题部分
            memcpy(response + 12, rx_buffer + 12, question_end - 12);
            
            // 添加回答部分
            int answer_offset = question_end;
            
            // 指向查询中的域名
            response[answer_offset++] = 0xC0;
            response[answer_offset++] = 0x0C;
            
            // 设置类型 A
            response[answer_offset++] = 0x00;
            response[answer_offset++] = 0x01;
            
            // 设置类 IN
            response[answer_offset++] = 0x00;
            response[answer_offset++] = 0x01;
            
            // 设置TTL (短TTL，防止太长缓存)
            response[answer_offset++] = 0x00;
            response[answer_offset++] = 0x00;
            response[answer_offset++] = 0x00;
            response[answer_offset++] = 0x0A; // 10秒
            
            // 设置数据长度 (IPv4地址为4字节)
            response[answer_offset++] = 0x00;
            response[answer_offset++] = 0x04;
            
            // 设置IPv4地址值为AP IP
            memcpy(&response[answer_offset], &ip_info.ip.addr, 4);
            answer_offset += 4;
            
            // 发送DNS响应
            sendto(sock, response, answer_offset, 0,
                  (struct sockaddr *)&client_addr, addr_len);
        }
        
        // 短暂延时
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    // 不会执行到这里
    close(sock);
    vTaskDelete(NULL);
}

// 启动WiFi SoftAP配网模式
esp_err_t board_wifi_softap_start(void)
{
    ESP_LOGI(TAG_CONFIG, "启动 WiFi SoftAP 配网模式...");
    
    // 初始化网络接口 (如果尚未初始化)
    if (esp_netif_init() != ESP_OK) {
        ESP_LOGW(TAG_CONFIG, "网络接口已初始化");
    }
    
    // 创建事件循环 (如果尚未创建)
    if (esp_event_loop_create_default() != ESP_OK) {
        ESP_LOGW(TAG_CONFIG, "事件循环已创建");
    }
    
    // 创建默认AP网络接口
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG_CONFIG, "创建AP网络接口失败");
            return ESP_FAIL;
        }
    }
    
    // 获取设备MAC地址 (用于AP SSID)
    uint8_t mac[6];
    char ssid[33] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    
    // 生成AP SSID (使用前缀+MAC地址后三字节)
    snprintf(ssid, sizeof(ssid), "%s%02X%02X%02X", 
             BOARD_SOFTAP_SSID_PREFIX, mac[3], mac[4], mac[5]);
    
    // 配置AP参数
    wifi_config_t wifi_config = {
        .ap = {
            .max_connection = BOARD_SOFTAP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN, // 开放式网络
            .channel = BOARD_SOFTAP_CHANNEL,
            .ssid_hidden = 0,             // SSID不隐藏
            .beacon_interval = 100,       // 默认beacon间隔
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    // 复制SSID和密码
    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    if (strlen(BOARD_SOFTAP_PASSWORD) > 0) {
        strncpy((char *)wifi_config.ap.password, BOARD_SOFTAP_PASSWORD, sizeof(wifi_config.ap.password) - 1);
        // 设置认证模式
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    
    // 初始化WiFi (如果尚未初始化)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG_CONFIG, "初始化 WiFi 栈失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 设置AP模式
    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CONFIG, "设置 WiFi 模式为 AP 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 设置AP配置
    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CONFIG, "设置 AP 配置失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 启动WiFi
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CONFIG, "启动 WiFi AP 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG_CONFIG, "WiFi AP 已启动, SSID: %s, 信道: %d", 
             ssid, BOARD_SOFTAP_CHANNEL);
    
    // 启动HTTP服务器
    ret = start_http_server();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CONFIG, "启动 HTTP 服务器失败: %s", esp_err_to_name(ret));
        // 继续运行，因为这不是致命错误
    }
    
    // 启动DNS服务器任务
    dns_server_task_param_t *dns_params = malloc(sizeof(dns_server_task_param_t));
    if (dns_params) {
        dns_params->port = BOARD_DNS_SERVER_PORT;
        dns_params->netif = s_ap_netif;
        
        // 创建DNS服务器任务
        xTaskCreate(dns_server_task, "dns_server", 3072, dns_params, 5, NULL);
    } else {
        ESP_LOGE(TAG_CONFIG, "创建 DNS 服务器任务失败");
    }
    
    return ESP_OK;
}

// 停止WiFi SoftAP配网模式
esp_err_t board_wifi_softap_stop(void)
{
    ESP_LOGI(TAG_CONFIG, "停止 WiFi SoftAP 配网模式...");
    
    // 停止HTTP服务器
    esp_err_t ret = stop_http_server();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CONFIG, "停止 HTTP 服务器失败: %s", esp_err_to_name(ret));
        // 继续运行，因为DNS服务器任务会自动删除
    }
    
    // 停止WiFi (DNS服务器任务会因为套接字错误而退出)
    ret = esp_wifi_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CONFIG, "停止 WiFi 失败: %s", esp_err_to_name(ret));
    }
    
    return ESP_OK;
}

/**************************** WebSocket 客户端相关函数 ****************************/

/**
 * @brief 初始化 WebSocket 客户端
 */
esp_err_t board_websocket_init(esp_websocket_client_handle_t *client_handle_out, 
                              esp_event_handler_t event_handler, void *handler_args)
{
    if (client_handle_out == NULL || event_handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 解析WebSocket URL
    char *url = strdup(BOARD_WS_SERVER_URL);
    if (url == NULL) {
        ESP_LOGE(TAG, "内存分配失败");
        return ESP_ERR_NO_MEM;
    }
    
    // 修改WebSocket URL，确保包含客户端ID
    char *full_url = NULL;
    // 检查URL是否以"/"结尾
    if (url[strlen(url) - 1] == '/') {
        // URL已经以"/"结尾，直接添加客户端ID
        full_url = malloc(strlen(url) + strlen(BOARD_WS_DEVICE_CLIENT_ID) + 1);
        if (full_url == NULL) {
            ESP_LOGE(TAG, "内存分配失败");
            free(url);
            return ESP_ERR_NO_MEM;
        }
        sprintf(full_url, "%s%s", url, BOARD_WS_DEVICE_CLIENT_ID);
    } else {
        // URL不以"/"结尾，添加"/"和客户端ID
        full_url = malloc(strlen(url) + strlen(BOARD_WS_DEVICE_CLIENT_ID) + 2);
        if (full_url == NULL) {
            ESP_LOGE(TAG, "内存分配失败");
            free(url);
            return ESP_ERR_NO_MEM;
        }
        sprintf(full_url, "%s/%s", url, BOARD_WS_DEVICE_CLIENT_ID);
    }
    free(url);
    
    // 配置WebSocket客户端
    esp_websocket_client_config_t ws_config = {
        .uri = full_url,
        .disable_auto_reconnect = false,
        .reconnect_timeout_ms = BOARD_WS_RECONNECT_INTERVAL_MS,
        .network_timeout_ms = BOARD_WS_NETWORK_TIMEOUT_MS,
        .pingpong_timeout_sec = BOARD_WS_PING_INTERVAL_SEC,
        .transport = WEBSOCKET_TRANSPORT_OVER_TCP,
    };
    
    // 创建WebSocket客户端
    *client_handle_out = esp_websocket_client_init(&ws_config);
    if (*client_handle_out == NULL) {
        ESP_LOGE(TAG, "初始化WebSocket客户端失败");
        free(full_url);
        return ESP_FAIL;
    }
    
    // 注册事件处理函数
    esp_websocket_register_events(*client_handle_out, WEBSOCKET_EVENT_ANY, event_handler, handler_args);
    
    ESP_LOGI(TAG, "WebSocket客户端初始化成功，服务器URL: %s", full_url);
    free(full_url);
    
    return ESP_OK;
}

/**
 * @brief 启动 WebSocket 客户端连接
 */
esp_err_t board_websocket_start(esp_websocket_client_handle_t client)
{
    if (client == NULL) {
        ESP_LOGE(TAG, "无效的 WebSocket 客户端句柄");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "启动 WebSocket 客户端连接...");
    esp_err_t ret = esp_websocket_client_start(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动 WebSocket 客户端失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "WebSocket 客户端启动成功");
    return ESP_OK;
}

/**
 * @brief 停止 WebSocket 客户端连接
 */
esp_err_t board_websocket_stop(esp_websocket_client_handle_t client)
{
    if (client == NULL) {
        ESP_LOGE(TAG, "无效的 WebSocket 客户端句柄");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "停止 WebSocket 客户端连接...");
    esp_err_t ret = esp_websocket_client_stop(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "停止 WebSocket 客户端失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "WebSocket 客户端已停止");
    return ESP_OK;
}

/**
 * @brief 销毁 WebSocket 客户端
 */
esp_err_t board_websocket_destroy(esp_websocket_client_handle_t client)
{
    if (client == NULL) {
        ESP_LOGE(TAG, "无效的 WebSocket 客户端句柄");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "销毁 WebSocket 客户端...");
    esp_websocket_client_destroy(client);
    
    // 清除全局WebSocket客户端句柄
    if (s_websocket_client == client) {
        s_websocket_client = NULL;
    }
    
    ESP_LOGI(TAG, "WebSocket 客户端已销毁");
    return ESP_OK;
}

/**************************** 恢复出厂设置相关函数 ****************************/

// 恢复出厂设置按键定时器处理
static void factory_reset_btn_timer_cb(TimerHandle_t xTimer)
{
    // 定时器回调函数实现
    ESP_LOGI(TAG, "恢复出厂设置定时器触发");
    
    // 创建一个任务来执行恢复出厂设置操作
    xTaskCreate(board_factory_reset_task, 
                "factory_reset", 
                4096,  // 增加栈大小
                NULL, 
                5, 
                NULL);
}

// 恢复出厂设置任务
void board_factory_reset_task(void *arg)
{
    ESP_LOGW(TAG, "执行恢复出厂设置操作...");
    
    // 关闭WiFi
    esp_wifi_stop();
    
    // 打开NVS存储
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(BOARD_WIFI_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret == ESP_OK) {
        // 清除WiFi配置
        nvs_erase_key(nvs_handle, BOARD_WIFI_SSID_KEY);
        nvs_erase_key(nvs_handle, BOARD_WIFI_PASSWORD_KEY);
        
        // 提交更改
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        
        ESP_LOGI(TAG, "WiFi 配置已清除");
    } else {
        ESP_LOGE(TAG, "打开 NVS 存储失败: %s", esp_err_to_name(ret));
    }
    
    // 清除所有事件位
    xEventGroupClearBits(board_event_group, 0xFF);
    
    // 延迟1秒后重启
    ESP_LOGW(TAG, "设备将在1秒后重启...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // 重启设备
    esp_restart();
}

// 按钮事件处理任务
static void factory_reset_btn_task(void *arg)
{
    factory_reset_btn_event_t event;
    static TimerHandle_t btn_timer = NULL;
    
    while (1) {
        if (xQueueReceive(factory_reset_btn_queue, &event, portMAX_DELAY) == pdTRUE) {
            if (event == FACTORY_RESET_BTN_PRESS) {
                // 按钮按下，启动定时器
                if (btn_timer == NULL) {
                    btn_timer = xTimerCreate("factory_reset_btn", 
                                          pdMS_TO_TICKS(BOARD_FACTORY_RESET_LONG_PRESS_TIME_MS),
                                          pdFALSE, // 不自动重载
                                          NULL, 
                                          factory_reset_btn_timer_cb);
                }
                if (btn_timer != NULL && xTimerIsTimerActive(btn_timer) == pdFALSE) {
                    xTimerStart(btn_timer, 0);
                    ESP_LOGI(TAG, "恢复出厂设置按钮按下，启动计时器");
                }
            } else {
                // 按钮释放，停止定时器
                if (btn_timer != NULL && xTimerIsTimerActive(btn_timer) == pdTRUE) {
                    xTimerStop(btn_timer, 0);
                    ESP_LOGI(TAG, "恢复出厂设置按钮释放，停止计时器");
                }
            }
        }
    }
}

// 恢复出厂设置按键中断处理
static void IRAM_ATTR factory_reset_btn_intr_handler(void* arg)
{
    int gpio_num = (int)arg;
    factory_reset_btn_event_t event;
    
    // 读取当前电平状态
    int level = gpio_get_level(gpio_num);
    
    // 根据电平状态确定事件类型
    event = (level == 0) ? FACTORY_RESET_BTN_PRESS : FACTORY_RESET_BTN_RELEASE;
    
    // 发送事件到队列
    xQueueSendFromISR(factory_reset_btn_queue, &event, NULL);
}

/**
 * @brief 初始化恢复出厂设置按键及中断
 */
esp_err_t board_factory_reset_init(void)
{
    ESP_LOGI(TAG, "初始化恢复出厂设置按键 (GPIO %d)", BOARD_FACTORY_RESET_GPIO);
    
    // 创建按钮事件队列
    factory_reset_btn_queue = xQueueCreate(10, sizeof(factory_reset_btn_event_t));
    if (factory_reset_btn_queue == NULL) {
        ESP_LOGE(TAG, "创建按钮事件队列失败");
        return ESP_FAIL;
    }
    
    // 创建按钮事件处理任务
    BaseType_t ret = xTaskCreate(factory_reset_btn_task, 
                                "factory_reset_btn", 
                                4096,  // 增加栈大小
                                NULL, 
                                5, 
                                NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建按钮事件处理任务失败");
        vQueueDelete(factory_reset_btn_queue);
        return ESP_FAIL;
    }
    
    // 配置GPIO为输入模式，启用上拉电阻
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOARD_FACTORY_RESET_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE, // 任意边沿触发中断
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配置恢复出厂设置按钮 GPIO 失败: %s", esp_err_to_name(err));
        vQueueDelete(factory_reset_btn_queue);
        return err;
    }
    
    // 安装GPIO中断服务
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "安装 GPIO 中断服务失败: %s", esp_err_to_name(err));
        vQueueDelete(factory_reset_btn_queue);
        return err;
    }
    
    // 添加GPIO中断处理函数
    err = gpio_isr_handler_add(BOARD_FACTORY_RESET_GPIO, factory_reset_btn_intr_handler, 
                          (void*)BOARD_FACTORY_RESET_GPIO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "添加 GPIO 中断处理函数失败: %s", esp_err_to_name(err));
        vQueueDelete(factory_reset_btn_queue);
        return err;
    }
    
    ESP_LOGI(TAG, "恢复出厂设置按钮初始化成功");
    return ESP_OK;
} 