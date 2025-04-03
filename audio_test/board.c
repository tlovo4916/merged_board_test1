/**
 * board.c - ESP32-S3开发板音频系统实现
 * 实现ES8311(播放)和ES7210(录音)相关的函数
 */

#include "board.h"

static const char *TAG = "BOARD";

/**
 * @brief 初始化I2C接口
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
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_I2C_SDA_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = BOARD_I2C_SCL_IO,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BOARD_I2C_FREQ_HZ
    };
    
    esp_err_t ret = i2c_param_config(BOARD_I2C_NUM, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C参数配置失败");
        return ret;
    }
    
    // 尝试安装I2C驱动
    ret = i2c_driver_install(BOARD_I2C_NUM, conf.mode, 0, 0, 0);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "I2C驱动已安装，继续执行");
        ret = ESP_OK;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C驱动安装失败");
        return ret;
    }
    
    is_initialized = true;
    ESP_LOGI(TAG, "I2C接口初始化成功");
    return ESP_OK;
}

/**
 * @brief 控制功放开关
 */
void board_pa_power(bool enable)
{
    // 配置功放控制引脚
    static bool initialized = false;
    if (!initialized) {
        esp_rom_gpio_pad_select_gpio(BOARD_PA_EN_GPIO);
        gpio_set_direction(BOARD_PA_EN_GPIO, GPIO_MODE_OUTPUT);
        initialized = true;
    }
    
    // 设置功放使能状态
    gpio_set_level(BOARD_PA_EN_GPIO, enable);
    ESP_LOGI(TAG, "功放电源: %s", enable ? "开启" : "关闭");
}

/**
 * @brief 初始化ES8311播放接口
 */
esp_err_t board_es8311_init(i2s_chan_handle_t *tx_handle)
{
    esp_err_t ret;
    
    // 1. 初始化I2C接口
    ret = board_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化I2C失败");
        return ret;
    }
    
    // 2. 配置I2S通道 - 如果已经存在通道句柄，则不再重新创建
    if (*tx_handle == NULL) {
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_ES8311_I2S_NUM, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true; // 自动清除DMA缓冲区中的旧数据
        ESP_LOGI(TAG, "创建I2S发送通道");
        ret = i2s_new_channel(&chan_cfg, tx_handle, NULL);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "创建I2S通道失败");
            return ret;
        }
    } else {
        ESP_LOGI(TAG, "使用已存在的I2S通道");
    }
    
    // 3. 配置I2S标准模式
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_SAMPLE_RATE),
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
    std_cfg.clk_cfg.mclk_multiple = BOARD_MCLK_MULTIPLE;
    
    ESP_LOGI(TAG, "初始化I2S标准模式");
    ret = i2s_channel_init_std_mode(*tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化I2S标准模式失败");
        return ret;
    }
    
    // 4. 初始化ES8311编解码器
    ESP_LOGI(TAG, "初始化ES8311编解码器");
    
    // 等待I2C总线稳定
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // 创建ES8311句柄
    es8311_handle_t es_handle = es8311_create(BOARD_I2C_NUM, BOARD_ES8311_ADDR);
    if (!es_handle) {
        ESP_LOGE(TAG, "创建ES8311句柄失败");
        return ESP_FAIL;
    }
    
    // 配置ES8311时钟
    const es8311_clock_config_t es_clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = BOARD_MCLK_FREQ_HZ,
        .sample_frequency = BOARD_SAMPLE_RATE
    };
    
    // 初始化ES8311
    ret = es8311_init(es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化ES8311失败");
        return ret;
    }
    
    // 配置采样率
    ret = es8311_sample_frequency_config(es_handle, BOARD_MCLK_FREQ_HZ, BOARD_SAMPLE_RATE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "配置ES8311采样率失败");
        return ret;
    }
    
    // 设置音量
    ret = es8311_voice_volume_set(es_handle, BOARD_ES8311_VOLUME, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "设置ES8311音量失败");
        return ret;
    }
    
    // 禁用麦克风输入(我们使用ES7210)
    ret = es8311_microphone_config(es_handle, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "配置ES8311麦克风失败");
        return ret;
    }
    
    ESP_LOGI(TAG, "ES8311播放接口初始化成功");
    return ESP_OK;
}

/**
 * @brief 初始化ES7210录音接口
 */
esp_err_t board_es7210_init(i2s_chan_handle_t *rx_handle)
{
    esp_err_t ret;
    
    // 1. 初始化I2C接口
    ret = board_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化I2C失败");
        return ret;
    }
    
    // 2. 创建I2S接收通道 - 检查是否已存在
    if (*rx_handle == NULL) {
        ESP_LOGI(TAG, "创建I2S接收通道");
        i2s_chan_config_t i2s_rx_conf = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_ES7210_I2S_NUM, I2S_ROLE_MASTER);
        ret = i2s_new_channel(&i2s_rx_conf, NULL, rx_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "创建I2S接收通道失败");
            return ret;
        }
    } else {
        ESP_LOGI(TAG, "使用已存在的I2S通道");
    }
    
    // 3. 配置I2S TDM模式
    ESP_LOGI(TAG, "配置I2S TDM模式");
    i2s_tdm_config_t i2s_tdm_rx_conf = {  
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, BOARD_ES7210_TDM_SLOT_MASK),
        .clk_cfg  = {
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .sample_rate_hz = BOARD_SAMPLE_RATE,
            .mclk_multiple = BOARD_MCLK_MULTIPLE
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
    
    ret = i2s_channel_init_tdm_mode(*rx_handle, &i2s_tdm_rx_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化I2S TDM模式失败");
        return ret;
    }
    
    // 4. 初始化ES7210编解码器
    ESP_LOGI(TAG, "初始化ES7210编解码器");
    
    // 创建ES7210设备句柄
    es7210_dev_handle_t es7210_handle = NULL;
    es7210_i2c_config_t es7210_i2c_conf = {
        .i2c_port = BOARD_I2C_NUM,
        .i2c_addr = BOARD_ES7210_ADDR
    };
    ret = es7210_new_codec(&es7210_i2c_conf, &es7210_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "创建ES7210句柄失败");
        return ret;
    }

    // 配置ES7210参数
    es7210_codec_config_t codec_conf = {
        .i2s_format = BOARD_ES7210_TDM_FORMAT,
        .mclk_ratio = BOARD_MCLK_MULTIPLE,
        .sample_rate_hz = BOARD_SAMPLE_RATE,
        .bit_width = (es7210_i2s_bits_t)I2S_DATA_BIT_WIDTH_16BIT,
        .mic_bias = BOARD_ES7210_MIC_BIAS,
        .mic_gain = BOARD_ES7210_MIC_GAIN,
        .flags.tdm_enable = true
    };
    
    ret = es7210_config_codec(es7210_handle, &codec_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "配置ES7210编解码器失败");
        return ret;
    }
    
    ret = es7210_config_volume(es7210_handle, BOARD_ES7210_ADC_VOLUME);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "配置ES7210音量失败");
        return ret;
    }
    
    ESP_LOGI(TAG, "ES7210录音接口初始化成功");
    return ESP_OK;
}

/**
 * @brief 从ES7210录制音频数据
 */
esp_err_t board_record_audio(i2s_chan_handle_t rx_handle, uint8_t *buffer, 
                             size_t buffer_size, int seconds, size_t *bytes_read)
{
    if (!rx_handle || !buffer || !bytes_read) {
        ESP_LOGE(TAG, "无效参数");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret;
    int retry_count;
    size_t bytes_read_once;
    size_t total_bytes = 0;
    
    // 启用I2S通道
    ESP_LOGI(TAG, "启动录音...");
    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启用I2S通道失败");
        return ret;
    }
    
    // 等待I2S通道稳定
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 计算总采样大小(字节)
    size_t bytes_per_second = BOARD_SAMPLE_RATE * 2 * 2; // 采样率 * 16位(2字节) * 双通道
    size_t total_bytes_to_read = bytes_per_second * seconds;
    
    // 确保缓冲区足够大
    if (buffer_size < total_bytes_to_read) {
        ESP_LOGW(TAG, "缓冲区大小不足，限制录音时长");
        total_bytes_to_read = buffer_size;
    }
    
    // 开始录音
    ESP_LOGI(TAG, "开始录制 %d 秒的音频...", seconds);
    uint32_t start_time = esp_log_timestamp();
    
    // 循环读取直到达到指定大小或超时
    while (total_bytes < total_bytes_to_read) {
        // 计算本次读取大小
        size_t bytes_to_read = (total_bytes_to_read - total_bytes > 1024) ? 1024 : (total_bytes_to_read - total_bytes);
        
        // 读取数据，增加重试机制
        retry_count = 0;
        do {
            ret = i2s_channel_read(rx_handle, buffer + total_bytes, bytes_to_read, 
                                    &bytes_read_once, pdMS_TO_TICKS(200));
            if (ret == ESP_ERR_TIMEOUT) {
                retry_count++;
                ESP_LOGW(TAG, "读取超时，重试次数: %d", retry_count);
                if (retry_count >= 3) {
                    ESP_LOGW(TAG, "读取超时，跳过本次采样");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50)); // 增加重试间隔
            } else if (ret != ESP_OK) {
                ESP_LOGE(TAG, "读取错误: %d", ret);
                i2s_channel_disable(rx_handle);
                return ret;
            }
        } while (ret == ESP_ERR_TIMEOUT && retry_count < 3);
        
        if (bytes_read_once > 0) {
            total_bytes += bytes_read_once;
            
            // 每秒显示进度
            if (esp_log_timestamp() - start_time >= 1000) {
                start_time = esp_log_timestamp();
                ESP_LOGI(TAG, "录音进度: %.1f%%", (float)total_bytes * 100 / total_bytes_to_read);
            }
        }
    }
    
    // 禁用I2S通道
    i2s_channel_disable(rx_handle);
    
    // 返回实际读取的字节数
    *bytes_read = total_bytes;
    
    ESP_LOGI(TAG, "录音完成，共录制 %u 字节的数据", (unsigned int)total_bytes);
    return ESP_OK;
}

/**
 * @brief 通过ES8311播放音频数据
 */
esp_err_t board_play_audio(i2s_chan_handle_t tx_handle, uint8_t *buffer, size_t buffer_size)
{
    if (!tx_handle || !buffer || buffer_size == 0) {
        ESP_LOGE(TAG, "无效参数");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret;
    size_t bytes_written = 0;
    
    // 重置I2S通道状态
    i2s_channel_disable(tx_handle);
    vTaskDelay(pdMS_TO_TICKS(10)); // 等待I2S通道状态稳定
    
    // 预加载部分数据
    size_t preload_size = buffer_size > 1024 ? 1024 : buffer_size;
    ret = i2s_channel_preload_data(tx_handle, buffer, preload_size, &bytes_written);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "预加载数据失败");
        return ret;
    }
    
    ESP_LOGI(TAG, "预加载了 %u 字节的音频数据", (unsigned int)bytes_written);
    
    // 打开功放
    board_pa_power(true);
    
    // 启用发送通道
    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启用I2S通道失败");
        board_pa_power(false);
        return ret;
    }
    
    // 播放剩余数据
    size_t remaining = buffer_size - bytes_written;
    size_t offset = bytes_written;
    
    ESP_LOGI(TAG, "开始播放音频...");
    uint32_t start_time = esp_log_timestamp();
    
    while (remaining > 0) {
        ret = i2s_channel_write(tx_handle, buffer + offset, remaining, &bytes_written, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "写入I2S通道失败: %d", ret);
            break;
        }
        
        if (bytes_written > 0) {
            remaining -= bytes_written;
            offset += bytes_written;
            
            // 每秒显示进度
            if (esp_log_timestamp() - start_time >= 1000) {
                start_time = esp_log_timestamp();
                ESP_LOGI(TAG, "播放进度: %.1f%%", (float)(buffer_size - remaining) * 100 / buffer_size);
            }
        }
    }
    
    // 等待所有数据播放完毕
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // 禁用I2S通道并关闭功放
    i2s_channel_disable(tx_handle);
    board_pa_power(false);
    
    ESP_LOGI(TAG, "音频播放完成");
    return ESP_OK;
} 