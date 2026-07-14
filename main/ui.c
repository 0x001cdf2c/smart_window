#include "ui.h"
#include <stdio.h>
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "ui";

/* ── Colors (WebUI palette) ── */
#define C_BG       lv_color_hex(0xF5F5F8)
#define C_CARD     lv_color_hex(0xFFFFFF)
#define C_ACCENT   lv_color_hex(0x6C5CE7)
#define C_GREEN    lv_color_hex(0x00B894)
#define C_RED      lv_color_hex(0xE74C3C)
#define C_TEXT     lv_color_hex(0x1A1A2E)
#define C_MUTED    lv_color_hex(0x9999B0)
#define C_BORDER   lv_color_hex(0xE8E8F0)
#define C_ORANGE   lv_color_hex(0xE65100)
#define C_BLUE     lv_color_hex(0x54A0FF)
#define C_YELLOW   lv_color_hex(0xF9CA24)

/* ── Persistent handles for value updates ── */
static lv_obj_t *g_lbl_conn    = NULL;
static lv_obj_t *g_led_conn    = NULL;
static lv_obj_t *g_btn_manual  = NULL;
static lv_obj_t *g_btn_auto    = NULL;
static lv_obj_t *g_btn_cw      = NULL;
static lv_obj_t *g_btn_ccw     = NULL;
static lv_obj_t *g_btn_open    = NULL;
static lv_obj_t *g_btn_close   = NULL;
static lv_obj_t *g_lbl_temp    = NULL;
static lv_obj_t *g_lbl_humi    = NULL;
static lv_obj_t *g_lbl_light   = NULL;
static lv_obj_t *g_lbl_w_city  = NULL;
static lv_obj_t *g_lbl_w_desc  = NULL;
static lv_obj_t *g_lbl_w_high  = NULL;
static lv_obj_t *g_lbl_w_low   = NULL;
static lv_obj_t *g_lbl_w_rain  = NULL;
static lv_obj_t *g_lbl_mode   = NULL;
static lv_obj_t *g_lbl_mode_detail = NULL;
static lv_obj_t *g_lbl_sched  = NULL;
static lv_obj_t *g_lbl_adp_left  = NULL;
static lv_obj_t *g_lbl_adp_right = NULL;
static ui_action_handler_t g_action_handler = NULL;

void ui_set_action_handler(ui_action_handler_t handler)
{
    g_action_handler = handler;
}

/* ── Button event callback ── */
static void on_btn_event(lv_event_t *e)
{
    const char *action = (const char *)lv_event_get_user_data(e);
    if (g_action_handler) {
        g_action_handler(action);
    }
}

/* ── Helpers ── */
static lv_obj_t *card_create(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 580, h);
    lv_obj_set_pos(c, 10, y);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, C_BORDER, 0);
    lv_obj_set_style_bg_color(c, C_CARD, 0);
    lv_obj_set_style_shadow_width(c, 6, 0);
    lv_obj_set_style_shadow_color(c, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(c, 15, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    return c;
}

static const lv_font_t *font_for_size(int size)
{
    (void)size;
    return &lv_font_montserrat_14;
}

LV_FONT_DECLARE(lv_font_cn_16);
static const lv_font_t *font_cjk(void)
{
    return &lv_font_cn_16;
}

static lv_obj_t *label_make(lv_obj_t *parent, const char *text, lv_color_t color, int size)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, font_for_size(size), 0);
    return l;
}

static lv_obj_t *cjk_label_make(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, font_cjk(), 0);
    return l;
}

/* ── Top bar: title + connection status ── */
static void top_bar_create(lv_obj_t *screen)
{
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_set_size(bar, 600, 70);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, C_CARD, 0);
    lv_obj_set_style_shadow_width(bar, 8, 0);
    lv_obj_set_style_shadow_opa(bar, 30, 0);
    lv_obj_set_style_shadow_color(bar, lv_color_hex(0x000000), 0);

    /* Title */
    lv_obj_t *title = cjk_label_make(bar, "智能窗帘", C_TEXT);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 20, 0);

    /* Connection LED + text */
    g_led_conn = lv_led_create(bar);
    lv_obj_set_size(g_led_conn, 10, 10);
    lv_obj_align(g_led_conn, LV_ALIGN_RIGHT_MID, -70, 0);
    lv_led_set_color(g_led_conn, C_GREEN);

    g_lbl_conn = cjk_label_make(bar, "已连接", C_MUTED);
    lv_obj_align(g_lbl_conn, LV_ALIGN_RIGHT_MID, -20, 0);
}

/* ── Mode toggle row ── */
static void mode_toggle_create(lv_obj_t *screen)
{
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_set_size(row, 580, 50);
    lv_obj_set_pos(row, 10, 80);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);

    lv_obj_t *lbl = cjk_label_make(row, "模式", C_MUTED);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

    g_btn_manual = lv_btn_create(row);
    lv_obj_set_size(g_btn_manual, 110, 36);
    lv_obj_set_style_radius(g_btn_manual, 20, 0);
    lv_obj_set_style_bg_color(g_btn_manual, C_ORANGE, 0);
    lv_obj_align(g_btn_manual, LV_ALIGN_RIGHT_MID, -100, 0);
    lv_obj_add_event_cb(g_btn_manual, on_btn_event, LV_EVENT_CLICKED, (void *)"manual");
    lv_obj_t *bl = cjk_label_make(g_btn_manual, "手动", lv_color_hex(0xFFFFFF));
    lv_obj_center(bl);

    g_btn_auto = lv_btn_create(row);
    lv_obj_set_size(g_btn_auto, 80, 36);
    lv_obj_set_style_radius(g_btn_auto, 20, 0);
    lv_obj_set_style_bg_color(g_btn_auto, lv_color_hex(0xD5F5E3), 0);
    lv_obj_set_style_shadow_width(g_btn_auto, 0, 0);
    lv_obj_align(g_btn_auto, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_event_cb(g_btn_auto, on_btn_event, LV_EVENT_CLICKED, (void *)"auto");
    bl = cjk_label_make(g_btn_auto, "自动", C_GREEN);
    lv_obj_center(bl);
}

/* ── Servo direction buttons (side by side) ── */
static void servo_buttons_create(lv_obj_t *screen)
{
    lv_obj_t *card = card_create(screen, 140, 160);
    lv_obj_set_style_pad_all(card, 15, 0);

    /* CW button */
    g_btn_cw = lv_btn_create(card);
    lv_obj_set_size(g_btn_cw, 265, 130);
    lv_obj_set_pos(g_btn_cw, 10, 15);
    lv_obj_set_style_radius(g_btn_cw, 16, 0);
    lv_obj_set_style_bg_color(g_btn_cw, C_ACCENT, 0);
    lv_obj_set_style_shadow_width(g_btn_cw, 10, 0);
    lv_obj_set_style_shadow_color(g_btn_cw, C_ACCENT, 0);
    lv_obj_set_style_shadow_opa(g_btn_cw, 80, 0);
    lv_obj_add_event_cb(g_btn_cw, on_btn_event, LV_EVENT_CLICKED, (void *)"cw");
    lv_obj_add_event_cb(g_btn_cw, on_btn_event, LV_EVENT_LONG_PRESSED_REPEAT, (void *)"cw_hold");

    lv_obj_t *l = cjk_label_make(g_btn_cw, "正转", lv_color_hex(0xFFFFFF));
    lv_obj_center(l);

    /* CCW button */
    g_btn_ccw = lv_btn_create(card);
    lv_obj_set_size(g_btn_ccw, 265, 130);
    lv_obj_set_pos(g_btn_ccw, 285, 15);
    lv_obj_set_style_radius(g_btn_ccw, 16, 0);
    lv_obj_set_style_bg_color(g_btn_ccw, lv_color_hex(0xa29bfe), 0);
    lv_obj_set_style_shadow_width(g_btn_ccw, 10, 0);
    lv_obj_set_style_shadow_color(g_btn_ccw, lv_color_hex(0xa29bfe), 0);
    lv_obj_set_style_shadow_opa(g_btn_ccw, 80, 0);
    lv_obj_add_event_cb(g_btn_ccw, on_btn_event, LV_EVENT_CLICKED, (void *)"ccw");
    lv_obj_add_event_cb(g_btn_ccw, on_btn_event, LV_EVENT_LONG_PRESSED_REPEAT, (void *)"ccw_hold");

    l = cjk_label_make(g_btn_ccw, "反转", lv_color_hex(0xFFFFFF));
    lv_obj_center(l);
}

/* ── Sensor row: T/H/L in one compact card ── */
static void sensors_create(lv_obj_t *screen)
{
    lv_obj_t *card = card_create(screen, 310, 80);

    /* Three columns: x=15, x=205, x=395 */
    static const int x_pos[] = {15, 205, 395};
    static const char *icons[] = {"温", "湿", "光"};
    static const char *units[] = {(const char *)"\xc2\xb0" "C", "%", "lux"};

    /* Temp */
    cjk_label_make(card, icons[0], C_MUTED);
    lv_obj_set_pos(lv_obj_get_child(card, lv_obj_get_child_cnt(card) - 1), x_pos[0], 8);
    g_lbl_temp = label_make(card, "--", C_TEXT, 24);
    lv_obj_set_pos(g_lbl_temp, x_pos[0] + 25, 12);
    lv_obj_set_width(g_lbl_temp, 100);
    label_make(card, units[0], C_MUTED, 12);
    lv_obj_set_pos(lv_obj_get_child(card, lv_obj_get_child_cnt(card) - 1), x_pos[0] + 25, 52);

    /* Humi */
    cjk_label_make(card, icons[1], C_MUTED);
    lv_obj_set_pos(lv_obj_get_child(card, lv_obj_get_child_cnt(card) - 1), x_pos[1], 8);
    g_lbl_humi = label_make(card, "--", C_TEXT, 24);
    lv_obj_set_pos(g_lbl_humi, x_pos[1] + 25, 12);
    lv_obj_set_width(g_lbl_humi, 100);
    label_make(card, units[1], C_MUTED, 12);
    lv_obj_set_pos(lv_obj_get_child(card, lv_obj_get_child_cnt(card) - 1), x_pos[1] + 25, 52);

    /* Light */
    cjk_label_make(card, icons[2], C_MUTED);
    lv_obj_set_pos(lv_obj_get_child(card, lv_obj_get_child_cnt(card) - 1), x_pos[2], 8);
    g_lbl_light = label_make(card, "--", C_TEXT, 24);
    lv_obj_set_pos(g_lbl_light, x_pos[2] + 25, 12);
    lv_obj_set_width(g_lbl_light, 100);
    label_make(card, units[2], C_MUTED, 12);
    lv_obj_set_pos(lv_obj_get_child(card, lv_obj_get_child_cnt(card) - 1), x_pos[2] + 25, 52);
}

/* ── Weather card ── */
static void weather_create(lv_obj_t *screen)
{
    lv_obj_t *card = card_create(screen, 400, 130);

    lv_obj_t *icon = label_make(card, "", C_MUTED, 32);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);  // placeholder
    lv_obj_set_pos(icon, 15, 12);

    g_lbl_w_city = cjk_label_make(card, "--", C_TEXT);
    lv_obj_set_pos(g_lbl_w_city, 55, 8);

    g_lbl_w_desc = cjk_label_make(card, "等待中...", C_MUTED);
    lv_obj_set_pos(g_lbl_w_desc, 55, 35);

    /* Detail row */
    lv_obj_t *d0 = cjk_label_make(card, "高:", C_MUTED);
    lv_obj_set_pos(d0, 15, 70);
    g_lbl_w_high = label_make(card, "--", C_RED, 16);
    lv_obj_set_pos(g_lbl_w_high, 35, 95);

    lv_obj_t *d1 = cjk_label_make(card, "低:", C_MUTED);
    lv_obj_set_pos(d1, 120, 70);
    g_lbl_w_low = label_make(card, "--", C_BLUE, 16);
    lv_obj_set_pos(g_lbl_w_low, 140, 95);

    lv_obj_t *d2 = cjk_label_make(card, "雨:", C_MUTED);
    lv_obj_set_pos(d2, 220, 70);
    g_lbl_w_rain = label_make(card, "--", C_MUTED, 16);
    lv_obj_set_pos(g_lbl_w_rain, 240, 95);

    /* Section label */
    lv_obj_t *lbl = cjk_label_make(card, "天气", C_MUTED);
    lv_obj_align(lbl, LV_ALIGN_RIGHT_MID, -20, 0);
}

/* ── Command buttons (side by side) ── */
static void commands_create(lv_obj_t *screen)
{
    lv_obj_t *card = card_create(screen, 540, 90);

    /* Open */
    g_btn_open = lv_btn_create(card);
    lv_obj_set_size(g_btn_open, 265, 60);
    lv_obj_set_pos(g_btn_open, 15, 15);
    lv_obj_set_style_radius(g_btn_open, 16, 0);
    lv_obj_set_style_bg_color(g_btn_open, C_GREEN, 0);
    lv_obj_set_style_shadow_width(g_btn_open, 6, 0);
    lv_obj_set_style_shadow_color(g_btn_open, C_GREEN, 0);
    lv_obj_set_style_shadow_opa(g_btn_open, 60, 0);
    lv_obj_add_event_cb(g_btn_open, on_btn_event, LV_EVENT_CLICKED, (void *)"open");
    lv_obj_t *l = cjk_label_make(g_btn_open, "打开", lv_color_hex(0xFFFFFF));
    lv_obj_center(l);

    /* Close */
    g_btn_close = lv_btn_create(card);
    lv_obj_set_size(g_btn_close, 265, 60);
    lv_obj_set_pos(g_btn_close, 295, 15);
    lv_obj_set_style_radius(g_btn_close, 16, 0);
    lv_obj_set_style_bg_color(g_btn_close, C_RED, 0);
    lv_obj_set_style_shadow_width(g_btn_close, 6, 0);
    lv_obj_set_style_shadow_color(g_btn_close, C_RED, 0);
    lv_obj_set_style_shadow_opa(g_btn_close, 60, 0);
    lv_obj_add_event_cb(g_btn_close, on_btn_event, LV_EVENT_CLICKED, (void *)"close");
    l = cjk_label_make(g_btn_close, "关闭", lv_color_hex(0xFFFFFF));
    lv_obj_center(l);
}

/* ── Mode info & schedule card ── */
static void mode_info_create(lv_obj_t *screen)
{
    lv_obj_t *card = card_create(screen, 640, 150);

    lv_obj_t *icon = label_make(card, LV_SYMBOL_HOME, C_ACCENT, 24);
    lv_obj_set_pos(icon, 15, 15);

    g_lbl_mode = cjk_label_make(card, "手动", C_ACCENT);
    lv_obj_set_pos(g_lbl_mode, 55, 8);

    g_lbl_mode_detail = cjk_label_make(card, "手动控制中", C_MUTED);
    lv_obj_set_pos(g_lbl_mode_detail, 55, 35);

    /* Schedule / prediction text */
    g_lbl_sched = cjk_label_make(card, "", C_MUTED);
    lv_obj_set_pos(g_lbl_sched, 15, 68);
    lv_obj_set_width(g_lbl_sched, 550);
    lv_obj_set_height(g_lbl_sched, 70);
    lv_label_set_long_mode(g_lbl_sched, LV_LABEL_LONG_WRAP);

    /* Adaptive mode: left column (predicted times) */
    g_lbl_adp_left = cjk_label_make(card, "", C_MUTED);
    lv_obj_set_pos(g_lbl_adp_left, 15, 68);
    lv_obj_set_width(g_lbl_adp_left, 270);
    lv_obj_set_height(g_lbl_adp_left, 70);
    lv_label_set_long_mode(g_lbl_adp_left, LV_LABEL_LONG_WRAP);

    /* Adaptive mode: right column (recent operations) */
    g_lbl_adp_right = cjk_label_make(card, "", C_MUTED);
    lv_obj_set_pos(g_lbl_adp_right, 300, 68);
    lv_obj_set_width(g_lbl_adp_right, 265);
    lv_obj_set_height(g_lbl_adp_right, 70);
    lv_label_set_long_mode(g_lbl_adp_right, LV_LABEL_LONG_WRAP);
}

/* ── Timer schedule display (replaces weather icon area) ── */
static lv_obj_t *g_lbl_timer_sched = NULL;
static lv_obj_t *g_btn_mode_manual   = NULL;
static lv_obj_t *g_btn_mode_env      = NULL;
static lv_obj_t *g_btn_mode_adaptive = NULL;

static void timer_schedule_create(lv_obj_t *screen)
{
    lv_obj_t *card = card_create(screen, 800, 100);

    lv_obj_t *icon = label_make(card, LV_SYMBOL_LIST, C_ACCENT, 24);
    lv_obj_set_pos(icon, 15, 15);

    lv_obj_t *title = cjk_label_make(card, "定时", C_ACCENT);
    lv_obj_set_pos(title, 50, 12);

    /* Delete button: removes last one-shot timer */
    lv_obj_t *btn_del = lv_btn_create(card);
    lv_obj_set_size(btn_del, 30, 30);
    lv_obj_set_pos(btn_del, 120, 8);
    lv_obj_set_style_radius(btn_del, 15, 0);
    lv_obj_set_style_bg_color(btn_del, C_RED, 0);
    lv_obj_set_style_shadow_width(btn_del, 0, 0);
    lv_obj_t *lbl_del = lv_label_create(btn_del);
    lv_label_set_text(lbl_del, LV_SYMBOL_CLOSE);
    lv_obj_center(lbl_del);
    lv_obj_add_event_cb(btn_del, on_btn_event, LV_EVENT_CLICKED, (void *)"timer_del_last");

    g_lbl_timer_sched = cjk_label_make(card, "暂无定时", C_MUTED);
    lv_obj_set_pos(g_lbl_timer_sched, 15, 48);
    lv_obj_set_width(g_lbl_timer_sched, 750);
    lv_obj_set_style_text_font(g_lbl_timer_sched, font_cjk(), 0);
}

/* ── Bottom mode control row ── */
static void mode_control_create(lv_obj_t *screen)
{
    lv_obj_t *card = card_create(screen, 910, 70);

    lv_obj_t *lbl = cjk_label_make(card, "控制", C_MUTED);
    lv_obj_set_pos(lbl, 15, 12);

    /* Manual button */
    g_btn_mode_manual = lv_btn_create(card);
    lv_obj_set_size(g_btn_mode_manual, 110, 40);
    lv_obj_set_pos(g_btn_mode_manual, 80, 15);
    lv_obj_set_style_radius(g_btn_mode_manual, 20, 0);
    lv_obj_set_style_bg_color(g_btn_mode_manual, C_ORANGE, 0);
    lv_obj_add_event_cb(g_btn_mode_manual, on_btn_event, LV_EVENT_CLICKED, (void *)"mode_manual");
    lv_obj_t *bl = cjk_label_make(g_btn_mode_manual, "手动", lv_color_hex(0xFFFFFF));
    lv_obj_center(bl);

    /* Environment button */
    g_btn_mode_env = lv_btn_create(card);
    lv_obj_set_size(g_btn_mode_env, 110, 40);
    lv_obj_set_pos(g_btn_mode_env, 210, 15);
    lv_obj_set_style_radius(g_btn_mode_env, 20, 0);
    lv_obj_set_style_bg_color(g_btn_mode_env, lv_color_hex(0xE8E8F0), 0);
    lv_obj_add_event_cb(g_btn_mode_env, on_btn_event, LV_EVENT_CLICKED, (void *)"mode_env");
    bl = cjk_label_make(g_btn_mode_env, "环境", C_TEXT);
    lv_obj_center(bl);

    /* Adaptive button */
    g_btn_mode_adaptive = lv_btn_create(card);
    lv_obj_set_size(g_btn_mode_adaptive, 110, 40);
    lv_obj_set_pos(g_btn_mode_adaptive, 340, 15);
    lv_obj_set_style_radius(g_btn_mode_adaptive, 20, 0);
    lv_obj_set_style_bg_color(g_btn_mode_adaptive, lv_color_hex(0xE8E8F0), 0);
    lv_obj_add_event_cb(g_btn_mode_adaptive, on_btn_event, LV_EVENT_CLICKED, (void *)"mode_adaptive");
    bl = cjk_label_make(g_btn_mode_adaptive, "自适应", C_TEXT);
    lv_obj_center(bl);
}

void ui_init(void)
{
    ESP_LOGI(TAG, "ui_init: start");
    lv_obj_t *scr = lv_screen_active();
    ESP_LOGI(TAG, "ui_init: screen active");
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    ESP_LOGI(TAG, "ui_init: bg set");

    /* Disable default scroll */
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ESP_LOGI(TAG, "ui_init: top_bar");
    top_bar_create(scr);
    ESP_LOGI(TAG, "ui_init: mode_toggle");
    mode_toggle_create(scr);
    ESP_LOGI(TAG, "ui_init: servo_buttons");
    servo_buttons_create(scr);
    ESP_LOGI(TAG, "ui_init: sensors");
    sensors_create(scr);
    ESP_LOGI(TAG, "ui_init: weather");
    weather_create(scr);
    ESP_LOGI(TAG, "ui_init: commands");
    commands_create(scr);
    ESP_LOGI(TAG, "ui_init: mode_info");
    mode_info_create(scr);
    ESP_LOGI(TAG, "ui_init: timer_schedule");
    timer_schedule_create(scr);
    ESP_LOGI(TAG, "ui_init: mode_control");
    mode_control_create(scr);

    ESP_LOGI(TAG, "UI layout ready (600x1024)");
}

/* ── Update functions ── */
void ui_update_sensor(float temp, float humidity, int light)
{
    char buf[16];
    if (g_lbl_temp)  { snprintf(buf, sizeof(buf), "%.1f", temp); lv_label_set_text(g_lbl_temp, buf); }
    if (g_lbl_humi)  { snprintf(buf, sizeof(buf), "%.0f", humidity); lv_label_set_text(g_lbl_humi, buf); }
    if (g_lbl_light) { snprintf(buf, sizeof(buf), "%d", light); lv_label_set_text(g_lbl_light, buf); }
}

void ui_update_weather(const char *city, const char *weather, int high, int low, int rain_pct)
{
    if (g_lbl_w_city)  lv_label_set_text(g_lbl_w_city, city);
    if (g_lbl_w_desc)  lv_label_set_text_fmt(g_lbl_w_desc, "%s", weather);
    if (g_lbl_w_high)  lv_label_set_text_fmt(g_lbl_w_high, "%d" "\xc2\xb0", high);
    if (g_lbl_w_low)   lv_label_set_text_fmt(g_lbl_w_low, "%d" "\xc2\xb0", low);
    if (g_lbl_w_rain)  lv_label_set_text_fmt(g_lbl_w_rain, "%d%%", rain_pct);
}

void ui_update_mode(bool is_auto)
{
    if (g_btn_manual) {
        lv_obj_set_style_bg_color(g_btn_manual, is_auto ? lv_color_hex(0xE8E8F0) : C_ORANGE, 0);
    }
    if (g_btn_auto) {
        lv_obj_set_style_bg_color(g_btn_auto, is_auto ? C_GREEN : lv_color_hex(0xE8E8F0), 0);
    }
}

void ui_update_connection(bool connected)
{
    if (g_led_conn) {
        lv_led_set_color(g_led_conn, connected ? C_GREEN : C_RED);
        lv_led_on(g_led_conn);
    }
    if (g_lbl_conn) {
        lv_label_set_text(g_lbl_conn, connected ? "已连接" : "已断开");
    }
}

void ui_update_mode_info(const char *mode_name, const char *detail, const char *schedule_text)
{
    if (g_lbl_mode)        lv_label_set_text(g_lbl_mode, mode_name);
    if (g_lbl_mode_detail) lv_label_set_text(g_lbl_mode_detail, detail);
    if (g_lbl_sched)       lv_label_set_text(g_lbl_sched, schedule_text ? schedule_text : "");
    if (g_lbl_adp_left)    lv_label_set_text(g_lbl_adp_left, "");
    if (g_lbl_adp_right)   lv_label_set_text(g_lbl_adp_right, "");
}

void ui_update_adaptive_info(const char *detail,
                              const char *pred_left,
                              const char *recent_right)
{
    if (g_lbl_mode)        lv_label_set_text(g_lbl_mode, "自适应");
    if (g_lbl_mode_detail) lv_label_set_text(g_lbl_mode_detail, detail ? detail : "");
    if (g_lbl_sched)       lv_label_set_text(g_lbl_sched, "");
    if (g_lbl_adp_left)    lv_label_set_text(g_lbl_adp_left, pred_left ? pred_left : "");
    if (g_lbl_adp_right)   lv_label_set_text(g_lbl_adp_right, recent_right ? recent_right : "");
}

void ui_update_timer_schedule(const char *text)
{
    if (!g_lbl_timer_sched) return;
    if (!text || !text[0]) {
        lv_label_set_text(g_lbl_timer_sched, "暂无定时");
        return;
    }
    lv_label_set_text(g_lbl_timer_sched, text);
}

void ui_update_mode_highlight(const char *mode_name)
{
    if (!g_btn_mode_manual || !g_btn_mode_env || !g_btn_mode_adaptive) return;

    lv_color_t color_manual   = lv_color_hex(0xE8E8F0);
    lv_color_t color_env      = lv_color_hex(0xE8E8F0);
    lv_color_t color_adaptive = lv_color_hex(0xE8E8F0);

    if (strcmp(mode_name, "manual") == 0)
        color_manual = C_ORANGE;
    else if (strcmp(mode_name, "env") == 0)
        color_env = C_GREEN;
    else if (strcmp(mode_name, "adaptive") == 0)
        color_adaptive = C_ACCENT;

    lv_obj_set_style_bg_color(g_btn_mode_manual,   color_manual,   0);
    lv_obj_set_style_bg_color(g_btn_mode_env,      color_env,      0);
    lv_obj_set_style_bg_color(g_btn_mode_adaptive, color_adaptive, 0);
}
