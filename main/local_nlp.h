/*
 * local_nlp.h — 本地 NLP 意图分类器 (规则引擎 + 加权评分)
 *
 * 职责: 将中文语音指令分类为 5 种意图, 给出置信度,
 *       高置信度本地执行, 低置信度/闲聊/定时转发云端 DeepSeek
 */

#pragma once

#include <stdbool.h>

typedef enum {
    NLP_INTENT_OPEN     = 0,
    NLP_INTENT_CLOSE    = 1,
    NLP_INTENT_STOP     = 2,
    NLP_INTENT_TIMER    = 3,
    NLP_INTENT_CHAT     = 4,
} nlp_intent_t;

typedef struct {
    nlp_intent_t intent;
    float        confidence;   /* 0.0 ~ 1.0 */
    const char  *reply;        /* 模板回复文本 */
    const char  *intent_name;  /* 调试用 */
    bool         force_execute; /* 用户说了"执行" → 忠实执行, 不经过云端决策 */
} nlp_result_t;

void nlp_result_t_init(nlp_result_t *r);
void local_nlp_init(void);
nlp_result_t local_nlp_classify(const char *text);
bool local_nlp_should_use_cloud(const nlp_result_t *result);
