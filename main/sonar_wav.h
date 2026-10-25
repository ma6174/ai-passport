// main/sonar_wav.h
// Sonar 原声的 WAV 头生成、前缀解析与规格校验。纯 C,不依赖 ESP-IDF,由主机测试覆盖。
//
// 服务端契约:16 kHz / 16-bit signed LE / 单声道,0 < 帧数 ≤ 128000(8 秒),
// WAV 文件 ≤ 300 KiB(256000 字节 PCM + 头)。解析必须按 RIFF chunk 找 data,
// 不能假设头永远是 44 字节。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SONAR_WAV_RATE 16000
#define SONAR_WAV_CHANNELS 1
#define SONAR_WAV_BITS 16
#define SONAR_WAV_MAX_FRAMES 128000u
// 256000 字节 PCM 是 8 秒;再多服务端会以 AUDIO_TOO_LARGE 拒绝。
#define SONAR_WAV_MAX_PCM_BYTES (SONAR_WAV_MAX_FRAMES * SONAR_WAV_CHANNELS * (SONAR_WAV_BITS / 8))
// 标准 44 字节头(PCM + 单个 data chunk)。
#define SONAR_WAV_HEADER_BYTES 44u
// 解析前缀时的 chunk 扫描上限,防止畸形文件让偏移无限推进。
#define SONAR_WAV_MAX_HEADER_SCAN 8192u

enum {
    SONAR_WAV_OK = 0,
    SONAR_WAV_ERR_PARAM = -1,
    SONAR_WAV_ERR_RIFF = -2,
    SONAR_WAV_ERR_FMT = -3,
    SONAR_WAV_ERR_CHUNK = -4,
    SONAR_WAV_ERR_NO_DATA = -5,
    SONAR_WAV_ERR_FORMAT = -6,
    SONAR_WAV_ERR_SIZE = -7,
};

typedef struct {
    uint16_t format;       // 1 = PCM
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits;
    uint32_t data_offset;  // PCM 数据在文件内的偏移
    uint32_t data_bytes;   // data chunk 声明的字节数
} sonar_wav_info_t;

// 生成标准 44 字节头。录音时可先用 pcm_bytes=0 写占位头,录完再回填真实长度。
void sonar_wav_header(uint8_t out[SONAR_WAV_HEADER_BYTES], uint32_t pcm_bytes);

// 解析文件前缀(只需包含 RIFF 头与各 chunk 头;data 本体可不在缓冲区内)。
// 返回 SONAR_WAV_OK 且填好 info;否则返回负错误码。
int sonar_wav_parse(const uint8_t *buf, size_t len, sonar_wav_info_t *info);

// 校验是否符合服务端规格(格式/声道/采样率/帧数/字节上限)。
int sonar_wav_validate(const sonar_wav_info_t *info);

uint32_t sonar_wav_frames(const sonar_wav_info_t *info);

// 服务端允许的最大 PCM 字节数(= 8 秒)。
uint32_t sonar_wav_max_pcm_bytes(void);
