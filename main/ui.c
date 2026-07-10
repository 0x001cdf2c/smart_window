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
static lv_obj_t *g_lbl_ai      = NULL;

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
    /* Only montserrat_14 is compiled in; enable others via Kconfig if needed */
    (void)size;
    return &lv_font_montserrat_14;
}

static lv_obj_t *label_make(lv_obj_t *parent, const char *text, lv_color_t color, int size)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, font_for_size(size), 0);
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
    lv_obj_t *title = label_make(bar, "Smart Blinds", C_TEXT, 24);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 20, 0);

    /* Connection LED + text */
    g_led_conn = lv_led_create(bar);
    lv_obj_set_size(g_led_conn, 10, 10);
    lv_obj_align(g_led_conn, LV_ALIGN_RIGHT_MID, -70, 0);
    lv_led_set_color(g_led_conn, C_GREEN);

    g_lbl_conn = label_make(bar, "Online", C_MUTED, 12);
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

    lv_obj_t *lbl = label_make(row, "Mode", C_MUTED, 14);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 10, 0);

    g_btn_manual = lv_btn_create(row);
    lv_obj_set_size(g_btn_manual, 110, 36);
    lv_obj_set_style_radius(g_btn_manual, 20, 0);
    lv_obj_set_style_bg_color(g_btn_manual, C_ORANGE, 0);
    lv_obj_align(g_btn_manual, LV_ALIGN_RIGHT_MID, -100, 0);
    lv_obj_add_event_cb(g_btn_manual, on_btn_event, LV_EVENT_CLICKED, (void *)"manual");
    lv_obj_t *bl = label_make(g_btn_manual, "MANUAL", lv_color_hex(0xFFFFFF), 14);
    lv_obj_center(bl);

    g_btn_auto = lv_btn_create(row);
    lv_obj_set_size(g_btn_auto, 80, 36);
    lv_obj_set_style_radius(g_btn_auto, 20, 0);
    lv_obj_set_style_bg_color(g_btn_auto, lv_color_hex(0xD5F5E3), 0);
    lv_obj_set_style_shadow_width(g_btn_auto, 0, 0);
    lv_obj_align(g_btn_auto, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_event_cb(g_btn_auto, on_btn_event, LV_EVENT_CLICKED, (void *)"auto");
    bl = label_make(g_btn_auto, "AUTO", C_GREEN, 14);
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

    lv_obj_t *l = lv_label_create(g_btn_cw);
    lv_label_set_text(l, "CW");
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
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

    l = lv_label_create(g_btn_ccw);
    lv_label_set_text(l, "CCW");
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
}

/* ── Sensor rows ── */
static lv_obj_t *sensor_row_create(lv_obj_t *parent, int y, const char *icon, const char *unit)
{
    lv_obj_t *card = card_create(parent, y, 80);

    /* Icon */
    lv_obj_t *ic = label_make(card, icon, C_MUTED, 28);
    lv_obj_set_pos(ic, 15, 20);

    /* Value (dynamic) */
    lv_obj_t *val = label_make(card, "--", C_TEXT, 28);
    lv_obj_set_pos(val, 55, 12);
    lv_obj_set_width(val, 120);  /* ensure enough width for values like "27.5" */

    /* Unit */
    lv_obj_t *u = label_make(card, unit, C_MUTED, 14);
    lv_obj_set_pos(u, 55, 50);

    /* Label */
    lv_obj_t *name = label_make(card, "", C_MUTED, 14);
    lv_obj_align(name, LV_ALIGN_RIGHT_MID, -20, 0);

    return val;
}

static void sensors_create(lv_obj_t *screen)
{
    lv_obj_t *card;

    g_lbl_temp  = sensor_row_create(screen, 310, "T", (const char *)"\xc2\xb0" "C");
    card = lv_obj_get_parent(g_lbl_temp);
    lv_label_set_text(lv_obj_get_child(card, -1), "Temp");

    g_lbl_humi  = sensor_row_create(screen, 400, "H", "%");
    card = lv_obj_get_parent(g_lbl_humi);
    lv_label_set_text(lv_obj_get_child(card, -1), "Humidity");

    g_lbl_light = sensor_row_create(screen, 490, "L", "lux");
    card = lv_obj_get_parent(g_lbl_light);
    lv_label_set_text(lv_obj_get_child(card, -1), "Light");
}

/* ── Weather card ── */
static void weather_create(lv_obj_t *screen)
{
    lv_obj_t *card = card_create(screen, 580, 130);

    lv_obj_t *icon = label_make(card, "", C_MUTED, 32);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);  // placeholder
    lv_obj_set_pos(icon, 15, 12);

    g_lbl_w_city = label_make(card, "--", C_TEXT, 18);
    lv_obj_set_pos(g_lbl_w_city, 55, 8);

    g_lbl_w_desc = label_make(card, "Waiting...", C_MUTED, 12);
    lv_obj_set_pos(g_lbl_w_desc, 55, 35);

    /* Detail row */
    lv_obj_t *d0 = label_make(card, "H:", C_MUTED, 12);
    lv_obj_set_pos(d0, 15, 70);
    g_lbl_w_high = label_make(card, "--", C_RED, 16);
    lv_obj_set_pos(g_lbl_w_high, 35, 95);

    lv_obj_t *d1 = label_make(card, "L:", C_MUTED, 12);
    lv_obj_set_pos(d1, 120, 70);
    g_lbl_w_low = label_make(card, "--", C_BLUE, 16);
    lv_obj_set_pos(g_lbl_w_low, 140, 95);

    lv_obj_t *d2 = label_make(card, "R:", C_MUTED, 12);
    lv_obj_set_pos(d2, 220, 70);
    g_lbl_w_rain = label_make(card, "--", C_MUTED, 16);
    lv_obj_set_pos(g_lbl_w_rain, 240, 95);

    /* Section label */
    lv_obj_t *lbl = label_make(card, "WEATHER", C_MUTED, 12);
    lv_obj_align(lbl, LV_ALIGN_RIGHT_MID, -20, 0);
}

/* ── AI suggestion card ── */
static void ai_create(lv_obj_t *screen)
{
    lv_obj_t *card = card_create(screen, 720, 130);

    lv_obj_t *icon = label_make(card, LV_SYMBOL_SETTINGS, C_ACCENT, 24);
    lv_obj_set_pos(icon, 15, 15);

    lv_obj_t *lbl = label_make(card, "AI SUGGESTION", C_MUTED, 12);
    lv_obj_set_pos(lbl, 55, 10);

    g_lbl_ai = label_make(card, "Tap button for advice", C_TEXT, 14);
    lv_obj_set_pos(g_lbl_ai, 55, 48);
    lv_obj_set_width(g_lbl_ai, 490);
    lv_label_set_long_mode(g_lbl_ai, LV_LABEL_LONG_WRAP);
}

/* ── Command buttons (side by side) ── */
static void commands_create(lv_obj_t *screen)
{
    lv_obj_t *card = card_create(screen, 860, 90);

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
    lv_obj_t *l = label_make(g_btn_open, "OPEN", lv_color_hex(0xFFFFFF), 24);
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
    l = label_make(g_btn_close, "CLOSE", lv_color_hex(0xFFFFFF), 24);
    lv_obj_center(l);
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
    ESP_LOGI(TAG, "ui_init: ai");
    ai_create(scr);
    ESP_LOGI(TAG, "ui_init: commands");
    commands_create(scr);

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

void ui_update_suggestion(const char *text)
{
    if (g_lbl_ai) lv_label_set_text(g_lbl_ai, text);
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
        lv_label_set_text(g_lbl_conn, connected ? "Online" : "Offline");
    }
}
