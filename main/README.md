# ESP32-S3 集成板载应用

## 数派硬件设计部
- 验证人：wentao
- 最新验证日期：2025-04-02

## 概述

## 待优化细节

1. **自动配网针对不同设备再次优化**  （优先级2）
   - 优化不同设备的配网页面适配
   - 改进配网流程的用户体验 （自动扫描当前wifi （信号强度）可以手动选择然后输入密码 密码可以显示）
   - 增加配网失败的重试机制 

2. **主板驱动过长，分离各模块使代码更清晰更模块化**   （优先级1）
   - 将board.c拆分为多个功能模块
   - 分离音频、网络、WebSocket等独立模块
   - 优化代码结构，提高可维护性

3. **WebSocket事件**  （优先级0） 
   - 完善WebSocket事件处理机制 
   - 增加更多事件类型支持
   - 优化事件处理的可靠性
   服务端传输数据固定格式：
   {"event":"eventname","data":{}}

目前事件有

服务端重启设备 （正式功能）
{
  "clientId": "esp32s3_board_01",
  "param": {
  },
  "eventName": "restart"
}


播放不同的pcm 4个（测试功能）
{
  "clientId": "esp32s3_board_01",
  "param": {
    "id": 4
  },
  "eventName": "play_pcm"
}


录音5秒后播放 （测试功能）
{
  "clientId": "esp32s3_board_01",
  "param": {
    "duration": 5
  },
  "eventName": "start_recording"
}


4. **语音播报测试**     （优先级3）
   - 完善语音播报功能测试 （更多的提示音） 
   - 增加更多音频格式支持 （mp3）
   - 优化音频播放质量 （目前播放第一段音频存在加速） （尝试多种方法未解决）


5. **spiff系统** 


本项目整合了三个独立功能模块到一个统一的应用中：
1. **音频系统**：支持通过ES7210录音和ES8311播放
2. **WiFi连接与配网**：包括SoftAP配网模式和WiFi连接管理
3. **WebSocket客户端**：与远程服务器通信，接收指令并执行操作

## 功能特点

### 启动流程
- 上电后检查所有协议（I2C, I2S），确认各芯片正常工作
- 自动进入配网流程，如已配置过WiFi则直接连接
- 成功连接WiFi后，启动WebSocket客户端与服务器建立实时通信

### 音频功能
- 支持ES7210高品质立体声录音
- 支持ES8311高品质音频播放
- 支持服务器远程触发录音功能

### 网络功能
- 智能WiFi配网（热点自动跳转到配网页面）
- 连接参数存储到NVS，断电不丢失
- 支持恢复出厂设置（长按BOOT按钮）

### 远程控制功能
- 基于WebSocket的双向通信
- 服务器可发送命令控制设备行为
- 设备状态实时上报服务器

## 文件结构

```
main/
├── board.c         # 板级驱动实现（I2C, I2S, WiFi, WebSocket等）
├── board.h         # 板级驱动头文件（硬件定义、API声明）
├── main.c          # 主程序入口（应用逻辑、事件处理）
├── index.html      # 配网页面
├── CMakeLists.txt  # 编译配置
└── idf_component.yml  # 依赖管理
```

## API 说明

### 板级初始化
- `board_init()`: 初始化所有板载硬件
- `board_check_chip_status()`: 检查所有芯片状态

### 音频控制
- `board_audio_playback_init()`: 初始化ES8311播放设备
- `board_audio_record_init()`: 初始化ES7210录音设备
- `board_audio_play()`: 播放音频数据
- `board_audio_record()`: 录制音频数据

### WiFi配置
- `board_wifi_sta_init()`: 初始化WiFi STA模式
- `board_wifi_softap_start()`: 启动配网模式
- `board_wifi_has_valid_config()`: 检查是否有有效的WiFi配置

### WebSocket通信
- `board_websocket_init()`: 初始化WebSocket客户端
- `board_websocket_start()`: 启动WebSocket连接

## 服务器通信协议

WebSocket客户端和服务器之间采用JSON格式通信：

### 客户端到服务器
```json
{
  "clientId": "esp32s3_board_01",
  "status": "connected",
  "type": "esp32s3"
}
```

### 服务器到客户端
```json
{
  "event": "start_recording",
  "duration": 5
}
```

## 如何构建

1. 确保已安装ESP-IDF (v5.0+)
2. 克隆仓库到本地
3. 运行以下命令构建和烧录:

```bash
idf.py build
idf.py -p (PORT) flash monitor
```

## 依赖项

- ESP-IDF v5.0 或更高版本
- ES8311驱动 (v1.0.0)
- ES7210驱动 (v1.0.0)
- ESP WebSocket客户端 (v1.4.0)
- JSON处理库 

