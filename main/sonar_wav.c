// main/sonar_wav.c
// Sonar 原声的 WAV 编解码。Sonar 要求 16 kHz / 16-bit / 单声道、0 < 帧数 ≤ 128000、
// 文件 ≤ 300 KiB；本文件不依赖 ESP-IDF，便于主机测试。
#include "sonar_wav.h"

#include <string.h>

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void wr_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

void sonar_wav_header(uint8_t out[SONAR_WAV_HEADER_BYTES], uint32_t pcm_bytes) {
    uint8_t *p = out;
    memcpy(p, "RIFF", 4);
    wr_u32(p + 4, 36u + pcm_bytes);
    memcpy(p + 8, "WAVE", 4);
    memcpy(p + 12, "fmt ", 4);
    wr_u32(p + 16, 16);
    wr_u16(p + 20, 1);                        // PCM
    wr_u16(p + 22, SONAR_WAV_CHANNELS);       // 单声道
    wr_u32(p + 24, SONAR_WAV_RATE);           // 16 kHz
    wr_u32(p + 28, SONAR_WAV_RATE * 2u);      // byte rate
    wr_u16(p + 32, 2);                        // block align
    wr_u16(p + 34, SONAR_WAV_BITS);           // 16 bit
    memcpy(p + 36, "data", 4);
    wr_u32(p + 40, pcm_bytes);
}

int sonar_wav_parse(const uint8_t *buf, size_t len, sonar_wav_info_t *info) {
    if (buf == NULL || info == NULL) {
        return SONAR_WAV_ERR_PARAM;
    }
    memset(info, 0, sizeof(*info));
    // 只要求读到的前缀里包含 RIFF 头与各 chunk 头；data 本体可以不在缓冲区内，
    // 因此这里按 chunk 头推进，而不是要求 data 完整。
    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return SONAR_WAV_ERR_RIFF;
    }
    size_t pos = 12;
    bool have_fmt = false;
    while (pos + 8 <= len) {
        const uint8_t *id = buf + pos;
        uint32_t size = rd_u32(buf + pos + 4);
        size_t body = pos + 8;
        if (memcmp(id, "fmt ", 4) == 0) {
            if (size < 16 || body + 16 > len) {
                return SONAR_WAV_ERR_FMT;
            }
            info->format = rd_u16(buf + body);
            info->channels = rd_u16(buf + body + 2);
            info->sample_rate = rd_u32(buf + body + 4);
            info->bits = rd_u16(buf + body + 14);
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!have_fmt) {
                return SONAR_WAV_ERR_FMT;
            }
            info->data_offset = (uint32_t)body;
            info->data_bytes = size;
            return SONAR_WAV_OK;
        }
        pos = body + size + (size & 1u);  // chunk 按偶数字节对齐
        if (pos > SONAR_WAV_MAX_HEADER_SCAN) {
            return SONAR_WAV_ERR_CHUNK;
        }
    }
    if (!have_fmt) {
        return SONAR_WAV_ERR_FMT;
    }
    return SONAR_WAV_ERR_NO_DATA;
}

int sonar_wav_validate(const sonar_wav_info_t *info) {
    if (info == NULL) {
        return SONAR_WAV_ERR_PARAM;
    }
    if (info->format != 1 || info->channels != SONAR_WAV_CHANNELS ||
        info->bits != SONAR_WAV_BITS || info->sample_rate != SONAR_WAV_RATE) {
        return SONAR_WAV_ERR_FORMAT;
    }
    uint32_t frame_bytes = (uint32_t)info->channels * (info->bits / 8u);
    if (frame_bytes == 0 || info->data_bytes % frame_bytes != 0) {
        return SONAR_WAV_ERR_FORMAT;
    }
    if (info->data_bytes == 0 || info->data_bytes > SONAR_WAV_MAX_PCM_BYTES) {
        return SONAR_WAV_ERR_SIZE;
    }
    return SONAR_WAV_OK;
}

uint32_t sonar_wav_frames(const sonar_wav_info_t *info) {
    if (info == NULL || info->channels == 0 || info->bits == 0) {
        return 0;
    }
    return info->data_bytes / ((uint32_t)info->channels * (info->bits / 8u));
}

uint32_t sonar_wav_max_pcm_bytes(void) {
    return SONAR_WAV_MAX_FRAMES * SONAR_WAV_CHANNELS * (SONAR_WAV_BITS / 8u);
}
