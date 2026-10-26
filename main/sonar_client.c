// main/sonar_client.c
// Sonar 业务接口实现。只在应用任务里调用(内部复用一块静态响应缓冲)。
#include "sonar_client.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "sonar_http.h"
#include "sonar_net.h"
#include "sonar_time.h"
#include "sonar_uri.h"
#include "sonar_wav.h"

static const char *TAG = "sonar_api";
// inbox 最多 10 条,留 12 KB 足够;更大的响应直接报错而不是挤爆堆。
// 4 条投递的 JSON 约 6 KB;再大就是白白占着 DAU,mbedtls 握手要的是连续块。
static char s_resp_buf[8192];

void sonar_client_init(sonar_client_t *client, const sonar_config_t *cfg) {
    if (client == NULL || cfg == NULL) {
        return;
    }
    memset(client, 0, sizeof(*client));
    snprintf(client->base_url, sizeof(client->base_url), "%s", cfg->base_url);
    snprintf(client->token, sizeof(client->token), "%s", cfg->token);
    snprintf(client->focus, sizeof(client->focus), "%s", "愿意回答附近的现场问题");
}

static void copy_str(const cJSON *obj, const char *key, char *out, size_t out_len) {
    out[0] = '\0';
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(out, out_len, "%s", item->valuestring);
    }
}

static int64_t item_time(const cJSON *obj, const char *key) {
    char text[32];
    copy_str(obj, key, text, sizeof(text));
    int64_t epoch = 0;
    if (text[0] == '\0' || sonar_time_parse_rfc3339(text, &epoch) != 0) {
        return 0;
    }
    return epoch;
}

static int item_int(const cJSON *obj, const char *key, int fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return fallback;
}

static bool item_bool(const cJSON *obj, const char *key, bool fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return fallback;
}

static double item_double(const cJSON *obj, const char *key, double fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    return fallback;
}

// 服务端每次响应都带 server_time;顺手校准本机时钟,避免 expires_at 判断被 1970 带偏。
static void maybe_sync_time(const cJSON *root) {
    char text[32];
    copy_str(root, "server_time", text, sizeof(text));
    int64_t epoch = 0;
    if (text[0] != '\0' && sonar_time_parse_rfc3339(text, &epoch) == 0) {
        sonar_net_apply_server_time(epoch);
    }
}

static esp_err_t call(sonar_client_t *client, const char *method, const char *path,
                      const char *json, sonar_http_resp_t *resp, cJSON **root) {
    char url[SONAR_URL_MAX + 160];
    if (sonar_url_join(client->base_url, path, url, sizeof(url)) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    sonar_http_resp_init(resp, s_resp_buf, sizeof(s_resp_buf));
    esp_err_t err = sonar_http_json(method, url, client->token, json, resp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s 请求失败: %s", path, esp_err_to_name(err));
        return err;
    }
    if (root != NULL) {
        *root = cJSON_Parse(resp->body);
        if (*root == NULL) {
            ESP_LOGW(TAG, "%s 响应不是合法 JSON", path);
            return ESP_ERR_INVALID_RESPONSE;
        }
        maybe_sync_time(*root);
    }
    return ESP_OK;
}

static void log_error_body(const char *what, const sonar_http_resp_t *resp) {
    cJSON *root = cJSON_Parse(resp->body);
    if (root != NULL) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        char code[48] = {0};
        char message[128] = {0};
        if (error != NULL) {
            copy_str(error, "code", code, sizeof(code));
            copy_str(error, "message", message, sizeof(message));
        }
        ESP_LOGW(TAG, "%s 失败 HTTP %d %s %s", what, resp->status, code, message);
        cJSON_Delete(root);
    } else {
        ESP_LOGW(TAG, "%s 失败 HTTP %d (无 JSON 错误体)", what, resp->status);
    }
}

esp_err_t sonar_me(sonar_client_t *client) {
    sonar_http_resp_t resp;
    cJSON *root = NULL;
    esp_err_t err = call(client, "GET", "/v1/me", NULL, &resp, &root);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status != 200) {
        log_error_body("GET /v1/me", &resp);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *profile = cJSON_GetObjectItemCaseSensitive(root, "profile");
    if (profile == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    copy_str(profile, "user_id", client->user_id, sizeof(client->user_id));
    copy_str(profile, "focus", client->focus, sizeof(client->focus));
    client->revision = item_int(profile, "revision", 1);
    char mode[16];
    copy_str(profile, "mode", mode, sizeof(mode));
    client->quiet = strcmp(mode, "quiet") == 0;
    client->assist_enabled = item_bool(profile, "assist_enabled", true);
    ESP_LOGI(TAG, "me: user=%s revision=%d mode=%s", client->user_id, client->revision,
             client->quiet ? "quiet" : "listen");
    cJSON_Delete(root);
    return ESP_OK;
}

// focus 是用户自己的文字,可能含引号/换行;嵌进 JSON 前先做最小转义,避免发出非法请求。
static void json_escape_into(const char *in, char *out, size_t out_len) {
    size_t used = 0;
    for (const char *p = in; p != NULL && *p != '\0' && used + 2 < out_len; ++p) {
        if (*p == '"' || *p == '\\') {
            out[used++] = '\\';
            out[used++] = *p;
        } else if (*p == '\n' || *p == '\r' || *p == '\t') {
            out[used++] = ' ';
        } else {
            out[used++] = *p;
        }
    }
    out[used] = '\0';
}

esp_err_t sonar_set_mode(sonar_client_t *client, bool quiet) {
    char focus[300];
    json_escape_into(client->focus, focus, sizeof(focus));
    char body[640];
    snprintf(body, sizeof(body),
             "{\"expected_revision\":%d,\"presence\":null,\"assist_enabled\":%s,"
             "\"mode\":\"%s\",\"focus\":\"%s\"}",
             client->revision, client->assist_enabled ? "true" : "false", quiet ? "quiet" : "listen",
             focus);
    sonar_http_resp_t resp;
    cJSON *root = NULL;
    esp_err_t err = call(client, "PUT", "/v1/me/context", body, &resp, &root);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status != 200) {
        log_error_body("PUT /v1/me/context", &resp);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *profile = cJSON_GetObjectItemCaseSensitive(root, "profile");
    if (profile != NULL) {
        client->revision = item_int(profile, "revision", client->revision);
        client->assist_enabled = item_bool(profile, "assist_enabled", client->assist_enabled);
    }
    client->quiet = quiet;  // 本地先生效;同步失败也不出声(交接书要求)
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t sonar_set_location(sonar_client_t *client, double latitude, double longitude,
                             double accuracy_m, int64_t sampled_at, const char *source) {
    char stamp[32];
    if (sonar_time_format_rfc3339(sampled_at, stamp, sizeof(stamp)) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char body[512];
    snprintf(body, sizeof(body),
             "{\"expected_revision\":%d,\"location\":{\"latitude\":%.6f,\"longitude\":%.6f,"
             "\"coordinate_system\":\"gcj02\",\"accuracy_m\":%.1f,\"sampled_at\":\"%s\","
             "\"source\":\"%s\"}}",
             client->revision, latitude, longitude, accuracy_m, stamp, source);
    sonar_http_resp_t resp;
    cJSON *root = NULL;
    esp_err_t err = call(client, "PUT", "/v1/me/location", body, &resp, &root);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status != 200) {
        log_error_body("PUT /v1/me/location", &resp);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *profile = cJSON_GetObjectItemCaseSensitive(root, "profile");
    if (profile != NULL) {
        client->revision = item_int(profile, "revision", client->revision);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t sonar_set_presence_simulated(sonar_client_t *client, const char *place_id,
                                       const char *direction) {
    char focus[300];
    json_escape_into(client->focus, focus, sizeof(focus));
    char body[640];
    snprintf(body, sizeof(body),
             "{\"expected_revision\":%d,\"presence\":{\"place_id\":\"%s\","
             "\"direction\":\"%s\",\"source\":\"simulated\"},\"assist_enabled\":%s,"
             "\"mode\":\"%s\",\"focus\":\"%s\"}",
             client->revision, place_id, direction, client->assist_enabled ? "true" : "false",
             client->quiet ? "quiet" : "listen", focus);
    sonar_http_resp_t resp;
    cJSON *root = NULL;
    esp_err_t err = call(client, "PUT", "/v1/me/context", body, &resp, &root);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status != 200) {
        log_error_body("PUT /v1/me/context(presence)", &resp);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *profile = cJSON_GetObjectItemCaseSensitive(root, "profile");
    if (profile != NULL) {
        client->revision = item_int(profile, "revision", client->revision);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t sonar_places(sonar_client_t *client, const char *query, sonar_place_t *out, size_t max,
                       size_t *count) {
    if (query == NULL || out == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    char encoded[256];
    if (sonar_uri_encode(query, encoded, sizeof(encoded)) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[320];
    int path_len = snprintf(path, sizeof(path), "/v1/places?query=%s", encoded);
    if (path_len <= 0 || (size_t)path_len >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char url[SONAR_URL_MAX + 320];
    if (sonar_url_join(client->base_url, path, url, sizeof(url)) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    sonar_http_resp_t resp;
    sonar_http_resp_init(&resp, s_resp_buf, sizeof(s_resp_buf));
    esp_err_t err = sonar_http_json("GET", url, client->token, NULL, &resp);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status != 200) {
        log_error_body("GET /v1/places", &resp);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *root = cJSON_Parse(resp.body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    maybe_sync_time(root);
    const cJSON *places = cJSON_GetObjectItemCaseSensitive(root, "places");
    const cJSON *item = NULL;
    size_t n = 0;
    cJSON_ArrayForEach(item, places) {
        if (n >= max) {
            break;
        }
        copy_str(item, "id", out[n].id, sizeof(out[n].id));
        copy_str(item, "label", out[n].label, sizeof(out[n].label));
        out[n].latitude = item_double(item, "latitude", 0);
        out[n].longitude = item_double(item, "longitude", 0);
        if (out[n].id[0] != '\0') {
            ++n;
        }
    }
    *count = n;
    cJSON_Delete(root);
    ESP_LOGI(TAG, "places(%s): %u 条", query, (unsigned)n);
    return n > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void parse_delivery(const cJSON *item, sonar_delivery_t *out) {
    memset(out, 0, sizeof(*out));
    copy_str(item, "id", out->id, sizeof(out->id));
    copy_str(item, "type", out->type, sizeof(out->type));
    copy_str(item, "question_id", out->question_id, sizeof(out->question_id));
    copy_str(item, "request_id", out->request_id, sizeof(out->request_id));
    copy_str(item, "state", out->state, sizeof(out->state));
    copy_str(item, "reason_code", out->reason_code, sizeof(out->reason_code));
    out->revision = item_int(item, "profile_revision", 0);
    out->actionable = item_bool(item, "actionable", false);
    out->expires_at = item_time(item, "expires_at");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(item, "payload");
    if (payload == NULL) {
        return;
    }
    copy_str(payload, "post_id", out->post_id, sizeof(out->post_id));
    copy_str(payload, "transcript", out->transcript, sizeof(out->transcript));
    copy_str(payload, "place_id", out->place_id, sizeof(out->place_id));
    copy_str(payload, "place_name", out->place_name, sizeof(out->place_name));
    copy_str(payload, "author_label", out->author_label, sizeof(out->author_label));
    copy_str(payload, "kind", out->kind, sizeof(out->kind));
    copy_str(payload, "audio_url", out->audio_url, sizeof(out->audio_url));
    copy_str(payload, "question_audio_url", out->question_audio_url,
             sizeof(out->question_audio_url));
    const cJSON *attention = cJSON_GetObjectItemCaseSensitive(payload, "attention");
    if (attention != NULL) {
        copy_str(attention, "action", out->attention_action, sizeof(out->attention_action));
    }
}

esp_err_t sonar_inbox(sonar_client_t *client, sonar_delivery_t *items, size_t max, size_t *count) {
    if (items == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    sonar_http_resp_t resp;
    cJSON *root = NULL;
    esp_err_t err = call(client, "GET", "/v1/inbox", NULL, &resp, &root);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status != 200) {
        log_error_body("GET /v1/inbox", &resp);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "items");
    const cJSON *item = NULL;
    size_t n = 0;
    cJSON_ArrayForEach(item, list) {
        if (n >= max) {
            break;
        }
        parse_delivery(item, &items[n]);
        if (items[n].id[0] != '\0') {
            ++n;
        }
    }
    *count = n;
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t sonar_create_draft(sonar_client_t *client, const char *metadata_json,
                             const char *wav_path, size_t wav_len, char *post_id,
                             size_t post_id_len) {
    if (post_id != NULL && post_id_len > 0) {
        post_id[0] = '\0';
    }
    char url[SONAR_URL_MAX + 32];
    if (sonar_url_join(client->base_url, "/v1/posts/drafts", url, sizeof(url)) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    sonar_http_resp_t resp;
    sonar_http_resp_init(&resp, s_resp_buf, sizeof(s_resp_buf));
    esp_err_t err =
        sonar_http_upload_wav(url, client->token, metadata_json, wav_path, wav_len, &resp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "上传草稿传输失败: %s", esp_err_to_name(err));
        return err;
    }
    if (resp.status != 200 && resp.status != 201) {
        log_error_body("POST /v1/posts/drafts", &resp);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *root = cJSON_Parse(resp.body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    maybe_sync_time(root);
    if (post_id != NULL && post_id_len > 0) {
        copy_str(root, "post_id", post_id, post_id_len);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "草稿已创建: %s", post_id == NULL ? "" : post_id);
    return ESP_OK;
}

esp_err_t sonar_confirm(sonar_client_t *client, const char *post_id, char *question_id,
                        size_t question_id_len, char *question_status, size_t status_len) {
    char path[256];
    char encoded[SONAR_ID_MAX * 3 + 1];
    if (sonar_uri_encode(post_id, encoded, sizeof(encoded)) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(path, sizeof(path), "/v1/posts/%s/confirm", encoded);
    sonar_http_resp_t resp;
    cJSON *root = NULL;
    esp_err_t err = call(client, "POST", path, "{}", &resp, &root);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status != 200) {
        log_error_body("POST /v1/posts/{id}/confirm", &resp);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (question_id != NULL && question_id_len > 0) {
        copy_str(root, "question_id", question_id, question_id_len);
    }
    if (question_status != NULL && status_len > 0) {
        copy_str(root, "question_status", question_status, status_len);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t sonar_delivery(sonar_client_t *client, const char *delivery_id, sonar_delivery_t *out) {
    if (delivery_id == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char encoded[SONAR_ID_MAX * 3 + 1];
    if (sonar_uri_encode(delivery_id, encoded, sizeof(encoded)) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[256];
    snprintf(path, sizeof(path), "/v1/inbox/%s", encoded);
    sonar_http_resp_t resp;
    cJSON *root = NULL;
    esp_err_t err = call(client, "GET", path, NULL, &resp, &root);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status != 200) {
        log_error_body("GET /v1/inbox/{id}", &resp);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "delivery");
    if (item == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    parse_delivery(item, out);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t sonar_ack(sonar_client_t *client, const char *delivery_id, const char *event,
                    const char *request_id) {
    if (delivery_id == NULL || event == NULL || request_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char encoded[SONAR_ID_MAX * 3 + 1];
    if (sonar_uri_encode(delivery_id, encoded, sizeof(encoded)) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[256];
    snprintf(path, sizeof(path), "/v1/inbox/%s/ack", encoded);
    char body[256];
    snprintf(body, sizeof(body), "{\"event\":\"%s\",\"request_id\":\"%s\"}", event, request_id);
    sonar_http_resp_t resp;
    cJSON *root = NULL;
    esp_err_t err = call(client, "POST", path, body, &resp, &root);
    if (err != ESP_OK) {
        return err;
    }
    bool ok = resp.status == 200;
    if (!ok) {
        log_error_body("POST /v1/inbox/{id}/ack", &resp);
    }
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t sonar_question(sonar_client_t *client, const char *question_id, sonar_question_t *out) {
    if (question_id == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    char encoded[SONAR_ID_MAX * 3 + 1];
    if (sonar_uri_encode(question_id, encoded, sizeof(encoded)) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[256];
    snprintf(path, sizeof(path), "/v1/questions/%s", encoded);
    sonar_http_resp_t resp;
    cJSON *root = NULL;
    esp_err_t err = call(client, "GET", path, NULL, &resp, &root);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status != 200) {
        log_error_body("GET /v1/questions/{id}", &resp);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    copy_str(root, "status", out->status, sizeof(out->status));
    out->expires_at = item_time(root, "expires_at");
    const cJSON *answer = cJSON_GetObjectItemCaseSensitive(root, "answer");
    if (cJSON_IsObject(answer)) {
        out->has_answer = true;
        copy_str(answer, "post_id", out->answer_post_id, sizeof(out->answer_post_id));
        copy_str(answer, "transcript", out->answer_transcript, sizeof(out->answer_transcript));
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t sonar_cancel_question(sonar_client_t *client, const char *question_id) {
    if (question_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char encoded[SONAR_ID_MAX * 3 + 1];
    if (sonar_uri_encode(question_id, encoded, sizeof(encoded)) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[256];
    snprintf(path, sizeof(path), "/v1/questions/%s/cancel", encoded);
    sonar_http_resp_t resp;
    cJSON *root = NULL;
    esp_err_t err = call(client, "POST", path, "{}", &resp, &root);
    if (err != ESP_OK) {
        return err;
    }
    bool ok = resp.status == 200;
    if (!ok) {
        log_error_body("POST /v1/questions/{id}/cancel", &resp);
    }
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t sonar_download_audio(sonar_client_t *client, const char *post_id, const char *path,
                               size_t *out_len) {
    if (post_id == NULL || path == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char encoded[SONAR_ID_MAX * 3 + 1];
    if (sonar_uri_encode(post_id, encoded, sizeof(encoded)) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char url[SONAR_URL_MAX + SONAR_PATH_MAX];
    if (sonar_url_join(client->base_url, "/v1/audio/", url, sizeof(url)) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t used = strlen(url);
    snprintf(url + used, sizeof(url) - used, "%s", encoded);

    char content_type[48] = {0};
    // 300 KiB 上限来自服务端;多留 8 KiB 给 RIFF 头余量,超过就判定异常。
    esp_err_t err = sonar_http_download(url, client->token, path,
                                        SONAR_WAV_MAX_PCM_BYTES + 8192u, out_len, content_type,
                                        sizeof(content_type));
    if (err != ESP_OK) {
        return err;
    }
    if (content_type[0] != '\0' && strstr(content_type, "wav") == NULL) {
        ESP_LOGW(TAG, "原声 Content-Type 异常: %s", content_type);
        remove(path);
        return ESP_ERR_INVALID_RESPONSE;
    }
    // 下载后立即按规格校验 WAV,不合格不进入播放。
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t head[512];
    size_t got = fread(head, 1, sizeof(head), fp);
    fclose(fp);
    sonar_wav_info_t info;
    int rc = sonar_wav_parse(head, got, &info);
    if (rc != SONAR_WAV_OK || sonar_wav_validate(&info) != SONAR_WAV_OK) {
        ESP_LOGW(TAG, "原声 WAV 不合格: parse=%d bytes=%u", rc, (unsigned)info.data_bytes);
        remove(path);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}
