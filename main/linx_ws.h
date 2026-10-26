// main/linx_ws.h
// 最小 WebSocket(wss) 客户端:esp_tls 负责 TLS,本模块负责升级握手与帧编解码。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    // 收到一条完整文本消息(服务端 JSON)。
    void (*on_text)(const char *data, size_t len);
    // 收到一条完整二进制消息(服务端下行的 PCM 音频)。
    void (*on_bin)(const uint8_t *data, size_t len);
    // 连接被对端关闭或读出错,后续不会再回调。
    void (*on_close)(void);
} linx_ws_callbacks_t;

// 连接 wss://host[:port]/path,headers 是 "Key: Value\r\n" 拼接的额外请求头。
// 关闭证书校验:仅在严格校验反复失败时作为最后一次尝试,调用方负责打印告警。
void linx_ws_set_verify_relaxed(bool relaxed);

esp_err_t linx_ws_open(const char *url, const char *headers, const linx_ws_callbacks_t *cb,
                       uint32_t timeout_ms);
esp_err_t linx_ws_send_text(const char *data, size_t len);
esp_err_t linx_ws_send_bin(const void *data, size_t len);
bool linx_ws_is_open(void);
void linx_ws_close(void);
