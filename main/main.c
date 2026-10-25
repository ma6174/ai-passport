// main/main.c
// 声呐 Sonar 车用/现场语音客户端入口。
//
// 板级初始化沿用 BSP(I2C/显示/LVGL/音频/电量),业务在 app_state:
// 轮询 Sonar 邀请与原声、按住 OK 说话、灵矽转写、发布与自动播放。
#include "app_state.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "main";

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新初始化,擦除后重建");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // ES8311 与 CW2017 共用 I2C0;BSP 内部幂等。
    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(bsp_display_init());
    if (bsp_lvgl_init() == NULL) {
        ESP_LOGE(TAG, "LVGL 初始化失败,状态页不可用");
    } else {
        // 先黑屏:等状态页画出第一帧、且确实有内容时再由应用任务点亮背光。
        bsp_display_backlight(0);
    }
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用,状态页显示 -1");
    }

    ESP_ERROR_CHECK(app_state_start());
}
