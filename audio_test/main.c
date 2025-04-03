/**
 * main.c - ESP32-S3开发板音频系统
 * 最新验证日期 20250327
 * 验证人 wentao
 * 上电后先通过ES7210录音5秒钟，然后停顿2秒后通过ES8311播放录音的内容
 */

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

static const char *TAG = "AUDIO_MAIN";

// 用于保存录音数据的缓冲区
static uint8_t *audio_buffer = NULL;

// 录音时长(秒)
#define RECORD_TIME_SECONDS BOARD_RECORD_SECONDS

/**
 * @brief 打印内存使用情况
 */
static void print_memory_info(void)
{
    ESP_LOGI(TAG, "内存使用情况统计:");
    
    // 内部内存统计
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t total_internal = heap_caps_get_total_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t min_free_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    
    ESP_LOGI(TAG, "内部内存: 空闲/总计/最小空闲 = %u/%u/%u 字节", 
             (unsigned int)free_internal, 
             (unsigned int)total_internal, 
             (unsigned int)min_free_internal);
    ESP_LOGI(TAG, "内部内存使用率: %.2f%%", 
             (total_internal - free_internal) * 100.0 / total_internal);
    
    // 检查是否有PSRAM
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_psram > 0) {
        size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t min_free_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
        
        ESP_LOGI(TAG, "外部PSRAM: 空闲/总计/最小空闲 = %u/%u/%u 字节", 
                 (unsigned int)free_psram, 
                 (unsigned int)total_psram, 
                 (unsigned int)min_free_psram);
        ESP_LOGI(TAG, "外部PSRAM使用率: %.2f%%", 
                 (total_psram - free_psram) * 100.0 / total_psram);
    } else {
        ESP_LOGI(TAG, "系统未检测到外部PSRAM");
    }
    
    // 音频缓冲区信息
    if (audio_buffer) {
        size_t buffer_size = heap_caps_get_allocated_size(audio_buffer);
        bool is_psram = heap_caps_check_integrity_addr((intptr_t)audio_buffer, true);
        ESP_LOGI(TAG, "音频缓冲区: 大小 = %u 字节, 位置 = %s", 
                 (unsigned int)buffer_size, 
                 is_psram ? "外部PSRAM" : "内部内存");
    } else {
        ESP_LOGI(TAG, "音频缓冲区未分配");
    }
    
    ESP_LOGI(TAG, "------------------------");
}

void app_main(void)
{
    esp_err_t ret;
    size_t bytes_recorded = 0;
    i2s_chan_handle_t rx_handle = NULL; // ES7210 接收通道
    i2s_chan_handle_t tx_handle = NULL; // ES8311 发送通道
    
    ESP_LOGI(TAG, "ESP32-S3音频系统启动");
    ESP_LOGI(TAG, "------------------------");
    
    /************************ 第一阶段：分配内存 ************************/
    // 为录音数据分配缓冲区 - 使用外部PSRAM
    ESP_LOGI(TAG, "分配录音缓冲区...");
    audio_buffer = heap_caps_malloc(BOARD_RECORD_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (audio_buffer == NULL) {
        ESP_LOGE(TAG, "分配录音缓冲区失败，尝试使用内部内存");
        // 尝试使用内部内存分配较小的缓冲区
        size_t smaller_size = BOARD_RECORD_BUFFER_SIZE / 2;
        audio_buffer = heap_caps_malloc(smaller_size, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        if (audio_buffer == NULL) {
            ESP_LOGE(TAG, "无法分配内存，退出");
            goto exit;
        }
        ESP_LOGW(TAG, "使用较小的内存缓冲区: %u 字节", (unsigned int)smaller_size);
    } else {
        ESP_LOGI(TAG, "成功在PSRAM中分配 %u 字节的内存", (unsigned int)BOARD_RECORD_BUFFER_SIZE);
    }
    
    /************************ 第二阶段：录音 ************************/
    // 初始化ES7210
    ESP_LOGI(TAG, "初始化ES7210录音接口...");
    ret = board_es7210_init(&rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化ES7210失败");
        goto exit;
    }
    
    ESP_LOGI(TAG, "------------------------");
    ESP_LOGI(TAG, "开始录音阶段 (%d秒)", RECORD_TIME_SECONDS);
    ESP_LOGI(TAG, "请对着麦克风说话...");
    
    // 录制音频
    ret = board_record_audio(rx_handle, audio_buffer, 
                            heap_caps_get_allocated_size(audio_buffer), // 使用实际分配的大小
                            RECORD_TIME_SECONDS, 
                            &bytes_recorded);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "录音失败");
        goto exit;
    }
    
    ESP_LOGI(TAG, "录音完成，共录制 %u 字节的数据", (unsigned int)bytes_recorded);
    if (bytes_recorded == 0) {
        ESP_LOGE(TAG, "未录制到任何数据");
        goto exit;
    }
    
    // 释放录音资源
    if (rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
    }
    
    /************************ 第三阶段：暂停 ************************/
    ESP_LOGI(TAG, "------------------------");
    ESP_LOGI(TAG, "暂停5秒，打印内存使用情况...");
    
    // 打印内存使用情况
    print_memory_info();
    
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    /************************ 第四阶段：播放 ************************/
    // 初始化ES8311
    ESP_LOGI(TAG, "初始化ES8311播放接口...");
    ret = board_es8311_init(&tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化ES8311失败");
        goto exit;
    }
    
    ESP_LOGI(TAG, "------------------------");
    ESP_LOGI(TAG, "开始播放录制的音频");
    
    // 播放录制的音频
    ret = board_play_audio(tx_handle, audio_buffer, bytes_recorded);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "播放失败");
        goto exit;
    }
    
    ESP_LOGI(TAG, "播放完成");
    
    /************************ 第五阶段：结束 ************************/
    ESP_LOGI(TAG, "------------------------");
    ESP_LOGI(TAG, "音频系统测试完成");
    ESP_LOGI(TAG, "如果您听到刚才录制的声音，则表示音频系统工作正常");
    
exit:
    // 释放资源
    if (rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
    }
    
    if (tx_handle) {
        i2s_channel_disable(tx_handle);
        i2s_del_channel(tx_handle);
    }
    
    if (audio_buffer) {
        free(audio_buffer);
        audio_buffer = NULL;
    }
    
    // 测试结束，进入空闲状态
    ESP_LOGI(TAG, "进入空闲状态，每隔1秒打印一次内存使用情况");
    ESP_LOGI(TAG, "------------------------");
    
    while (1) {
        // 每2秒打印内存使用情况
        print_memory_info();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
} 