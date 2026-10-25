// main/linx_client.h
// 灵矽(Linx)语音接入:设备激活、WSS 语音会话、STT 与私人助手 TTS。
//
// 设备身份:Device-Id 用本机 Wi-Fi STA MAC(稳定、MAC 格式),Client-Id 是持久化
// UUID v4;两者与 Sonar 的设备 Token 相互独立。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sonar_config.h"

typedef struct {
    char code[16];  // 未绑定时平台返回的激活码
    bool bound;     // true 表示已拿到 websocket url/token
} linx_ota_result_t;

// 计算并缓存本机身份(MAC 格式 Device-Id)。启动时调一次即可。
esp_err_t linx_client_init(void);

// 语音链路进度(连接/重试)回报给界面,避免用户只看到静止的 transcribing。
void linx_set_progress(void (*cb)(const char *msg));

const char *linx_device_id(void);

// 走官方 OTA:未绑定返回激活码(供用户在灵矽智控台绑定),已绑定则把
// websocket url/token 写回 cfg 并返回 bound=true。
esp_err_t linx_ota(sonar_config_t *cfg, linx_ota_result_t *out);

// 只做一次 WSS 握手探针(hello 往返),用完立刻断开。用于开机自检和排障,
// 不播放、不发送任何录音。
esp_err_t linx_probe(const sonar_config_t *cfg);

// 上报的语音转写:把已录好的 WAV 的 PCM 部分按协商格式发送,返回唯一候选文本。
// 超时/无转写返回错误,调用方必须把原声留在本地,不发布。
esp_err_t linx_speech_to_text(const sonar_config_t *cfg, const char *wav_path, char *text,
                              size_t text_len, uint32_t timeout_ms);

// 私人助手对话:同一段语音,服务端 TTS 的 PCM 写入 pcm_path(补 WAV 头),
// reply_text 返回文本(可为 NULL)。服务端给 Opus 时明确失败,不播放压缩字节。
esp_err_t linx_chat(const sonar_config_t *cfg, const char *wav_path, const char *pcm_path,
                    char *reply_text, size_t reply_len, uint32_t timeout_ms);
