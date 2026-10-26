// main/app_ui.h
// 设备状态页(重新设计,不使用基线 demo 菜单/页面外壳)。
//
// 显示:阶段、当前地点、网络/Sonar/灵矽状态、待处理条数、电量、按键提示。
// 只做状态可视化,不做业务控制——所有动作都由按键与语音触发。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    APP_PHASE_BOOT = 0,
    APP_PHASE_CONFIG,
    APP_PHASE_NET,
    APP_PHASE_SYNC,
    APP_PHASE_IDLE,
    APP_PHASE_RECORD,
    APP_PHASE_THINK,
    APP_PHASE_CONFIRM,
    APP_PHASE_PLAY,
    APP_PHASE_ERROR,
} app_phase_t;

typedef struct {
    app_phase_t phase;
    char line1[40];  // 阶段标题(ASCII,避免缺字)
    char line2[64];  // 当前动作/提示
    char place[40];  // 演示地点
    char detail[160]; // 最近事件(常驻显示,按字符截断)
    bool quiet;
    bool sonar_ok;
    bool linx_ok;
    bool wifi_ok;
    int pending;
    int battery;
} app_view_t;

esp_err_t app_ui_init(void);

// 有内容时点亮背光,没有可信内容时保持黑屏(不亮屏)。
void app_ui_set_backlight(bool on);

void app_ui_update(const app_view_t *view);

// 未绑定灵矽时把激活码显示出来,提示去灵矽智控台绑定。
void app_ui_show_activation(const char *code);
