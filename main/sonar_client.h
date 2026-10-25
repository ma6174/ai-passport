// main/sonar_client.h
// Sonar 业务接口(设备 Token 鉴权,走 HTTPS)。
//
// 契约以服务端 `openapi.json` 与 `server/app/*.py` 为准:交接书 §5 是设计意图,
// 字段细节(metadata 的 kind、ack 事件、places 依赖高德)与服务端实现一致地按这里实现。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sonar_config.h"

#define SONAR_ID_MAX 64
#define SONAR_TEXT_MAX 512
#define SONAR_PATH_MAX 128
// 一次最多解析 4 条:结构体较大,放静态区而不是栈上;剩下的下一秒再取。
#define SONAR_MAX_DELIVERIES 4

typedef struct {
    char id[SONAR_ID_MAX];
    char label[64];
    double latitude;
    double longitude;
} sonar_place_t;

typedef struct {
    char id[SONAR_ID_MAX];
    char type[16];  // invitation / audio
    char question_id[SONAR_ID_MAX];
    char request_id[SONAR_ID_MAX];
    int revision;
    char state[16];
    int64_t expires_at;
    bool actionable;
    char reason_code[48];
    // payload
    char post_id[SONAR_ID_MAX];
    char transcript[SONAR_TEXT_MAX];
    char place_id[SONAR_ID_MAX];
    char place_name[64];
    char author_label[64];
    char kind[24];
    char audio_url[SONAR_PATH_MAX];
    char question_audio_url[SONAR_PATH_MAX];
    char attention_action[16];
} sonar_delivery_t;

typedef struct {
    char base_url[SONAR_URL_MAX];
    char token[SONAR_TOKEN_MAX];
    char user_id[32];
    char focus[128];
    int revision;
    bool quiet;
    bool assist_enabled;
} sonar_client_t;

typedef struct {
    char status[16];
    bool has_answer;
    char answer_post_id[SONAR_ID_MAX];
    char answer_transcript[SONAR_TEXT_MAX];
    int64_t expires_at;
} sonar_question_t;

void sonar_client_init(sonar_client_t *client, const sonar_config_t *cfg);

esp_err_t sonar_me(sonar_client_t *client);
esp_err_t sonar_set_mode(sonar_client_t *client, bool quiet);
esp_err_t sonar_set_location(sonar_client_t *client, double latitude, double longitude,
                             double accuracy_m, int64_t sampled_at, const char *source);

// 模拟现场:不声称 GPS,只声明"我在这个演示路口,方向 demo_exit"。
esp_err_t sonar_set_presence_simulated(sonar_client_t *client, const char *place_id,
                                       const char *direction);

esp_err_t sonar_places(sonar_client_t *client, const char *query, sonar_place_t *out, size_t max,
                       size_t *count);

esp_err_t sonar_create_draft(sonar_client_t *client, const char *metadata_json,
                             const char *wav_path, size_t wav_len, char *post_id,
                             size_t post_id_len);
esp_err_t sonar_confirm(sonar_client_t *client, const char *post_id, char *question_id,
                        size_t question_id_len, char *question_status, size_t status_len);

esp_err_t sonar_inbox(sonar_client_t *client, sonar_delivery_t *items, size_t max, size_t *count);
esp_err_t sonar_delivery(sonar_client_t *client, const char *delivery_id, sonar_delivery_t *out);
esp_err_t sonar_ack(sonar_client_t *client, const char *delivery_id, const char *event,
                    const char *request_id);

esp_err_t sonar_question(sonar_client_t *client, const char *question_id, sonar_question_t *out);
esp_err_t sonar_cancel_question(sonar_client_t *client, const char *question_id);

// 下载原声到本地文件并按 Sonar 规格校验 WAV。
esp_err_t sonar_download_audio(sonar_client_t *client, const char *post_id, const char *path,
                               size_t *out_len);
