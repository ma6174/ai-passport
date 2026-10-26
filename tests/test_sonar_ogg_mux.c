// tests/test_sonar_ogg_mux.c
// Ogg Opus 封装的主机测试。不需要 libopus:用 40 / 300 / 1000 字节三种假包生成
// .ogg,再用本文件里的最小解析器逐页校验页头、CRC、granule 与 lacing,最后交给
// 系统 ffprobe 确认容器能被识别成 opus 流(假包本身无法解码)。
#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sonar_ogg_mux.h"

#define OUT_PATH "/tmp/test_sonar_ogg_mux.ogg"
#define OUT_PATH_BIG "/tmp/test_sonar_ogg_mux_big.ogg"
#define OUT_PATH_REJECT "/tmp/test_sonar_ogg_mux_reject.bin"
#define FFPROBE "/opt/homebrew/bin/ffprobe"

#define MAX_PAGES 64

#define OGG_FLAG_CONTINUATION 0x01
#define OGG_FLAG_BOS 0x02
#define OGG_FLAG_EOS 0x04
#define OGG_CRC_POLY 0x04C11DB7u

typedef struct {
    uint8_t flags;
    uint32_t serial;
    uint32_t seq;
    uint64_t granule;
    const uint8_t *segs;
    size_t seg_count;
    const uint8_t *payload;
    size_t payload_len;
} page_t;

static void check(bool ok, const char *what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        abort();
    }
}

static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd_le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

// 与模块独立实现的同一套 CRC(多项式 0x04C11DB7,初值 0,不反射,不异或)。
static uint32_t crc32_ogg(const uint8_t *data, size_t len, uint32_t crc) {
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ OGG_CRC_POLY) : (crc << 1);
        }
    }
    return crc;
}

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    check(fp != NULL, "打开输出的 ogg 文件");
    check(fseek(fp, 0, SEEK_END) == 0, "seek 到文件尾");
    long size = ftell(fp);
    check(size > 0, "ogg 文件非空");
    check(fseek(fp, 0, SEEK_SET) == 0, "seek 回文件头");
    uint8_t *buf = malloc((size_t)size);
    check(buf != NULL, "分配文件缓冲");
    check(fread(buf, 1, (size_t)size, fp) == (size_t)size, "读回整个 ogg 文件");
    fclose(fp);
    *len = (size_t)size;
    return buf;
}

// 解析一页并校验页头字段与 CRC;返回该页占用的字节数。
static size_t parse_page(const uint8_t *buf, size_t len, page_t *page) {
    check(len >= 27, "页头至少 27 字节");
    check(memcmp(buf, "OggS", 4) == 0, "页头 magic 是 OggS");
    check(buf[4] == 0, "版本为 0");
    page->flags = buf[5];
    page->granule = rd_le64(buf + 6);
    page->serial = rd_le32(buf + 14);
    page->seq = rd_le32(buf + 18);
    uint32_t crc_want = rd_le32(buf + 22);
    page->seg_count = buf[26];
    check(27 + page->seg_count <= len, "lacing 表在文件内");
    page->segs = buf + 27;
    size_t body = 0;
    for (size_t i = 0; i < page->seg_count; i++) {
        body += page->segs[i];
    }
    check(27 + page->seg_count + body <= len, "payload 在文件内");
    page->payload = buf + 27 + page->seg_count;
    page->payload_len = body;

    uint8_t hdr[27 + 255];
    memcpy(hdr, buf, 27 + page->seg_count);
    memset(hdr + 22, 0, 4);  // 校验时 CRC 字段按 0 参与计算
    uint32_t crc = crc32_ogg(hdr, 27 + page->seg_count, 0);
    crc = crc32_ogg(page->payload, page->payload_len, crc);
    check(crc == crc_want, "页 CRC 与内容匹配");
    return 27 + page->seg_count + body;
}

// 假包内容:确定性图案,用于校验 payload 没有被截断或错位。
static void make_packet(uint8_t *out, size_t len, unsigned salt) {
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(salt * 31u + i * 7u + (i >> 8));
    }
}

// 全量校验一个 ogg 文件:页头/CRC/granule/lacing/包内容/EOS。
// 返回总页数,data_pages 返回音频页数(不含 OpusHead 与 OpusTags)。
static size_t verify_stream(const char *path, const size_t *sizes, size_t packet_count,
                            const char *vendor, size_t *data_pages) {
    size_t file_len = 0;
    uint8_t *buf = read_all(path, &file_len);

    page_t pages[MAX_PAGES];
    size_t page_count = 0;
    size_t pos = 0;
    while (pos < file_len) {
        check(page_count < MAX_PAGES, "页数在测试上限内");
        pos += parse_page(buf + pos, file_len - pos, &pages[page_count]);
        page_count++;
    }
    check(pos == file_len, "各页长度之和等于文件大小");
    check(page_count >= 3, "至少 OpusHead / OpusTags / 音频页三页");

    // 第 1 页:OpusHead,BOS,单独成页。
    const page_t *head = &pages[0];
    check(head->seq == 0, "首页页序号为 0");
    check(head->flags == OGG_FLAG_BOS, "首页只有 BOS 标志");
    check(head->granule == 0, "首页 granule 为 0");
    check(head->seg_count == 1 && head->segs[0] == 19, "OpusHead 用单段 lacing 表示 19 字节");
    check(head->payload_len == 19, "OpusHead 长 19 字节");
    check(memcmp(head->payload, "OpusHead", 8) == 0, "OpusHead magic");
    check(head->payload[8] == 1, "OpusHead 版本 1");
    check(head->payload[9] == 1, "OpusHead 声道数 1");
    check(rd_le16(head->payload + 10) == SONAR_OGG_MUX_PRE_SKIP, "OpusHead pre-skip 312");
    check(rd_le32(head->payload + 12) == 16000, "OpusHead input sample rate 16000");
    check(rd_le16(head->payload + 16) == 0, "OpusHead output gain 0");
    check(head->payload[18] == 0, "OpusHead mapping family 0");

    // 第 2 页:OpusTags,0 条 comment。
    const page_t *tags = &pages[1];
    check(tags->seq == 1, "tags 页序号 1");
    check(tags->flags == 0, "tags 页不带 BOS/EOS");
    check(tags->granule == 0, "tags 页 granule 为 0");
    check(tags->payload_len >= 16, "OpusTags 至少有 magic 与长度字段");
    check(memcmp(tags->payload, "OpusTags", 8) == 0, "OpusTags magic");
    size_t vendor_len = rd_le32(tags->payload + 8);
    check(vendor_len == strlen(vendor), "vendor 长度与入参一致");
    check(memcmp(tags->payload + 12, vendor, vendor_len) == 0, "vendor 内容与入参一致");
    check(rd_le32(tags->payload + 12 + vendor_len) == 0, "OpusTags 有 0 条 comment");
    check(tags->payload_len == 16 + vendor_len, "OpusTags 长度 = magic + 长度 + vendor + 计数");

    // 音频页:包不跨页,lacing 能完整还原每个包。
    uint64_t granule = 0;
    size_t packet_index = 0;
    for (size_t p = 2; p < page_count; p++) {
        const page_t *pg = &pages[p];
        check(pg->seq == p, "页序号逐页 +1");
        check(pg->serial == head->serial, "所有页 serial 一致");
        check((pg->flags & OGG_FLAG_CONTINUATION) == 0, "音频页不置 continuation");
        check((pg->flags & OGG_FLAG_BOS) == 0, "音频页不置 BOS");
        check((pg->flags & OGG_FLAG_EOS) == (p + 1 == page_count ? OGG_FLAG_EOS : 0),
              "只有末页置 EOS");

        size_t packets_in_page = 0;
        size_t cur_start = 0;
        size_t cur_len = 0;
        for (size_t s = 0; s < pg->seg_count; s++) {
            cur_len += pg->segs[s];
            if (pg->segs[s] == 255) {
                continue;  // 续段:包还没结束
            }
            check(packet_index < packet_count, "页里的包数不超过输入包数");
            check(cur_len == sizes[packet_index], "还原出的包长度与输入一致");
            uint8_t expect[8192];
            make_packet(expect, cur_len, (unsigned)packet_index);
            check(memcmp(pg->payload + cur_start, expect, cur_len) == 0, "还原出的包内容与输入一致");
            granule += SONAR_OGG_MUX_FRAME_SAMPLES_48K;
            packet_index++;
            packets_in_page++;
            cur_start += cur_len;
            cur_len = 0;
        }
        check(cur_len == 0, "音频页在包边界结束(没有跨页续包)");
        check(cur_start == pg->payload_len, "lacing 段长之和等于 payload 长度");
        check(pg->granule == granule, "页 granule = 到该页为止的累计采样数");
        if (pg->payload_len > SONAR_OGG_MUX_PAGE_PAYLOAD) {
            check(packets_in_page == 1, "超过页粒度上限的包独占一页");
        }
    }
    check(packet_index == packet_count, "所有输入包都被写入");
    check(pages[page_count - 1].granule ==
              (uint64_t)packet_count * SONAR_OGG_MUX_FRAME_SAMPLES_48K,
          "EOS 页 granule = 总采样数");

    *data_pages = page_count - 2;
    free(buf);
    return page_count;
}

static void verify_with_ffprobe(const char *path) {
    if (access(FFPROBE, X_OK) != 0) {
        printf("跳过 ffprobe 校验: %s 不存在\n", FFPROBE);
        return;
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "%s -v error -show_entries stream=codec_name,sample_rate,channels "
             "-of default=nw=1 \"%s\" 2>&1",
             FFPROBE, path);
    FILE *pipe = popen(cmd, "r");
    check(pipe != NULL, "启动 ffprobe");
    bool opus = false;
    bool rate = false;
    bool channels = false;
    char line[256];
    printf("ffprobe %s:\n", path);
    while (fgets(line, sizeof(line), pipe) != NULL) {
        printf("  %s", line);
        if (strstr(line, "codec_name=opus") != NULL) {
            opus = true;
        }
        if (strstr(line, "sample_rate=48000") != NULL) {
            rate = true;
        }
        if (strstr(line, "channels=1") != NULL) {
            channels = true;
        }
    }
    int rc = pclose(pipe);
    check(rc == 0, "ffprobe 退出码为 0");
    check(opus, "ffprobe 认出 opus 流");
    check(rate, "ffprobe 报采样率 48000");
    check(channels, "ffprobe 报单声道");
}

int main(void) {
    // 未 begin 就调用必须被拒绝,不能写出半个文件。
    FILE *reject = fopen(OUT_PATH_REJECT, "wb");
    check(reject != NULL, "创建拒绝用例文件");
    uint8_t dummy[4] = {0};
    check(sonar_ogg_mux_begin(NULL, 1, SONAR_OGG_MUX_PRE_SKIP, NULL) == ESP_ERR_INVALID_ARG,
          "begin(fp=NULL) 返回 INVALID_ARG");
    check(sonar_ogg_mux_begin(reject, 0, SONAR_OGG_MUX_PRE_SKIP, NULL) == ESP_ERR_INVALID_ARG,
          "begin(channels=0) 返回 INVALID_ARG");
    check(sonar_ogg_mux_write(reject, dummy, sizeof(dummy), SONAR_OGG_MUX_FRAME_SAMPLES_48K) ==
              ESP_ERR_INVALID_STATE,
          "未 begin 的 write 返回 INVALID_STATE");
    check(sonar_ogg_mux_end(reject) == ESP_ERR_INVALID_STATE,
          "未 begin 的 end 返回 INVALID_STATE");
    fclose(reject);
    remove(OUT_PATH_REJECT);

    // 12 个包,尺寸按 40 / 300 / 1000 循环:1000 字节需要 4 段 lacing。
    enum { PACKET_COUNT = 12 };
    static const size_t PATTERN[] = {40, 300, 1000};
    const char *vendor = "sonar-ogg-mux-test";
    static uint8_t packets[PACKET_COUNT][1024];
    size_t sizes[PACKET_COUNT];
    FILE *fp = fopen(OUT_PATH, "wb");
    check(fp != NULL, "创建 opus.ogg");
    check(sonar_ogg_mux_begin(fp, 1, SONAR_OGG_MUX_PRE_SKIP, vendor) == ESP_OK, "begin 成功");
    for (size_t i = 0; i < PACKET_COUNT; i++) {
        sizes[i] = PATTERN[i % (sizeof(PATTERN) / sizeof(PATTERN[0]))];
        make_packet(packets[i], sizes[i], (unsigned)i);
        check(sonar_ogg_mux_write(fp, packets[i], sizes[i], SONAR_OGG_MUX_FRAME_SAMPLES_48K) ==
                  ESP_OK,
              "write 成功");
    }
    check(sonar_ogg_mux_end(fp) == ESP_OK, "end 成功");
    fclose(fp);

    size_t data_pages = 0;
    size_t page_count = verify_stream(OUT_PATH, sizes, PACKET_COUNT, vendor, &data_pages);
    printf("%s: %zu 页(%zu 音频页),%d 个包,%d 采样\n", OUT_PATH, page_count, data_pages,
           PACKET_COUNT, PACKET_COUNT * (int)SONAR_OGG_MUX_FRAME_SAMPLES_48K);
    // 前 10 个包 4060 字节,再加第 11 个会超过 4096,所以刚好分成两页。
    check(page_count == 4, "12 个包压成 2 个音频页(+2 个头页)");
    check(data_pages == 2, "音频页数量为 2");
    verify_with_ffprobe(OUT_PATH);

    // 单个包大于页粒度上限:独占一页,其余包照常分页。
    enum { BIG_COUNT = 3 };
    static const size_t BIG_PATTERN[BIG_COUNT] = {100, 5000, 100};
    static uint8_t big[BIG_COUNT][5000];
    fp = fopen(OUT_PATH_BIG, "wb");
    check(fp != NULL, "创建 big.ogg");
    check(sonar_ogg_mux_begin(fp, 1, SONAR_OGG_MUX_PRE_SKIP, vendor) == ESP_OK, "begin 成功");
    for (size_t i = 0; i < BIG_COUNT; i++) {
        make_packet(big[i], BIG_PATTERN[i], (unsigned)i);
        check(sonar_ogg_mux_write(fp, big[i], BIG_PATTERN[i],
                                  SONAR_OGG_MUX_FRAME_SAMPLES_48K) == ESP_OK,
              "write 超大包成功");
    }
    check(sonar_ogg_mux_end(fp) == ESP_OK, "end 成功");
    fclose(fp);

    page_count = verify_stream(OUT_PATH_BIG, BIG_PATTERN, BIG_COUNT, vendor, &data_pages);
    printf("%s: %zu 页(%zu 音频页),含一个 %zu 字节的单包页\n", OUT_PATH_BIG, page_count,
           data_pages, BIG_PATTERN[1]);
    check(page_count == 5, "三个包各占一页(+2 个头页)");
    check(data_pages == 3, "音频页数量为 3");

    // vendor 传 NULL:写空串,OpusTags 仍合法。
    {
        static const size_t NULL_VENDOR_SIZES[] = {40};
        fp = fopen(OUT_PATH_REJECT, "wb");
        check(fp != NULL, "创建无 vendor 用例文件");
        check(sonar_ogg_mux_begin(fp, 1, SONAR_OGG_MUX_PRE_SKIP, NULL) == ESP_OK,
              "begin(vendor=NULL) 成功");
        uint8_t packet[40];
        make_packet(packet, sizeof(packet), 0);
        check(sonar_ogg_mux_write(fp, packet, sizeof(packet),
                                  SONAR_OGG_MUX_FRAME_SAMPLES_48K) == ESP_OK,
              "write 成功");
        check(sonar_ogg_mux_end(fp) == ESP_OK, "end 成功");
        fclose(fp);
        page_count = verify_stream(OUT_PATH_REJECT, NULL_VENDOR_SIZES, 1, "", &data_pages);
        check(page_count == 3 && data_pages == 1, "无 vendor 的流是 1 个音频页");
        remove(OUT_PATH_REJECT);
    }

    // 单页段数边界:65024 字节刚好用满 255 段 lacing,65025 字节就装不下。
    {
        const size_t too_big = 65025;
        const size_t max_packet = 65024;
        uint8_t *packet = malloc(too_big);
        check(packet != NULL, "分配 65025 字节的包缓冲");
        make_packet(packet, too_big, 7);
        fp = fopen(OUT_PATH_REJECT, "wb");
        check(fp != NULL, "创建段数边界用例文件");
        check(sonar_ogg_mux_begin(fp, 1, SONAR_OGG_MUX_PRE_SKIP, vendor) == ESP_OK, "begin 成功");
        check(sonar_ogg_mux_write(fp, packet, too_big, SONAR_OGG_MUX_FRAME_SAMPLES_48K) ==
                  ESP_ERR_INVALID_ARG,
              "65025 字节的包被拒绝(需要 256 段 lacing)");
        check(sonar_ogg_mux_write(fp, packet, max_packet, SONAR_OGG_MUX_FRAME_SAMPLES_48K) ==
                  ESP_OK,
              "65024 字节的包能独占一页");
        check(sonar_ogg_mux_end(fp) == ESP_OK, "end 成功");
        fclose(fp);
        free(packet);

        size_t len = 0;
        uint8_t *buf = read_all(OUT_PATH_REJECT, &len);
        page_t pages[4];
        size_t pos = 0;
        for (size_t i = 0; i < 4; i++) {
            pos += parse_page(buf + pos, len - pos, &pages[i]);
        }
        check(pos == len, "边界用例共 4 页");
        check(pages[2].seg_count == 255, "最大包用满 255 段 lacing");
        check(pages[2].payload_len == max_packet, "最大包 payload 完整");
        check(pages[2].flags == 0, "最大包所在页不是末页,不带 EOS");
        // 超过页粒度的包写完就已经落盘,收尾时没有待刷的包:
        // 按规范补一个零段的 EOS 页承载最终 granule。
        check(pages[3].seg_count == 0 && pages[3].payload_len == 0, "空 EOS 页用零段表示");
        check(pages[3].flags == OGG_FLAG_EOS, "末页带 EOS");
        // 被拒绝的写不能推进 granule:只有成功的那一个包计入。
        check(pages[3].granule == SONAR_OGG_MUX_FRAME_SAMPLES_48K,
              "被拒绝的写不影响 granule");
        free(buf);
        remove(OUT_PATH_REJECT);
    }

    // EOS 之后不能再写,必须重新 begin。
    uint8_t tail[8] = {0};
    fp = fopen(OUT_PATH_BIG, "ab");
    check(fp != NULL, "重开文件用于收尾校验");
    check(sonar_ogg_mux_write(fp, tail, sizeof(tail), SONAR_OGG_MUX_FRAME_SAMPLES_48K) ==
              ESP_ERR_INVALID_STATE,
          "end 之后的 write 返回 INVALID_STATE");
    fclose(fp);

    printf("test_sonar_ogg_mux ok\n");
    return 0;
}
