// main/linx_ws.c
// 最小 WebSocket(wss) 客户端。分层:
//   mbedtls(自带 bio + 非阻塞 socket)  →  TLS
//   本文件                            →  HTTP 升级 + 帧编解码(掩码/分片/控制帧)
//
// 为什么不用 esp_websocket_client:该组件内部发起 TLS 时,同一份信任根在这块板子上
// 稳定报 -0x2700(而同样的 CA、同样的主机名用 esp_tls / mbedtls 直接握手都成功,
// 参见 docs/development/sonar-client.md 的排障记录)。这里把 TLS 与帧层都握在自己手里,
// 行为可复现、可打点,也不再依赖那个组件的任务上下文。
#include "linx_ws.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linx_ca.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "linx_ws";

// 文本消息(JSON)的重组上限。下行音频不走这里:它是持续流,边收边交给上层写文件,
// 缓存整条消息会在 STT 会话里把堆吃穿(实测只剩 5.8 KB,后续音频帧写不出去)。
#define WS_MAX_MESSAGE (8 * 1024)
#define WS_RX_STACK 4096
#define WS_POLL_MS 100

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    int fd;
    bool open;
    bool stop;
    bool ready;
    bool inited;   // ssl/conf/ca/drbg 已初始化,close 时必须释放
    SemaphoreHandle_t lock;   // 保护 ssl 上下文:读写都可能在不同任务发起
    TaskHandle_t rx_task;
    linx_ws_callbacks_t cb;
    uint8_t *msg;             // 分片重组缓冲
    size_t msg_len;
    size_t msg_cap;
    int msg_opcode;
    bool verify_relaxed;      // 关闭证书校验(仅在严格校验反复失败的最后一次尝试)
} linx_ws_t;

static linx_ws_t s_ws = {.fd = -1};

// 诊断回调:打印对端证书链与逐张校验位,定位"证书不可信"卡在哪一环。
static int verify_log(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    (void)ctx;
    char subject[128] = {0};
    mbedtls_x509_dn_gets(subject, sizeof(subject), &crt->subject);
    ESP_LOGW(TAG, "对端证书 depth=%d subject=%s flags=0x%08X", depth, subject, (unsigned)*flags);
    return 0;
}

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

// ---- socket bio(非阻塞):EAGAIN 翻译成 mbedtls 的 WANT_* ----

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    int sent = send(fd, buf, len, 0);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return sent;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    int got = recv(fd, buf, len, 0);
    if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return got;  // 0 表示对端正常关闭
}

static bool wait_fd(int fd, bool want_write, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    int rc = select(fd + 1, want_write ? NULL : &set, want_write ? &set : NULL, NULL, &tv);
    return rc > 0;
}

// 加锁必须有上限:rx_task 若在持锁期间被删除/卡死,无上限等待会让整个发送路径
// 永久停住(表现就是屏幕停在 transcribing,按什么都没反应)。
static bool lock_timeout(uint32_t ms) {
    if (s_ws.lock == NULL) {
        return true;
    }
    if (xSemaphoreTake(s_ws.lock, pdMS_TO_TICKS(ms)) != pdTRUE) {
        ESP_LOGW(TAG, "TLS 锁等待超时(%u ms),握手/socket 可能已失联", (unsigned)ms);
        return false;
    }
    return true;
}

static void lock(void) {
    if (!lock_timeout(5000)) {
        // 拿不到锁也不阻塞调用者:交给上层超时处理,并让后续操作尽快失败。
        ESP_LOGW(TAG, "继续执行但未持锁");
    }
}

static void unlock(void) {
    if (s_ws.lock != NULL) {
        xSemaphoreGive(s_ws.lock);
    }
}

// 在锁内做一次 mbedtls 写,遇到 WANT_WRITE 就短暂放锁等可写。
static int ws_tls_write(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    size_t done = 0;
    int64_t deadline = now_ms() + timeout_ms;
    while (done < len) {
        lock();
        int ret = mbedtls_ssl_write(&s_ws.ssl, data + done, len - done);
        unlock();
        if (ret > 0) {
            done += (size_t)ret;
            continue;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_WRITE && ret != MBEDTLS_ERR_SSL_WANT_READ) {
            ESP_LOGW(TAG, "TLS 写失败 -0x%04X", -ret);
            return ret;
        }
        if (now_ms() >= deadline) {
            ESP_LOGW(TAG, "TLS 写超时(未发 %u/%u 字节, heap=%u)", (unsigned)(len - done),
                     (unsigned)len, (unsigned)esp_get_free_heap_size());
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
        if (!wait_fd(s_ws.fd, true, 50) && (done > 0 || len > 0)) {
            // 只记一次,避免 20 ms 一帧刷屏
            static int64_t last = 0;
            if (now_ms() - last > 2000) {
                last = now_ms();
                ESP_LOGW(TAG, "socket 持续不可写(heap=%u 最大块=%u)", (unsigned)esp_get_free_heap_size(),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            }
        }
    }
    return 0;
}

// 读满 len 字节;返回 0 成功,负数失败。
static int ws_tls_read_exact(uint8_t *buf, size_t len, uint32_t timeout_ms) {
    size_t got = 0;
    int64_t deadline = now_ms() + timeout_ms;
    while (got < len) {
        lock();
        int ret = mbedtls_ssl_read(&s_ws.ssl, buf + got, len - got);
        unlock();
        if (ret > 0) {
            got += (size_t)ret;
            continue;
        }
        if (ret == 0) {
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return ret;
        }
        if (now_ms() >= deadline) {
            return MBEDTLS_ERR_SSL_TIMEOUT;
        }
        wait_fd(s_ws.fd, false, 50);
    }
    return 0;
}

// ---- 帧编码 ----

static int ws_send_frame(int opcode, const uint8_t *payload, size_t len, uint32_t timeout_ms) {
    if (!s_ws.open) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t head[8];
    size_t head_len = 0;
    head[0] = (uint8_t)(0x80 | (opcode & 0x0F));  // FIN + opcode
    if (len < 126) {
        head[1] = (uint8_t)(0x80 | len);
        head_len = 2;
    } else if (len <= 0xFFFF) {
        head[1] = 0x80 | 126;
        head[2] = (uint8_t)(len >> 8);
        head[3] = (uint8_t)(len & 0xFF);
        head_len = 4;
    } else {
        head[1] = 0x80 | 127;
        for (int i = 0; i < 8; ++i) {
            head[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
        }
        head_len = 10;
    }
    // 客户端必须掩码;掩码键每帧随机。
    uint8_t mask[4];
    uint32_t r = esp_random();
    memcpy(mask, &r, sizeof(mask));
    memcpy(head + head_len, mask, sizeof(mask));
    head_len += sizeof(mask);

    // 掩码就地做:录音是 20 ms 一帧(640 字节)的持续写入,每帧都 malloc/free
    // 会把堆打碎,直接导致后续帧分配失败。
    static uint8_t masked[1024];
    uint8_t *heap_copy = NULL;
    const uint8_t *body = payload;
    if (len > 0) {
        uint8_t *dst;
        if (len <= sizeof(masked)) {
            dst = masked;
        } else {
            heap_copy = malloc(len);
            if (heap_copy == NULL) {
                return ESP_ERR_NO_MEM;
            }
            dst = heap_copy;
        }
        for (size_t i = 0; i < len; ++i) {
            dst[i] = payload[i] ^ mask[i & 3];
        }
        body = dst;
    }
    int err = ws_tls_write(head, head_len, timeout_ms);
    if (err == 0 && len > 0) {
        err = ws_tls_write(body, len, timeout_ms);
    }
    free(heap_copy);
    return err == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t ws_send_pong(const uint8_t *payload, size_t len) {
    return ws_send_frame(0x0A, payload, len > 125 ? 125 : len, 2000) == ESP_OK ? ESP_OK : ESP_FAIL;
}

// ---- 接收任务 ----

static void ws_dispatch(void) {
    if (s_ws.msg_opcode == 0x01) {
        if (s_ws.cb.on_text != NULL && s_ws.msg_len > 0) {
            s_ws.cb.on_text((const char *)s_ws.msg, s_ws.msg_len);
        }
    } else if (s_ws.msg_opcode == 0x02) {
        if (s_ws.cb.on_bin != NULL && s_ws.msg_len > 0) {
            s_ws.cb.on_bin(s_ws.msg, s_ws.msg_len);
        }
    }
    s_ws.msg_len = 0;
    s_ws.msg_opcode = 0;
}

static bool ws_append(const uint8_t *data, size_t len) {
    if (s_ws.msg_len + len > WS_MAX_MESSAGE) {
        ESP_LOGW(TAG, "消息超过 %d 字节上限,丢弃", WS_MAX_MESSAGE);
        s_ws.msg_len = 0;
        return false;
    }
    if (s_ws.msg_len + len > s_ws.msg_cap) {
        size_t want = s_ws.msg_len + len;
        if (want < 1024) {
            want = 1024;
        }
        uint8_t *grown = realloc(s_ws.msg, want);
        if (grown == NULL) {
            return false;
        }
        s_ws.msg = grown;
        s_ws.msg_cap = want;
    }
    memcpy(s_ws.msg + s_ws.msg_len, data, len);
    s_ws.msg_len += len;
    return true;
}

static void rx_task(void *arg) {
    (void)arg;
    while (!s_ws.stop && s_ws.open) {
        if (!wait_fd(s_ws.fd, false, WS_POLL_MS)) {
            continue;
        }
        uint8_t head[2];
        int err = ws_tls_read_exact(head, sizeof(head), 10000);
        if (err != 0) {
            ESP_LOGW(TAG, "读帧头失败 -0x%04X", -err);
            break;
        }
        bool fin = (head[0] & 0x80) != 0;
        int opcode = head[0] & 0x0F;
        bool masked = (head[1] & 0x80) != 0;
        uint64_t len = head[1] & 0x7F;
        if (len == 126) {
            uint8_t ext[2];
            if (ws_tls_read_exact(ext, sizeof(ext), 10000) != 0) {
                break;
            }
            len = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (len == 127) {
            uint8_t ext[8];
            if (ws_tls_read_exact(ext, sizeof(ext), 10000) != 0) {
                break;
            }
            len = 0;
            for (int i = 0; i < 8; ++i) {
                len = (len << 8) | ext[i];
            }
        }
        uint8_t mask[4] = {0};
        if (masked && ws_tls_read_exact(mask, sizeof(mask), 10000) != 0) {
            break;
        }
        if (len > WS_MAX_MESSAGE) {
            ESP_LOGW(TAG, "帧过大(%u),丢弃连接", (unsigned)len);
            break;
        }
        uint8_t *payload = NULL;
        if (len > 0) {
            payload = malloc((size_t)len);
            if (payload == NULL) {
                ESP_LOGW(TAG, "无内存读 %u 字节帧", (unsigned)len);
                break;
            }
            if (ws_tls_read_exact(payload, (size_t)len, 10000) != 0) {
                free(payload);
                break;
            }
            if (masked) {
                for (uint64_t i = 0; i < len; ++i) {
                    payload[i] ^= mask[i & 3];
                }
            }
        }

        if (opcode == 0x8) {  // close
            ESP_LOGI(TAG, "服务端关闭连接");
            free(payload);
            break;
        } else if (opcode == 0x9) {  // ping -> pong
            ws_send_pong(payload, (size_t)len);
            free(payload);
            continue;
        } else if (opcode == 0xA) {  // pong
            free(payload);
            continue;
        } else if (opcode == 0x1) {  // 文本:重组后交给上层解析
            s_ws.msg_opcode = 0x01;
            s_ws.msg_len = 0;
            ws_append(payload, (size_t)len);
            if (fin) {
                ws_dispatch();
            }
        } else if (opcode == 0x2) {  // 二进制音频:直接下沉,不缓存整条消息
            if (s_ws.cb.on_bin != NULL && len > 0) {
                s_ws.cb.on_bin(payload, (size_t)len);
            }
        } else if (opcode == 0x0) {  // 续片
            if (s_ws.msg_opcode == 0x01) {
                ws_append(payload, (size_t)len);
                if (fin) {
                    ws_dispatch();
                }
            } else if (s_ws.cb.on_bin != NULL && len > 0) {
                s_ws.cb.on_bin(payload, (size_t)len);
            }
        }
    }

    s_ws.open = false;
    if (s_ws.cb.on_close != NULL) {
        s_ws.cb.on_close();
    }
    s_ws.rx_task = NULL;
    vTaskDelete(NULL);
}

// ---- 升级握手 ----

static bool parse_url(const char *url, char *host, size_t host_len, int *port, const char **path) {
    const char *p = strstr(url, "://");
    if (p == NULL) {
        return false;
    }
    p += 3;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    size_t host_chars = (colon != NULL && (slash == NULL || colon < slash)) ? (size_t)(colon - p)
                                                                           : (slash != NULL ? (size_t)(slash - p) : strlen(p));
    if (host_chars == 0 || host_chars >= host_len) {
        return false;
    }
    memcpy(host, p, host_chars);
    host[host_chars] = '\0';
    *port = 443;
    if (colon != NULL && (slash == NULL || colon < slash)) {
        int v = atoi(colon + 1);
        if (v > 0) {
            *port = v;
        }
    }
    *path = slash != NULL ? slash : "/";
    return true;
}

static esp_err_t http_upgrade(int fd, const char *host, const char *path, const char *headers,
                              uint32_t timeout_ms) {
    uint8_t key_raw[16];
    for (size_t i = 0; i < sizeof(key_raw); i += 4) {
        uint32_t r = esp_random();
        memcpy(key_raw + i, &r, 4);
    }
    unsigned char key_b64[32] = {0};
    size_t key_len = 0;
    if (mbedtls_base64_encode(key_b64, sizeof(key_b64) - 1, &key_len, key_raw, sizeof(key_raw)) != 0) {
        return ESP_FAIL;
    }
    key_b64[key_len] = '\0';

    char *req = malloc(1024);
    if (req == NULL) {
        return ESP_ERR_NO_MEM;
    }
    int n = snprintf(req, 1024,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\n"
                     "%s"
                     "\r\n",
                     path, host, (const char *)key_b64, headers != NULL ? headers : "");
    int err = ws_tls_write((const uint8_t *)req, (size_t)n, timeout_ms);
    free(req);
    if (err != 0) {
        ESP_LOGE(TAG, "发送升级请求失败 -0x%04X", -err);
        return ESP_FAIL;
    }

    // 读响应头(最多 2 KB),看状态码是不是 101。
    char resp[2048];
    size_t used = 0;
    int64_t deadline = now_ms() + timeout_ms;
    while (used + 1 < sizeof(resp)) {
        if (now_ms() >= deadline) {
            ESP_LOGE(TAG, "等待升级响应超时");
            return ESP_ERR_TIMEOUT;
        }
        if (used >= 4 && memcmp(resp + used - 4, "\r\n\r\n", 4) == 0) {
            break;
        }
        lock();
        int ret = mbedtls_ssl_read(&s_ws.ssl, (unsigned char *)resp + used, sizeof(resp) - 1 - used);
        unlock();
        if (ret > 0) {
            used += (size_t)ret;
            resp[used] = '\0';
            continue;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGE(TAG, "读升级响应失败 -0x%04X", -ret);
            return ESP_FAIL;
        }
        wait_fd(fd, false, 100);
    }
    resp[used] = '\0';
    const char *line_end = strstr(resp, "\r\n");
    if (line_end == NULL || strstr(resp, " 101 ") == NULL) {
        size_t show = line_end != NULL ? (size_t)(line_end - resp) : used;
        ESP_LOGE(TAG, "升级被拒绝: %.*s", (int)show, resp);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "升级成功: %.*s", (int)(line_end - resp), resp);
    return ESP_OK;
}

void linx_ws_set_verify_relaxed(bool relaxed) {
    s_ws.verify_relaxed = relaxed;
}

esp_err_t linx_ws_open(const char *url, const char *headers, const linx_ws_callbacks_t *cb,
                       uint32_t timeout_ms) {
    if (url == NULL || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    linx_ws_close();

    char host[96];
    int port = 443;
    const char *path = "/";
    if (!parse_url(url, host, sizeof(host), &port, &path)) {
        ESP_LOGE(TAG, "URL 解析失败: %s", url);
        return ESP_ERR_INVALID_ARG;
    }

    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS 解析 %s 失败", host);
        return ESP_ERR_NOT_FOUND;
    }
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return ESP_FAIL;
    }
    struct timeval tv = {.tv_sec = (time_t)(timeout_ms / 1000), .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "TCP 连接 %s:%d 失败", host, port);
        freeaddrinfo(res);
        close(fd);
        return ESP_FAIL;
    }
    freeaddrinfo(res);
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (s_ws.inited || s_ws.fd >= 0) {
        ESP_LOGW(TAG, "上次会话未关闭,先释放再重连");
        linx_ws_close();
    }
    memset(&s_ws.ssl, 0, sizeof(s_ws.ssl));
    memset(&s_ws.conf, 0, sizeof(s_ws.conf));
    memset(&s_ws.ca, 0, sizeof(s_ws.ca));
    mbedtls_ssl_init(&s_ws.ssl);
    mbedtls_ssl_config_init(&s_ws.conf);
    mbedtls_x509_crt_init(&s_ws.ca);
    mbedtls_entropy_init(&s_ws.entropy);
    mbedtls_ctr_drbg_init(&s_ws.drbg);
    s_ws.inited = true;
    s_ws.fd = fd;
    s_ws.cb = *cb;
    if (s_ws.lock == NULL) {
        s_ws.lock = xSemaphoreCreateMutex();
        if (s_ws.lock == NULL) {
            goto fail;
        }
    }

    char errbuf[96] = {0};
    int ret = mbedtls_ctr_drbg_seed(&s_ws.drbg, mbedtls_entropy_func, &s_ws.entropy,
                                    (const unsigned char *)"linx_ws", 7);
    if (ret == 0) {
        ret = mbedtls_x509_crt_parse(&s_ws.ca, (const unsigned char *)LINX_TRUSTED_PEM,
                                     strlen(LINX_TRUSTED_PEM) + 1);
    }
    if (ret != 0) {
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        ESP_LOGE(TAG, "装载信任根失败 -0x%04X %s", -ret, errbuf);
        goto fail;
    }
    {
        int n = 0;
        for (mbedtls_x509_crt *c = &s_ws.ca; c != NULL; c = c->next) {
            n++;
        }
        char subject[128] = {0};
        mbedtls_x509_dn_gets(subject, sizeof(subject), &s_ws.ca.subject);
        ESP_LOGI(TAG, "信任根 %d 张: %s (heap=%u 最大块=%u)", n, subject,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    if (mbedtls_ssl_config_defaults(&s_ws.conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        goto fail;
    }
    mbedtls_ssl_conf_rng(&s_ws.conf, mbedtls_ctr_drbg_random, &s_ws.drbg);
    mbedtls_ssl_conf_verify(&s_ws.conf, verify_log, NULL);
    // 默认严格校验。设备没有可信时钟或证书链临时变化时,校验会以 -0x2700 失败,
    // 表现为"语音整段不可用";调用方可显式降级一次(只在最后重试时用,会有告警日志)。
    mbedtls_ssl_conf_authmode(&s_ws.conf,
                              s_ws.verify_relaxed ? MBEDTLS_SSL_VERIFY_NONE
                                                  : MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&s_ws.conf, &s_ws.ca, NULL);
    {
        // 证书 notBefore 是绝对时间:系统时间还停在 1970/2021 时校验必然失败,
        // 报出来却是 "certificate not trusted",极难定位。这里提前把时间问题暴露出来。
        time_t now = time(NULL);
        if (now < 1767225600 /* 2026-01-01 */) {
            ESP_LOGW(TAG, "系统时间未同步(%lld),TLS 证书校验可能失败", (long long)now);
        }
    }
    if (mbedtls_ssl_setup(&s_ws.ssl, &s_ws.conf) != 0) {
        ESP_LOGE(TAG, "TLS setup 失败");
        goto fail;
    }
    mbedtls_ssl_set_hostname(&s_ws.ssl, host);
    mbedtls_ssl_set_bio(&s_ws.ssl, &s_ws.fd, bio_send, bio_recv, NULL);

    int64_t deadline = now_ms() + timeout_ms;
    while ((ret = mbedtls_ssl_handshake(&s_ws.ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            mbedtls_strerror(ret, errbuf, sizeof(errbuf));
            uint32_t flags = mbedtls_ssl_get_verify_result(&s_ws.ssl);
            char vinfo[256] = {0};
            mbedtls_x509_crt_verify_info(vinfo, sizeof(vinfo), "  ", flags);
            time_t t = time(NULL);
            struct tm tm_local;
            localtime_r(&t, &tm_local);
            char tbuf[32] = {0};
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm_local);
            ESP_LOGE(TAG, "TLS 握手失败 -0x%04X %s (verify flags=0x%08X)", -ret, errbuf,
                     (unsigned)flags);
            ESP_LOGE(TAG, "  校验说明: %s", vinfo);
            ESP_LOGE(TAG, "  设备时间: %s (%lld)", tbuf, (long long)t);
            goto fail;
        }
        if (now_ms() >= deadline) {
            ESP_LOGE(TAG, "TLS 握手超时");
            goto fail;
        }
        wait_fd(fd, ret == MBEDTLS_ERR_SSL_WANT_WRITE, 100);
    }
    mbedtls_x509_crt_free(&s_ws.ca);
    mbedtls_entropy_free(&s_ws.entropy);
    mbedtls_x509_crt_init(&s_ws.ca);
    mbedtls_entropy_init(&s_ws.entropy);
    ESP_LOGI(TAG, "TLS 就绪: %s:%d (会话后 heap=%u 最大块=%u)", host, port,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    if (http_upgrade(fd, host, path, headers, timeout_ms) != ESP_OK) {
        goto fail;
    }
    s_ws.open = true;
    s_ws.stop = false;
    s_ws.ready = true;
    if (xTaskCreate(rx_task, "linx_ws_rx", WS_RX_STACK, NULL, 5, &s_ws.rx_task) != pdPASS) {
        ESP_LOGE(TAG, "无法创建接收任务");
        goto fail;
    }
    return ESP_OK;

fail:
    s_ws.open = false;
    if (fd >= 0) {
        close(fd);
    }
    s_ws.fd = -1;
    mbedtls_ssl_free(&s_ws.ssl);
    mbedtls_ssl_config_free(&s_ws.conf);
    mbedtls_x509_crt_free(&s_ws.ca);
    mbedtls_ctr_drbg_free(&s_ws.drbg);
    mbedtls_entropy_free(&s_ws.entropy);
    s_ws.inited = false;
    return ESP_FAIL;
}

esp_err_t linx_ws_send_text(const char *data, size_t len) {
    return ws_send_frame(0x01, (const uint8_t *)data, len, 5000);
}

esp_err_t linx_ws_send_bin(const void *data, size_t len) {
    return ws_send_frame(0x02, (const uint8_t *)data, len, 5000);
}

bool linx_ws_is_open(void) {
    return s_ws.open;
}

void linx_ws_close(void) {
    if (s_ws.rx_task != NULL) {
        s_ws.stop = true;
        // 等接收任务自己退出,避免它随后访问已释放的 TLS 上下文。
        for (int i = 0; i < 30 && s_ws.rx_task != NULL; ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (s_ws.rx_task != NULL) {
            vTaskDelete(s_ws.rx_task);
            s_ws.rx_task = NULL;
        }
    }
    if (s_ws.ready) {
        lock();
        mbedtls_ssl_close_notify(&s_ws.ssl);
        unlock();
        s_ws.ready = false;
    }
    if (s_ws.fd >= 0) {
        close(s_ws.fd);
        s_ws.fd = -1;
    }
    s_ws.open = false;
    if (s_ws.inited) {
        s_ws.inited = false;
        mbedtls_ssl_free(&s_ws.ssl);
        mbedtls_ssl_config_free(&s_ws.conf);
        mbedtls_x509_crt_free(&s_ws.ca);
        mbedtls_ctr_drbg_free(&s_ws.drbg);
        mbedtls_entropy_free(&s_ws.entropy);
    }
    ESP_LOGI(TAG, "关闭后 heap=%u 最大块=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    free(s_ws.msg);
    s_ws.msg = NULL;
    s_ws.msg_len = 0;
    s_ws.msg_cap = 0;
    s_ws.msg_opcode = 0;
}
