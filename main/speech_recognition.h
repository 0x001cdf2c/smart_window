#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 唤醒词回调: wake_word_index 从 1 开始, 对应 wake_words.h 中 WAKE_WORDS 的顺序 */
typedef void (*sr_on_wake_t)(int wake_word_index, const char *wake_word_name);

/* 语音命令回调: command_str 为识别到的中文命令文本 */
typedef void (*sr_on_command_t)(const char *command_str);

/* 音频流回调: samples=PCM数据, count=采样点数, is_end=是否为最后一块 */
typedef void (*sr_on_audio_t)(const int16_t *samples, int count, bool is_end);

/* 初始化语音识别 (I2S 麦克风 + AFE + WakeNet)
 * 返回 0 成功, -1 失败 */
int sr_init(void);

/* 注册唤醒回调 */
void sr_on_wake_cb(sr_on_wake_t cb);

/* 注册命令回调 (MultiNet 模式) */
void sr_on_command_cb(sr_on_command_t cb);

/* 注册音频流回调 (ASR 流模式, 替代 MultiNet) */
void sr_on_audio_cb(sr_on_audio_t cb);

/* 启动语音识别 */
void sr_start(void);

/* 停止当前音频流 (ASR 结果返回后调用) */
void sr_stop_streaming(void);

/* 轮询: 需在主循环或独立任务中周期性调用 */
void sr_poll(void);

/* 暂停/恢复 I2S 录音 (播放语音回复时暂停) */
void sr_pause(bool pause);

/* 获取 I2S TX 通道句柄 (供 voice_reply 播放 TTS 音频) */
i2s_chan_handle_t sr_get_tx_chan(void);

/* 恢复 I2S RX 通道 (在 TTS 初始化可能干扰 RX 状态后调用) */
void sr_recover_rx(void);

/* 配置 ES8311 用于播放 (音量 + 取消静音) */
void sr_configure_playback(void);

#ifdef __cplusplus
}
#endif
