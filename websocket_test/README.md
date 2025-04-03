# ESP32-S3 WebSocket客户端测试

## 数派硬件设计部
- 验证人：wentao
- 最新验证日期：2025-03-27

这是一个基于ESP32-S3的WebSocket客户端应用程序测试，用于实现设备与王力宏本地服务器的实时通信。

## 功能特性

- 自动WiFi连接
- WebSocket客户端通信
- 支持服务器远程控制（如重启设备）
- 自动重连机制
- 心跳检测

## 硬件要求

- ESP32-S3开发板
- WiFi网络环境

## 软件依赖

- ESP-IDF (ESP32开发框架)
- FreeRTOS
- cJSON库
- esp_websocket_client组件

## 配置说明

在 `board.h` 文件中可以修改以下配置：

### WiFi配置
```c
#define WIFI_SSID               "your_wifi_ssid"
#define WIFI_PASS               "your_wifi_password"
#define WIFI_MAXIMUM_RETRY      5
```

### WebSocket服务器配置
```c
#define WS_SERVER_HOST          "your_server_ip"
#define WS_SERVER_PORT          8086
#define WS_SERVER_PATH          "/robws"
```

### 设备ID配置
```c
#define DEVICE_CLIENT_ID        "your_device_id"
```

## 通信协议

### 连接建立
设备连接WebSocket服务器后，会发送以下格式的消息：
```json
{
    "clientId": "device_id",
    "status": "connected"
}
```

### 命令处理
服务器可以发送JSON格式的命令，目前支持：
- 重启命令：
```json
{
    "event": "restart"
}
```

### 命令响应
设备执行命令后会发送响应：
```json
{
    "status": "ok",
    "message": "command result",
    "clientId": "device_id"
}
```

## 编译和烧录

1. 确保已安装ESP-IDF开发环境
2. 进入项目目录
3. 执行以下命令：
```bash
idf.py build
idf.py -p [PORT] flash monitor
```

## 调试信息

程序运行时会输出以下调试信息：
- WiFi连接状态
- WebSocket连接状态
- 收到的服务器命令
- 命令执行结果

## 注意事项

1. 确保WiFi网络可用且配置正确
2. 确保WebSocket服务器地址和端口配置正确
3. 设备ID需要唯一，避免冲突
4. 建议在开发环境中先测试WebSocket服务器连接

## 许可证

[添加许可证信息]