// main/app_state.c
// 业务编排:网络轮询在独立任务,主任务只管按键、录音、发布与非阻塞播放队列。
//
// 这样设计的原因是半双工交互的响应性:一次 TLS 请求在 C3 上约 1–2 秒,如果放在
// 主任务里,"按住 OK 说话"就会被轮询拖慢;播放同理,阻塞会让人按 OK 停不下来。
#include "app_state.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_ui.h"
#include "audio_coordinator.h"
#include "sonar_ogg.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "cJSON.h"
#if __has_include("local_secrets.h")
#include "local_secrets.h"
#endif
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include <time.h>

#include "nvs.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linx_client.h"
#include "linx_ws.h"
#include "sonar_client.h"
#include "sonar_config.h"
#include "sonar_decision.h"
#include "sonar_geo.h"
#include "sonar_net.h"
#include "sonar_policy.h"
#include "sonar_session.h"
#include "sonar_store.h"
#include "sonar_time.h"
#include "sonar_wav.h"

#define SONAR_TTS_PATH "/spiffs/tts.wav"

static const char *TAG = "app";

extern const uint8_t prompt_wav_start[] asm("_binary_prompt_wav_start");
extern const uint8_t prompt_wav_end[] asm("_binary_prompt_wav_end");
extern const uint8_t beep_wav_start[] asm("_binary_beep_wav_start");
extern const uint8_t beep_wav_end[] asm("_binary_beep_wav_end");

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} key_event_t;

typedef enum {
    PENDING_NONE = 0,
    PENDING_ANSWER,
    PENDING_QUESTION,
} pending_kind_t;

typedef struct {
    pending_kind_t kind;
    char client_request_id[SONAR_CLIENT_ID_LEN + 1];
    char question_id[SONAR_ID_MAX];
    char invitation_id[SONAR_ID_MAX];
    char target_place_id[SONAR_ID_MAX];
    char direction[16];
} pending_publish_t;

// 播放队列项:内存素材(提示音)或磁盘 WAV(问题原声/真人回答)。
typedef struct {
    uint8_t is_mem;
    const uint8_t *data;
    size_t len;
    int64_t queued_ms;      // 入队时刻:用于日志区分"排队久"和"播放久"
    char path[48];
    char ack_event[12];
    char delivery_id[SONAR_ID_MAX];
    char request_id[SONAR_ID_MAX];
    char ctx_post_id[SONAR_ID_MAX];  // 播完要写回会话 context 的原声 post
} play_req_t;

// 会话模式下"这一轮该用哪个远程地点"。固定为模力社区(交接书要求不与作者所在地混用)。
static char s_ctx_pending[SONAR_ID_MAX];

static QueueHandle_t s_keys;
static QueueHandle_t s_play_q;
static SemaphoreHandle_t s_api_lock;
static SemaphoreHandle_t s_inv_lock;

static sonar_config_t s_cfg;
// 启动时抓到的复位原因,留到网络就绪时再打一次(开机瞬间日志容易被串口吞掉)
static esp_reset_reason_t s_reset_reason;
static sonar_client_t s_client;
static const sonar_place_preset_t *s_presets;
static size_t s_preset_count;
static sonar_place_t s_places[3];
static bool s_place_resolved[3];

static sonar_delivery_t s_invitation;
static bool s_has_invitation;
static char s_last_question[SONAR_ID_MAX];

static app_phase_t s_phase = APP_PHASE_BOOT;
static char s_action[64] = "boot";
static char s_detail[64];
// 用户需要反复看的信息(转写、反问、回答人、播放结果)单独存一份:
// 周期性状态(loc/inbox/question)不能把它冲掉,否则看一眼屏幕就忘了刚才说了什么。
static char s_detail_sticky[160];
static bool s_sticky;
static char s_activation[16];
static bool s_quiet;
static bool s_sonar_ok;
static bool s_linx_ok;
static bool s_linx_bound;
static int s_pending;
static int s_battery = -1;

static bool s_ptt;
static int64_t s_next_view_ms;
static int64_t s_next_battery_ms;
static char s_transcript[SONAR_TEXT_MAX];
static char s_metadata[1400];
static pending_publish_t s_pending_pub;

static bool s_playing;
static play_req_t s_current;
static int64_t s_play_started_ms;
static int64_t s_release_ms;    // 本轮松手时刻:端到端延迟的主指标起点

static sonar_delivery_t s_inbox[SONAR_MAX_DELIVERIES];

#define API_LOCK() xSemaphoreTake(s_api_lock, portMAX_DELAY)
#define API_UNLOCK() xSemaphoreGive(s_api_lock)

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void set_phase(app_phase_t phase, const char *action) {
    s_phase = phase;
    if (action != NULL) {
        snprintf(s_action, sizeof(s_action), "%s", action);
    }
}

static void set_detail(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_detail, sizeof(s_detail), fmt, args);
    va_end(args);
    ESP_LOGI(TAG, "%s", s_detail);
}

// 按"字符"截断 UTF-8:%.*s 是按字节截的,中文会被切半个字符显示成尖括号问号。
static size_t utf8_prefix_len(const char *text, size_t max_chars) {
    size_t chars = 0;
    const char *p = text;
    while (*p != '\0' && chars < max_chars) {
        unsigned char c = (unsigned char)*p;
        size_t step = (c < 0x80) ? 1 : ((c >> 5) == 0x06) ? 2 : ((c >> 4) == 0x0E) ? 3 : 4;
        for (size_t i = 1; i < step && p[i] != '\0'; i++) {
        }
        p += step;
        chars++;
    }
    return (size_t)(p - text);
}

// 常驻显示:直到下一轮开口才被替换。
static void set_sticky(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_detail_sticky, sizeof(s_detail_sticky), fmt, args);
    va_end(args);
    // 版面宽 212 px、16 px 字号 ≈ 13 字/行 × 4 行,留一点余量按 40 字符截。
    size_t keep = utf8_prefix_len(s_detail_sticky, 40);
    s_detail_sticky[keep] = '\0';
    s_sticky = true;
    ESP_LOGI(TAG, "%s", s_detail_sticky);
}

static const char *current_detail(void) {
    return s_sticky ? s_detail_sticky : s_detail;
}

static const char *phase_title(app_phase_t phase) {
    switch (phase) {
        case APP_PHASE_BOOT:
            return "BOOT";
        case APP_PHASE_CONFIG:
            return "NEED CONFIG";
        case APP_PHASE_NET:
            return "NETWORK";
        case APP_PHASE_SYNC:
            return "SYNC";
        case APP_PHASE_IDLE:
            return "READY";
        case APP_PHASE_RECORD:
            return "RECORDING";
        case APP_PHASE_THINK:
            return "TRANSCRIBE";
        case APP_PHASE_CONFIRM:
            return "PUBLISH?";
        case APP_PHASE_PLAY:
            return "PLAYING";
        case APP_PHASE_ERROR:
            return "ERROR";
        default:
            return "?";
    }
}

static const char *place_label_ascii(uint8_t idx) {
    switch (idx) {
        case 0:
            return "P1 MOLI";
        case 1:
            return "P2 AI-ISLAND";
        case 2:
            return "P3 JINKE RD";
        default:
            return "P?";
    }
}

static void refresh_view(void) {
    bool has_content = s_phase != APP_PHASE_BOOT && s_phase != APP_PHASE_CONFIG;
    app_ui_set_backlight(has_content);
    app_view_t view;
    memset(&view, 0, sizeof(view));
    view.phase = s_phase;
    snprintf(view.line1, sizeof(view.line1), "%s", phase_title(s_phase));
    snprintf(view.line2, sizeof(view.line2), "%s", s_action);
    snprintf(view.place, sizeof(view.place), "%s", place_label_ascii(s_cfg.place_idx));
    snprintf(view.detail, sizeof(view.detail), "%s", current_detail());
    if (!s_linx_bound && s_activation[0] != '\0') {
        snprintf(view.line2, sizeof(view.line2), "BIND CODE %s", s_activation);
    }
    view.quiet = s_quiet;
    view.sonar_ok = s_sonar_ok;
    view.linx_ok = s_linx_ok;
    view.wifi_ok = sonar_net_connected();
    view.pending = s_pending;
    view.battery = s_battery;
    app_ui_update(&view);
}

// ---------------------------------------------------------------- 播放队列

static bool play_busy(void) {
    return s_playing || (s_play_q != NULL && uxQueueMessagesWaiting(s_play_q) > 0);
}

static void finish_current(bool completed, bool interrupted) {
    if (!s_playing) {
        return;
    }
    if (s_current.ack_event[0] != '\0' && s_current.delivery_id[0] != '\0') {
        // completed=false 有两种:用户按键打断(回 stopped/acked)、播放本身失败(回 failed)。
        const char *event = completed ? s_current.ack_event
                                      : (interrupted ? sonar_policy_ack_for_stop(s_quiet)
                                                     : "failed");
        ESP_LOGI(TAG, "回执: 播完=%d 按键打断=%d -> %s (delivery=%s)", (int)completed,
                 (int)interrupted, event == NULL ? "none" : event, s_current.delivery_id);
        if (event != NULL) {
            API_LOCK();
            esp_err_t err = sonar_ack(&s_client, s_current.delivery_id, event,
                                      s_current.request_id);
            API_UNLOCK();
            if (err != ESP_OK) {
                set_detail("ack %s failed (no replay)", event);
            }
        }
    }
    if (completed && s_current.ctx_post_id[0] != '\0') {
        // 真播完才写 context:后续追问要关联"实际听到的那条"。
        API_LOCK();
        sonar_session_context(&s_client, s_current.ctx_post_id);
        API_UNLOCK();
    }
    if (s_current.path[0] != '\0') {
        sonar_store_remove(s_current.path);
    }
    memset(&s_current, 0, sizeof(s_current));
    s_playing = false;
    if (s_phase == APP_PHASE_PLAY) {
        set_phase(APP_PHASE_IDLE, "ready");
    }
}

// 统一入队口:盖入队时间戳,方便日志区分"排队久"和"播放久"。
static esp_err_t enqueue_play(play_req_t *req) {
    if (s_play_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    req->queued_ms = now_ms();
    return xQueueSend(s_play_q, req, pdMS_TO_TICKS(200)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t enqueue_mem(const uint8_t *data, size_t len, const char *ack_event,
                             const sonar_delivery_t *delivery) {
    play_req_t req;
    memset(&req, 0, sizeof(req));
    req.is_mem = 1;
    req.data = data;
    req.len = len;
    if (ack_event != NULL) {
        snprintf(req.ack_event, sizeof(req.ack_event), "%s", ack_event);
    }
    if (delivery != NULL) {
        snprintf(req.delivery_id, sizeof(req.delivery_id), "%s", delivery->id);
        snprintf(req.request_id, sizeof(req.request_id), "%s", delivery->request_id);
    }
    return enqueue_play(&req);
}

static esp_err_t enqueue_file(const char *path, const char *ack_event,
                              const sonar_delivery_t *delivery) {
    play_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.path, sizeof(req.path), "%s", path);
    if (ack_event != NULL) {
        snprintf(req.ack_event, sizeof(req.ack_event), "%s", ack_event);
    }
    if (delivery != NULL) {
        snprintf(req.delivery_id, sizeof(req.delivery_id), "%s", delivery->id);
        snprintf(req.request_id, sizeof(req.request_id), "%s", delivery->request_id);
    }
    return enqueue_play(&req);
}

// 用户按键停声:立即静音、回执 skipped、丢掉还没播的队列项。
static void stop_playback_now(void) {
    audio_stop();
    finish_current(false, true);
    play_req_t dropped;
    while (s_play_q != NULL && xQueueReceive(s_play_q, &dropped, 0) == pdTRUE) {
        if (dropped.path[0] != '\0') {
            sonar_store_remove(dropped.path);
        }
    }
    set_phase(APP_PHASE_IDLE, "stopped");
}

static void start_next_playback(void) {
    if (s_playing || s_play_q == NULL || !audio_idle()) {
        return;
    }
    play_req_t req;
    if (xQueueReceive(s_play_q, &req, 0) != pdTRUE) {
        return;
    }
    // 出声前才登记"已尝试":崩溃/重启后不自动重播,也不冒充成功。
    if (req.delivery_id[0] != '\0') {
        sonar_journal_mark_attempted(req.delivery_id);
    }
    esp_err_t err = req.is_mem ? audio_play_mem(req.data, req.len) : audio_play_file(req.path);
    if (req.queued_ms > 0) {
        ESP_LOGI(TAG, "[t] 入队→开播 %lld ms", (long long)(now_ms() - req.queued_ms));
    }
    ESP_LOGI(TAG, "开始播放 %s -> %s", req.is_mem ? "内存素材" : req.path,
             esp_err_to_name(err));
    if (err == ESP_OK && s_release_ms > 0 && !req.is_mem) {
        // 主指标:用户有效语音结束 → 第一个可听回答(I2S 开始出声)
        ESP_LOGI(TAG, "[t] 松手→首个可听帧 %lld ms", (long long)(now_ms() - s_release_ms));
        s_release_ms = 0;
    }
    if (err != ESP_OK) {
        if (req.path[0] != '\0') {
            sonar_store_remove(req.path);
        }
        if (req.ack_event[0] != '\0' && req.delivery_id[0] != '\0') {
            API_LOCK();
            sonar_ack(&s_client, req.delivery_id, "failed", req.request_id);
            API_UNLOCK();
        }
        return;
    }
    s_current = req;
    s_playing = true;
    s_play_started_ms = now_ms();
    set_phase(APP_PHASE_PLAY, req.delivery_id[0] != '\0' ? "voice" : "prompt");
}

// ---------------------------------------------------------------- 地点/位置

static void refresh_location_locked(void) {
    if (s_cfg.presence_mode == 0) {
        esp_err_t err = sonar_set_presence_simulated(&s_client, "zhonghuan_jinke", "demo_exit");
        set_detail(err == ESP_OK ? "presence simulated" : "presence failed");
        return;
    }
    uint8_t idx = s_cfg.place_idx;
    double lat = s_presets[idx].latitude;
    double lon = s_presets[idx].longitude;
    if (s_place_resolved[idx]) {
        lat = s_places[idx].latitude;
        lon = s_places[idx].longitude;
    }
    esp_err_t err = sonar_set_location(&s_client, lat, lon, 30.0, sonar_net_now(), "android_mock");
    if (err == ESP_OK) {
        set_detail("loc %.5f,%.5f", lat, lon);
    } else {
        set_detail("loc failed (allow_mock_location?)");
    }
}

static void refresh_location(void) {
    API_LOCK();
    refresh_location_locked();
    API_UNLOCK();
}

static void resolve_places_locked(void) {
    for (size_t i = 0; i < s_preset_count && i < 3; ++i) {
        sonar_place_t candidates[5];
        size_t count = 0;
        s_place_resolved[i] = false;
        if (sonar_places(&s_client, s_presets[i].query, candidates, 5, &count) != ESP_OK) {
            ESP_LOGW(TAG, "地点 %u 解析失败(%s)", (unsigned)i, s_presets[i].query);
            continue;
        }
        // 只接受离预设锚点 3 km 以内的候选:高德会返回同名异地 POI。
        double best = 1e9;
        bool found_preferred = false;
        for (size_t c = 0; c < count; ++c) {
            double d = sonar_geo_distance_m(s_presets[i].latitude, s_presets[i].longitude,
                                            candidates[c].latitude, candidates[c].longitude);
            ESP_LOGI(TAG, "候选 %u.%u %s (%.2f km)", (unsigned)i, (unsigned)c,
                     candidates[c].label, d / 1000.0);
            if (d > 3000.0) {
                continue;
            }
            bool preferred = s_presets[i].prefer != NULL &&
                             strstr(candidates[c].label, s_presets[i].prefer) != NULL;
            if (preferred && !found_preferred) {
                found_preferred = true;
                best = d;
                s_places[i] = candidates[c];
            } else if (!found_preferred && d < best) {
                best = d;
                s_places[i] = candidates[c];
            }
        }
        if (best <= 3000.0) {
            s_place_resolved[i] = true;
            ESP_LOGI(TAG, "地点 %u -> %s (%s), %.2f km", (unsigned)i, s_places[i].id,
                     s_places[i].label, best / 1000.0);
        } else {
            ESP_LOGW(TAG, "地点 %u 无就近候选(最近 %.1f km)", (unsigned)i, best / 1000.0);
        }
    }
}

// ---------------------------------------------------------------- 发布

static bool build_answer_metadata(void) {
    cJSON *m = cJSON_CreateObject();
    if (m == NULL) {
        return false;
    }
    cJSON_AddStringToObject(m, "client_request_id", s_pending_pub.client_request_id);
    cJSON_AddStringToObject(m, "kind", "answer");
    cJSON_AddStringToObject(m, "transcript", s_transcript);
    cJSON_AddStringToObject(m, "reply_to", s_pending_pub.question_id);
    cJSON_AddStringToObject(m, "invitation_id", s_pending_pub.invitation_id);
    char *text = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (text == NULL) {
        return false;
    }
    snprintf(s_metadata, sizeof(s_metadata), "%s", text);
    cJSON_free(text);
    return true;
}

static bool build_question_metadata(void) {
    cJSON *m = cJSON_CreateObject();
    if (m == NULL) {
        return false;
    }
    cJSON_AddStringToObject(m, "client_request_id", s_pending_pub.client_request_id);
    cJSON_AddStringToObject(m, "kind", "question");
    cJSON_AddStringToObject(m, "transcript", s_transcript);
    cJSON_AddStringToObject(m, "target_place_id", s_pending_pub.target_place_id);
    cJSON_AddStringToObject(m, "direction",
                            s_pending_pub.direction[0] != '\0' ? s_pending_pub.direction
                                                              : "unknown");
    char *text = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (text == NULL) {
        return false;
    }
    snprintf(s_metadata, sizeof(s_metadata), "%s", text);
    cJSON_free(text);
    return true;
}

static void publish_pending(void) {
    if (s_pending_pub.kind == PENDING_NONE) {
        return;
    }
    long size = sonar_store_file_size(SONAR_REC_PATH);
    if (size <= (long)SONAR_WAV_HEADER_BYTES) {
        set_phase(APP_PHASE_IDLE, "no audio");
        s_pending_pub.kind = PENDING_NONE;
        return;
    }
    bool ok = s_pending_pub.kind == PENDING_ANSWER ? build_answer_metadata()
                                                   : build_question_metadata();
    if (!ok) {
        set_phase(APP_PHASE_ERROR, "metadata failed");
        return;
    }
    char post_id[SONAR_ID_MAX] = {0};
    set_phase(APP_PHASE_THINK, "uploading");
    API_LOCK();
    esp_err_t err = sonar_create_draft(&s_client, s_metadata, SONAR_REC_PATH, (size_t)size, post_id,
                                       sizeof(post_id));
    API_UNLOCK();
    if (err != ESP_OK || post_id[0] == '\0') {
        set_phase(APP_PHASE_ERROR, "draft failed (kept)");
        set_detail("draft err %s", esp_err_to_name(err));
        return;
    }
    sonar_journal_set_draft(post_id, s_pending_pub.client_request_id);
    char question_id[SONAR_ID_MAX] = {0};
    char question_status[24] = {0};
    API_LOCK();
    err = sonar_confirm(&s_client, post_id, question_id, sizeof(question_id), question_status,
                        sizeof(question_status));
    API_UNLOCK();
    if (err != ESP_OK) {
        set_phase(APP_PHASE_ERROR, "confirm failed");
        set_detail("confirm err %s", esp_err_to_name(err));
        return;
    }
    if (s_pending_pub.kind == PENDING_ANSWER) {
        set_phase(APP_PHASE_IDLE, "answer published");
        set_detail("answer -> %s", s_pending_pub.question_id);
        xSemaphoreTake(s_inv_lock, portMAX_DELAY);
        s_has_invitation = false;
        memset(&s_invitation, 0, sizeof(s_invitation));
        xSemaphoreGive(s_inv_lock);
    } else {
        set_phase(APP_PHASE_IDLE, "question published");
        set_detail("question %s", question_status);
        snprintf(s_last_question, sizeof(s_last_question), "%s", question_id);
        sonar_journal_set_last_question(s_last_question);
    }
    s_pending_pub.kind = PENDING_NONE;
}

// ---------------------------------------------------------------- 轮询(网络任务)

static void lock_invitation(const sonar_delivery_t *delivery) {
    xSemaphoreTake(s_inv_lock, portMAX_DELAY);
    s_invitation = *delivery;
    s_has_invitation = true;
    xSemaphoreGive(s_inv_lock);
    // 提示音 + 滴 + 问题原声,均入队;alerted 回执挂在提示音完成上。
    enqueue_mem(prompt_wav_start, (size_t)(prompt_wav_end - prompt_wav_start), "alerted",
                delivery);
    enqueue_mem(beep_wav_start, (size_t)(beep_wav_end - beep_wav_start), NULL, NULL);
    if (delivery->question_audio_url[0] != '\0') {
        const char *post_id = strrchr(delivery->question_audio_url, '/');
        post_id = post_id == NULL ? delivery->question_audio_url : post_id + 1;
        API_LOCK();
        size_t len = 0;
        esp_err_t err = sonar_download_audio(&s_client, post_id, SONAR_Q_PATH, &len);
        API_UNLOCK();
        if (err == ESP_OK) {
            enqueue_file(SONAR_Q_PATH, NULL, NULL);
        }
    }
    set_detail("invite %s", delivery->question_id);
    set_phase(APP_PHASE_IDLE, "hold OK to answer");
}

static void poll_inbox_locked(void) {
    size_t count = 0;
    // 语音连接常驻会占着 ~15KB;若轮到 HTTP 时内存不够,先把语音连接让出来再试一次。
    if (linx_ws_is_open() && esp_get_free_heap_size() < 24000) {
        ESP_LOGW(TAG, "内存紧张,先释放常驻语音连接 (heap=%u)",
                 (unsigned)esp_get_free_heap_size());
        linx_ws_close();
    }
    // 有会话就取会话收件箱:它包含真人回答与配对闲聊(chat),也可按 chatd 前缀给 ID。
    esp_err_t err = sonar_session_inbox(&s_client, s_inbox, SONAR_MAX_DELIVERIES, &count);
    if (err != ESP_OK) {
        err = sonar_inbox(&s_client, s_inbox, SONAR_MAX_DELIVERIES, &count);
    }
    if (err != ESP_OK) {
        s_sonar_ok = false;
        set_detail("inbox err %s", esp_err_to_name(err));
        return;
    }
    s_sonar_ok = true;
    s_pending = (int)count;
    ESP_LOGI(TAG, "inbox n=%u", (unsigned)count);
    for (size_t i = 0; i < count; ++i) {
        ESP_LOGI(TAG, "inbox %u/%u type=%s state=%s actionable=%d reason=%s post=%s",
                 (unsigned)i + 1, (unsigned)count, s_inbox[i].type, s_inbox[i].state,
                 s_inbox[i].actionable, s_inbox[i].reason_code, s_inbox[i].post_id);
        sonar_delivery_t *item = &s_inbox[i];
        if (!item->actionable) {
            continue;
        }
        bool has_inv = false;
        xSemaphoreTake(s_inv_lock, portMAX_DELAY);
        has_inv = s_has_invitation;
        xSemaphoreGive(s_inv_lock);
        if (strcmp(item->type, "invitation") == 0 && strcmp(item->state, "pending") == 0 &&
            !has_inv) {
            lock_invitation(item);
            return;
        }
        if (strcmp(item->type, "audio") == 0 && strcmp(item->state, "pending") == 0) {
            if (item->post_id[0] == '\0') {
                sonar_ack(&s_client, item->id, "failed", item->request_id);
                continue;
            }
            sonar_gate_t gate = sonar_policy_delivery_gate(
                item->actionable, item->state, item->expires_at, sonar_net_now(), s_client.revision,
                item->revision, true, s_quiet, play_busy());
            if (gate != SONAR_GATE_OK) {
                ESP_LOGI(TAG, "delivery %s gated: %s", item->id, sonar_policy_gate_reason(gate));
                continue;
            }
            if (sonar_journal_attempted(item->id)) {
                continue;  // 尝试过但结果未知:不自动重播
            }
            size_t len = 0;
            ESP_LOGI(TAG, "原声投递 %s post=%s kind=%s url=%s", item->id, item->post_id,
                     item->kind, item->audio_url);
            esp_err_t derr = ESP_OK;
            if (item->audio_url[0] != '\0') {
                // 用服务端返回的地址(chat 是 /v1/chat/audio/{mid},回答是 /v1/audio/{pid})。
                derr = sonar_session_download(&s_client, item->audio_url, SONAR_PLAY_PATH, &len);
            } else {
                derr = sonar_download_audio(&s_client, item->post_id, SONAR_PLAY_PATH, &len);
            }
            if (derr != ESP_OK) {
                ESP_LOGW(TAG, "原声下载失败,回执 failed (delivery=%s)", item->id);
                sonar_ack(&s_client, item->id, "failed", item->request_id);
                continue;
            }
            ESP_LOGI(TAG, "原声已下载 %u 字节,入队播放 (delivery=%s)", (unsigned)len, item->id);
            play_req_t req = {.is_mem = 0, .data = NULL, .len = 0};
            snprintf(req.path, sizeof(req.path), "%s", SONAR_PLAY_PATH);
            snprintf(req.ack_event, sizeof(req.ack_event), "%s", "played");
            snprintf(req.delivery_id, sizeof(req.delivery_id), "%s", item->id);
            snprintf(req.request_id, sizeof(req.request_id), "%s", item->request_id);
            snprintf(req.ctx_post_id, sizeof(req.ctx_post_id), "%s", item->post_id);
            enqueue_play(&req);
            return;  // 一次只处理一条
        }
    }
}

static void poll_question_locked(void) {
    if (s_last_question[0] == '\0') {
        return;
    }
    sonar_question_t q;
    if (sonar_question(&s_client, s_last_question, &q) != ESP_OK) {
        return;
    }
    if (strcmp(q.status, "answered") == 0) {
        set_detail("answered, waiting voice");
    } else if (strcmp(q.status, "waiting") == 0) {
        if (q.expires_at > 0 && q.expires_at <= sonar_net_now()) {
            set_detail("question expired");
            s_last_question[0] = '\0';
        } else {
            set_detail("waiting for answer");
        }
    } else {
        set_detail("question %s", q.status);
        s_last_question[0] = '\0';
    }
}

static void net_task(void *arg) {
    (void)arg;
    int64_t next_me = 0, next_linx = 0, next_inbox = 0, next_q = 0, next_loc = 0, next_wifi = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(300));
        int64_t now = now_ms();
        if (!sonar_net_connected()) {
            if (now >= next_wifi) {
                ESP_LOGW(TAG, "Wi-Fi 未连接,重试");
                sonar_net_reconnect();
                next_wifi = now + 5000;
            }
            continue;
        }
        if (!s_sonar_ok) {
            if (now >= next_me) {
                API_LOCK();
                esp_err_t err = sonar_me(&s_client);
                if (err == ESP_OK) {
                    resolve_places_locked();
                    refresh_location_locked();
                }
                API_UNLOCK();
                if (err == ESP_OK) {
                    s_sonar_ok = true;
                    s_quiet = s_client.quiet;
                    set_phase(APP_PHASE_IDLE, "online");
                    set_detail("sonar online, heap %u", (unsigned)esp_get_free_heap_size());
                    ESP_LOGI(TAG, "sonar online heap=%u 复位原因=%d",
                             (unsigned)esp_get_free_heap_size(), (int)s_reset_reason);
                } else {
                    next_me = now + 5000;
                }
            }
            continue;
        }
        if (!s_linx_bound && now >= next_linx) {
            linx_ota_result_t ota;
            if (linx_ota(&s_cfg, &ota) == ESP_OK) {
                sonar_config_save(&s_cfg);
                s_linx_bound = ota.bound;
                s_linx_ok = ota.bound;
                if (!ota.bound) {
                    snprintf(s_activation, sizeof(s_activation), "%s", ota.code);
                    app_ui_show_activation(ota.code);
                    set_detail("bind code %s", ota.code);
                } else {
                    set_detail("linx bound");
                    linx_probe(&s_cfg);
#ifdef SONAR_STT_SELFTEST
        // 临时自检:用内置 prompt.wav 走一遍完整 STT,不依赖按键。
        {
            // 优先复放最近一次真人录音,没有就用内置提示音。
            const char *src = "/spiffs/stt_selftest.wav";
            long rec = sonar_store_file_size(SONAR_REC_PATH);
            if (rec > (long)SONAR_WAV_HEADER_BYTES) {
                src = SONAR_REC_PATH;
                ESP_LOGI(TAG, "自检用上次录音 %ld 字节", rec);
            } else {
                FILE *f = fopen(src, "wb");
                if (f != NULL) {
                    fwrite(prompt_wav_start, 1, (size_t)(prompt_wav_end - prompt_wav_start), f);
                    fclose(f);
                }
            }
            if (true) {
                static char text[256];
                esp_err_t e = linx_speech_to_text(&s_cfg, src, text, sizeof(text), 30000);
                if (e == ESP_OK) {
                    ESP_LOGI(TAG, "自检 STT 结果: %s", text);
                } else {
                    ESP_LOGW(TAG, "自检 STT 失败: %s", esp_err_to_name(e));
                }
            }
        }
#endif

                }
            }
            next_linx = now + 15000;
            continue;
        }
        // 语音会话期间必须独占网络与堆:录音上传是 20 ms 一帧的持续写,
        // 同时再发一条 HTTP 会直接触发分配失败,把音频帧一起带崩。
        bool busy = play_busy() || audio_record_active() || s_phase == APP_PHASE_THINK ||
                    s_phase == APP_PHASE_CONFIRM || s_phase == APP_PHASE_SYNC;
        if (busy) {
            next_loc = now + 5000;
            next_inbox = now + 1000;
            next_q = now + 2000;
            continue;
        }
        if (now >= next_loc) {
            refresh_location();
            next_loc = now + 45000;
            continue;
        }
        if (!busy && now >= next_inbox) {
            API_LOCK();
            poll_inbox_locked();
            API_UNLOCK();
            next_inbox = now + 1000;
            continue;
        }
        if (now >= next_q) {
            API_LOCK();
            poll_question_locked();
            API_UNLOCK();
            next_q = now + 2000;
        }
    }
}

// ---------------------------------------------------------------- 语音轮次

static void handle_transcript(void) {
    sonar_place_hint_t hints[3];
    for (size_t i = 0; i < s_preset_count && i < 3; ++i) {
        hints[i].label = s_presets[i].label;
        hints[i].alias = s_presets[i].alias;
    }
    int matched = sonar_match_place(s_transcript, hints,
                                    s_preset_count < 3 ? s_preset_count : 3);
    bool has_inv = false;
    xSemaphoreTake(s_inv_lock, portMAX_DELAY);
    has_inv = s_has_invitation && s_invitation.expires_at > sonar_net_now();
    if (has_inv) {
        snprintf(s_pending_pub.question_id, sizeof(s_pending_pub.question_id), "%s",
                 s_invitation.question_id);
        snprintf(s_pending_pub.invitation_id, sizeof(s_pending_pub.invitation_id), "%s",
                 s_invitation.id);
    }
    xSemaphoreGive(s_inv_lock);

    sonar_decision_input_t input = {
        .has_invitation = has_inv,
        .place_matched = matched >= 0,
        .question_like = sonar_text_is_question(s_transcript),
        .quiet = s_quiet,
    };
    const char *reason = NULL;
    sonar_action_t action = sonar_decide(&input, &reason);
    ESP_LOGI(TAG, "decision=%s reason=%s place=%d", sonar_action_name(action), reason, matched);
    if (action == SONAR_ACTION_NONE) {
        set_phase(APP_PHASE_IDLE, "quiet: not published");
        return;
    }
    if (action == SONAR_ACTION_PRIVATE) {
        set_phase(APP_PHASE_THINK, "assistant");
        char reply[128] = {0};
        // 灵矽只在绑定智控台智能体后才会吐 LLM/TTS;没配好时 12 s 就放弃,
        // 别让界面卡在 assistant 上半分钟(实测服务端只回 stt 后就不出声了)。
        esp_err_t err =
            linx_chat(&s_cfg, SONAR_REC_PATH, SONAR_TTS_PATH, reply, sizeof(reply), 12000);
        if (err == ESP_OK) {
            enqueue_file(SONAR_TTS_PATH, NULL, NULL);
        } else {
            set_detail("assistant: no reply from linx");
            sonar_store_remove(SONAR_TTS_PATH);
        }
        set_phase(APP_PHASE_IDLE, "private, nothing public");
        return;
    }
    memset(&s_pending_pub, 0, sizeof(s_pending_pub));
    sonar_config_new_uuid(s_pending_pub.client_request_id);
    if (action == SONAR_ACTION_ANSWER) {
        s_pending_pub.kind = PENDING_ANSWER;
        set_phase(APP_PHASE_CONFIRM, "OK: send answer");
        set_detail("answer to %s", s_pending_pub.question_id);
    } else {
        // 语音里说到的地名优先:说「模力社区」不该因为上次按 UP 停在别处就发到别的点位。
        uint8_t idx = matched >= 0 ? (uint8_t)matched : s_cfg.place_idx;
        if (matched >= 0 && s_cfg.place_idx != idx) {
            s_cfg.place_idx = idx;
            sonar_config_save(&s_cfg);
            refresh_location_locked();  // 把位置也切到问的这个点,附近的人才能收到
        }
        s_pending_pub.kind = PENDING_QUESTION;
        if (!s_place_resolved[idx]) {
            if (idx != 2) {
                set_phase(APP_PHASE_IDLE, "place unknown (check AMap)");
                set_detail("place %u unresolved", (unsigned)idx);
                s_pending_pub.kind = PENDING_NONE;
                return;
            }
            snprintf(s_pending_pub.target_place_id, sizeof(s_pending_pub.target_place_id), "%s",
                     "zhonghuan_jinke");
            snprintf(s_pending_pub.direction, sizeof(s_pending_pub.direction), "%s", "demo_exit");
        } else {
            snprintf(s_pending_pub.target_place_id, sizeof(s_pending_pub.target_place_id), "%s",
                     s_places[idx].id);
            snprintf(s_pending_pub.direction, sizeof(s_pending_pub.direction), "%s", "unknown");
        }
        set_phase(APP_PHASE_CONFIRM, "OK: send question");
        set_detail("ask %s", s_places[idx].label);
    }
    if (s_cfg.auto_publish) {
        publish_pending();
    }
}

// 堆探针:每一步都打一次 free/largest,用来定位内存在哪个环节没回来。
// 会话模式的目标地点:配置里的远程点位优先(与作者所在地分离),否则用解析结果。
static const char *session_place_id(void) {
    if (s_cfg.remote_place_id[0] != '\0') {
        return s_cfg.remote_place_id;
    }
    uint8_t idx = s_cfg.place_idx < 3 ? s_cfg.place_idx : 0;
    return s_place_resolved[idx] ? s_places[idx].id : NULL;
}

// 一句话走会话接口:转写 + 音频 → 结构化结果,按 voice_action 播放,不自己发问题。
// 返回 ESP_OK 表示这一轮已由会话路径处理完(不再走旧的 drafts/confirm 流程)。
// 服务端每句话都独立理解,不保留对话状态(实测:问"模力社区可以停车吗"→被反问,
// 回"要"→它又问"你说的是哪个地点")。所以被反问后,把上一句和这一句拼成一个
// utterance 发出去——两句都是用户自己说的,不改写、不臆造。
static char s_prev_utterance[SONAR_TEXT_MAX];
static bool s_clarify_pending;

static bool session_turn(const char *transcript) {
    const char *place = session_place_id();
    if (place == NULL) {
        ESP_LOGW(TAG, "会话缺少目标地点,回退到单人流程");
        return false;
    }
    char merged[SONAR_TEXT_MAX * 2];
    const char *utterance = transcript;
    if (s_clarify_pending && s_prev_utterance[0] != '\0') {
        snprintf(merged, sizeof(merged), "%s %s", s_prev_utterance, transcript);
        utterance = merged;
        ESP_LOGI(TAG, "补上被反问前的原话: %s", merged);
    }
    int64_t t0 = now_ms();
    API_LOCK();
    esp_err_t err = sonar_session_ensure(&s_client, &s_cfg, place);
    int64_t t1 = now_ms();
    if (err == ESP_OK) {
        sonar_turn_t turn;
        err = sonar_session_utterance(&s_client, SONAR_REC_PATH,
                                      (size_t)sonar_store_file_size(SONAR_REC_PATH), utterance,
                                      &turn);
        int64_t t2 = now_ms();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "[t] 会话 %lld ms, utterance %lld ms", (long long)(t1 - t0),
                     (long long)(t2 - t1));
            ESP_LOGI(TAG,
                     "turn %s status=%s action=%s reason=%s task=%s chat=%s conv=%s items=%u",
                     turn.turn_id, turn.status, turn.action, turn.reason, turn.task_id,
                     turn.chat_id, turn.conversation_id, (unsigned)turn.item_count);
            s_clarify_pending = false;
            s_prev_utterance[0] = '\0';
            if (strcmp(turn.action, "private_chat") == 0) {
                // 私人助手由灵矽负责:服务端对 private_chat 只回 outcome。
                // 首次语音调用里平台已经顺手生成了一份助手回复(音频 + 文本),
                // 正常情况下直接播它,不再建第二次连、不再重传同一段录音。
                set_phase(APP_PHASE_THINK, "assistant");
                char reply[128] = {0};
                uint32_t pcm = 0;
                esp_err_t cerr = ESP_OK;
                if (linx_assistant_take(reply, sizeof(reply), &pcm)) {
                    play_req_t pr = {.is_mem = 0, .data = NULL, .len = 0};
                    snprintf(pr.path, sizeof(pr.path), "%s", SONAR_ASSISTANT_PATH);
                    enqueue_play(&pr);
                } else {
                    cerr = linx_chat(&s_cfg, SONAR_REC_PATH, SONAR_TTS_PATH, reply,
                                     sizeof(reply), 12000);
                }
                if (cerr == ESP_OK) {
                    set_sticky("助手: %s", reply);
                    if (pcm > 0) {
                        // 音频已在上面入队(复用的首轮回复),这里不再重复入队
                    } else {
                        play_req_t pr = {.is_mem = 0, .data = NULL, .len = 0};
                        snprintf(pr.path, sizeof(pr.path), "%s", SONAR_TTS_PATH);
                        enqueue_play(&pr);
                    }
                } else {
                    set_detail("assistant: no reply from linx");
                    sonar_store_remove(SONAR_TTS_PATH);
                }
                set_phase(APP_PHASE_IDLE, "private chat");
            } else if (turn.item_count > 0) {
                // 结构化动作优先:有音频就先放音频,agent 文本只当备注。
                sonar_voice_item_t *item = &turn.items[0];
                size_t len = 0;
                int64_t t3 = now_ms();
                esp_err_t derr =
                    sonar_session_download(&s_client, item->audio_url, SONAR_PLAY_PATH, &len);
                ESP_LOGI(TAG, "[t] 原声下载 %lld ms (%u 字节)", (long long)(now_ms() - t3),
                         (unsigned)len);
                if (derr == ESP_OK) {
                    ESP_LOGI(TAG, "turn 原声 %u 字节 post=%s kind=%s", (unsigned)len,
                             item->post_id, item->kind);
                    // 服务端按存储格式原样返回:历史素材是 RIFF/WAV,手机新录的是
                    // Ogg Opus。播放链路只认 16 kHz PCM,所以 Ogg 要先解码成 WAV。
                    uint8_t magic[4] = {0};
                    FILE *mf = fopen(SONAR_PLAY_PATH, "rb");
                    if (mf != NULL) {
                        if (fread(magic, 1, sizeof(magic), mf) != sizeof(magic)) {
                            memset(magic, 0, sizeof(magic));
                        }
                        fclose(mf);
                    }
                    const char *play_path = SONAR_PLAY_PATH;
                    if (memcmp(magic, "OggS", 4) == 0) {
                        uint32_t pcm = 0;
                        int64_t td = now_ms();
                        esp_err_t oerr =
                            sonar_ogg_decode_to_wav(SONAR_PLAY_PATH, SONAR_PLAY_PCM_PATH, &pcm);
                        ESP_LOGI(TAG, "[t] Ogg 解码 %lld ms (%u 字节 PCM)",
                                 (long long)(now_ms() - td), (unsigned)pcm);
                        if (oerr != ESP_OK) {
                            // 合同要求:不能把"解不了"当成播放成功,不发 played。
                            set_detail("answer format unsupported");
                            set_phase(APP_PHASE_IDLE, "answer format unsupported");
                            goto turn_done;
                        }
                        play_path = SONAR_PLAY_PCM_PATH;
                    } else if (memcmp(magic, "RIFF", 4) != 0) {
                        set_detail("answer format unknown");
                        set_phase(APP_PHASE_IDLE, "answer format unknown");
                        goto turn_done;
                    }
                    play_req_t req = {.is_mem = 0, .data = NULL, .len = 0};
                    snprintf(req.path, sizeof(req.path), "%s", play_path);
                    snprintf(req.ctx_post_id, sizeof(req.ctx_post_id), "%s", item->post_id);
                    enqueue_play(&req);
                    set_phase(APP_PHASE_IDLE, "playing answer");
                } else {
                    set_detail("turn audio failed %s", esp_err_to_name(derr));
                }
            } else if (strcmp(turn.action, "clarify") == 0 && turn.agent_text[0] != '\0') {
                // 反问只在屏幕上给出来;同时记住原话,用户下一句确认时合并重发。
                snprintf(s_prev_utterance, sizeof(s_prev_utterance), "%.*s",
                         (int)(sizeof(s_prev_utterance) - 1), utterance);
                s_clarify_pending = true;
                set_sticky("问: %s", turn.agent_text);
                // 必须回到可按键状态:停在 THINK 会让 OK 被 busy 判定吞掉。
                set_phase(APP_PHASE_IDLE, "answer the question");
            } else {
                if (strcmp(turn.status, "published") == 0) {
                    set_sticky("已发布,等真人回答");
                } else {
                    set_detail("turn %s %s", turn.status, turn.reason);
                }
                set_phase(APP_PHASE_IDLE, strcmp(turn.status, "published") == 0
                                              ? "published, waiting voice"
                                              : "waiting voice");
                // 服务端只给了 clarify/defer(例如 MODEL_UNCERTAIN)且没有音频动作时,
                // 回退到本机确定性规则发布提问,避免"说完什么都不发生"。
                if (strcmp(turn.action, "defer") == 0 || strcmp(turn.action, "clarify") == 0 ||
                    strcmp(turn.action, "failed") == 0) {
                    ESP_LOGW(TAG, "会话未给出音频动作(status=%s reason=%s),回退到本机规则",
                             turn.status, turn.reason);
                    API_UNLOCK();
                    return false;
                }
            }
        } else {
            set_detail("turn failed %s", esp_err_to_name(err));
        }
    } else {
        set_detail("session %s", esp_err_to_name(err));
    }
turn_done:
    API_UNLOCK();
    return err == ESP_OK;
}

static void voice_progress(const char *msg) {
    set_detail("%s", msg);
}

static void log_heap(const char *stage) {
    ESP_LOGI(TAG, "[heap] %-16s free=%u largest=%u", stage, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void stop_recording_and_process(void) {
    log_heap("record stop");
    uint32_t pcm_bytes = 0;
    bool truncated = false;
    if (audio_record_active()) {
        audio_record_stop(&pcm_bytes, &truncated, 3000);
    } else {
        long size = sonar_store_file_size(SONAR_REC_PATH);
        pcm_bytes =
            size > (long)SONAR_WAV_HEADER_BYTES ? (uint32_t)(size - SONAR_WAV_HEADER_BYTES) : 0;
    }
    s_ptt = false;
    if (pcm_bytes < 3200) {
        set_phase(APP_PHASE_IDLE, "too short");
        sonar_store_remove(SONAR_REC_PATH);
        return;
    }
    // 录音体检:字节数 + 峰值幅度。麦克风没采到声音时 STT 只会返回空转写,
    // 这里先把"有没有录到波形"和"转写失败"区分开。
    {
        FILE *fp = fopen(SONAR_REC_PATH, "rb");
        int peak = 0;
        if (fp != NULL) {
            if (fseek(fp, SONAR_WAV_HEADER_BYTES, SEEK_SET) == 0) {
                int16_t buf[256];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), fp)) >= sizeof(int16_t)) {
                    for (size_t i = 0; i + 1 < n; i += 2) {
                        int v = (int16_t)((uint16_t)buf[i / 2]);
                        if (v < 0) {
                            v = -v;
                        }
                        if (v > peak) {
                            peak = v;
                        }
                    }
                }
            }
            fclose(fp);
        }
        ESP_LOGI(TAG, "录音 %u 字节%s, 峰值 %d", (unsigned)pcm_bytes,
                 truncated ? "(已截断)" : "", peak);
    }
    int64_t t_rec_stop = now_ms();
    s_release_ms = t_rec_stop;
    set_phase(APP_PHASE_THINK, "transcribing");
    // TLS 握手要一块 5 KB 以上的连续内存,这里把可用堆和最大连续块记下来,
    // 握手失败时就能立刻判断是"内存不够"还是"证书/协议"问题。
    log_heap("before stt");
    s_transcript[0] = '\0';
    esp_err_t err = linx_speech_to_text(&s_cfg, SONAR_REC_PATH, s_transcript, sizeof(s_transcript),
                                       25000);
    log_heap("after stt");
    if (err != ESP_OK || s_transcript[0] == '\0') {
        ESP_LOGW(TAG, "STT 未给出文本: err=%s, buf=\"%s\"", esp_err_to_name(err),
                 s_transcript);
        set_phase(APP_PHASE_IDLE, "no transcript (voice kept)");
        set_sticky("识别失败 %s", esp_err_to_name(err));
        return;
    }
    int64_t t_stt = now_ms();
    ESP_LOGI(TAG, "转写: %s", s_transcript);
    // 地名归一化:ASR 把「模力」写成「茉莉」时,服务端会地理编码到完全不同的 POI。
    // 这里只替换**别名表里已知的**口语写法,不改动其他内容。
    {
        char normalized[SONAR_TEXT_MAX];
        if (sonar_place_normalize(s_transcript, normalized, sizeof(normalized)) > 0 &&
            strcmp(normalized, s_transcript) != 0) {
            ESP_LOGW(TAG, "地名归一化: %s -> %s", s_transcript, normalized);
            snprintf(s_transcript, sizeof(s_transcript), "%s", normalized);
        }
    }
    set_sticky("我说: %s", s_transcript);
    // 多轮会话优先:一句话 = 一个 utterance,由服务端决定私聊回答、发布提问还是配对闲聊。
    // 会话不可用(没地点/未建会话/传输失败)时才回退到单轮的 drafts/confirm 流程。
    if (session_turn(s_transcript)) {
        ESP_LOGI(TAG, "[t] 录音结束→转写 %lld ms, 转写→服务端结果 %lld ms", (long long)(t_stt - t_rec_stop),
                 (long long)(now_ms() - t_stt));
        key_event_t stale2;
        while (xQueueReceive(s_keys, &stale2, 0) == pdTRUE) {
        }
        return;
    }
    handle_transcript();
    // 转写/上传期间积压的按键事件属于上一条录音,丢掉以免误触发新录音。
    key_event_t stale;
    while (xQueueReceive(s_keys, &stale, 0) == pdTRUE) {
    }
}

static void begin_ptt(void) {
    if (s_phase == APP_PHASE_CONFIRM) {
        publish_pending();
        return;
    }
    if (s_phase == APP_PHASE_PLAY || s_playing || play_busy()) {
        stop_playback_now();
        return;
    }
    if (s_phase == APP_PHASE_THINK || s_phase == APP_PHASE_SYNC) {
        return;
    }
    if (!audio_idle()) {
        return;
    }
    sonar_store_remove(SONAR_REC_PATH);
    log_heap("before record");
    if (audio_record_start(SONAR_REC_PATH) == ESP_OK) {
        s_ptt = true;
        s_sticky = false;   // 新的一轮开始,清掉上一轮常驻信息
        set_phase(APP_PHASE_RECORD, "release to send");
        linx_prewarm(&s_cfg);   // 边说边握手,松手后直接发音频
        set_detail("recording (8s max)");
    } else {
        set_phase(APP_PHASE_ERROR, "mic busy");
    }
}

// ---------------------------------------------------------------- 按键

static void switch_place(void) {
    s_cfg.place_idx = (uint8_t)((s_cfg.place_idx + 1) % s_preset_count);
    sonar_config_save(&s_cfg);
    set_detail("place %s", place_label_ascii(s_cfg.place_idx));
    refresh_location();
    refresh_view();
}

static void toggle_quiet(void) {
    s_quiet = !s_quiet;
    if (s_quiet) {
        stop_playback_now();
    }
    API_LOCK();
    sonar_set_mode(&s_client, s_quiet);
    API_UNLOCK();
    set_detail(s_quiet ? "quiet on" : "quiet off");
    refresh_view();
}

static void resync(void) {
    set_phase(APP_PHASE_SYNC, "resync");
    API_LOCK();
    esp_err_t err = sonar_me(&s_client);
    if (err == ESP_OK) {
        s_sonar_ok = true;
        s_quiet = s_client.quiet;
        resolve_places_locked();
        refresh_location_locked();
    }
    API_UNLOCK();
    set_phase(APP_PHASE_IDLE, err == ESP_OK ? "resynced" : "resync failed");
}

static void handle_key(const key_event_t *event) {
    if (event->btn == BSP_BTN_OK) {
        if (event->ev == BSP_BTN_PRESS) {
            begin_ptt();
        } else if (event->ev == BSP_BTN_RELEASE && s_ptt) {
            stop_recording_and_process();
        }
        return;
    }
    if (event->btn == BSP_BTN_UP) {
        if (event->ev == BSP_BTN_CLICK) {
            switch_place();
        } else if (event->ev == BSP_BTN_LONG) {
            resync();
        }
        return;
    }
    if (event->btn == BSP_BTN_DOWN) {
        if (event->ev == BSP_BTN_CLICK) {
            toggle_quiet();
        } else if (event->ev == BSP_BTN_LONG && s_last_question[0] != '\0') {
            API_LOCK();
            esp_err_t err = sonar_cancel_question(&s_client, s_last_question);
            API_UNLOCK();
            if (err == ESP_OK) {
                set_detail("question cancelled");
                s_last_question[0] = '\0';
            } else {
                set_detail("cancel failed");
            }
        }
    }
}

static void key_callback(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    key_event_t event = {.btn = btn, .ev = ev};
    if (s_keys != NULL) {
        xQueueSend(s_keys, &event, 0);
    }
}

// ---------------------------------------------------------------- 启动

static bool setup_network_and_account(void) {
    set_phase(APP_PHASE_NET, "wifi...");
    if (sonar_net_start(&s_cfg) != ESP_OK) {
        set_phase(APP_PHASE_ERROR, "wifi start failed");
        return false;
    }
    {
        time_t t0 = time(NULL);
        struct tm tmv;
        localtime_r(&t0, &tmv);
        char b[32] = {0};
        strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv);
        ESP_LOGI(TAG, "[time] 首次校时返回前 UTC=%lld (%s)", (long long)t0, b);
    }
    if (sonar_net_wait_time(15000) != ESP_OK) {
        set_detail("time not synced (TLS may fail)");
    }
    set_phase(APP_PHASE_SYNC, "sonar...");
    API_LOCK();
    esp_err_t err = sonar_me(&s_client);
    if (err == ESP_OK) {
        resolve_places_locked();
        refresh_location_locked();
    }
    API_UNLOCK();
    if (err != ESP_OK) {
        s_sonar_ok = false;
        set_phase(APP_PHASE_ERROR, "sonar unreachable");
        return false;
    }
    s_sonar_ok = true;
    s_quiet = s_client.quiet;

    // 每次启动都做一次 WSS 探针(hello 往返,不发录音),把语音链路问题在
    // 按键之前就暴露出来。
    if (s_cfg.linx_token[0] != '\0') {
        ESP_LOGI(TAG, "启动 WSS 探针(有 token)");
        linx_probe(&s_cfg);
    } else {
        ESP_LOGW(TAG, "启动 WSS 探针跳过:没有 linx token");
    }
    linx_ota_result_t ota;
    if (linx_ota(&s_cfg, &ota) == ESP_OK) {
        sonar_config_save(&s_cfg);
        s_linx_bound = ota.bound;
        s_linx_ok = ota.bound;
        if (!ota.bound) {
            snprintf(s_activation, sizeof(s_activation), "%s", ota.code);
            app_ui_show_activation(ota.code);
            set_detail("bind code %s", ota.code);
        }
    } else {
        set_detail("linx ota failed");
    }
    set_phase(APP_PHASE_IDLE, "ready");
    return true;
}

static void app_task(void *arg) {
    (void)arg;
    s_presets = sonar_place_presets(&s_preset_count);

    app_ui_init();  // 先建状态页:没配置时也要有黑底画面,而不是面板白底
    // 弄清"它为什么重启":掉电/看门狗/panic 的处理方式完全不同。
    ESP_LOGI(TAG, "固件 %s / IDF %s", esp_app_get_description()->version,
             esp_app_get_description()->idf_ver);
    {
        esp_reset_reason_t rr = esp_reset_reason();
        const char *name = "unknown";
        switch (rr) {
        case ESP_RST_POWERON: name = "poweron"; break;
        case ESP_RST_EXT: name = "external"; break;
        case ESP_RST_SW: name = "software"; break;
        case ESP_RST_PANIC: name = "panic"; break;
        case ESP_RST_INT_WDT: name = "int_wdt"; break;
        case ESP_RST_TASK_WDT: name = "task_wdt"; break;
        case ESP_RST_WDT: name = "wdt"; break;
        case ESP_RST_BROWNOUT: name = "brownout"; break;
        case ESP_RST_SDIO: name = "sdio"; break;
        case ESP_RST_DEEPSLEEP: name = "deepsleep"; break;
        default: break;
        }
        s_reset_reason = rr;
        ESP_LOGW(TAG, "复位原因: %s(%d)", name, (int)rr);
    }
    if (sonar_config_load(&s_cfg) != ESP_OK) {
        set_phase(APP_PHASE_ERROR, "config read failed");
    }
    sonar_config_seed(&s_cfg);
    if (!sonar_config_has_wifi(&s_cfg) || !sonar_config_has_sonar(&s_cfg)) {
        set_phase(APP_PHASE_CONFIG, "wifi/token missing");
        set_detail("fill local_secrets.h and reflash");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    sonar_client_init(&s_client, &s_cfg);
    linx_client_init();
    linx_set_progress(voice_progress);

    if (sonar_store_mount() != ESP_OK) {
        set_phase(APP_PHASE_ERROR, "storage mount failed");
    }
    if (audio_init() != ESP_OK) {
        set_phase(APP_PHASE_ERROR, "audio init failed");
    }

    // 临时自检:开机放一声内置 beep,验证 ES8311/功放/I2S 通路是否真的出声。
    // 听不到说明是音频硬件路径问题,不是投递或回执问题。
    {
        esp_err_t perr = audio_play_mem(beep_wav_start, (size_t)(beep_wav_end - beep_wav_start));
        bool done = false;
        audio_wait_playback_done(3000, &done);
        ESP_LOGI(TAG, "开机提示音: 入队=%s 播完=%d heap=%u", esp_err_to_name(perr), (int)done,
                 (unsigned)esp_get_free_heap_size());
    }

    s_api_lock = xSemaphoreCreateMutex();
    s_inv_lock = xSemaphoreCreateMutex();
    s_play_q = xQueueCreate(4, sizeof(play_req_t));
    if (s_api_lock == NULL || s_inv_lock == NULL || s_play_q == NULL) {
        set_phase(APP_PHASE_ERROR, "sync primitives failed");
    }
    // 旧固件在"播放其实没出声"时也写过 attempted 记录;回执语义已改为"真播完才 played",
    // 这些记录不可信,开机清一次,否则待播原声会被永久跳过。
    {
        nvs_handle_t hj;
        // journal 自己的命名空间是 sonar_journal,键是 attempted
        if (nvs_open("sonar_journal", NVS_READWRITE, &hj) == ESP_OK) {
            uint8_t done = 0;
            if (nvs_get_u8(hj, "journal_reset_v2", &done) != ESP_OK || done == 0) {
                nvs_erase_key(hj, "attempted");
                nvs_set_u8(hj, "journal_reset_v2", 1);
                nvs_commit(hj);
                ESP_LOGW(TAG, "清理旧的播放去重记录(音量修复前写入)");
            }
            nvs_close(hj);
        }
    }
    if (sonar_journal_last_question(s_last_question, sizeof(s_last_question))) {
        ESP_LOGI(TAG, "恢复上次问题 %s", s_last_question);
    }

    setup_network_and_account();
    if (xTaskCreate(net_task, "sonar_net", 10240, NULL, 4, NULL) != pdPASS) {
        set_phase(APP_PHASE_ERROR, "net task failed");
    }
    s_battery = bsp_battery_soc();

    while (true) {
        key_event_t event;
        if (xQueueReceive(s_keys, &event, pdMS_TO_TICKS(50)) == pdTRUE) {
            handle_key(&event);
        }
        start_next_playback();
        if (s_playing && audio_state() == AUDIO_STATE_IDLE &&
            now_ms() - s_play_started_ms > 50) {
            // 任务回到 IDLE 不等于真播完了(可能打不开文件/I2S 写失败),
            // 按音频任务的真实结果回执,避免"没出声却告诉服务端已播放"。
            finish_current(audio_last_play_completed(), false);
        }
        if (s_phase == APP_PHASE_RECORD && !audio_record_active()) {
            stop_recording_and_process();
        }
        int64_t now = now_ms();
        if (now >= s_next_view_ms) {
            refresh_view();
            s_next_view_ms = now + 500;
        }
        if (now >= s_next_battery_ms) {
            s_battery = bsp_battery_soc();
            s_next_battery_ms = now + 5000;
        }
    }
}

esp_err_t app_state_start(void) {
    s_keys = xQueueCreate(8, sizeof(key_event_t));
    if (s_keys == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (bsp_button_init(key_callback, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "按键初始化失败,交互不可用");
    }
    if (xTaskCreate(app_task, "sonar", 10240, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
