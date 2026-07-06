#pragma once

/* ── 关键词唤醒列表 ──
 * 每个唤醒词对应一个 wakenet 模型, 模型名格式: wn9_<唤醒词>
 * 可用的中文唤醒词模型:
 *   "wn9_hilexin"        → "你好乐鑫"
 *   "wn9_xiaoaitongxue"  → "小爱同学"
 *   "wn9_hiesp"          → "嗨 ESP"
 *   "wn9_nihaoxiaozhi"   → "你好小智"
 *   "wn9_hiwalle_tts2"   → "Hi Walle"
 */

/* 当前使用的唤醒词模型列表, 以 NULL 结尾
 * 顺序必须与 AFE 内部加载顺序一致:
 *   model 1: wn9_hiesp    → "嗨 ESP"
 *   model 2: wn9_hilexin  → "你好乐鑫" */
static const char *WAKE_WORDS[] = {
    "wn9_hiesp",          /* "嗨 ESP" */
    NULL
};

/* 每个唤醒词对应的显示名称, 索引 = wakenet_model_index - 1 */
static const char *WAKE_WORD_NAMES[] = {
    "嗨 ESP",
};

/* ── 中文命令词列表 (拼音无音调, MultiNet7 要求) ──
 * 格式: 拼音音节以空格分隔, 不带声调数字
 * 如 "da kai chuang lian" = 打开窗帘
 */
static const char *SPEECH_COMMANDS[] = {
    "da kai chuang lian",   /* 打开窗帘 */
    "guan bi chuang lian",  /* 关闭窗帘 */
    "ting zhi",             /* 停止 */
    "da kai deng guang",    /* 打开灯光 */
    "guan bi deng guang",   /* 关闭灯光 */
    NULL
};
