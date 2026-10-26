// main/sonar_ogg.c
// Ogg 分页/lacing/OpusHead 解析是纯 C,主机测试直接编译本文件;
// 解码驱动需要 libopus,只在固件构建(ESP_PLATFORM)时编译。
#include "sonar_ogg.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "opus.h"

#include "sonar_wav.h"

static const char *TAG = "sonar_ogg";
#endif

static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

bool sonar_ogg_is_ogg(const uint8_t *head, size_t len) {
    return head != NULL && len >= 4 && memcmp(head, "OggS", 4) == 0;
}

int sonar_ogg_parse_page(const uint8_t *buf, size_t len, sonar_ogg_page_t *page) {
    if (buf == NULL || page == NULL) {
        return SONAR_OGG_ERR_PARAM;
    }
    if (!sonar_ogg_is_ogg(buf, len)) {
        return SONAR_OGG_ERR_MAGIC;
    }
    if (len < 27) {
        return SONAR_OGG_ERR_TRUNCATED;
    }
    if (buf[4] != 0) {
        return SONAR_OGG_ERR_VERSION;
    }
    uint8_t segments = buf[26];
    if (len < (size_t)27 + segments) {
        return SONAR_OGG_ERR_TRUNCATED;
    }
    memset(page, 0, sizeof(*page));
    page->continued = (buf[5] & 0x01u) != 0;
    page->first = (buf[5] & 0x02u) != 0;
    page->last = (buf[5] & 0x04u) != 0;
    page->segments = segments;
    page->table = buf + 27;
    page->granule = rd_le32(buf + 6);
    // 包体长度由段表决定,不信任文件里的其他长度字段。
    size_t body = 0;
    for (uint8_t i = 0; i < segments; i++) {
        body += page->table[i];
    }
    page->body_bytes = body;
    return SONAR_OGG_OK;
}

int sonar_ogg_parse_head(const uint8_t *pkt, size_t len, sonar_ogg_head_t *head) {
    if (pkt == NULL || head == NULL) {
        return SONAR_OGG_ERR_PARAM;
    }
    // 固定 19 字节:magic(8) version(1) channels(1) pre-skip(2) rate(4)
    // gain(2) mapping family(1)。更长的头带声道映射表,本模块不支持。
    if (len < 19 || memcmp(pkt, "OpusHead", 8) != 0) {
        return SONAR_OGG_ERR_HEAD;
    }
    memset(head, 0, sizeof(*head));
    head->version = pkt[8];
    head->channels = pkt[9];
    head->pre_skip = rd_le16(pkt + 10);
    head->input_rate = rd_le32(pkt + 12);
    head->mapping_family = pkt[18];
    return SONAR_OGG_OK;
}

uint32_t sonar_ogg_pre_skip_frames(uint16_t pre_skip) {
    return (uint32_t)pre_skip / (SONAR_OGG_PRESKIP_RATE / SONAR_OGG_RATE);
}

void sonar_ogg_lacing_init(sonar_ogg_lacing_t *lacing, uint8_t *pkt, size_t cap) {
    if (lacing == NULL) {
        return;
    }
    memset(lacing, 0, sizeof(*lacing));
    lacing->pkt = pkt;
    lacing->cap = cap;
}

int sonar_ogg_lacing_begin(sonar_ogg_lacing_t *lacing, const sonar_ogg_page_t *page) {
    if (lacing == NULL || page == NULL || lacing->pkt == NULL) {
        return SONAR_OGG_ERR_PARAM;
    }
    // 续页标志必须和"上一页留下未结束的包"一致,否则拼接结果会错位。
    if (page->continued != (lacing->len > 0)) {
        return SONAR_OGG_ERR_PAGE;
    }
    lacing->table = page->table;
    lacing->segments = page->segments;
    lacing->seg = 0;
    lacing->seg_left = 0;
    lacing->in_segment = false;
    return SONAR_OGG_OK;
}

int sonar_ogg_lacing_feed(sonar_ogg_lacing_t *lacing, const uint8_t *data, size_t len,
                          sonar_ogg_packet_cb_t cb, void *ctx) {
    if (lacing == NULL || (data == NULL && len > 0)) {
        return SONAR_OGG_ERR_PARAM;
    }
    size_t pos = 0;
    for (;;) {
        if (!lacing->in_segment) {
            if (lacing->seg >= lacing->segments) {
                // 段表处理完:本页包体不能比段表声明的更长。
                return pos < len ? SONAR_OGG_ERR_PAGE : SONAR_OGG_OK;
            }
            lacing->seg_left = lacing->table[lacing->seg];
            lacing->in_segment = true;
        }
        size_t take = len - pos;
        if (take > lacing->seg_left) {
            take = lacing->seg_left;
        }
        if (lacing->len + take > lacing->cap) {
            return SONAR_OGG_ERR_PACKET;
        }
        if (take > 0) {
            memcpy(lacing->pkt + lacing->len, data + pos, take);
            lacing->len += take;
            pos += take;
            lacing->seg_left = (uint8_t)(lacing->seg_left - take);
        }
        if (lacing->seg_left > 0) {
            return SONAR_OGG_OK;  // 段还没读满,等下一块数据
        }
        // 段读完:255 表示包继续到下一段,其余值结束当前包。
        bool end_of_packet = lacing->table[lacing->seg] < 255;
        lacing->seg++;
        lacing->in_segment = false;
        if (end_of_packet) {
            int rc = cb != NULL ? cb(ctx, lacing->pkt, lacing->len) : 0;
            lacing->len = 0;
            if (rc < 0) {
                return SONAR_OGG_ERR_CALLBACK;
            }
        }
    }
}

#ifdef ESP_PLATFORM

// 一次解码任务的全部工作缓冲。C3 没有 PSRAM,可用堆只有几十 KB:
// 页头 282 + 读缓冲 8192 + 单包 8192 + 单帧 PCM 3840,一次性分配、一次释放。
typedef struct {
    uint8_t page[27 + 255];
    uint8_t read[SONAR_OGG_READ_BUF];
    uint8_t pkt[SONAR_OGG_MAX_PACKET];
    int16_t pcm[SONAR_OGG_MAX_FRAME_SAMPLES];
} ogg_work_t;

typedef struct {
    ogg_work_t *work;
    sonar_ogg_lacing_t lacing;
    OpusDecoder *dec;
    FILE *wav;
    uint32_t skip_left;      // 还没丢掉的 pre-skip 采样数(16 kHz)
    uint32_t dropped;        // 已丢弃的 pre-skip 采样数
    uint32_t pcm_bytes;      // 已写入 WAV 的 PCM 字节数
    uint32_t samples_out;    // 已写入 WAV 的采样数
    uint32_t packets;        // 成功解码的音频包
    uint32_t failed;         // 解码失败的音频包
    uint32_t consecutive_failures;
    bool head_ok;
    bool tags_ok;
    bool stop;               // 达到时长上限,正常停止
    esp_err_t err;           // 非 ESP_OK 表示要失败退出
} ogg_decode_t;

static int decode_packet(void *ctx, const uint8_t *pkt, size_t len) {
    ogg_decode_t *d = (ogg_decode_t *)ctx;
    ogg_work_t *w = d->work;

    if (!d->head_ok) {
        sonar_ogg_head_t head;
        if (sonar_ogg_parse_head(pkt, len, &head) != SONAR_OGG_OK) {
            ESP_LOGW(TAG, "首包不是 OpusHead(len=%u)", (unsigned)len);
            d->err = ESP_ERR_INVALID_RESPONSE;
            return -1;
        }
        ESP_LOGI(TAG, "OpusHead v%u 声道=%u 输入=%lu Hz pre-skip=%u", (unsigned)head.version,
                 (unsigned)head.channels, (unsigned long)head.input_rate,
                 (unsigned)head.pre_skip);
        // 播放链路只接单声道;带映射表的头也无法按单声道解释。
        if (head.channels != 1 || head.mapping_family != 0) {
            ESP_LOGW(TAG, "只支持单声道 Ogg Opus,收到 %u 声道(mapping family %u)",
                     (unsigned)head.channels, (unsigned)head.mapping_family);
            d->err = ESP_ERR_NOT_SUPPORTED;
            return -1;
        }
        int err = 0;
        d->dec = opus_decoder_create(SONAR_OGG_RATE, 1, &err);
        if (d->dec == NULL || err != OPUS_OK) {
            ESP_LOGE(TAG, "创建解码器失败: %d", err);
            d->dec = NULL;
            d->err = ESP_FAIL;
            return -1;
        }
        d->skip_left = sonar_ogg_pre_skip_frames(head.pre_skip);
        d->head_ok = true;
        return 0;
    }

    // OpusTags 只承载元数据,跳过;个别编码器不写,不能因此丢掉音频包。
    if (!d->tags_ok && len >= 8 && memcmp(pkt, "OpusTags", 8) == 0) {
        d->tags_ok = true;
        return 0;
    }
    if (len == 0) {
        return 0;
    }

    // 解码输出直接是 16 kHz:libopus 内部按 48 kHz 解码,允许降采样输出。
    int frames = opus_decode(d->dec, pkt, (opus_int32)len, w->pcm,
                            SONAR_OGG_MAX_FRAME_SAMPLES, 0);
    if (frames < 0) {
        d->failed++;
        d->consecutive_failures++;
        ESP_LOGW(TAG, "解码失败 %d(第 %u 个音频包)", frames,
                 (unsigned)(d->packets + d->failed));
        if (d->consecutive_failures >= SONAR_OGG_MAX_CONSECUTIVE_FAILURES) {
            ESP_LOGE(TAG, "连续 %u 个包解码失败,判定文件损坏",
                     (unsigned)d->consecutive_failures);
            d->err = ESP_ERR_INVALID_RESPONSE;
            return -1;
        }
        return 0;
    }
    d->consecutive_failures = 0;
    d->packets++;

    int16_t *pcm = w->pcm;
    uint32_t n = (uint32_t)frames;
    if (d->skip_left > 0) {
        // 首帧不足时继续丢后续帧,直到 pre-skip 全部丢完。
        uint32_t drop = d->skip_left < n ? d->skip_left : n;
        d->skip_left -= drop;
        d->dropped += drop;
        pcm += drop;
        n -= drop;
    }
    if (n == 0) {
        return 0;
    }
    if (d->pcm_bytes + n * sizeof(int16_t) > SONAR_WAV_MAX_PCM_BYTES) {
        // 播放链路和 SPIFFS 都按 8 秒设计;超长音频截断到上限并留日志。
        ESP_LOGW(TAG, "解码超过 %u 字节上限,截断", (unsigned)SONAR_WAV_MAX_PCM_BYTES);
        d->stop = true;
        return -1;
    }
    size_t bytes = (size_t)n * sizeof(int16_t);
    if (fwrite(pcm, 1, bytes, d->wav) != bytes) {
        ESP_LOGE(TAG, "写 WAV 失败");
        d->err = ESP_FAIL;
        return -1;
    }
    d->pcm_bytes += (uint32_t)bytes;
    d->samples_out += n;
    return 0;
}

esp_err_t sonar_ogg_decode_to_wav(const char *src_ogg, const char *dst_wav,
                                  uint32_t *out_pcm_bytes) {
    if (src_ogg == NULL || dst_wav == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ogg_work_t *work = malloc(sizeof(*work));
    if (work == NULL) {
        ESP_LOGE(TAG, "内存不足: 需要 %u 字节", (unsigned)sizeof(*work));
        return ESP_ERR_NO_MEM;
    }
    ogg_decode_t d;
    memset(&d, 0, sizeof(d));
    d.work = work;
    d.err = ESP_OK;
    sonar_ogg_lacing_init(&d.lacing, work->pkt, sizeof(work->pkt));

    FILE *src = fopen(src_ogg, "rb");
    if (src == NULL) {
        ESP_LOGW(TAG, "打不开 Ogg 文件: %s", src_ogg);
        free(work);
        return ESP_ERR_NOT_FOUND;
    }
    d.wav = fopen(dst_wav, "wb");
    if (d.wav == NULL) {
        ESP_LOGE(TAG, "创建 WAV 文件失败: %s", dst_wav);
        fclose(src);
        free(work);
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    uint8_t wav_header[SONAR_WAV_HEADER_BYTES];
    sonar_wav_header(wav_header, 0);
    if (fwrite(wav_header, 1, sizeof(wav_header), d.wav) != sizeof(wav_header)) {
        ret = ESP_FAIL;
        goto out;
    }

    for (;;) {
        size_t got = fread(work->page, 1, 27, src);
        if (got == 0 && feof(src)) {
            break;
        }
        if (got < 27) {
            ESP_LOGW(TAG, "页头不完整: %u 字节", (unsigned)got);
            ret = ESP_ERR_INVALID_RESPONSE;
            goto out;
        }
        if (!sonar_ogg_is_ogg(work->page, got)) {
            ESP_LOGW(TAG, "不是 OggS 页,拒绝解码");
            ret = ESP_ERR_NOT_SUPPORTED;
            goto out;
        }
        uint8_t segments = work->page[26];
        if (segments > 0 && fread(work->page + 27, 1, segments, src) != segments) {
            ESP_LOGW(TAG, "段表不完整");
            ret = ESP_ERR_INVALID_RESPONSE;
            goto out;
        }
        sonar_ogg_page_t page;
        int rc = sonar_ogg_parse_page(work->page, (size_t)27 + segments, &page);
        if (rc == SONAR_OGG_OK) {
            rc = sonar_ogg_lacing_begin(&d.lacing, &page);
        }
        // 包体分块读入:单页最大约 64 KiB,不整页驻留内存。
        size_t left = rc == SONAR_OGG_OK ? page.body_bytes : 0;
        while (rc == SONAR_OGG_OK && left > 0) {
            size_t chunk = left < sizeof(work->read) ? left : sizeof(work->read);
            if (fread(work->read, 1, chunk, src) != chunk) {
                ESP_LOGW(TAG, "包体不完整");
                rc = SONAR_OGG_ERR_TRUNCATED;
                break;
            }
            left -= chunk;
            rc = sonar_ogg_lacing_feed(&d.lacing, work->read, chunk, decode_packet, &d);
        }
        if (rc != SONAR_OGG_OK) {
            if (d.err == ESP_OK && !d.stop) {
                ESP_LOGW(TAG, "分页解析失败: %d", rc);
                d.err = ESP_ERR_INVALID_RESPONSE;
            }
            break;
        }
        if (d.stop) {
            break;
        }
    }

    if (d.err != ESP_OK) {
        ret = d.err;
        goto out;
    }
    if (!d.head_ok) {
        ESP_LOGW(TAG, "没有 OpusHead,不是 Ogg Opus");
        ret = ESP_ERR_INVALID_RESPONSE;
        goto out;
    }
    if (d.pcm_bytes == 0) {
        ESP_LOGW(TAG, "没有可播放的采样");
        ret = ESP_ERR_INVALID_RESPONSE;
        goto out;
    }
    // 回填真实长度,否则播放链路按 0 字节处理。
    sonar_wav_header(wav_header, d.pcm_bytes);
    if (fseek(d.wav, 0, SEEK_SET) != 0 ||
        fwrite(wav_header, 1, sizeof(wav_header), d.wav) != sizeof(wav_header) ||
        fflush(d.wav) != 0) {
        ESP_LOGE(TAG, "回填 WAV 头失败");
        ret = ESP_FAIL;
        goto out;
    }
    ESP_LOGI(TAG, "解码完成: %u 包(失败 %u)、%u 采样 / %u 字节,丢弃 pre-skip %u 采样",
             (unsigned)d.packets, (unsigned)d.failed, (unsigned)d.samples_out,
             (unsigned)d.pcm_bytes, (unsigned)d.dropped);
    if (out_pcm_bytes != NULL) {
        *out_pcm_bytes = d.pcm_bytes;
    }

out:
    if (d.dec != NULL) {
        opus_decoder_destroy(d.dec);
    }
    fclose(d.wav);
    fclose(src);
    free(work);
    if (ret != ESP_OK) {
        remove(dst_wav);  // 失败不留半成品,避免播放链路读到坏 WAV
    }
    return ret;
}

#endif
