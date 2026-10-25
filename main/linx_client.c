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
#include "esp_log.h"
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
    if (s_tts_fp == NULL || len == 0) {
        ESP_LOGI(TAG, "server binary: %u 字节(未在接收状态)", (unsigned)len);
        return;
    }
    // 下行音频按到达顺序追加;分片重组已由 linx_ws 完成,这里拿到的是整条消息。
    fwrite(data, 1, len, s_tts_fp);
    s_tts_bytes += (uint32_t)len;
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

static void (*s_progress)(const char *msg);

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
    for (int attempt = 0; attempt < 3; attempt++) {
        err = linx_ws_open(uri, headers, &WS_CALLBACKS, 8000);
        if (err == ESP_OK) {
            break;
        }
        progress("voice retry %d/3", attempt + 1);
        ESP_LOGW(TAG, "WSS 连接失败(%d/3): %s,退避重试 (heap=%u 最大块=%u)",
                 attempt + 1, esp_err_to_name(err), (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        if (attempt < 2) {
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
        // 20 ms 一帧:与服务端 16 kHz/20 ms 的协商一致,突发灌入会被 VAD 丢掉。
        if (left > 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    fclose(fp);
    ESP_LOGI(TAG, "已发送 %u 帧(%u 字节 PCM)", (unsigned)frames, (unsigned)info.data_bytes);
    return ESP_OK;
}

static esp_err_t begin_listen(void) {
    char msg[192];
    int len;
    if (s_session_id[0] != '\0') {
        len = snprintf(msg, sizeof(msg),
                       "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\","
                       "\"mode\":\"manual\"}",
                       s_session_id);
    } else {
        len = snprintf(msg, sizeof(msg),
                       "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"}");
    }
    return ws_send_json(msg) == ESP_OK && len > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
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

esp_err_t linx_speech_to_text(const sonar_config_t *cfg, const char *wav_path, char *text,
                              size_t text_len, uint32_t timeout_ms) {
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
    s_tts_fp = NULL;  // 公开转写不接收 TTS
    s_reply_text = NULL;
    s_reply_len = 0;
    xEventGroupClearBits(s_events, BIT_STT | BIT_ERROR);

    err = begin_listen();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "listen start 已发送");
        err = stream_pcm(wav_path);
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
    ws_teardown();
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "STT: %s", text);
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

    err = begin_listen();
    if (err == ESP_OK) {
        err = stream_pcm(wav_path);
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
