/*
 * local_nlp.c — 本地 NLP 意图分类器
 *
 * 算法: 多模式加权评分 + 否定词检测 + 时间词识别
 *       纯规则引擎, 零依赖, 无模型文件, 推理 < 1ms
 *
 * 与云端 DeepSeek 分工:
 *   - 高置信度 open/close/stop → 本地执行 + 模板回复
 *   - 含时间词 → 转发云端做实体抽取
 *   - 低置信度/闲聊 → 转发云端 DeepSeek
 */

#include "local_nlp.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_log.h"

static const char *TAG = "nlp";

/* ── 分类阈值 ── */
#define NLP_CLOUD_THRESHOLD   0.70f   /* 低于此值 → 云端兜底 */

/* ── 关键词模式 ── */
typedef struct {
    const char  *word;
    nlp_intent_t intent;
    float        weight;       /* 0.0 ~ 1.0 */
    const char  *desc;         /* 调试标签 */
} rule_t;

static const rule_t RULES[] = {
    /* ═══ 开窗 ═══ */
    {"打开",        NLP_INTENT_OPEN,  0.30f, "打开"},
    {"开窗",        NLP_INTENT_OPEN,  0.50f, "开窗"},
    {"开一下",      NLP_INTENT_OPEN,  0.50f, "开一下"},
    {"开开",        NLP_INTENT_OPEN,  0.45f, "开开"},
    {"通通风",      NLP_INTENT_OPEN,  0.55f, "通通风"},
    {"透透气",      NLP_INTENT_OPEN,  0.55f, "透透气"},
    {"通风",        NLP_INTENT_OPEN,  0.45f, "通风"},
    {"换气",        NLP_INTENT_OPEN,  0.45f, "换气"},
    {"凉快",        NLP_INTENT_OPEN,  0.30f, "凉快"},
    {"太闷",        NLP_INTENT_OPEN,  0.30f, "太闷(情绪)"},
    {"闷死",        NLP_INTENT_OPEN,  0.35f, "闷死(情绪)"},
    {"热死",        NLP_INTENT_OPEN,  0.35f, "热死(情绪)"},
    {"有点热",      NLP_INTENT_OPEN,  0.35f, "有点热"},
    {"屋里",        NLP_INTENT_OPEN,  0.10f, "屋里"},  /* 弱信号 */

    /* ═══ 关窗 ═══ */
    {"关闭",        NLP_INTENT_CLOSE, 0.30f, "关闭"},
    {"关窗",        NLP_INTENT_CLOSE, 0.50f, "关窗"},
    {"关上",        NLP_INTENT_CLOSE, 0.50f, "关上"},
    {"关一下",      NLP_INTENT_CLOSE, 0.50f, "关一下"},
    {"遮光",        NLP_INTENT_CLOSE, 0.45f, "遮光"},
    {"遮阳",        NLP_INTENT_CLOSE, 0.45f, "遮阳"},
    {"防晒",        NLP_INTENT_CLOSE, 0.45f, "防晒"},
    {"太晒",        NLP_INTENT_CLOSE, 0.55f, "太晒"},
    {"太亮",        NLP_INTENT_CLOSE, 0.55f, "太亮"},
    {"刺眼",        NLP_INTENT_CLOSE, 0.55f, "刺眼"},
    {"反光",        NLP_INTENT_CLOSE, 0.45f, "反光"},
    {"光线",        NLP_INTENT_CLOSE, 0.25f, "光线"},
    {"太阳太大",    NLP_INTENT_CLOSE, 0.55f, "太阳太大"},

    /* ═══ 停止 ═══ */
    {"停止",        NLP_INTENT_STOP,  0.60f, "停止"},
    {"停",          NLP_INTENT_STOP,  0.35f, "停"},
    {"暂停",        NLP_INTENT_STOP,  0.55f, "暂停"},
    {"别动",        NLP_INTENT_STOP,  0.50f, "别动"},
    {"取消",        NLP_INTENT_STOP,  0.40f, "取消"},
};

static const int RULE_COUNT = sizeof(RULES) / sizeof(RULES[0]);

/* ── 否定词 ── */
static const char *NEG_WORDS[] = {"不", "别", "不要", "不想", "不用", "别开", "别关", NULL};
static const char *TIMER_WORDS[] = {"点", "分", "半", "定时", "后", "分钟", "小时",
                                     "上午", "下午", "中午", "早上", "晚上", "凌晨", NULL};

/* ── 模板回复池 ── */
static const char *REPLY_OPEN[]  = {"好的，打开窗户~", "开窗透透气~", "收到，开窗!"};
static const char *REPLY_CLOSE[] = {"好的，关上窗户~", "窗户已关闭", "收到，关窗!"};
static const char *REPLY_STOP[]  = {"收到，已停止", "好的，停下来啦"};

static const int REPLY_OPEN_N  = sizeof(REPLY_OPEN)  / sizeof(REPLY_OPEN[0]);
static const int REPLY_CLOSE_N = sizeof(REPLY_CLOSE) / sizeof(REPLY_CLOSE[0]);
static const int REPLY_STOP_N  = sizeof(REPLY_STOP)  / sizeof(REPLY_STOP[0]);

static const char *INTENT_NAMES[] = {"开窗", "关窗", "停止", "定时", "闲聊"};
static uint32_t g_call_count = 0;

/* ═══════════════════════════════════════════════
 * 公开接口
 * ═══════════════════════════════════════════════ */

void local_nlp_init(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  本地 NLP 分类器已就绪");
    ESP_LOGI(TAG, "  引擎: 规则引擎 + 加权评分");
    ESP_LOGI(TAG, "  规则: %d 条关键词 | 5 路意图", RULE_COUNT);
    ESP_LOGI(TAG, "  云端阈值: %.0f%% | 否定词: %s",
             NLP_CLOUD_THRESHOLD * 100, "启用");
    ESP_LOGI(TAG, "  [就绪] 推理延迟 < 1ms, 零 PSRAM 占用");
    ESP_LOGI(TAG, "========================================");
}

nlp_result_t local_nlp_classify(const char *text) {
    g_call_count++;

    nlp_result_t r = { .intent = NLP_INTENT_CHAT, .confidence = 0.0f,
                       .reply = "", .intent_name = "闲聊", .force_execute = false };

    if (!text || text[0] == '\0') return r;

    /* ── "执行" 关键词: 用户强制指令, 不做云端天气决策 ── */
    r.force_execute = (strstr(text, "执行") != NULL);

    /* ═══ 炫酷输出: 分类器头部 ═══ */
    printf("\n");
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   ESP32-P4 本地 NLP 分类器  #%-5d             ║\n", (int)g_call_count);
    printf("║   引擎: 规则加权 | %d 条规则 | 5 路意图        ║\n", RULE_COUNT);
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║ [输入] \"%s\"\n", text);
    printf("╠══════════════════════════════════════════════╣\n");

    /* ── 否定词扫描 ── */
    bool has_neg = false;
    const char *neg_hit = NULL;
    for (int i = 0; NEG_WORDS[i]; i++) {
        if (strstr(text, NEG_WORDS[i])) { has_neg = true; neg_hit = NEG_WORDS[i]; break; }
    }
    printf("║ 否定词扫描: %s", has_neg ? "检测到" : "未检测到");
    if (has_neg) printf(" → \"%s\" (降低开/关置信度)", neg_hit);
    printf("\n");

    /* ── 时间词扫描 ── */
    bool has_time = false;
    const char *time_hit = NULL;
    for (int i = 0; TIMER_WORDS[i]; i++) {
        if (strstr(text, TIMER_WORDS[i])) { has_time = true; time_hit = TIMER_WORDS[i]; break; }
    }
    printf("║ 时间词扫描: %s", has_time ? "检测到" : "未检测到");
    if (has_time) printf(" → \"%s\" (可能为定时指令)", time_hit);
    printf("\n╠══════════════════════════════════════════════╣\n");

    /* ── 多模式匹配 ── */
    float scores[5] = {0};
    int hit_count = 0;
    printf("║ 关键词匹配:\n");

    for (int i = 0; i < RULE_COUNT; i++) {
        if (strstr(text, RULES[i].word)) {
            float w = RULES[i].weight;
            /* 否定词: 开窗/关窗相关权重降为 30%% */
            if (has_neg && (RULES[i].intent == NLP_INTENT_OPEN ||
                            RULES[i].intent == NLP_INTENT_CLOSE)) {
                w *= 0.30f;
            }
            scores[RULES[i].intent] += w;
            hit_count++;

            printf("║   ✓ \"%-12s\" → +%.2f (%-4s)",
                   RULES[i].word, w, INTENT_NAMES[RULES[i].intent]);
            if (has_neg && (RULES[i].intent == NLP_INTENT_OPEN ||
                            RULES[i].intent == NLP_INTENT_CLOSE)) {
                printf(" [否决权衰减]");
            }
            printf("\n");
        }
    }

    if (hit_count == 0) {
        printf("║   ✗ 全部未命中\n");
    }

    /* ── 否定句特殊处理: 开窗意图转为关窗 ── */
    if (has_neg && scores[NLP_INTENT_OPEN] > 0.1f) {
        printf("║   ⚠ 否定句修正: open→close 得分转移\n");
        scores[NLP_INTENT_CLOSE] += scores[NLP_INTENT_OPEN] * 0.6f;
        scores[NLP_INTENT_OPEN]  *= 0.15f;
    }

    /* ── 时间词: 定时意图锁定最高分 ── */
    if (has_time) {
        /* 定时意图至少得 0.75 分 */
        if (scores[NLP_INTENT_TIMER] < 0.75f)
            scores[NLP_INTENT_TIMER] = 0.75f;
        printf("║   ⌚ 时间词触发: timer 得分 → %.2f\n", scores[NLP_INTENT_TIMER]);
    }

    /* ── 得分汇总柱状图 ── */
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║ 得分汇总:\n");

    int best = NLP_INTENT_CHAT;
    for (int i = 0; i < 5; i++) {
        float conf_raw = scores[i];
        /* sigmoid 归一化 */
        float conf = (conf_raw > 0.01f)
            ? 1.0f / (1.0f + expf(-7.0f * (conf_raw - 0.30f)))
            : 0.0f;

        if (conf < 0.15f) conf = 0.0f;  /* 去噪 */

        /* 柱状图: 每 0.1 分一个字符块, 最大 25 格 */
        int bar_len = (int)(conf_raw * 25.0f);
        if (bar_len > 25) bar_len = 25;

        char bar[28];
        for (int j = 0; j < bar_len; j++) bar[j] = '▓';
        bar[bar_len] = '\0';

        printf("║   %-4s [%-25s] %5.2f → conf=%3.0f%%%s\n",
               INTENT_NAMES[i], bar, conf_raw,
               conf * 100,
               (i == 0 && conf_raw >= NLP_CLOUD_THRESHOLD) ? " ★" : "");

        if (conf_raw > scores[best]) best = i;
        if (i == 0) {
            /* 实际存入 result 的置信度 */
            r.confidence = conf;
        }
    }

    /* 更新 best */
    float best_raw = 0;
    for (int i = 0; i < 5; i++) {
        if (scores[i] > best_raw) { best_raw = scores[i]; best = i; }
    }

    /* 最终置信度 */
    r.confidence = (best_raw > 0.01f)
        ? 1.0f / (1.0f + expf(-7.0f * (best_raw - 0.30f)))
        : 0.0f;
    if (r.confidence < 0.15f) r.confidence = 0.0f;

    /* 置信度太低 → 归为闲聊 */
    if (r.confidence < 0.40f) {
        best = NLP_INTENT_CHAT;
        r.confidence = 0.05f;
    }

    /* 定时 → 强制云端处理 (需要实体抽取) */
    if (has_time) {
        best = NLP_INTENT_TIMER;
        r.confidence = 0.80f;
    }

    r.intent = (nlp_intent_t)best;
    r.intent_name = INTENT_NAMES[best];

    /* ── 模板回复 ── */
    switch (best) {
    case NLP_INTENT_OPEN:
        r.reply = REPLY_OPEN[g_call_count % REPLY_OPEN_N];
        break;
    case NLP_INTENT_CLOSE:
        r.reply = REPLY_CLOSE[g_call_count % REPLY_CLOSE_N];
        break;
    case NLP_INTENT_STOP:
        r.reply = REPLY_STOP[g_call_count % REPLY_STOP_N];
        break;
    default:
        r.reply = "";
        break;
    }

    /* ── 路由决策框 ── */
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║\n");
    printf("║  预测意图: %-4s    置信度: %3.0f%%\n",
           r.intent_name, r.confidence * 100);

    bool use_cloud = local_nlp_should_use_cloud(&r);

    if (use_cloud) {
        printf("║  路由决策: → 转发云端 DeepSeek\n");
        if (r.intent == NLP_INTENT_TIMER) {
            printf("║  原因: 需要提取时间实体\n");
        } else if (r.intent == NLP_INTENT_CHAT) {
            printf("║  原因: 闲聊类, 需生成式回复\n");
        } else {
            printf("║  原因: 置信度低于阈值 (%.0f%%)\n",
                   NLP_CLOUD_THRESHOLD * 100);
        }
        printf("║  [网络] 发送到 relay_server → DeepSeek\n");
        if (r.intent == NLP_INTENT_TIMER) {
            printf("║  [期望] 返回 {time, cmd, reply}\n");
        } else {
            printf("║  [期望] 返回 {reply} 文本\n");
        }
    } else {
        printf("║  路由决策: ★ 本地执行 (跳过云端)\n");
        printf("║  执行动作: ");
        switch (r.intent) {
        case NLP_INTENT_OPEN:  printf("servo_set_angle(0°) + adaptive_record(open)\n"); break;
        case NLP_INTENT_CLOSE: printf("servo_set_angle(90°) + adaptive_record(close)\n"); break;
        case NLP_INTENT_STOP:
            printf("g_auto_running=false + SERVO_MODE_MANUAL\n"); break;
        default: printf("-\n"); break;
        }
        printf("║  TTS 播报: \"%s\"\n", r.reply);
    }

    printf("╚══════════════════════════════════════════════╝\n");
    fflush(stdout);

    return r;
}

bool local_nlp_should_use_cloud(const nlp_result_t *r) {
    if (!r) return true;
    if (r->intent == NLP_INTENT_CHAT)   return true;
    if (r->intent == NLP_INTENT_TIMER)  return true;
    /* 用户说了"执行" → 本地执行, 不经过云端决策 */
    if (r->force_execute)               return false;
    /* 开/关窗但没有"执行" → 转发云端, 让 AI 根据天气自主判断 */
    if (r->intent == NLP_INTENT_OPEN || r->intent == NLP_INTENT_CLOSE) return true;
    if (r->confidence < NLP_CLOUD_THRESHOLD) return true;
    return false;
}
