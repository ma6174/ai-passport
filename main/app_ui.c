// main/app_ui.c
// 状态页:黑底、大字阶段、三行设备状态、一行按键提示。
#include "app_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp_battery.h"
#include "esp_log.h"
#include "esp_system.h"
#include "bsp_display.h"
#include "lvgl.h"

static const char *TAG = "app_ui";

static lv_obj_t *s_phase;
static lv_obj_t *s_action;
static lv_obj_t *s_place;
static lv_obj_t *s_net;
static lv_obj_t *s_recent;
static lv_obj_t *s_hint;

// 16 px 中文点阵(GB2312 一级字 + ASCII + 常用标点),由 lv_font_conv 生成。
// 默认的 Montserrat 没有中文字形,转写/地名这类内容会显示成方块。
extern const lv_font_t font_cjk16;

// 与上次写入的文本不同才写屏。注意不能用 lv_label_get_text 比较:
// LV_LABEL_LONG_DOT 会就地改写 label 文本(末尾补省略号),永远不相等。
static void set_text_if_changed(lv_obj_t **label, char *cache, size_t cache_len,
                                const char *text) {
    if (strncmp(cache, text, cache_len - 1) == 0) {
        return;
    }
    snprintf(cache, cache_len, "%s", text);
    lv_label_set_text(*label, cache);
}

static char s_cache_phase[32];
static char s_cache_action[128];
static char s_cache_place[48];
static char s_cache_net[64];
static char s_cache_recent[160];

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int y) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_pos(label, 14, y);
    lv_label_set_text(label, "");
    return label;
}

esp_err_t app_ui_init(void) {
    ESP_LOGI(TAG, "init: 开始建状态页 (heap=%u)", (unsigned)esp_get_free_heap_size());
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "init: 拿不到 LVGL 锁,屏幕不会更新");
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b0f14), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x6ee7b7), 0);
    lv_obj_set_pos(title, 14, 10);
    lv_label_set_text(title, "SONAR / AI PASSPORT");

    s_phase = make_label(scr, &lv_font_montserrat_20, lv_color_hex(0xffffff), 34);
    // 240x320 屏:每块固定高度 + LONG_DOT,中文折行时截断,不会溢出屏外或压到下一块。
    s_action = make_label(scr, &font_cjk16, lv_color_hex(0xfbbf24), 62);
    lv_obj_set_size(s_action, 212, 44);
    lv_label_set_long_mode(s_action, LV_LABEL_LONG_DOT);

    s_place = make_label(scr, &font_cjk16, lv_color_hex(0x93c5fd), 108);
    lv_obj_set_size(s_place, 212, 20);
    lv_label_set_long_mode(s_place, LV_LABEL_LONG_DOT);

    s_net = make_label(scr, &lv_font_montserrat_14, lv_color_hex(0xcbd5e1), 130);
    lv_obj_set_size(s_net, 212, 18);
    lv_label_set_long_mode(s_net, LV_LABEL_LONG_DOT);

    s_recent = make_label(scr, &font_cjk16, lv_color_hex(0xcbd5e1), 152);
    lv_obj_set_size(s_recent, 212, 92);
    lv_label_set_long_mode(s_recent, LV_LABEL_LONG_DOT);

    s_hint = make_label(scr, &font_cjk16, lv_color_hex(0x64748b), 250);
    lv_obj_set_size(s_hint, 212, 60);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_hint, "HOLD OK: talk   CLICK OK: stop/confirm\nCLICK UP: place   CLICK DOWN: quiet");

    {
        lv_font_glyph_dsc_t dsc;
        bool a = lv_font_get_glyph_dsc(&font_cjk16, &dsc, 'A', 0);
        lv_font_glyph_dsc_t dsc2;
        bool cjk = lv_font_get_glyph_dsc(&font_cjk16, &dsc2, 0x6A21, 0);
        ESP_LOGI(TAG, "字形查表 ASCII=%d(%dx%d) 模=%d(%dx%d) box_w=%d bpp=%d",
                 (int)a, (int)dsc.box_w, (int)dsc.box_h, (int)cjk, (int)dsc2.box_w,
                 (int)dsc2.box_h, (int)font_cjk16.line_height, (int)font_cjk16.subpx);
    }

    // 面板上电后是白底;必须立刻画出第一帧,否则没内容时屏幕会一直白着。
    lv_obj_invalidate(scr);
    lv_refr_now(NULL);
    bsp_lvgl_unlock();
    // 常亮:没有背光就看不出设备已经起来,联调时非常难判断状态。
    bsp_display_backlight(90);
    ESP_LOGI(TAG, "init: 状态页就绪");
    return ESP_OK;
}

void app_ui_set_backlight(bool on) {
    // 屏幕常亮:黑屏会让"有没有开机"无从判断。on=false 仅用于彻底熄屏的调试场景。
    bsp_display_backlight(on ? 90 : 0);
}

void app_ui_update(const app_view_t *view) {
    if (view == NULL || s_phase == NULL) {
        return;
    }
    if (!bsp_lvgl_lock(200)) {
        static int warned;
        if (warned++ < 3) {
            ESP_LOGW(TAG, "更新拿不到 LVGL 锁");
        }
        return;
    }
    char buf[96];
    char place[96];
    char net[96];
    char recent[128];
    snprintf(place, sizeof(place), "PLACE %s", view->place);
    // 14 px 下 212 px 只放得下约 30 个字符,用缩写而不是让 DOT 截断。
    snprintf(net, sizeof(net), "W:%s  S:%s  L:%s  %d%%", view->wifi_ok ? "OK" : "--",
             view->sonar_ok ? "OK" : "--", view->linx_ok ? "OK" : "--", view->battery);
    snprintf(buf, sizeof(buf), "PENDING %d  %s", view->pending, view->quiet ? "QUIET" : "LIVE");
    snprintf(recent, sizeof(recent), "%s", view->detail[0] != '\0' ? view->detail : buf);
    set_text_if_changed(&s_phase, s_cache_phase, sizeof(s_cache_phase), view->line1);
    set_text_if_changed(&s_action, s_cache_action, sizeof(s_cache_action), view->line2);
    set_text_if_changed(&s_place, s_cache_place, sizeof(s_cache_place), place);
    set_text_if_changed(&s_net, s_cache_net, sizeof(s_cache_net), net);
    set_text_if_changed(&s_recent, s_cache_recent, sizeof(s_cache_recent), recent);
    bsp_lvgl_unlock();
}

void app_ui_show_activation(const char *code) {
    if (code == NULL || s_phase == NULL) {
        return;
    }
    if (!bsp_lvgl_lock(200)) {
        return;
    }
    lv_label_set_text(s_phase, "LINX BIND");
    char buf[64];
    snprintf(buf, sizeof(buf), "CODE %s\nxrobo.qiniu.com", code);
    lv_label_set_text(s_action, buf);
    bsp_lvgl_unlock();
}
