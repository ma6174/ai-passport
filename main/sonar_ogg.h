// main/sonar_ogg.h
// Ogg Opus 解封装:分页/lacing/OpusHead 解析是纯 C,不依赖 ESP-IDF 与 libopus,
// 由主机测试直接编译覆盖;解码成 WAV 的驱动需要 libopus,只在固件构建时提供。
//
// 服务端 `/v1/audio/{pid}` 按存储格式原样返回:历史素材是 RIFF/WAV,新素材是
// Ogg Opus。播放链路只认 16 kHz/16-bit/单声道,这里把 Ogg Opus 解成同规格 WAV。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#endif

// 播放链路固定的目标格式。
#define SONAR_OGG_RATE 16000
// Opus 单包最长 120 ms;16 kHz 下 1920 采样,作为 opus_decode 的输出上限。
#define SONAR_OGG_MAX_FRAME_SAMPLES 1920
// 单个 packet 上限:语音包远小于此值,超过即视为文件异常,不无限吃堆。
#define SONAR_OGG_MAX_PACKET 8192u
// 一页最大约 64 KiB,包体分块读入,避免整页驻留内存。
#define SONAR_OGG_READ_BUF 8192u
// OpusHead 的 pre-skip 以 48 kHz 采样记录,输出 16 kHz 时按 3:1 折算。
#define SONAR_OGG_PRESKIP_RATE 48000
// 连续解码失败的容忍次数,超过即判定文件损坏,不能假装播放成功。
#define SONAR_OGG_MAX_CONSECUTIVE_FAILURES 8

enum {
    SONAR_OGG_OK = 0,
    SONAR_OGG_ERR_PARAM = -1,      // 入参为空或缓冲不足
    SONAR_OGG_ERR_MAGIC = -2,      // 首部不是 OggS 捕获模式
    SONAR_OGG_ERR_VERSION = -3,    // 页版本不是 0
    SONAR_OGG_ERR_TRUNCATED = -4,  // 页头、段表或包体不完整
    SONAR_OGG_ERR_PAGE = -5,       // 分页自相矛盾:续页标志与未完成包不一致
    SONAR_OGG_ERR_PACKET = -6,     // 单个包超过 SONAR_OGG_MAX_PACKET
    SONAR_OGG_ERR_HEAD = -7,       // OpusHead 不合法
    SONAR_OGG_ERR_CALLBACK = -8,   // 包回调主动中止
};

// OpusHead 的关键字段(RFC 7845 5.1)。
typedef struct {
    uint8_t version;
    uint8_t channels;
    uint16_t pre_skip;      // 48 kHz 采样数,解码后必须从开头丢弃
    uint32_t input_rate;    // 录制采样率,仅记录;解码固定输出 16 kHz
    uint8_t mapping_family; // 0 = 单/双声道;其他取值带映射表,本模块不支持
} sonar_ogg_head_t;

// 一页的固定头 + 段表信息;包体不在此结构中。
typedef struct {
    bool continued;         // 页首延续上一页未结束的包
    bool first;             // BOS
    bool last;              // EOS
    uint8_t segments;       // lacing 段数(0..255)
    const uint8_t *table;   // 指向 lacing 段表,生命周期同页缓冲
    size_t body_bytes;      // 本页包体字节数(= 段表各项之和)
    uint32_t granule;       // 页尾 granule 位置,低 32 位(仅用于诊断)
} sonar_ogg_page_t;

// 跨页拼接一个 packet 的状态机;包体可以任意分块喂入。
typedef struct {
    const uint8_t *table;
    uint8_t segments;
    uint8_t seg;            // 下一个待处理的段下标
    uint8_t seg_left;       // 当前段还差多少字节
    bool in_segment;
    uint8_t *pkt;           // 拼接缓冲,由调用方提供并跨页复用
    size_t cap;
    size_t len;             // 当前包的已拼接长度;非 0 表示包未结束
} sonar_ogg_lacing_t;

// 每凑满一个包回调一次;返回负值表示调用方要中止,feed 会转成 SONAR_OGG_ERR_CALLBACK。
typedef int (*sonar_ogg_packet_cb_t)(void *ctx, const uint8_t *pkt, size_t len);

// 首部是否为 Ogg(`OggS`)。
bool sonar_ogg_is_ogg(const uint8_t *head, size_t len);

// 解析页头:buf 至少包含 27 字节固定头 + 段表(segments 字节);包体可不在缓冲内。
int sonar_ogg_parse_page(const uint8_t *buf, size_t len, sonar_ogg_page_t *page);

// 解析 OpusHead 包(固定 19 字节,映射族 0)。不校验声道策略,由调用方决定。
int sonar_ogg_parse_head(const uint8_t *pkt, size_t len, sonar_ogg_head_t *head);

// pre-skip 从 48 kHz 采样折算到 16 kHz 采样数。
uint32_t sonar_ogg_pre_skip_frames(uint16_t pre_skip);

// 初始化一次,跨页复用同一个拼接缓冲。
void sonar_ogg_lacing_init(sonar_ogg_lacing_t *lacing, uint8_t *pkt, size_t cap);

// 开始一页。未结束的包必须落在续页里,否则视为分页损坏。
int sonar_ogg_lacing_begin(sonar_ogg_lacing_t *lacing, const sonar_ogg_page_t *page);

// 喂入本页包体的一段;每凑满一个包回调一次。整页包体喂完后再取下一页。
int sonar_ogg_lacing_feed(sonar_ogg_lacing_t *lacing, const uint8_t *data, size_t len,
                          sonar_ogg_packet_cb_t cb, void *ctx);

#ifdef ESP_PLATFORM
// 把 Ogg Opus 文件解码成 16 kHz/16-bit/单声道 WAV。
// 逐页读取(不整文件入内存),按 OpusHead 的 pre-skip 丢开头采样,失败不留半成品文件。
// 非 Ogg、非单声道、没有可播放采样都返回错误,不会假装成功。
esp_err_t sonar_ogg_decode_to_wav(const char *src_ogg, const char *dst_wav,
                                  uint32_t *out_pcm_bytes);
#endif
