// main/sonar_config.h
// 设备配置:Wi-Fi、Sonar 地址与 Token、Linx 身份、演示地点选择。
//
// 凭证只落在 NVS,不进入仓库。首次烧录可由被 gitignore 的 `main/local_secrets.h`
// 编译期播种(模板见 `main/local_secrets.h.example`),之后可直接改 NVS。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SONAR_WIFI_SSID_MAX 33
#define SONAR_WIFI_PASS_MAX 65
#define SONAR_URL_MAX 128
#define SONAR_TOKEN_MAX 96
#define SONAR_CLIENT_ID_LEN 36
#define SONAR_PLACE_MAX 8

// 演示地点:label 用于匹配转写,query 用于 `/v1/places` 查询。
typedef struct {
    const char *label;  // 展示名/匹配用
    const char *query;  // 交给高德的查询词
    const char *alias;  // 口语别名(可为 NULL)
    const char *prefer; // 候选里优先选择的名称关键词(可为 NULL)
    double latitude;    // 兜底坐标(设备离线/查询失败时用于上报位置)
    double longitude;
} sonar_place_preset_t;

typedef struct {
    char wifi_ssid[SONAR_WIFI_SSID_MAX];
    char wifi_pass[SONAR_WIFI_PASS_MAX];
    char base_url[SONAR_URL_MAX];
    char token[SONAR_TOKEN_MAX];
    char client_id[SONAR_CLIENT_ID_LEN + 1];
    char linx_url[SONAR_URL_MAX];
    char linx_token[SONAR_TOKEN_MAX];
    bool auto_publish;   // true: 松手后直接发布;false: 需再按一次 OK 确认
    uint8_t place_idx;   // 当前演示地点(sonar_place_presets 下标)
    // 0 = 模拟现场(simulated presence,只匹配 zhonghuan_jinke 的问题,不需要服务端开关)
    // 1 = 模拟定位(android_mock + 坐标,可匹配真实 POI 与附近公告,需服务端
    //     SONAR_ALLOW_MOCK_LOCATION=true)
    // 会话模式的目标地点(远程点位,例如模力社区 B0K2KHYN77)。
    // 为空时回退到解析出来的收藏点位 ID,不硬编码在业务代码里。
    char remote_place_id[64];
    uint8_t presence_mode;
} sonar_config_t;

// 三个演示地点:模力社区、人工智能岛 19 号楼、中环线金科路路口。
const sonar_place_preset_t *sonar_place_presets(size_t *count);

// 读取 NVS 配置。缺关键字段返回 ESP_ERR_NOT_FOUND(调用方应先播种)。
esp_err_t sonar_config_load(sonar_config_t *out);

esp_err_t sonar_config_save(const sonar_config_t *cfg);

// 首次启动(或 NVS 被擦除)时用编译期秘密补齐缺失字段;`client_id` 一定会生成并落盘。
esp_err_t sonar_config_seed(sonar_config_t *cfg);

// 生成一个 UUID v4(36 字符,带结尾 NUL)。
void sonar_config_new_uuid(char out[SONAR_CLIENT_ID_LEN + 1]);

bool sonar_config_has_wifi(const sonar_config_t *cfg);
bool sonar_config_has_sonar(const sonar_config_t *cfg);
