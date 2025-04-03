/**
 * board.h - ESP32-S3开发板音频系统配置文件
 * 包含ES8311(播放)和ES7210(录音)相关的配置及函数声明
 */

#pragma once

#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "es8311.h"
#include "es7210.h"

/**************************** 公共定义 ****************************/
/* I2C配置 */
#define BOARD_I2C_NUM             0       // I2C 控制器的通道编号
#define BOARD_I2C_SDA_IO          1       // I2C 数据引脚   
#define BOARD_I2C_SCL_IO          2       // I2C 时钟引脚
#define BOARD_I2C_FREQ_HZ         100000  // 100kHz

/* 音频采样配置 */
#define BOARD_SAMPLE_RATE         48000   // 采样率
#define BOARD_MCLK_MULTIPLE       256     // MCLK倍率
#define BOARD_MCLK_FREQ_HZ        (BOARD_SAMPLE_RATE * BOARD_MCLK_MULTIPLE)  // MCLK频率

/**************************** ES8311 播放相关配置 ****************************/
/* ES8311 I2C地址 */
#define BOARD_ES8311_ADDR         ES8311_ADDRRES_0  // ES8311 I2C地址

/* ES8311 I2S接口定义 */
#define BOARD_ES8311_I2S_NUM      0  // I2S 控制器的通道编号
#define BOARD_ES8311_MCLK_IO      38 // 主时钟引脚
#define BOARD_ES8311_BCK_IO       14 
#define BOARD_ES8311_WS_IO        13 // 数据时钟引脚
#define BOARD_ES8311_DO_IO        45 // 数据输出引脚
#define BOARD_ES8311_DI_IO        -1  // 8311不需要数据输入 没有DI引脚

/* ES8311 播放参数 */
#define BOARD_ES8311_VOLUME       85  // 音量(0-100)

/* 功放控制GPIO */
#define BOARD_PA_EN_GPIO          3   // 功放使能引脚

/**************************** ES7210 录音相关配置 ****************************/
/* ES7210 I2C地址 */
#define BOARD_ES7210_ADDR         0x41  // ES7210 I2C地址
#define BOARD_ES7210_I2C_CLK      50000  // ES7210 I2C时钟

/* ES7210 I2S接口定义 */
#define BOARD_ES7210_I2S_NUM      0      // I2S 控制器的通道编号
#define BOARD_ES7210_MCLK_IO      38     // 主时钟引脚
#define BOARD_ES7210_BCK_IO       14     // 数据时钟引脚
#define BOARD_ES7210_WS_IO        13     // 数据时钟引脚
#define BOARD_ES7210_DI_IO        12     // 数据输入引脚
#define BOARD_ES7210_DO_IO        -1     // 7210不需要数据输出 没有DO引脚

/* ES7210 录音参数 */
#define BOARD_ES7210_MIC_GAIN     ES7210_MIC_GAIN_30DB  // 麦克风增益
#define BOARD_ES7210_MIC_BIAS     ES7210_MIC_BIAS_2V87  // 麦克风偏置电压
#define BOARD_ES7210_ADC_VOLUME   0                     // ADC音量

/* ES7210 TDM配置 */
#define BOARD_ES7210_TDM_FORMAT   ES7210_I2S_FMT_I2S  // 音频格式
#define BOARD_ES7210_TDM_SLOT_MASK (I2S_TDM_SLOT0 | I2S_TDM_SLOT1)  // 使用两个时隙

/* 录音缓冲区配置 */
#define BOARD_RECORD_BUFFER_SIZE  (48000 * 2 * 2 * BOARD_RECORD_SECONDS)

/* 录音时长配置 */
#define BOARD_RECORD_SECONDS      5  // 默认录音时长为5秒 最大为40秒

/**************************** 函数声明 ****************************/
/**
 * @brief 初始化I2C接口
 * @return ESP_OK成功，其他值失败
 */
esp_err_t board_i2c_init(void);

/**
 * @brief 初始化ES8311播放接口
 * @param[out] tx_handle 返回I2S发送通道句柄
 * @return ESP_OK成功，其他值失败
 */
esp_err_t board_es8311_init(i2s_chan_handle_t *tx_handle);

/**
 * @brief 初始化ES7210录音接口
 * @param[out] rx_handle 返回I2S接收通道句柄
 * @return ESP_OK成功，其他值失败
 */
esp_err_t board_es7210_init(i2s_chan_handle_t *rx_handle);

/**
 * @brief 从ES7210录制音频数据
 * @param rx_handle I2S接收通道句柄
 * @param buffer 存储录制音频的缓冲区
 * @param buffer_size 缓冲区大小(字节)
 * @param seconds 录制时长(秒)
 * @param[out] bytes_read 实际录制的数据大小
 * @return ESP_OK成功，其他值失败
 */
esp_err_t board_record_audio(i2s_chan_handle_t rx_handle, uint8_t *buffer, 
                             size_t buffer_size, int seconds, size_t *bytes_read);

/**
 * @brief 通过ES8311播放音频数据
 * @param tx_handle I2S发送通道句柄
 * @param buffer 要播放的音频数据
 * @param buffer_size 缓冲区大小(字节)
 * @return ESP_OK成功，其他值失败
 */
esp_err_t board_play_audio(i2s_chan_handle_t tx_handle, uint8_t *buffer, size_t buffer_size);

/**
 * @brief 控制功放开关
 * @param enable true开启，false关闭
 */
void board_pa_power(bool enable); 