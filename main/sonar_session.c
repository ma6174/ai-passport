// main/sonar_session.c
// Sonar 会话式多轮语音的客户端实现:一次会话、多句 utterance、会话收件箱。
//
// 设计要点:
//   * 会话 ID 与 revision 只在内存 + NVS 里保留,不新建身份;
//   * revision 冲突(409)时刷新会话后重试一次,不重复发布;
//   * 结构化结果以 voice_action 为准,agent_response.text 只作为展示兜底;
//   * 音频地址一律用服务端返回的 audio_url,不假设都在 /v1/audio/。
#include "sonar_session.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"

#include "sonar_http.h"
#include "sonar_net.h"
#include "sonar_uri.h"

static const char *TAG = "sonar_sess";

typedef struct {
    bool active;
    char id[SONAR_ID_MAX];
    int revision;
    char place_id[SONAR_ID_MAX];
} session_state_t;

// 创建参数版本号:改动 create_session 的语义时 +1,让旧会话失效重建。
#define SESSION_GEN 2

static session_state_t s_sess;
// 会话响应都是很小的 JSON(turn 约 1–3 KB)。C3 没有 PSRAM,共享一块 6 KB 静态缓冲,
// 别照搬全局 16 KB 上限——静态区每 1 KB 都是永久占用。
#define SONAR_SESSION_RESP_MAX 3072
static char s_resp_body[SONAR_SESSION_RESP_MAX];
static const char *NS = "sonar";

static void copy_str(const cJSON *obj, const char *key, char *out, size_t out_len) {
    if (out == NULL || out_len == 0) {
        return;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(out, out_len, "%s", item->valuestring);
    }
}

static int get_int(const cJSON *obj, const char *key, int fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

// 时间戳格式固定为 UTC ISO8601(秒精度),服务端只要求 recorded_at 是时间点。
static void iso_now(char *out, size_t out_len) {
    time_t now = (time_t)sonar_net_now();
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static void persist(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, "sess_id", s_sess.id);
    nvs_set_i32(h, "sess_rev", s_sess.revision);
    nvs_set_str(h, "sess_place", s_sess.place_id);
    nvs_set_u8(h, "sess_gen", SESSION_GEN);
    nvs_commit(h);
    nvs_close(h);
}

static void restore(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    // 会话的授权面貌由创建参数决定;参数语义变了(例如从 legacy 授权改成完整 capabilities)
    // 就必须重建,否则旧会话会一直 403。
    uint8_t gen = 0;
    if (nvs_get_u8(h, "sess_gen", &gen) != ESP_OK || gen != SESSION_GEN) {
        nvs_close(h);
        if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, "sess_id");
            nvs_erase_key(h, "sess_rev");
            nvs_close(h);
        }
        ESP_LOGW(TAG, "会话创建参数已升级,丢弃旧会话");
        return;
    }
    size_t len = sizeof(s_sess.id);
    if (nvs_get_str(h, "sess_id", s_sess.id, &len) == ESP_OK && s_sess.id[0] != '\0') {
        int32_t rev = 0;
        nvs_get_i32(h, "sess_rev", &rev);
        s_sess.revision = (int)rev;
        len = sizeof(s_sess.place_id);
        nvs_get_str(h, "sess_place", s_sess.place_id, &len);
        s_sess.active = true;
        ESP_LOGI(TAG, "恢复会话 %s rev=%d place=%s", s_sess.id, s_sess.revision, s_sess.place_id);
    }
    nvs_close(h);
}

void sonar_session_forget(void) {
    if (s_sess.id[0] != '\0') {
        ESP_LOGW(TAG, "丢弃会话 %s", s_sess.id);
    }
    memset(&s_sess, 0, sizeof(s_sess));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "sess_id");
        nvs_erase_key(h, "sess_rev");
        nvs_erase_key(h, "sess_place");
        nvs_commit(h);
        nvs_close(h);
    }
}

const char *sonar_session_id(void) {
    return s_sess.id;
}

int sonar_session_revision(void) {
    return s_sess.revision;
}

static int parse_session(const cJSON *root) {
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session");
    if (!cJSON_IsObject(session)) {
        // 有的实现直接返回会话对象本身。
        session = root;
    }
    char id[SONAR_ID_MAX] = {0};
    copy_str(session, "id", id, sizeof(id));
    if (id[0] == '\0') {
        return -1;
    }
    snprintf(s_sess.id, sizeof(s_sess.id), "%s", id);
    s_sess.revision = get_int(session, "revision", 1);
    return 0;
}

// 从 NVS 拿到的会话可能已经过期/被清:只有 state=active 才复用。
static esp_err_t refresh_session(sonar_client_t *client) {
    char url[SONAR_URL_MAX + 96];
    if (sonar_url_join(client->base_url, "/v1/exchange/sessions/", url, sizeof(url)) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t used = strlen(url);
    snprintf(url + used, sizeof(url) - used, "%s", s_sess.id);
    char *body = s_resp_body;
    sonar_http_resp_t resp;
    sonar_http_resp_init(&resp, body, SONAR_SESSION_RESP_MAX);
    esp_err_t err = sonar_http_json("GET", url, client->token, NULL, &resp);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status != 200) {
        ESP_LOGW(TAG, "会话刷新失败 HTTP %d", resp.status);
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(session, "state");
    bool ok = cJSON_IsString(state) && strcmp(state->valuestring, "active") == 0;
    if (ok) {
        parse_session(root);
    }
    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGW(TAG, "会话已不在 active 状态,重新建立");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

// auto_publish 固定为 true:服务端只有在 auto_publish=true 时才会下发完整 capabilities;
// auto_publish=false 得到的是 legacy 会话(仅 auto_play 可用),任何 utterance 都 403
// EXCHANGE_NOT_AUTHORIZED。是否真的发布仍然由服务端按语义判断(闲聊会走 private_chat)。
static esp_err_t create_session(sonar_client_t *client, const sonar_config_t *cfg,
                                const char *place_id) {
    (void)cfg;
    char url[SONAR_URL_MAX + 64];
    if (sonar_url_join(client->base_url, "/v1/exchange/sessions", url, sizeof(url)) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    char uuid[SONAR_CLIENT_ID_LEN + 1];
    sonar_config_new_uuid(uuid);
    static char req[640];
    snprintf(req, sizeof(req),
             "{\"client_request_id\":\"%s\",\"expected_revision\":%d,\"place_id\":\"%s\","
             "\"interaction_mode\":\"voice_first\",\"timezone\":\"Asia/Shanghai\","
             "\"auto_publish\":true,\"auto_play\":true,\"network_enabled\":true}",
             uuid, client->revision, place_id);
    char *body = s_resp_body;
    sonar_http_resp_t resp;
    sonar_http_resp_init(&resp, body, SONAR_SESSION_RESP_MAX);
    esp_err_t err = sonar_http_json("POST", url, client->token, req, &resp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "会话创建传输失败: %s", esp_err_to_name(err));
        return err;
    }
    if (resp.status != 200 && resp.status != 201) {
        ESP_LOGW(TAG, "会话创建失败 HTTP %d: %.200s", resp.status, body);
        return ESP_ERR_INVALID_STATE;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGW(TAG, "会话响应不是 JSON: %.120s", body);
        return ESP_ERR_INVALID_RESPONSE;
    }
    int rc = parse_session(root);
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session");
    const cJSON *caps = cJSON_GetObjectItemCaseSensitive(session, "capabilities");
    cJSON_Delete(root);
    if (rc != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_sess.active = true;
    snprintf(s_sess.place_id, sizeof(s_sess.place_id), "%s", place_id);
    persist();
    ESP_LOGI(TAG, "会话就绪 id=%s rev=%d place=%s caps=%s", s_sess.id,
             s_sess.revision, place_id,
             cJSON_IsObject(caps) ? "有" : "无");
    return ESP_OK;
}

esp_err_t sonar_session_ensure(sonar_client_t *client, const sonar_config_t *cfg,
                               const char *place_id) {
    if (place_id == NULL || place_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_sess.active && s_sess.id[0] == '\0') {
        restore();
    }
    if (s_sess.active && s_sess.id[0] != '\0' && strcmp(s_sess.place_id, place_id) == 0) {
        if (refresh_session(client) == ESP_OK) {
            return ESP_OK;
        }
        sonar_session_forget();
    }
    return create_session(client, cfg, place_id);
}

static void parse_voice_items(const cJSON *voice, sonar_turn_t *out) {
    copy_str(voice, "type", out->voice_type, sizeof(out->voice_type));
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(voice, "items");
    if (!cJSON_IsArray(items)) {
        return;
    }
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, items) {
        if (out->item_count >= SONAR_SESSION_MAX_ITEMS) {
            break;
        }
        sonar_voice_item_t *item = &out->items[out->item_count];
        copy_str(it, "audio_url", item->audio_url, sizeof(item->audio_url));
        copy_str(it, "post_id", item->post_id, sizeof(item->post_id));
        copy_str(it, "message_id", item->message_id, sizeof(item->message_id));
        copy_str(it, "kind", item->kind, sizeof(item->kind));
        copy_str(it, "transcript", item->transcript, sizeof(item->transcript));
        item->duration_s = (uint32_t)get_int(it, "duration_seconds", 0);
        if (item->audio_url[0] != '\0') {
            out->item_count++;
        }
    }
}

static void parse_turn(const char *json, sonar_turn_t *out) {
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "turn 响应不是 JSON: %.120s", json);
        return;
    }
    copy_str(root, "turn_id", out->turn_id, sizeof(out->turn_id));
    copy_str(root, "status", out->status, sizeof(out->status));
    copy_str(root, "action", out->action, sizeof(out->action));
    copy_str(root, "reason", out->reason, sizeof(out->reason));
    copy_str(root, "task_id", out->task_id, sizeof(out->task_id));
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsObject(result)) {
        copy_str(result, "message_id", out->message_id, sizeof(out->message_id));
        copy_str(result, "conversation_id", out->conversation_id, sizeof(out->conversation_id));
        copy_str(result, "chat_id", out->chat_id, sizeof(out->chat_id));
        if (out->reason[0] == '\0') {
            copy_str(result, "reason", out->reason, sizeof(out->reason));
        }
        const cJSON *resp = cJSON_GetObjectItemCaseSensitive(result, "response");
        if (cJSON_IsObject(resp)) {
            copy_str(resp, "text", out->agent_text, sizeof(out->agent_text));
            copy_str(resp, "kind", out->agent_kind, sizeof(out->agent_kind));
        }
    }
    const cJSON *agent = cJSON_GetObjectItemCaseSensitive(root, "agent_response");
    if (cJSON_IsObject(agent) && out->agent_text[0] == '\0') {
        copy_str(agent, "text", out->agent_text, sizeof(out->agent_text));
        copy_str(agent, "kind", out->agent_kind, sizeof(out->agent_kind));
    }
    const cJSON *voice = cJSON_GetObjectItemCaseSensitive(root, "voice_action");
    if (cJSON_IsObject(voice)) {
        parse_voice_items(voice, out);
    } else if (cJSON_IsObject(agent) && out->item_count == 0) {
        // 兼容:个别实现把音频动作挂在 agent_response.voice_action 下。
        const cJSON *nested = cJSON_GetObjectItemCaseSensitive(agent, "voice_action");
        if (cJSON_IsObject(nested)) {
            parse_voice_items(nested, out);
        }
    }
    cJSON_Delete(root);
}

esp_err_t sonar_session_utterance(sonar_client_t *client, const char *wav_path, size_t wav_len,
                                  const char *transcript, sonar_turn_t *out) {
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_sess.active || s_sess.id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char url[SONAR_URL_MAX + 128];
    if (sonar_url_join(client->base_url, "/v1/exchange/sessions/", url, sizeof(url)) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t used = strlen(url);
    snprintf(url + used, sizeof(url) - used, "%s/utterances", s_sess.id);

    for (int attempt = 0; attempt < 2; ++attempt) {
        char recorded_at[32];
        iso_now(recorded_at, sizeof(recorded_at));
        char uuid[SONAR_CLIENT_ID_LEN + 1];
        sonar_config_new_uuid(uuid);
        static char meta[SONAR_TEXT_MAX + 320];
        snprintf(meta, sizeof(meta),
                 "{\"client_request_id\":\"%s\",\"expected_session_revision\":%d,"
                 "\"transcript\":\"%s\",\"recorded_at\":\"%s\"}",
                 uuid, s_sess.revision, transcript == NULL ? "" : transcript, recorded_at);
        char *body = s_resp_body;
        sonar_http_resp_t resp;
        sonar_http_resp_init(&resp, body, SONAR_SESSION_RESP_MAX);
        esp_err_t err = sonar_http_upload_wav(url, client->token, meta, wav_path, wav_len, &resp);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "utterance 传输失败: %s (wav=%u B)", esp_err_to_name(err),
                     (unsigned)wav_len);
            return err;
        }
        ESP_LOGI(TAG, "utterance HTTP %d bytes=%u rev=%d transcript=%s", resp.status,
                 (unsigned)resp.len, s_sess.revision, transcript == NULL ? "" : transcript);
        if (resp.status == 409 && attempt == 0) {
            // revision 过期:刷新会话后重试同一句话。
            if (refresh_session(client) == ESP_OK) {
                ESP_LOGW(TAG, "revision 冲突,刷新到 rev=%d 后重试", s_sess.revision);
                continue;
            }
            sonar_session_forget();
            return ESP_ERR_INVALID_STATE;
        }
        if (resp.status != 200 && resp.status != 201) {
            ESP_LOGW(TAG, "utterance 失败 HTTP %d: %.200s", resp.status, body);
            return ESP_ERR_INVALID_RESPONSE;
        }
        parse_turn(body, out);
        // 会话 revision 不随 utterance 推进(实测发布后 GET 仍是 rev=1),
        // 盲目 +1 会让下一句必然 409。这里回读一次真实版本,顺便拿到 chat/context 状态。
        if (refresh_session(client) != ESP_OK) {
            ESP_LOGW(TAG, "turn 后会话刷新失败,沿用 rev=%d", s_sess.revision);
        }
        persist();
        return ESP_OK;
    }
    return ESP_ERR_INVALID_STATE;
}

static void parse_delivery(const cJSON *item, sonar_delivery_t *out) {
    memset(out, 0, sizeof(*out));
    copy_str(item, "id", out->id, sizeof(out->id));
    copy_str(item, "type", out->type, sizeof(out->type));
    copy_str(item, "question_id", out->question_id, sizeof(out->question_id));
    copy_str(item, "request_id", out->request_id, sizeof(out->request_id));
    copy_str(item, "state", out->state, sizeof(out->state));
    copy_str(item, "reason_code", out->reason_code, sizeof(out->reason_code));
    out->revision = get_int(item, "revision", 0);
    const cJSON *actionable = cJSON_GetObjectItemCaseSensitive(item, "actionable");
    out->actionable = cJSON_IsTrue(actionable);
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(item, "payload");
    if (!cJSON_IsObject(payload)) {
        payload = item;
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
    if (out->post_id[0] == '\0') {
        // 真人聊天交付:payload.post_id 可能缺省,用 message_id 兜底取音频。
        copy_str(payload, "message_id", out->post_id, sizeof(out->post_id));
    }
}

esp_err_t sonar_session_inbox(sonar_client_t *client, sonar_delivery_t *out, size_t max,
                              size_t *count) {
    if (out == NULL || count == NULL || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    if (!s_sess.active || s_sess.id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char url[SONAR_URL_MAX + 128];
    if (sonar_url_join(client->base_url, "/v1/exchange/sessions/", url, sizeof(url)) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t used = strlen(url);
    snprintf(url + used, sizeof(url) - used, "%s/inbox", s_sess.id);
    char *body = s_resp_body;
    sonar_http_resp_t resp;
    sonar_http_resp_init(&resp, body, SONAR_SESSION_RESP_MAX);
    esp_err_t err = sonar_http_json("GET", url, client->token, NULL, &resp);
    if (err != ESP_OK) {
        return err;
    }
    if (resp.status != 200) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, items) {
        if (*count >= max) {
            break;
        }
        parse_delivery(it, &out[*count]);
        (*count)++;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t sonar_session_context(sonar_client_t *client, const char *post_id) {
    if (post_id == NULL || post_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_sess.active || s_sess.id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char url[SONAR_URL_MAX + 128];
    if (sonar_url_join(client->base_url, "/v1/exchange/sessions/", url, sizeof(url)) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t used = strlen(url);
    snprintf(url + used, sizeof(url) - used, "%s/context", s_sess.id);
    static char req[320];
    snprintf(req, sizeof(req),
             "{\"expected_session_revision\":%d,\"post_id\":\"%s\"}", s_sess.revision, post_id);
    char *body = s_resp_body;
    sonar_http_resp_t resp;
    sonar_http_resp_init(&resp, body, SONAR_SESSION_RESP_MAX);
    esp_err_t err = sonar_http_json("PUT", url, client->token, req, &resp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "context 传输失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "context post=%s HTTP %d", post_id, resp.status);
    if (resp.status == 200) {
        if (refresh_session(client) != ESP_OK) {
            ESP_LOGW(TAG, "context 后会话刷新失败,沿用 rev=%d", s_sess.revision);
        }
        persist();
        return ESP_OK;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

// 按服务端给的 audio_url 下载(不假设都在 /v1/audio/)。
esp_err_t sonar_session_download(sonar_client_t *client, const char *audio_url, const char *path,
                                 size_t *out_len) {
    if (audio_url == NULL || audio_url[0] == '\0' || path == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char url[SONAR_URL_MAX + SONAR_PATH_MAX];
    if (audio_url[0] == '/') {
        if (sonar_url_join(client->base_url, audio_url, url, sizeof(url)) == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        snprintf(url, sizeof(url), "%s", audio_url);
    }
    ESP_LOGI(TAG, "下载原声 %s", url);
    return sonar_http_download(url, client->token, path, 4 * 1024 * 1024, out_len, NULL, 0);
}

esp_err_t sonar_session_task_action(sonar_client_t *client, const char *task_id,
                                    const char *action) {
    if (task_id == NULL || task_id[0] == '\0' || action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char encoded[SONAR_ID_MAX * 3 + 1];
    if (sonar_uri_encode(task_id, encoded, sizeof(encoded)) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    char url[SONAR_URL_MAX + SONAR_PATH_MAX];
    if (sonar_url_join(client->base_url, "/v1/tasks/", url, sizeof(url)) == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t used = strlen(url);
    snprintf(url + used, sizeof(url) - used, "%s/actions", encoded);
    char uuid[SONAR_CLIENT_ID_LEN + 1];
    sonar_config_new_uuid(uuid);
    static char req[320];
    snprintf(req, sizeof(req),
             "{\"expected_revision\":%d,\"client_request_id\":\"%s\",\"action\":\"%s\"}",
             s_sess.revision > 0 ? s_sess.revision : 1, uuid, action);
    char *body = s_resp_body;
    sonar_http_resp_t resp;
    sonar_http_resp_init(&resp, body, SONAR_SESSION_RESP_MAX);
    esp_err_t err = sonar_http_json("POST", url, client->token, req, &resp);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "task %s action=%s HTTP %d", task_id, action, resp.status);
    return resp.status == 200 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
