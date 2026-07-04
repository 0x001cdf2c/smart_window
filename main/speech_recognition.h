#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 唤醒词回调: wake_word_index 从 1 开始, 对应 wake_words.h 中 WAKE_WORDS 的顺序 */
typedef void (*sr_on_wake_t)(int wake_word_index, const char *wake_word_name);

/* 语音命令回调: command_str 为识别到的中文命令文本 */
typedef void (*sr_on_command_t)(const char *command_str);

/* 初始化语音识别 (I2S 麦克风 + AFE + WakeNet + MultiNet)
 * 返回 0 成功, -1 失败 */
int sr_init(void);

/* 注册唤醒回调 */
void sr_on_wake_cb(sr_on_wake_t cb);

/* 注册命令回调 */
void sr_on_command_cb(sr_on_command_t cb);

/* 启动语音识别 */
void sr_start(void);

/* 轮询: 需在主循环或独立任务中周期性调用 */
void sr_poll(void);

#ifdef __cplusplus
}
#endif
