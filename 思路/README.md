# 智能百叶窗 — 通信模块总体设计

## 我的分工

- **WiFi 通信**（ESP32-P4 + C6）：设备端通用消息收发
- **手机端 APP**：Web UI 控制面板，通用消息收发

## 整体架构

```
┌──────────────────┐       WebSocket        ┌──────────────────────┐       WebSocket        ┌──────────────┐
│   ESP32-P4 + C6  │ ◄───────────────────► │  阿里云 Windows 服务器   │ ◄───────────────────► │  手机浏览器     │
│   (设备端 client) │     JSON 双向消息       │  47.105.45.239         │     JSON 双向消息       │  Vue.js WebUI │
└──────────────────┘                        └──────────────────────┘                        └──────────────┘
```

## 为什么需要服务器

ESP32 通过 Wi-Fi 连接局域网，没有公网 IP。手机在 4G/5G 或外部网络无法直接访问设备。
服务器充当**消息中继**，ESP32 和手机都作为 WebSocket client 连接到服务器，服务器负责转发消息。

## 三层代码

| 位置 | 代码 | 语言 | 说明 |
|------|------|------|------|
| Windows 服务器 | WebSocket 消息中继 | Python (websockets) | 核心：设备注册 + 消息路由转发 |
| ESP32-P4 + C6 | WebSocket client + 消息收发 | C (ESP-IDF) | 核心：通用 send/recv 接口 |
| 手机浏览器 | Web UI 控制面板 | HTML + Vue.js | 核心：通用 send/recv 接口 |

## 消息协议

统一使用 JSON 格式。通信层使用固定信封 `{ type, payload }` 传输，上层消息内容完全透明：

```json
{
  "type": "send",
  "payload": {
    "type": "用户自定义消息类型",
    ...用户自定义字段...
  }
}
```

服务器只负责路由转发，不解析 payload 内部内容：
1. ESP32 → 服务器 → 手机：原样转发
2. 手机 → 服务器 → ESP32：原样转发

## 设备标识

每个 ESP32 设备启动后指定唯一 device_id（如 `blinds_001`），服务器通过 device_id 区分不同设备。
手机 Web UI 通过指定 device_id 来连接目标设备。

## 消息流向

```
ESP32 ──→ {"type":"sensor_data", "payload":{...}} ──→ 服务器 ──→ 手机
手机   ──→ {"type":"cmd",         "payload":{...}} ──→ 服务器 ──→ ESP32
```

## 访问方式

| 场景 | 方式 |
|------|------|
| 局域网开发/调试 | 手机连同一 Wi-Fi，浏览器直接访问 ESP32 内网 IP |
| 外网远程访问 | 手机浏览器访问 `http://47.105.45.239:端口`，通过服务器中继 |

## 消息类型（预留）

由上层应用自行定义和扩展，通信层不做限制。示例：

| type | 方向 | 说明 |
|------|------|------|
| `sensor_data` | ESP32 → 手机 | 传感器数据上报 |
| `cmd` | 手机 → ESP32 | 控制指令 |
| `status` | ESP32 → 手机 | 设备状态 |
| `ack` | 双向 | 确认/响应 |

## 目录结构

```
code/Net/
├── main/
│   ├── CMakeLists.txt          # ESP-IDF 组件配置
│   ├── main.c                  # 示例入口 (初始化消息总线 + 传感器任务)
│   ├── msg_bus.h               # 通用消息总线接口 (send / recv / poll)
│   └── msg_bus.c               # 消息总线实现 (WiFi + WebSocket client)
├── server/
│   ├── relay_server.py         # WebSocket 中继服务器
│   └── requirements.txt        # Python 依赖
├── webui/
│   └── index.html              # 手机端 Web 控制面板 (Vue.js)
└── 思路/
    └── README.md               # 本文件
```

## 快速开始

### 1. 启动服务器

```bash
pip install -r server/requirements.txt
python server/relay_server.py
```

### 2. 烧录 ESP32

修改 `main/msg_bus.c` 中的 WiFi SSID/密码，编译烧录。

### 3. 打开手机 Web UI

- **局域网**: 手机连同一 WiFi，浏览器打开 `index.html`，连服务器地址
- **外网**: 将 `index.html` 部署到服务器，手机浏览器访问 `http://47.105.45.239:端口/webui/index.html`

## ESP32 端 API 参考

```c
// 初始化 (连接 WiFi + WebSocket + 注册设备)
int msg_bus_init("ws://47.105.45.239:8765", "blinds_001");

// 发送消息
msg_bus_send("sensor_data", "{\"temp\":25.3}");

// 注册接收回调
void on_msg(const char *type, const char *data, uint16_t len) {
    // 处理收到的消息
}
msg_bus_on_recv(on_msg);

// 轮询 (如有需要)
msg_bus_poll();
```

## 后续扩展方向

- 接入大模型 API：服务器端添加 `nl_cmd` 类型处理，调用 LLM API 将自然语言转为结构化指令，其余层不变
- 多设备支持：当前已支持多 device_id，服务器自动路由
- 消息持久化：服务器端加 SQLite 记录历史消息
- HTTPS/WSS：Nginx 反向代理 + SSL 证书
claude --resume 78bf2cab-6a38-45f9-b6fa-510d0a78a8f5 --dangerously-skip-permissions