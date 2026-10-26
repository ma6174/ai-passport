// main/sonar_net.c
// Wi-Fi STA 与 SNTP。
#include "sonar_net.h"

#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include <sys/time.h>

#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "sonar_net";

#define BIT_CONNECTED BIT0
#define BIT_FAIL BIT1

static EventGroupHandle_t s_events;
static int64_t s_offset = 0;
static bool s_started = false;
static char s_ssid[SONAR_WIFI_SSID_MAX];
static char s_pass[SONAR_WIFI_PASS_MAX];

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // 不要在事件任务里等待/重连:阻塞这里会拖住整个默认事件循环。
        // 由应用任务按退避节奏调用 sonar_net_reconnect()。
        ESP_LOGW(TAG, "Wi-Fi 断开,等待应用任务重连");
        if (s_events != NULL) {
            xEventGroupClearBits(s_events, BIT_CONNECTED);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "已获得 IP");
        if (s_events != NULL) {
            xEventGroupSetBits(s_events, BIT_CONNECTED);
        }
    }
}

esp_err_t sonar_net_start(const sonar_config_t *cfg) {
    if (cfg == NULL || !sonar_config_has_wifi(cfg)) {
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(s_ssid, sizeof(s_ssid), "%s", cfg->wifi_ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", cfg->wifi_pass);
    if (s_started) {
        return ESP_OK;
    }
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    // C3 无 PSRAM:收包缓冲压到够用即可,把内部 RAM 留给 TLS 与音频。
    init_cfg.static_rx_buf_num = 6;
    init_cfg.dynamic_rx_buf_num = 8;
    init_cfg.tx_buf_type = 1;
    init_cfg.static_tx_buf_num = 6;
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    wifi_config_t wifi_cfg = {0};
    memcpy(wifi_cfg.sta.ssid, s_ssid, strnlen(s_ssid, sizeof(wifi_cfg.sta.ssid)));
    memcpy(wifi_cfg.sta.password, s_pass, strnlen(s_pass, sizeof(wifi_cfg.sta.password) - 1));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    s_started = true;
    return ESP_OK;
}

void sonar_net_reconnect(void) {
    if (!s_started) {
        return;
    }
    esp_wifi_disconnect();
    esp_wifi_connect();
}

bool sonar_net_connected(void) {
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & BIT_CONNECTED) != 0;
}

int64_t sonar_net_now(void) {
    time_t now = time(NULL);
    return (int64_t)now + s_offset;
}

esp_err_t sonar_net_wait_time(uint32_t timeout_ms) {
    if (!sonar_net_connected()) {
        EventBits_t bits =
            xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
        if ((bits & BIT_CONNECTED) == 0) {
            return ESP_ERR_TIMEOUT;
        }
    }
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    if (esp_netif_sntp_init(&sntp_cfg) != ESP_OK) {
        // 已经初始化过就只需等待同步。
    }
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP 未在 %u ms 内完成: %s", (unsigned)timeout_ms, esp_err_to_name(err));
        return err;
    }
    time_t now = time(NULL);
    if (now < 1704067200) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "校时完成,UTC epoch=%lld", (long long)now);
    return ESP_OK;
}

void sonar_net_apply_server_time(int64_t server_epoch) {
    if (server_epoch < 1704067200) {
        return;
    }
    int64_t local = (int64_t)time(NULL);
    int64_t diff = server_epoch - (local + s_offset);
    // 本地钟明显不可信或偏差超过 60 秒时才采纳,避免每次请求都抖动。
    if (local < 1704067200 || diff > 60 || diff < -60) {
        if (local < 1704067200) {
            // 系统钟还停在 1970(SNTP 没通)时,只记偏移是不够的:mbedtls 校验证书用的是
            // time(NULL),证书 notBefore 是绝对时间,会一直报 "certificate not trusted"。
            // 这里直接写 RTC,之后所有 TLS 握手才有可信时间。
            struct timeval tv = {.tv_sec = (time_t)(local + diff), .tv_usec = 0};
            if (settimeofday(&tv, NULL) == 0) {
                s_offset = 0;
                ESP_LOGW(TAG, "系统钟未同步,按服务端时间设置 %lld", (long long)server_epoch);
            } else {
                s_offset += diff;
                ESP_LOGW(TAG, "设置系统时间失败,改用偏移 %lld 秒", (long long)diff);
            }
        } else {
            s_offset += diff;
            ESP_LOGW(TAG, "用服务端时间校准偏移 %lld 秒", (long long)diff);
        }
    }
}
