// main/sonar_config.c
// 配置读写与演示地点表。
#include "sonar_config.h"
#include "sonar_text.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

#if __has_include("local_secrets.h")
#include "local_secrets.h"
#endif

static const char *TAG = "sonar_cfg";
static const char *NS = "sonar";

// 三个演示点位与高德在 2026-10-24 返回的 GCJ-02 锚点一致(见 Sonar 开发文档
// docs/development/demo-audio.md)。坐标只在拿不到 /v1/places 时兜底上报。
static const sonar_place_preset_t PRESETS[] = {
    // 查询词带上城市/区/门牌,否则高德会命中同名异地 POI(模力社区曾命中北京的同名邮局)。
    // 别名用 '|' 分隔。ASR 对「模力」的写法极不稳定,实测出现过
    // 魔力/摩力/膜力/文丽/我去/莫力,再加一个「社区」这种独有词兜底(三个点位里只有它带社区)。
    {"模力社区附近", "上海市浦东新区模力社区",
     "模力社区|魔力社区|摩力社区|膜力社区|文丽社区|我去社区|莫力社区|磨力社区|茉莉社区|"
     "木里社区|社区|模力|魔力|茉莉",
     "模力社区", 31.184698, 121.617324},
    {"人工智能岛19号楼附近", "上海市浦东新区川和路55弄19号",
     "人工智能岛|智能岛|19号楼|十九号楼|人工岛|岛|号楼", "人工智能岛", 31.184820,
     121.609497},
    {"中环线金科路路口", "上海市浦东新区金科路华夏中路",
     "金科路|华夏中路|中环线|金科|华夏路|路口", "交叉口", 31.173564, 121.602421},
};
#define PRESET_COUNT (sizeof(PRESETS) / sizeof(PRESETS[0]))

// ASR 对地名的写法极不稳定(模力→魔力/茉莉/我去…)。发出去之前把**已知别名**
// 换成规范名,服务端地理编码才不会跑到别的城市(实测「茉莉社区」被解成
// 保利茉莉公馆/百旺·茉莉园,白多一轮反问)。
// 只做"整词命中 + 最左最长"替换,最多一处;返回替换次数。
size_t sonar_place_normalize(const char *in, char *out, size_t out_len) {
    sonar_text_place_t places[PRESET_COUNT];
    for (size_t i = 0; i < PRESET_COUNT; i++) {
        places[i].label = PRESETS[i].label;
        places[i].alias = PRESETS[i].alias;
    }
    return sonar_text_normalize_place(places, PRESET_COUNT, in, out, out_len);
}

const sonar_place_preset_t *sonar_place_presets(size_t *count) {
    if (count != NULL) {
        *count = PRESET_COUNT;
    }
    return PRESETS;
}

// 取 "host[:port]/path" 部分,忽略 scheme:用来判断两条 URL 是否指向同一个服务。
static const char *url_after_scheme(const char *url) {
    const char *p = strstr(url, "://");
    return p != NULL ? p + 3 : url;
}

static bool get_str(nvs_handle_t h, const char *key, char *out, size_t out_len) {
    size_t len = out_len;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    out[out_len - 1] = '\0';
    return out[0] != '\0';
}

static esp_err_t set_str(nvs_handle_t h, const char *key, const char *value) {
    return nvs_set_str(h, key, value == NULL ? "" : value);
}

// UUID v4:随机数 + 版本/变体位。持久化后重启不重建,避免平台侧把同一台设备
// 当成新设备重新激活。
void sonar_config_new_uuid(char out[SONAR_CLIENT_ID_LEN + 1]) {
    uint8_t b[16];
    for (size_t i = 0; i < sizeof(b); i += 4) {
        uint32_t r = esp_random();
        memcpy(b + i, &r, 4);
    }
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    snprintf(out, SONAR_CLIENT_ID_LEN + 1,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0],
             b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
             b[14], b[15]);
}

esp_err_t sonar_config_load(sonar_config_t *out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;  // 空配置,由 seed 补
    }
    get_str(h, "wifi_ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
    get_str(h, "wifi_pass", out->wifi_pass, sizeof(out->wifi_pass));
    get_str(h, "base_url", out->base_url, sizeof(out->base_url));
    get_str(h, "token", out->token, sizeof(out->token));
    get_str(h, "client_id", out->client_id, sizeof(out->client_id));
    get_str(h, "linx_url", out->linx_url, sizeof(out->linx_url));
    get_str(h, "linx_token", out->linx_token, sizeof(out->linx_token));
    get_str(h, "remote_place", out->remote_place_id, sizeof(out->remote_place_id));
    uint8_t flag = 0;
    if (nvs_get_u8(h, "auto_pub", &flag) == ESP_OK) {
        out->auto_publish = flag != 0;
    }
    if (nvs_get_u8(h, "place_idx", &flag) == ESP_OK && flag < PRESET_COUNT) {
        out->place_idx = flag;
    }
    out->presence_mode = 1;
    if (nvs_get_u8(h, "presence_mode", &flag) == ESP_OK) {
        out->presence_mode = flag == 0 ? 0 : 1;
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t sonar_config_save(const sonar_config_t *cfg) {
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    set_str(h, "wifi_ssid", cfg->wifi_ssid);
    set_str(h, "wifi_pass", cfg->wifi_pass);
    set_str(h, "base_url", cfg->base_url);
    set_str(h, "token", cfg->token);
    set_str(h, "client_id", cfg->client_id);
    set_str(h, "linx_url", cfg->linx_url);
    set_str(h, "linx_token", cfg->linx_token);
    set_str(h, "remote_place", cfg->remote_place_id);
    nvs_set_u8(h, "auto_pub", cfg->auto_publish ? 1 : 0);
    nvs_set_u8(h, "place_idx", cfg->place_idx);
    nvs_set_u8(h, "presence_mode", cfg->presence_mode);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t sonar_config_seed(sonar_config_t *cfg) {
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bool changed = false;
    if (cfg->client_id[0] == '\0') {
        sonar_config_new_uuid(cfg->client_id);
        changed = true;
    }
#ifdef SONAR_SEED_WIFI_SSID
    // SSID 也跟随编译期配置:换网络时刷固件即生效(密码同理,见下)。
    if (strcmp(cfg->wifi_ssid, SONAR_SEED_WIFI_SSID) != 0) {
        ESP_LOGW(TAG, "Wi-Fi 改用编译期配置: %s", SONAR_SEED_WIFI_SSID);
        snprintf(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "%s", SONAR_SEED_WIFI_SSID);
        changed = true;
    }
#endif
#ifdef SONAR_SEED_WIFI_PASS
    // Wi-Fi 密码同样跟随编译期配置:换了网络刷固件就该生效,不能被 NVS 里的旧值挡住。
    if (strcmp(cfg->wifi_pass, SONAR_SEED_WIFI_PASS) != 0) {
        snprintf(cfg->wifi_pass, sizeof(cfg->wifi_pass), "%s", SONAR_SEED_WIFI_PASS);
        changed = true;
    }
#endif
#ifdef SONAR_SEED_BASE_URL
    // 服务端同一台机器换了协议(http/https)时,已落盘的旧 URL 要跟着编译期配置走,
    // 否则 NVS 里的旧 scheme 会一直生效,刷固件也改不过来。
    if (cfg->base_url[0] == '\0' ||
        (strcmp(url_after_scheme(cfg->base_url), url_after_scheme(SONAR_SEED_BASE_URL)) == 0 &&
         strcmp(cfg->base_url, SONAR_SEED_BASE_URL) != 0)) {
        snprintf(cfg->base_url, sizeof(cfg->base_url), "%s", SONAR_SEED_BASE_URL);
        changed = true;
    }
#endif
#ifdef SONAR_SEED_TOKEN
    if (cfg->token[0] == '\0') {
        snprintf(cfg->token, sizeof(cfg->token), "%s", SONAR_SEED_TOKEN);
        changed = true;
    }
#endif
#ifdef SONAR_SEED_REMOTE_PLACE
    // 远程点位跟随编译期配置:换点位时刷固件即可生效,不受 NVS 旧值阻挡。
    if (cfg->remote_place_id[0] == '\0' ||
        strcmp(cfg->remote_place_id, SONAR_SEED_REMOTE_PLACE) != 0) {
        snprintf(cfg->remote_place_id, sizeof(cfg->remote_place_id), "%s",
                 SONAR_SEED_REMOTE_PLACE);
        changed = true;
    }
#endif
#ifdef SONAR_SEED_PRESENCE_MODE
    {
        uint8_t want = (SONAR_SEED_PRESENCE_MODE) != 0 ? 1 : 0;
        if (cfg->presence_mode != want) {
            cfg->presence_mode = want;
            changed = true;
        }
    }
#endif
#ifdef SONAR_SEED_AUTO_PUBLISH
    if (!cfg->auto_publish) {
        cfg->auto_publish = SONAR_SEED_AUTO_PUBLISH != 0;
        changed = cfg->auto_publish;
    }
#endif
    if (changed) {
        esp_err_t err = sonar_config_save(cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "保存配置失败: %s", esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "已写入配置(client_id=%s)", cfg->client_id);
    }
    return ESP_OK;
}

bool sonar_config_has_wifi(const sonar_config_t *cfg) {
    return cfg != NULL && cfg->wifi_ssid[0] != '\0';
}

bool sonar_config_has_sonar(const sonar_config_t *cfg) {
    return cfg != NULL && cfg->base_url[0] != '\0' && cfg->token[0] != '\0';
}
