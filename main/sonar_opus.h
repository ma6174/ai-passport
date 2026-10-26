// main/sonar_opus.h
// 上行语音编码:把录音 PCM(16 kHz/16-bit/单声道)编成 Opus 包。
// 目的只有一个——把 WS 上行从"实时 PCM 60 KB"压到"约 20 KB / 8 秒",
// 上传时间不再和说话时长成正比。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// 一帧 = 20 ms @16 kHz。libopus 的 SILK 编码在栈上按帧长开数组:60 ms 帧实测
// 会吃掉 >12 KB 栈("Stack protection fault"),20 ms 帧的栈需求小得多。
#define SONAR_OPUS_FRAME_SAMPLES 320
#define SONAR_OPUS_FRAME_MS 20
#define SONAR_OPUS_BITRATE 24000

// 上行是否走 Opus。
// 实测结论(2026-10-26,真实灵矽平台):
// - 编码本身没问题:4.9 秒录音 → 133 包 / 191 ms 即可传完(同长度 PCM 约 2 秒);
// - 但服务端只按 hello 协商的格式解上行,本平台协商结果仍是 PCM,
//   在 listen 里声明 opus 拿不到转写,必须先改 hello —— 那会连带把下行 TTS 变成
//   opus(24 kHz/60 ms),需要再做解码器。所以默认关掉,代码留着等平台支持。
#define SONAR_OPUS_UPLINK_ENABLED 0

// 懒创建编码器并常驻(约 20~30 KB);失败返回错误,调用方回退 PCM。
esp_err_t sonar_opus_open(void);
void sonar_opus_close(void);
bool sonar_opus_ready(void);

// 编码一帧(必须正好 SONAR_OPUS_FRAME_SAMPLES 个采样);返回包长度,失败 < 0。
int sonar_opus_encode(const int16_t *pcm, uint8_t *out, size_t out_len);
