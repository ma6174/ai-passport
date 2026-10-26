// main/sonar_ogg_mux.c
// 裸 Opus 包 -> Ogg Opus 容器(单声道语音文件,给业务上传)。
//
// 页结构按 RFC 3533 写入:27 字节页头 + lacing 长度表 + payload。
// 音频页只放完整的 Opus 包(不跨页续包),所以 header_type 里从不置
// continuation(0x01);单个超过页粒度上限的包独占一页。
// granule position 一律按 48 kHz 采样累加,最后一页(EOS)等于总采样数,
// 播放器据此算时长:duration = (granule - pre_skip) / 48000。
#include "sonar_ogg_mux.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "sonar_ogg_mux";

#define OGG_PAGE_HEADER_BYTES 27
// lacing 表长度字段只有一个字节,单页最多 255 段。
#define OGG_MAX_PAGE_SEGS 255
// 一个包必须以"小于 255"的段收尾(它标记包结束),所以单页能装的最大包是
// 254 段 255 字节 + 1 段 254 字节 = 65024 字节,正好用满 255 段。
#define OGG_MAX_PACKET_BYTES (254u * 255u + 254u)

// header_type 的三个位:BOS 0x02、EOS 0x04、continuation 0x01。
// 本模块的页都从包边界开始,所以从不置 continuation。
#define OGG_FLAG_BOS 0x02
#define OGG_FLAG_EOS 0x04

// Ogg 的 CRC32:多项式 0x04C11DB7,初值 0,输入输出都不反射,结果不异或。
// 和 zlib 的 crc32(反射 + 异或 0xFFFFFFFF)是两套算法,混用会让播放器整页丢弃。
#define OGG_CRC_POLY 0x04C11DB7u

// 固定序列号。模块内一次只封一个流,播放器按页里的 serial 分组即可,
// 不需要跨文件唯一,固定值省掉 esp_random 依赖。
#define OGG_SERIAL 0x4F505553u  // "OPUS"

// OpusHead 的 input sample rate 只是给工具看的提示:我们的编码器是 16 kHz,
// 解码后的采样率恒为 48 kHz。
#define OGG_INPUT_RATE 16000

// vendor 字符串上限:保证 OpusTags 仍能放进一页的单个 lacing 段(< 255 字节)。
#define OGG_MAX_VENDOR_BYTES 96

typedef struct {
    bool active;
    uint32_t page_seq;
    uint64_t granule;   // 已写进页的 48 kHz 采样数
    size_t payload_len; // 当前页累积的 payload
    size_t seg_count;   // 当前页累积的 lacing 段数
    uint8_t payload[SONAR_OGG_MUX_PAGE_PAYLOAD];
    uint8_t segs[OGG_MAX_PAGE_SEGS];
} ogg_mux_t;

static ogg_mux_t s_mux;

static void wr_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void wr_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
}

static uint32_t crc32_ogg(const uint8_t *data, size_t len, uint32_t crc) {
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ OGG_CRC_POLY) : (crc << 1);
        }
    }
    return crc;
}

// 写一个完整页:页头 + lacing 表 + payload。页序号随之自增。
static esp_err_t emit_page(FILE *fp, const uint8_t *payload, size_t payload_len,
                           const uint8_t *segs, size_t seg_count, uint64_t granule,
                           uint8_t flags) {
    if (seg_count > OGG_MAX_PAGE_SEGS) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t hdr[OGG_PAGE_HEADER_BYTES + OGG_MAX_PAGE_SEGS];
    memset(hdr, 0, OGG_PAGE_HEADER_BYTES);
    memcpy(hdr, "OggS", 4);
    hdr[4] = 0;  // 版本
    hdr[5] = flags;
    wr_le64(hdr + 6, granule);
    wr_le32(hdr + 14, OGG_SERIAL);
    wr_le32(hdr + 18, s_mux.page_seq);
    // hdr[22..25] 是 CRC 占位,算完再回填。
    hdr[26] = (uint8_t)seg_count;
    if (seg_count > 0) {
        memcpy(hdr + OGG_PAGE_HEADER_BYTES, segs, seg_count);
    }
    size_t hdr_len = OGG_PAGE_HEADER_BYTES + seg_count;
    uint32_t crc = crc32_ogg(hdr, hdr_len, 0);
    crc = crc32_ogg(payload, payload_len, crc);
    wr_le32(hdr + 22, crc);

    if (fwrite(hdr, 1, hdr_len, fp) != hdr_len) {
        ESP_LOGE(TAG, "写 Ogg 页头失败(页 %u)", (unsigned)s_mux.page_seq);
        return ESP_FAIL;
    }
    if (payload_len > 0 && fwrite(payload, 1, payload_len, fp) != payload_len) {
        ESP_LOGE(TAG, "写 Ogg 页 payload 失败(页 %u, %u 字节)", (unsigned)s_mux.page_seq,
                 (unsigned)payload_len);
        return ESP_FAIL;
    }
    s_mux.page_seq++;
    return ESP_OK;
}

static esp_err_t flush_page(FILE *fp, uint8_t flags) {
    esp_err_t err = emit_page(fp, s_mux.payload, s_mux.payload_len, s_mux.segs, s_mux.seg_count,
                              s_mux.granule, flags);
    s_mux.payload_len = 0;
    s_mux.seg_count = 0;
    return err;
}

// 生成一个包的 lacing:每满 255 字节一段 255,最后一段写余数(可为 0)。
// 返回段数;调用方需保证 segs 至少有 len/255 + 1 字节空间。
static size_t fill_lacing(uint8_t *segs, size_t len) {
    size_t full = len / 255;
    if (full > 0) {
        memset(segs, 255, full);
    }
    segs[full] = (uint8_t)(len % 255);
    return full + 1;
}

esp_err_t sonar_ogg_mux_begin(FILE *fp, int channels, uint16_t pre_skip_48k,
                             const char *vendor) {
    if (fp == NULL || channels <= 0 || channels > 255) {
        ESP_LOGE(TAG, "begin 参数非法: fp=%p, channels=%d", (void *)fp, channels);
        return ESP_ERR_INVALID_ARG;
    }
    size_t vendor_len = (vendor != NULL) ? strlen(vendor) : 0;
    if (vendor_len > OGG_MAX_VENDOR_BYTES) {
        ESP_LOGE(TAG, "vendor 太长: %u > %d 字节", (unsigned)vendor_len, OGG_MAX_VENDOR_BYTES);
        return ESP_ERR_INVALID_ARG;
    }
    memset(&s_mux, 0, sizeof(s_mux));
    s_mux.active = true;

    // 第 1 页:OpusHead,19 字节,BOS。必须单独占一页。
    uint8_t head[19];
    memcpy(head, "OpusHead", 8);
    head[8] = 1;                       // 版本
    head[9] = (uint8_t)channels;       // 声道数
    wr_le16(head + 10, pre_skip_48k);  // pre-skip(48 kHz 采样)
    wr_le32(head + 12, OGG_INPUT_RATE);
    wr_le16(head + 16, 0);  // output gain
    head[18] = 0;           // mapping family:单流
    uint8_t segs[1] = {(uint8_t)sizeof(head)};
    esp_err_t err = emit_page(fp, head, sizeof(head), segs, 1, 0, OGG_FLAG_BOS);
    if (err != ESP_OK) {
        s_mux.active = false;
        return err;
    }

    // 第 2 页:OpusTags = magic + vendor 长度 + vendor + comment 条数(0)。
    uint8_t tags[8 + 4 + OGG_MAX_VENDOR_BYTES + 4];
    memcpy(tags, "OpusTags", 8);
    wr_le32(tags + 8, (uint32_t)vendor_len);
    if (vendor_len > 0) {
        memcpy(tags + 12, vendor, vendor_len);
    }
    wr_le32(tags + 12 + vendor_len, 0);
    size_t tags_len = 12 + vendor_len + 4;
    uint8_t tags_segs[1] = {(uint8_t)tags_len};
    err = emit_page(fp, tags, tags_len, tags_segs, 1, 0, 0);
    if (err != ESP_OK) {
        s_mux.active = false;
        return err;
    }

    ESP_LOGI(TAG, "Ogg Opus 开始: %d 声道, pre-skip %u, vendor \"%s\"", channels,
             (unsigned)pre_skip_48k, (vendor != NULL) ? vendor : "");
    return ESP_OK;
}

esp_err_t sonar_ogg_mux_write(FILE *fp, const uint8_t *packet, size_t len,
                             uint32_t packet_samples_48k) {
    if (!s_mux.active) {
        ESP_LOGE(TAG, "write 前没有 begin");
        return ESP_ERR_INVALID_STATE;
    }
    if (fp == NULL || packet == NULL || len == 0) {
        ESP_LOGE(TAG, "write 参数非法: fp=%p, packet=%p, len=%u", (void *)fp,
                 (const void *)packet, (unsigned)len);
        return ESP_ERR_INVALID_ARG;
    }
    if (len > OGG_MAX_PACKET_BYTES) {
        ESP_LOGE(TAG, "包太大: %u > %u 字节", (unsigned)len, (unsigned)OGG_MAX_PACKET_BYTES);
        return ESP_ERR_INVALID_ARG;
    }

    size_t seg_count = len / 255 + 1;
    // 当前页装不下这个包就先落盘,保证一个包不跨页。
    if (s_mux.payload_len > 0 &&
        (s_mux.payload_len + len > SONAR_OGG_MUX_PAGE_PAYLOAD ||
         s_mux.seg_count + seg_count > OGG_MAX_PAGE_SEGS)) {
        esp_err_t err = flush_page(fp, 0);
        if (err != ESP_OK) {
            return err;
        }
    }

    // granule 记的是"已经写进页的包"的采样数,所以在 flush 之后累加。
    s_mux.granule += packet_samples_48k;

    if (len > SONAR_OGG_MUX_PAGE_PAYLOAD) {
        // 大于页粒度上限的包独占一页:直接从调用方缓冲写,不再多拷一次。
        fill_lacing(s_mux.segs, len);
        return emit_page(fp, packet, len, s_mux.segs, seg_count, s_mux.granule, 0);
    }

    s_mux.seg_count += fill_lacing(&s_mux.segs[s_mux.seg_count], len);
    memcpy(&s_mux.payload[s_mux.payload_len], packet, len);
    s_mux.payload_len += len;
    return ESP_OK;
}

esp_err_t sonar_ogg_mux_end(FILE *fp) {
    if (!s_mux.active) {
        ESP_LOGE(TAG, "end 前没有 begin");
        return ESP_ERR_INVALID_STATE;
    }
    if (fp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    // 缓冲里还有包就刷成 EOS 页;没有包则写一个零段的 EOS 页,
    // granule 同样等于总采样数——规范允许零段页,播放器据此拿到时长。
    esp_err_t err = flush_page(fp, OGG_FLAG_EOS);
    if (err != ESP_OK) {
        s_mux.active = false;
        return err;
    }
    if (fflush(fp) != 0) {
        ESP_LOGE(TAG, "flush Ogg 文件失败");
        s_mux.active = false;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Ogg Opus 收尾: %u 页, %llu 采样", (unsigned)s_mux.page_seq,
             (unsigned long long)s_mux.granule);
    s_mux.active = false;
    return ESP_OK;
}
