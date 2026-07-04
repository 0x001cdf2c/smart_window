#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 消息回调函数类型
 * @param type  消息类型字符串
 * @param data  消息负载 JSON 字符串
 * @param data_len 数据长度
 */
typedef void (*msg_bus_callback_t)(const char *type, const char *data, uint16_t data_len);

/**
 * 初始化消息总线
 * @param server_url   WebSocket 服务器地址, 如 "ws://47.105.45.239:8765"
 * @param device_id    设备唯一 ID, 如 "blinds_001"
 * @return 0 成功, -1 失败
 */
int msg_bus_init(const char *server_url, const char *device_id);

/**
 * 发送消息
 * @param type    消息类型, 如 "sensor_data", "status"
 * @param payload JSON 格式的负载字符串, 如 "{\"temp\":25.3}"
 * @return 0 成功, -1 失败
 */
int msg_bus_send(const char *type, const char *payload);

/**
 * 注册接收回调 (由上层调用此函数收听消息)
 * @param cb 回调函数指针
 */
void msg_bus_on_recv(msg_bus_callback_t cb);

/**
 * 轮询, 需在主循环或单独任务中周期性调用
 */
void msg_bus_poll(void);

#ifdef __cplusplus
}
#endif
