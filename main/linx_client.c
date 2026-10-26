// main/linx_client.c
// 灵矽 OTA + WSS 语音会话。协议按官方 xrobot WebSocket 文档与 xiaozhi-esp32 参考实现。
#include "linx_client.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_tls.h"
#include "esp_system.h"
#include "linx_ca.h"
#include "linx_ws.h"
#include "sonar_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sonar_http.h"
#include "sonar_time.h"
#include "sonar_wav.h"

static const char *TAG = "linx";
#define BIT_CONNECTED BIT0
#define BIT_HELLO BIT1
#define BIT_STT BIT2
#define BIT_TTS_DONE BIT3
#define BIT_ERROR BIT4

#define OTA_URL "https://xrobo.qiniuapi.com/v1/ota/"
#define ACTIVATE_URL "https://xrobo.qiniuapi.com/v1/ota/activate"
#define STT_TIMEOUT_MS 20000
#define CHAT_TIMEOUT_MS 45000

static char s_device_id[32];
static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_lock;
static char s_session_id[64];
static bool s_server_pcm = true;   // 服务端 hello 声明的下行格式是否为 PCM
static uint32_t s_server_rate = 16000;

static char *s_stt_text;
static size_t s_stt_len;
static FILE *s_tts_fp;
static uint32_t s_tts_bytes;
static char *s_reply_text;
static size_t s_reply_len;
// 首次语音调用(STT)期间平台会顺手生成一份助手回复:音频走二进制帧、文本走
// tts.sentence_start。这里把它收下来,私人聊天判定成立时直接播,省掉第二次
// 建连 + 重传同一段录音。
static SemaphoreHandle_t s_tts_mux;
static char s_assistant_text[256];

// OTA 响应只有几百字节;单独用小块,别和 Sonar 的 inbox 缓冲抢内部 RAM。
static char s_resp_buf[4096];

esp_err_t linx_client_init(void) {

    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        s_lock = xSemaphoreCreateMutex();
        if (s_events == NULL || s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(s_device_id, sizeof(s_device_id), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

const char *linx_device_id(void) {
    return s_device_id;
}

static void append_text(char *dst, size_t dst_len, const char *src) {
    if (dst == NULL || dst_len == 0 || src == NULL) {
        return;
    }
    size_t used = strlen(dst);
    if (used + 1 >= dst_len) {
        return;
    }
    snprintf(dst + used, dst_len - used, "%s", src);
}

static void handle_server_text(const char *json, size_t len) {
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "无法解析服务端文本帧");
        return;
    }
    char type[24] = {0};
    ESP_LOGD(TAG, "server text: %.*s", (int)(len > 200 ? 200 : len), json);
    const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type_item) && type_item->valuestring != NULL) {
        snprintf(type, sizeof(type), "%s", type_item->valuestring);
    }
    if (strcmp(type, "hello") == 0) {
        const cJSON *sid = cJSON_GetObjectItemCaseSensitive(root, "session_id");
        if (cJSON_IsString(sid) && sid->valuestring != NULL) {
            snprintf(s_session_id, sizeof(s_session_id), "%s", sid->valuestring);
        }
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
        if (params != NULL) {
            const cJSON *format = cJSON_GetObjectItemCaseSensitive(params, "format");
            const cJSON *rate = cJSON_GetObjectItemCaseSensitive(params, "sample_rate");
            if (cJSON_IsString(format) && format->valuestring != NULL) {
                s_server_pcm = strcmp(format->valuestring, "pcm") == 0;
                if (!s_server_pcm) {
                    ESP_LOGW(TAG, "服务端下行格式为 %s,本机只实现了 PCM 播放", format->valuestring);
                }
            }
            if (cJSON_IsNumber(rate) && rate->valueint > 0) {
                s_server_rate = (uint32_t)rate->valueint;
            }
        }
        ESP_LOGI(TAG, "会话 %s 已就绪(下行 %s/%uHz)", s_session_id,
                 s_server_pcm ? "pcm" : "非pcm", (unsigned)s_server_rate);
        xEventGroupSetBits(s_events, BIT_HELLO);
    } else if (strcmp(type, "stt") == 0) {
        ESP_LOGI(TAG, "收到 stt 消息");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && text->valuestring != NULL && text->valuestring[0] != '\0') {
            if (s_stt_text != NULL && s_stt_len > 0) {
                // 只接受第一个非空候选;后续候选由调用方按"冲突即失败"处理。
                if (s_stt_text[0] == '\0') {
                    snprintf(s_stt_text, s_stt_len, "%s", text->valuestring);
                    xEventGroupSetBits(s_events, BIT_STT);
                }
            }
        }
    } else if (strcmp(type, "tts") == 0) {
        ESP_LOGI(TAG, "收到 tts 消息");
        char state[16] = {0};
        const cJSON *state_item = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (cJSON_IsString(state_item) && state_item->valuestring != NULL) {
            snprintf(state, sizeof(state), "%s", state_item->valuestring);
        }
        if (strcmp(state, "start") == 0) {
            // 头已经在打开文件时写好;这里只等二进制数据。
        } else if (strcmp(state, "sentence_start") == 0) {
            const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
            if (cJSON_IsString(text) && text->valuestring != NULL) {
                append_text(s_reply_text, s_reply_len, text->valuestring);
            }
        } else if (strcmp(state, "stop") == 0) {
            xEventGroupSetBits(s_events, BIT_TTS_DONE);
        }
    } else if (strcmp(type, "error") == 0) {
        ESP_LOGW(TAG, "服务端错误: %.*s", (int)(len > 300 ? 300 : len), json);
        xEventGroupSetBits(s_events, BIT_ERROR);
    } else {
        ESP_LOGD(TAG, "忽略消息类型 %s", type);
    }
    cJSON_Delete(root);
}

static void on_ws_text(const char *data, size_t len) {
    ESP_LOGI(TAG, "server text: %.*s", (int)len, data);
    handle_server_text(data, len);
}

static void on_ws_bin(const uint8_t *data, size_t len) {
    if (len == 0) {
        return;
    }
    if (s_tts_mux == NULL || xSemaphoreTake(s_tts_mux, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    if (s_tts_fp == NULL) {
        xSemaphoreGive(s_tts_mux);
        ESP_LOGI(TAG, "server binary: %u 字节(未在接收状态)", (unsigned)len);
        return;
    }
    // 下行音频按到达顺序追加;分片重组已由 linx_ws 完成,这里拿到的是整条消息。
    fwrite(data, 1, len, s_tts_fp);
    s_tts_bytes += (uint32_t)len;
    xSemaphoreGive(s_tts_mux);
}

static void on_ws_close(void) {
    ESP_LOGW(TAG, "WSS 断开");
    if (s_events != NULL) {
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        xEventGroupSetBits(s_events, BIT_ERROR);
    }
}

static const linx_ws_callbacks_t WS_CALLBACKS = {
    .on_text = on_ws_text,
    .on_bin = on_ws_bin,
    .on_close = on_ws_close,
};

// 官方可能返回明文 ws://,按项目策略对已知官方主机升级成 wss://,不降级发送凭证。
static void upgrade_ws_url(const char *in, char *out, size_t out_len) {
    if (strncmp(in, "ws://xrobo-io.qiniuapi.com", 26) == 0) {
        snprintf(out, out_len, "wss://%s", in + 5);
    } else {
        snprintf(out, out_len, "%s", in);
    }
}

static void ws_teardown(void) {
    linx_ws_close();
    if (s_events != NULL) {
        xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_HELLO);
    }
}

static esp_err_t ws_connect(const sonar_config_t *cfg, uint32_t timeout_ms);

static void (*s_progress)(const char *msg);
static const sonar_config_t *s_cfg_ref;
static TaskHandle_t s_prewarm_task;

static void prewarm_task(void *arg) {
    (void)arg;
    // 必须和 STT/probe/OTA 共用同一把锁:s_ws 是全局单例,两个任务同时握手会
    // 互相踩掉 mbedtls 上下文,表现为 Instruction access fault(MEPC=0)。
    if (s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGW(TAG, "预连接放弃:拿不到语音锁");
        s_prewarm_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    if (s_cfg_ref != NULL && !linx_ws_is_open()) {
        esp_err_t err = ws_connect(s_cfg_ref, 6000);
        ESP_LOGI(TAG, "预连接%s: %s", err == ESP_OK ? "成功" : "失败", esp_err_to_name(err));
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    s_prewarm_task = NULL;
    vTaskDelete(NULL);
}

// 按下说话时调用:后台并行握手,松手后的转写就不用再等 TLS。
void linx_prewarm(const sonar_config_t *cfg) {
    s_cfg_ref = cfg;
    if (cfg == NULL || linx_ws_is_open() || s_prewarm_task != NULL) {
        return;
    }
    // 栈必须够大:TLS 握手会在**这个任务**里做 RSA 验证(mbedtls_mpi_exp_mod),
    // 4096 字节会栈溢出成 "Load access fault"(实测崩在 mbedtls_mpi_cmp_mpi)。
    if (xTaskCreate(prewarm_task, "linx_pre", 10240, NULL, 4, &s_prewarm_task) != pdPASS) {
        s_prewarm_task = NULL;
    }
}

void linx_set_progress(void (*cb)(const char *msg)) {
    s_progress = cb;
}

static void progress(const char *fmt, ...) {
    if (s_progress == NULL) {
        return;
    }
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_progress(buf);
}

static esp_err_t ws_connect(const sonar_config_t *cfg, uint32_t timeout_ms) {
    if (cfg->linx_url[0] == '\0' || cfg->linx_token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (linx_ws_is_open()) {
        return ESP_OK;
    }
    ws_teardown();
    char uri[SONAR_URL_MAX + 16];
    upgrade_ws_url(cfg->linx_url, uri, sizeof(uri));
    ESP_LOGI(TAG, "连接 WSS %s (token %s, 信任根 %u 字节, heap=%u, 最大块=%u)", uri,
             cfg->linx_token[0] ? "有" : "无", (unsigned)strlen(LINX_TRUSTED_PEM),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    static char headers[512];
    snprintf(headers, sizeof(headers),
             "Authorization: Bearer %s\r\n"
             "Device-Id: %s\r\n"
             "Client-Id: %s\r\n"
             "Protocol-Version: 1\r\n",
             cfg->linx_token, s_device_id, cfg->client_id);
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_HELLO | BIT_STT | BIT_TTS_DONE | BIT_ERROR);
    // 握手失败常见两类:堆被切碎(TLS 要一块连续内存)、Wi-Fi 刚重连路由未就绪。
    // 都是瞬时的,退避重试 3 次再判定失败,别一次就报 ESP_FAIL 给用户。
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2; attempt++) {
        // 验证链是 RSA 两级签发:C3 上完整握手实测要 4~6 秒,4 秒超时会稳定误判成
        // "握手失败"再重试,反而把一次 5 秒的握手拖成 13 秒。给到 8 秒,靠预连接
        // 把这段时间藏进用户说话的那几秒里。
        int64_t hs_start = esp_timer_get_time();
        err = linx_ws_open(uri, headers, &WS_CALLBACKS, 8000);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "握手成功,耗时 %lld ms",
                     (long long)((esp_timer_get_time() - hs_start) / 1000));
            break;
        }
        ESP_LOGW(TAG, "握手失败,耗时 %lld ms",
                 (long long)((esp_timer_get_time() - hs_start) / 1000));
        progress("voice retry %d/2", attempt + 1);
        ESP_LOGW(TAG, "WSS 连接失败(%d/2): %s,退避重试 (heap=%u 最大块=%u)",
                 attempt + 1, esp_err_to_name(err), (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        if (attempt < 1) {
            ws_teardown();
            vTaskDelay(pdMS_TO_TICKS(400 * (attempt + 1)));
        }
    }
    if (err != ESP_OK) {
        // 严格校验彻底失败才降级:宁可让语音可用并要求调用方告警,也不要让
        // "证书校验"这种底层问题把整条语音链路卡死。
        ESP_LOGW(TAG, "严格校验握手失败,降级一次(不校验证书)重试");
        linx_ws_set_verify_relaxed(true);
        err = linx_ws_open(uri, headers, &WS_CALLBACKS, 8000);
        linx_ws_set_verify_relaxed(false);
        if (err != ESP_OK) {
            ws_teardown();
            return err;
        }
    }
    xEventGroupSetBits(s_events, BIT_CONNECTED);
    progress("voice up");

    char hello[320];
    int hello_len = snprintf(hello, sizeof(hello),
                             "{\"type\":\"hello\",\"version\":1,\"features\":{\"mcp\":false},"
                             "\"transport\":\"websocket\",\"audio_params\":{\"format\":\"pcm\","
                             "\"sample_rate\":16000,\"channels\":1,\"bit_depth\":16,"
                             "\"endianness\":\"little\",\"frame_duration\":20,\"frame_size\":320,"
                             "\"sample_format\":\"signed_int16\",\"play_buffer_duration\":1000}}");
    if (linx_ws_send_text(hello, (size_t)hello_len) != ESP_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    EventBits_t bits =
        xEventGroupWaitBits(s_events, BIT_HELLO, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
    if ((bits & BIT_HELLO) == 0) {
        ESP_LOGW(TAG, "未收到服务端 hello(10s)");
        ws_teardown();
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "server hello ok: session=%s format=%s rate=%u", s_session_id,
             s_server_pcm ? "pcm" : "非pcm", (unsigned)s_server_rate);
    return ESP_OK;
}

static esp_err_t ws_send_json(const char *json) {
    if (!linx_ws_is_open()) {
        return ESP_ERR_INVALID_STATE;
    }
    return linx_ws_send_text(json, strlen(json));
}

// 把 WAV 的 PCM 数据部分按 20 ms 一帧(640 字节)发送。
static esp_err_t stream_pcm(const char *wav_path) {
    FILE *fp = fopen(wav_path, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t head[512];
    size_t got = fread(head, 1, sizeof(head), fp);
    sonar_wav_info_t info;
    if (sonar_wav_parse(head, got, &info) != SONAR_WAV_OK ||
        sonar_wav_validate(&info) != SONAR_WAV_OK) {
        fclose(fp);
        return ESP_ERR_INVALID_ARG;
    }
    if (fseek(fp, (long)info.data_offset, SEEK_SET) != 0) {
        fclose(fp);
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t frame[640];
    uint32_t left = info.data_bytes;
    uint32_t frames = 0;
    int64_t t_start = esp_timer_get_time();
    int64_t paced_start = t_start;
    while (left > 0) {
        size_t want = left > sizeof(frame) ? sizeof(frame) : left;
        size_t n = fread(frame, 1, want, fp);
        if (n == 0) {
            break;
        }
        esp_err_t frame_err = linx_ws_send_bin(frame, n);
        if (frame_err != ESP_OK) {
            fclose(fp);
            ESP_LOGW(TAG, "发送第 %u 帧失败: %s (heap=%u 最大块=%u)", (unsigned)frames,
                     esp_err_to_name(frame_err), (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            return ESP_ERR_INVALID_RESPONSE;
        }
        left -= (uint32_t)n;
        ++frames;
        // 必须按 20 ms 实时节奏发。实测(2026-10-26)全速灌入 2.5 秒语音只用 758 ms,
        // 服务端完全不给转写(三次都在 25 秒超时),说明它按实时流做 VAD/分段,
        // 突发数据会被丢掉。要省掉这段时间只能"边录边传",不能靠提速。
        //
        // 节奏按"绝对时间"算,不能每帧 vTaskDelay(20) 累加:LVGL 刷新、HTTP 轮询都会
        // 让单次延时不小于 20 ms,累加后 1.96 秒的语音实测被拖到 3.8 秒,服务端
        // 已经开始下一轮(tts stop 先到),转写就永远等不到。
        if (left > 0) {
            int64_t due = paced_start + (int64_t)frames * 20 * 1000;
            int64_t wait_ms = (due - esp_timer_get_time()) / 1000;
            if (wait_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS((uint32_t)wait_ms));
            }
        }
    }
    fclose(fp);
    ESP_LOGI(TAG, "已发送 %u 帧(%u 字节 PCM), 用时 %lld ms", (unsigned)frames,
             (unsigned)info.data_bytes, (long long)((esp_timer_get_time() - t_start) / 1000));
    return ESP_OK;
}

static bool s_listen_opus;

static esp_err_t begin_listen(bool opus) {
    char msg[320];
    int len;
    // 上行音频格式在这里声明(hello 的 audio_params 描述的是下行,保持 PCM 不变)。
    const char *params = opus ? ",\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
                                "\"channels\":1,\"frame_duration\":20}"
                              : "";
    s_listen_opus = opus;
    if (s_session_id[0] != '\0') {
        len = snprintf(msg, sizeof(msg),
                       "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\","
                       "\"mode\":\"manual\"%s}",
                       s_session_id, params);
    } else {
        len = snprintf(msg, sizeof(msg),
                       "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"%s}",
                       params);
    }
    return ws_send_json(msg) == ESP_OK && len > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

// 逐包发送 Opus(裸包流:2 字节小端长度 + 一帧),比 PCM 小一个数量级。
static esp_err_t stream_opus(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t head[2];
    uint8_t packet[512];
    uint32_t packets = 0;
    while (fread(head, 1, sizeof(head), fp) == sizeof(head)) {
        size_t len = (size_t)head[0] | ((size_t)head[1] << 8);
        if (len == 0 || len > sizeof(packet)) {
            ESP_LOGW(TAG, "Opus 包长度异常(%u),终止上行", (unsigned)len);
            fclose(fp);
            return ESP_ERR_INVALID_SIZE;
        }
        if (fread(packet, 1, len, fp) != len) {
            break;
        }
        esp_err_t err = linx_ws_send_bin(packet, len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "发送第 %u 个 Opus 包失败: %s", (unsigned)packets,
                     esp_err_to_name(err));
            fclose(fp);
            return err;
        }
        packets++;
    }
    fclose(fp);
    ESP_LOGI(TAG, "Opus 上行完成: %u 包", (unsigned)packets);
    return ESP_OK;
}

static esp_err_t end_listen(void) {
    char msg[192];
    if (s_session_id[0] != '\0') {
        snprintf(msg, sizeof(msg),
                 "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\","
                 "\"mode\":\"manual\"}",
                 s_session_id);
    } else {
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"listen\",\"state\":\"stop\",\"mode\":\"manual\"}");
    }
    return ws_send_json(msg);
}

esp_err_t linx_probe(const sonar_config_t *cfg) {
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    esp_err_t err = ws_connect(cfg, 25000);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WSS 探针通过: hello 往返正常");
    }
    ws_teardown();
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return err;
}

// 开始接收助手回复音频(占位头先写好,收尾时回填真实长度)。
static bool assistant_capture_open(void) {
    if (s_tts_mux == NULL) {
        s_tts_mux = xSemaphoreCreateMutex();
        if (s_tts_mux == NULL) {
            return false;
        }
    }
    xSemaphoreTake(s_tts_mux, portMAX_DELAY);
    if (s_tts_fp != NULL) {
        fclose(s_tts_fp);
        s_tts_fp = NULL;
    }
    s_tts_bytes = 0;
    s_assistant_text[0] = '\0';
    s_reply_text = s_assistant_text;
    s_reply_len = sizeof(s_assistant_text);
    s_tts_fp = fopen(SONAR_ASSISTANT_PATH, "wb");
    bool ok = s_tts_fp != NULL;
    if (ok) {
        uint8_t header[SONAR_WAV_HEADER_BYTES] = {0};
        fwrite(header, 1, sizeof(header), s_tts_fp);
    }
    xSemaphoreGive(s_tts_mux);
    return ok;
}

static void assistant_capture_close(void) {
    if (s_tts_mux == NULL) {
        return;
    }
    xSemaphoreTake(s_tts_mux, portMAX_DELAY);
    if (s_tts_fp != NULL) {
        uint8_t header[SONAR_WAV_HEADER_BYTES];
        sonar_wav_header(header, s_tts_bytes);
        fseek(s_tts_fp, 0, SEEK_SET);
        fwrite(header, 1, sizeof(header), s_tts_fp);
        fclose(s_tts_fp);
        s_tts_fp = NULL;
    }
    s_reply_text = NULL;
    s_reply_len = 0;
    xSemaphoreGive(s_tts_mux);
}

static esp_err_t stt_once(const sonar_config_t *cfg, const char *wav_path, bool use_opus,
                          char *text, size_t text_len, uint32_t timeout_ms) {
    if (text == NULL || text_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    text[0] = '\0';
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    esp_err_t err = ws_connect(cfg, 25000);
    if (err != ESP_OK) {
        if (s_lock != NULL) {
            xSemaphoreGive(s_lock);
        }
        return err;
    }
    s_stt_text = text;
    s_stt_len = text_len;
    assistant_capture_open();
    xEventGroupClearBits(s_events, BIT_STT | BIT_ERROR);

    err = begin_listen(use_opus);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "listen start 已发送(上行 %s)", use_opus ? "opus" : "pcm");
        err = use_opus ? stream_opus(SONAR_OPUS_PATH) : stream_pcm(wav_path);
        if (use_opus && err != ESP_OK) {
            ESP_LOGW(TAG, "Opus 上行失败(%s),回退 PCM", esp_err_to_name(err));
            err = stream_pcm(wav_path);
        }
    }
    if (err == ESP_OK) {
        err = end_listen();
        ESP_LOGI(TAG, "listen stop 已发送,等待转写");
    }
    if (err == ESP_OK) {
        EventBits_t bits = xEventGroupWaitBits(s_events, BIT_STT | BIT_ERROR, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(timeout_ms ? timeout_ms : STT_TIMEOUT_MS));
        if ((bits & BIT_STT) == 0) {
            ESP_LOGW(TAG, "未在超时内取得转写(bits=0x%x)", (unsigned)bits);
            err = ESP_ERR_TIMEOUT;
        }
    }
    s_stt_text = NULL;
    if (err != ESP_OK) {
        ws_teardown();
    } else {
        // 成功就留着连接:下次按句话不必再握手(C3 上握手要 ~5KB 连续内存,
        // 一旦堆被 HTTP 轮询切碎就会以"证书不可信"的形式失败)。
        ESP_LOGI(TAG, "保留 WSS 连接复用 free=%u largest=%u", (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "STT: %s", text);
    }
    return err;
}

bool linx_assistant_take(char *text, size_t text_len, uint32_t *pcm_bytes) {
    // 回复音频是异步流进来的:业务判定通常只要 1 秒左右,所以多半已经收完。
    // 等一小会儿再收尾,避免刚播到一半就没数据。
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_TTS_DONE, pdTRUE, pdTRUE,
                                           pdMS_TO_TICKS(1500));
    assistant_capture_close();
    uint32_t bytes = s_tts_bytes > SONAR_WAV_HEADER_BYTES ? s_tts_bytes : 0;
    if (pcm_bytes != NULL) {
        *pcm_bytes = bytes;
    }
    if ((bits & BIT_TTS_DONE) == 0) {
        ESP_LOGW(TAG, "助手回复未收完(1.5s 内无 tts stop),已收 %u 字节", (unsigned)bytes);
    }
    if (bytes < 8000) {   // 少于 0.25 秒的音频没有播放价值,按失败处理
        return false;
    }
    if (text != NULL && text_len > 0) {
        snprintf(text, text_len, "%s", s_assistant_text);
    }
    ESP_LOGI(TAG, "复用首轮助手回复: %u 字节 PCM, 文本 %u 字",
             (unsigned)bytes, (unsigned)strlen(s_assistant_text));
    return true;
}

esp_err_t linx_speech_to_text(const sonar_config_t *cfg, const char *wav_path, char *text,
                              size_t text_len, uint32_t timeout_ms) {
    // 上行优先用 Opus(录音时已编码好),失败自动回退 PCM:
    // 平台是否接受 opus 上行没法只靠文档确认,所以留一条确定能用的退路。
    bool have_opus = sonar_store_file_size(SONAR_OPUS_PATH) > 0;
    esp_err_t err = ESP_FAIL;
    if (have_opus) {
        err = stt_once(cfg, wav_path, true, text, text_len, timeout_ms);
        if (err != ESP_OK || text[0] == '\0') {
            ESP_LOGW(TAG, "Opus 上行未拿到转写(%s),回退 PCM 重试", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(200));
            err = stt_once(cfg, wav_path, false, text, text_len, timeout_ms);
        }
    } else {
        err = stt_once(cfg, wav_path, false, text, text_len, timeout_ms);
    }
    if (err != ESP_OK || text[0] == '\0') {
        ESP_LOGW(TAG, "转写仍失败(%s),最后再试一次 PCM", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(300));
        err = stt_once(cfg, wav_path, false, text, text_len, timeout_ms);
    }
    return err;
}

esp_err_t linx_chat(const sonar_config_t *cfg, const char *wav_path, const char *pcm_path,
                    char *reply_text, size_t reply_len, uint32_t timeout_ms) {
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    esp_err_t err = ws_connect(cfg, 25000);
    if (err != ESP_OK) {
        if (s_lock != NULL) {
            xSemaphoreGive(s_lock);
        }
        return err;
    }
    if (!s_server_pcm) {
        // 只实现了 PCM 播放;服务端给 Opus 时明确失败,不制造噪声。
        if (s_lock != NULL) {
            xSemaphoreGive(s_lock);
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (reply_text != NULL && reply_len > 0) {
        reply_text[0] = '\0';
    }
    s_reply_text = reply_text;
    s_reply_len = reply_len;
    s_tts_fp = fopen(pcm_path, "wb");
    s_tts_bytes = 0;
    if (s_tts_fp != NULL) {
        uint8_t placeholder[SONAR_WAV_HEADER_BYTES];
        sonar_wav_header(placeholder, 0);
        fwrite(placeholder, 1, sizeof(placeholder), s_tts_fp);
    }
    s_stt_text = NULL;
    if (s_tts_fp == NULL) {
        if (s_lock != NULL) {
            xSemaphoreGive(s_lock);
        }
        return ESP_ERR_NO_MEM;
    }
    xEventGroupClearBits(s_events, BIT_TTS_DONE | BIT_ERROR);

    bool have_opus = sonar_store_file_size(SONAR_OPUS_PATH) > 0;
    err = begin_listen(have_opus);
    if (err == ESP_OK) {
        err = have_opus ? stream_opus(SONAR_OPUS_PATH) : stream_pcm(wav_path);
        if (have_opus && err != ESP_OK) {
            err = stream_pcm(wav_path);
        }
    }
    if (err == ESP_OK) {
        err = end_listen();
    }
    if (err == ESP_OK) {
        EventBits_t bits =
            xEventGroupWaitBits(s_events, BIT_TTS_DONE | BIT_ERROR, pdFALSE, pdFALSE,
                                pdMS_TO_TICKS(timeout_ms ? timeout_ms : CHAT_TIMEOUT_MS));
        if ((bits & BIT_TTS_DONE) == 0) {
            err = ESP_ERR_TIMEOUT;
        }
    }
    // 回填 WAV 头:按服务端协商采样率,长度用实际收到的字节数。
    if (s_tts_bytes > 0) {
        uint8_t header[SONAR_WAV_HEADER_BYTES];
        sonar_wav_header(header, s_tts_bytes);
        // 协商采样率不是 16 kHz 时改写的不是字节率字段,这里只支持 16 kHz 的回复。
        if (s_server_rate != SONAR_WAV_RATE) {
            ESP_LOGW(TAG, "TTS 采样率 %u Hz 与 16 kHz 不符,回复可能变调", (unsigned)s_server_rate);
        }
        fseek(s_tts_fp, 0, SEEK_SET);
        fwrite(header, 1, sizeof(header), s_tts_fp);
    }
    fclose(s_tts_fp);
    s_tts_fp = NULL;
    s_reply_text = NULL;
    ws_teardown();
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    if (err != ESP_OK) {
        remove(pcm_path);
        return err;
    }
    return s_tts_bytes > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t linx_ota(sonar_config_t *cfg, linx_ota_result_t *out) {
    // OTA 与语音走同一家服务,信任锚统一用内置的中间 CA。
    sonar_http_set_trust_anchor(LINX_TRUSTED_PEM);
    if (cfg == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    char elf_sha[65] = {0};
    esp_app_get_elf_sha256(elf_sha, sizeof(elf_sha));
    char body[768];
    int len = snprintf(body, sizeof(body),
                       "{\"version\":0,\"uuid\":\"%s\",\"mac_address\":\"%s\","
                       "\"application\":{\"name\":\"sonar-esp32\",\"version\":\"0.1.0\","
                       "\"elf_sha256\":\"%s\"},"
                       "\"board\":{\"type\":\"ai-passport\",\"ssid\":\"\",\"rssi\":0}}",
                       cfg->client_id, s_device_id, elf_sha);
    if (len <= 0 || (size_t)len >= sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }
    sonar_http_header_t headers[] = {
        {"Activation-Version", "1"},
        {"Device-Id", s_device_id},
        {"Client-Id", cfg->client_id},
        {"User-Agent", "sonar-esp32/0.1.0"},
    };
    sonar_http_resp_t resp;
    sonar_http_resp_init(&resp, s_resp_buf, sizeof(s_resp_buf));
    esp_err_t err = sonar_http_request_ex("POST", OTA_URL, NULL, "application/json", body,
                                          strlen(body), headers,
                                          sizeof(headers) / sizeof(headers[0]), &resp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA 上报传输失败: %s (heap=%u)", esp_err_to_name(err),
                 (unsigned)esp_get_free_heap_size());
        return err;
    }
    if (resp.status != 200) {
        ESP_LOGW(TAG, "OTA 上报失败 HTTP %d", resp.status);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *root = cJSON_Parse(resp.body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *activation = cJSON_GetObjectItemCaseSensitive(root, "activation");
    if (cJSON_IsObject(activation)) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(activation, "code");
        if (cJSON_IsString(code) && code->valuestring != NULL) {
            snprintf(out->code, sizeof(out->code), "%s", code->valuestring);
        }
    }
    const cJSON *ws = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    if (cJSON_IsObject(ws)) {
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(ws, "url");
        const cJSON *token = cJSON_GetObjectItemCaseSensitive(ws, "token");
        if (cJSON_IsString(url) && url->valuestring != NULL) {
            snprintf(cfg->linx_url, sizeof(cfg->linx_url), "%s", url->valuestring);
        }
        if (cJSON_IsString(token) && token->valuestring != NULL) {
            snprintf(cfg->linx_token, sizeof(cfg->linx_token), "%s", token->valuestring);
        }
        out->bound = cfg->linx_token[0] != '\0';
    }
    const cJSON *server_time = cJSON_GetObjectItemCaseSensitive(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        const cJSON *ts = cJSON_GetObjectItemCaseSensitive(server_time, "timestamp");
        if (cJSON_IsNumber(ts)) {
            // Linx 是毫秒时间戳;Sonar 用 UTC 字符串,两者不混用。
        }
    }
    cJSON_Delete(root);
    if (!out->bound && out->code[0] == '\0') {
        ESP_LOGW(TAG, "OTA 既无激活码也无凭证");
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}
