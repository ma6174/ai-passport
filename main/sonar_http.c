// main/sonar_http.c
// HTTPS 客户端封装。所有请求都走证书校验(crt bundle),并带明确的响应上限。
#include "sonar_http.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lwip/netdb.h"

static const char *TAG = "sonar_http";
#define HTTP_TIMEOUT_MS 10000
#define HTTP_BUF 2048
#define TX_BUF 1024
#define IO_CHUNK 2048

void sonar_http_resp_init(sonar_http_resp_t *resp, char *buf, size_t cap) {
    if (resp == NULL) {
        return;
    }
    resp->body = buf;
    resp->cap = cap;
    resp->len = 0;
    resp->status = 0;
    resp->content_length = 0;
    if (buf != NULL && cap > 0) {
        buf[0] = '\0';
    }
}

// 连接前把主机名解析一次:失败说明是 DNS,而不是 TLS/证书,排障时省很多来回。
static void log_resolution(const char *url) {
    const char *host = url;
    if (strncmp(host, "https://", 8) == 0) {
        host += 8;
    } else if (strncmp(host, "http://", 7) == 0) {
        host += 7;
    }
    char name[96];
    size_t n = 0;
    while (host[n] != '\0' && host[n] != ':' && host[n] != '/' && n + 1 < sizeof(name)) {
        name[n] = host[n];
        ++n;
    }
    name[n] = '\0';
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(name, NULL, &hints, &res);
    if (rc != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS 解析失败 %s rc=%d heap=%u", name, rc, (unsigned)esp_get_free_heap_size());
        return;
    }
    char ip[16] = {0};
    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    snprintf(ip, sizeof(ip), "%s", inet_ntoa(addr->sin_addr));
    freeaddrinfo(res);
    ESP_LOGI(TAG, "DNS %s -> %s heap=%u", name, ip, (unsigned)esp_get_free_heap_size());
}

static const char *s_trust_anchor;

void sonar_http_set_trust_anchor(const char *pem) {
    s_trust_anchor = pem;
}

static esp_err_t base_config(esp_http_client_config_t *cfg, const char *url) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    // 只有 https 才需要信任根;http 纯明文,挂 bundle 是无意义开销。
    if (strncmp(url, "https://", 8) == 0) {
        if (s_trust_anchor != NULL) {
            cfg->cert_pem = s_trust_anchor;   // esp_http_client 用 cert_pem 表示信任锚
            cfg->cert_len = 0;                // 0 = 以 NUL 结尾的 PEM
        } else {
            cfg->crt_bundle_attach = esp_crt_bundle_attach;
        }
    }
    cfg->timeout_ms = HTTP_TIMEOUT_MS;
    cfg->buffer_size = HTTP_BUF;
    cfg->buffer_size_tx = TX_BUF;
    cfg->disable_auto_redirect = true;   // 不向重定向目标转发 Token
    cfg->keep_alive_enable = true;
    return ESP_OK;
}

static void set_common_headers(esp_http_client_handle_t client, const char *token,
                               const char *content_type) {
    if (token != NULL && token[0] != '\0') {
        char auth[160];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    if (content_type != NULL) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }
}

// 读完响应体,最多 cap-1 字节;超过上限即报错,绝不静默截断成"看似成功"的 JSON。
static esp_err_t read_body(esp_http_client_handle_t client, sonar_http_resp_t *resp) {
    if (resp == NULL || resp->body == NULL || resp->cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t used = 0;
    while (true) {
        int space = (int)(resp->cap - 1 - used);
        if (space <= 0) {
            ESP_LOGE(TAG, "响应超过 %u 字节上限", (unsigned)resp->cap);
            return ESP_ERR_INVALID_SIZE;
        }
        int read = esp_http_client_read(client, resp->body + used, space);
        if (read < 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (read == 0) {
            break;
        }
        used += (size_t)read;
    }
    resp->body[used] = '\0';
    resp->len = used;
    return ESP_OK;
}

static esp_err_t request(const char *method, const char *url, const char *token,
                         const char *content_type, const char *body, size_t body_len,
                         const sonar_http_header_t *headers, size_t header_count,
                         sonar_http_resp_t *resp) {
    if (url == NULL || resp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    log_resolution(url);
    esp_http_client_config_t cfg;
    base_config(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (strcmp(method, "POST") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else if (strcmp(method, "PUT") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_PUT);
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    }
    set_common_headers(client, token, content_type);
    for (size_t i = 0; i < header_count; ++i) {
        if (headers[i].name != NULL && headers[i].value != NULL) {
            esp_http_client_set_header(client, headers[i].name, headers[i].value);
        }
    }

    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接 %s 失败: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    size_t sent = 0;
    while (sent < body_len) {
        int wrote = esp_http_client_write(client, body + sent, (int)(body_len - sent));
        if (wrote <= 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_RESPONSE;
        }
        sent += (size_t)wrote;
    }
    int64_t declared = esp_http_client_fetch_headers(client);
    resp->content_length = declared > 0 ? (size_t)declared : 0;
    resp->status = esp_http_client_get_status_code(client);
    err = read_body(client, resp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGD(TAG, "%s %s -> %d (%u 字节)", method, url, resp->status, (unsigned)resp->len);
    return ESP_OK;
}

esp_err_t sonar_http_json(const char *method, const char *url, const char *token,
                          const char *json, sonar_http_resp_t *resp) {
    size_t body_len = json == NULL ? 0 : strlen(json);
    return request(method, url, token, json == NULL ? NULL : "application/json", json, body_len,
                   NULL, 0, resp);
}

esp_err_t sonar_http_request_ex(const char *method, const char *url, const char *token,
                                const char *content_type, const char *body, size_t body_len,
                                const sonar_http_header_t *headers, size_t header_count,
                                sonar_http_resp_t *resp) {
    return request(method, url, token, content_type, body, body_len, headers, header_count, resp);
}

esp_err_t sonar_http_text(const char *method, const char *url, const char *extra_header,
                          const char *extra_value, sonar_http_resp_t *resp) {
    sonar_http_header_t headers[1] = {{extra_header, extra_value}};
    size_t count = (extra_header != NULL && extra_value != NULL) ? 1 : 0;
    return request(method, url, NULL, NULL, NULL, 0, headers, count, resp);
}

esp_err_t sonar_http_download(const char *url, const char *token, const char *path,
                             size_t max_bytes, size_t *out_len, char *content_type,
                             size_t content_type_len) {
    if (url == NULL || path == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0;
    esp_http_client_config_t cfg;
    base_config(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    set_common_headers(client, token, NULL);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    int64_t declared = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (content_type != NULL && content_type_len > 0) {
        char *ct = NULL;
        content_type[0] = '\0';
        if (esp_http_client_get_header(client, "content-type", &ct) == ESP_OK && ct != NULL) {
            snprintf(content_type, content_type_len, "%s", ct);
        }
    }
    if (status != 200 || (declared > 0 && (size_t)declared > max_bytes)) {
        ESP_LOGW(TAG, "下载被拒: status=%d len=%lld", status, (long long)declared);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    uint8_t chunk[IO_CHUNK];
    size_t total = 0;
    err = ESP_OK;
    while (true) {
        int read = esp_http_client_read(client, (char *)chunk, sizeof(chunk));
        if (read < 0) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (read == 0) {
            break;
        }
        if (total + (size_t)read > max_bytes) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (fwrite(chunk, 1, (size_t)read, fp) != (size_t)read) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        total += (size_t)read;
    }
    fclose(fp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        remove(path);
        return err;
    }
    *out_len = total;
    return ESP_OK;
}

esp_err_t sonar_http_upload_wav(const char *url, const char *token, const char *metadata_json,
                                const char *wav_path, size_t wav_len, sonar_http_resp_t *resp) {
    if (url == NULL || metadata_json == NULL || wav_path == NULL || resp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *boundary = "sonar-boundary";
    char head[320];
    int head_len = snprintf(head, sizeof(head),
                            "--%s\r\n"
                            "Content-Disposition: form-data; name=\"metadata\"\r\n\r\n",
                            boundary);
    if (head_len <= 0 || (size_t)head_len >= sizeof(head)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char mid[256];
    int mid_len = snprintf(mid, sizeof(mid),
                           "\r\n--%s\r\n"
                           "Content-Disposition: form-data; name=\"audio\"; "
                           "filename=\"voice.wav\"\r\n"
                           "Content-Type: audio/wav\r\n\r\n",
                           boundary);
    if (mid_len <= 0 || (size_t)mid_len >= sizeof(mid)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char tail[64];
    int tail_len = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", boundary);
    size_t meta_len = strlen(metadata_json);
    size_t total = (size_t)head_len + meta_len + (size_t)mid_len + wav_len + (size_t)tail_len;

    esp_http_client_config_t cfg;
    base_config(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    char content_type[64];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    set_common_headers(client, token, content_type);

    esp_err_t err = esp_http_client_open(client, (int)total);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    if (esp_http_client_write(client, head, head_len) != head_len) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    if (esp_http_client_write(client, metadata_json, (int)meta_len) != (int)meta_len) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    if (esp_http_client_write(client, mid, mid_len) != mid_len) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    FILE *fp = fopen(wav_path, "rb");
    if (fp == NULL) {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    uint8_t chunk[IO_CHUNK];
    size_t sent = 0;
    while (sent < wav_len) {
        size_t want = wav_len - sent;
        if (want > sizeof(chunk)) {
            want = sizeof(chunk);
        }
        size_t got = fread(chunk, 1, want, fp);
        if (got == 0) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (esp_http_client_write(client, (const char *)chunk, (int)got) != (int)got) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        sent += got;
    }
    fclose(fp);
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (esp_http_client_write(client, tail, tail_len) != tail_len) {
        err = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    int64_t declared = esp_http_client_fetch_headers(client);
    resp->content_length = declared > 0 ? (size_t)declared : 0;
    resp->status = esp_http_client_get_status_code(client);
    err = read_body(client, resp);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}
