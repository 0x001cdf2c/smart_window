#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化语音回复 (I2S TX + ESP-TTS)
 * dout_gpio: I2S DOUT 引脚 (ESP32 → ES8311 DAC)
 * 返回 0 成功, -1 失败 */
int voice_reply_init(int dout_gpio);

/* 合成并播放中文文本 */
void voice_reply_say(const char *text);

#ifdef __cplusplus
}
#endif
