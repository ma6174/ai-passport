// main/sonar_http.h
// 受限的 HTTPS 客户端:统一证书校验、响应上限与超时,供 Sonar 与 Linx OTA 共用。
//
// 所有响应都必须有明确上限(交接书要求:已知长度先拒超限,未知长度累计到上限为止)。
#pragma once

#include <stddef.h>

#include "esp_err.h"

#define SONAR_HTTP_RESP_MAX 16384

typedef struct {
    char *body;   // 调用方提供
    size_t cap;   // 含结尾 '\0' 的容量
    size_t len;   // 已写入字节数(不含 '\0')
    int status;   // HTTP 状态码
    size_t content_length;  // 服务端声明的长度,0 表示未声明
} sonar_http_resp_t;

void sonar_http_resp_init(sonar_http_resp_t *resp, char *buf, size_t cap);

// 指定 https 请求的信任锚(PEM)。NULL 表示用系统证书 bundle。
// 灵矽的链是 DigiCert G2 的交叉签名版,系统 bundle 验不过,需要换成签发 leaf 的
// 中间 CA;Sonar 走明文 HTTP,不受这条设置影响。
void sonar_http_set_trust_anchor(const char *pem);

typedef struct {
    const char *name;
    const char *value;
} sonar_http_header_t;

// 通用请求:可选自定义头(Linx OTA 需要 Device-Id/Client-Id/Activation-Version)。
esp_err_t sonar_http_request_ex(const char *method, const char *url, const char *token,
                                const char *content_type, const char *body, size_t body_len,
                                const sonar_http_header_t *headers, size_t header_count,
                                sonar_http_resp_t *resp);

// 发送 JSON 或空体请求。transport 失败返回错误;HTTP 4xx/5xx 仍返回 ESP_OK,
// 由调用方按 status 与错误体分支处理。
esp_err_t sonar_http_json(const char *method, const char *url, const char *token,
                          const char *json, sonar_http_resp_t *resp);

// 纯文本响应(如 Linx `/v1/ota/activate` 返回 `success`)。
esp_err_t sonar_http_text(const char *method, const char *url, const char *extra_header,
                          const char *extra_value, sonar_http_resp_t *resp);

// multipart 上传:metadata 为 JSON 文本字段,audio 为磁盘上的 WAV 文件。
// 整个请求体分段写出,不把音频复制成大字符串(交接书要求)。
esp_err_t sonar_http_upload_wav(const char *url, const char *token, const char *metadata_json,
                                const char *wav_path, size_t wav_len, sonar_http_resp_t *resp);

// 下载二进制到文件,边收边写;超过 max_bytes 立即失败并删除半成品。
esp_err_t sonar_http_download(const char *url, const char *token, const char *path,
                             size_t max_bytes, size_t *out_len, char *content_type,
                             size_t content_type_len);
