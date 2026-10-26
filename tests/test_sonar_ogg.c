// tests/test_sonar_ogg.c
// Ogg 分页、lacing 拼接、OpusHead 解析与 pre-skip 折算。主机测试,不依赖 ESP-IDF 与 libopus。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sonar_ogg.h"

#define TEST_PKT_CAP 1024

// 收集回调收到的包,便于逐包比对内容与长度。
typedef struct {
    uint8_t buf[4096];
    size_t off[16];
    size_t len[16];
    uint32_t count;
} packets_t;

static int collect(void *ctx, const uint8_t *pkt, size_t len) {
    packets_t *p = (packets_t *)ctx;
    assert(p->count < 16);
    uint32_t i = p->count++;
    p->off[i] = len == 0 ? 0 : (p->count == 1 ? 0 : p->off[i - 1] + p->len[i - 1]);
    p->len[i] = len;
    if (len > 0) {
        memcpy(p->buf + p->off[i], pkt, len);
    }
    return 0;
}

static int abort_immediately(void *ctx, const uint8_t *pkt, size_t len) {
    (void)ctx;
    (void)pkt;
    (void)len;
    return -1;
}

static const uint8_t *packet_at(const packets_t *p, uint32_t i) {
    return p->buf + p->off[i];
}

// 造一页:27 字节固定头 + 段表 + 包体。
static size_t build_page(uint8_t *buf, uint8_t header_type, const uint8_t *lacing, size_t segments,
                        const uint8_t *body, size_t body_len) {
    memset(buf, 0, 27 + segments + body_len);
    memcpy(buf, "OggS", 4);
    buf[4] = 0;
    buf[5] = header_type;
    buf[26] = (uint8_t)segments;
    memcpy(buf + 27, lacing, segments);
    if (body_len > 0) {
        memcpy(buf + 27 + segments, body, body_len);
    }
    return 27 + segments + body_len;
}

static size_t build_opus_head(uint8_t *buf, uint8_t channels, uint16_t pre_skip, uint32_t rate) {
    memset(buf, 0, 19);
    memcpy(buf, "OpusHead", 8);
    buf[8] = 1;
    buf[9] = channels;
    buf[10] = (uint8_t)(pre_skip & 0xff);
    buf[11] = (uint8_t)(pre_skip >> 8);
    buf[12] = (uint8_t)(rate & 0xff);
    buf[13] = (uint8_t)((rate >> 8) & 0xff);
    buf[14] = (uint8_t)((rate >> 16) & 0xff);
    buf[15] = (uint8_t)((rate >> 24) & 0xff);
    buf[18] = 0;
    return 19;
}

static void test_is_ogg(void) {
    assert(sonar_ogg_is_ogg((const uint8_t *)"OggS", 4));
    assert(!sonar_ogg_is_ogg((const uint8_t *)"RIFF", 4));
    assert(!sonar_ogg_is_ogg((const uint8_t *)"Ogg", 3));
    assert(!sonar_ogg_is_ogg(NULL, 8));
}

static void test_single_page_single_packet(void) {
    uint8_t page_buf[128];
    const uint8_t lacing[] = {5};
    const uint8_t body[] = "hello";
    size_t page_len = build_page(page_buf, 0x02, lacing, 1, body, sizeof(body) - 1);

    sonar_ogg_page_t page;
    assert(sonar_ogg_parse_page(page_buf, page_len, &page) == SONAR_OGG_OK);
    assert(page.first && !page.continued && !page.last);
    assert(page.segments == 1);
    assert(page.body_bytes == 5);

    uint8_t pkt[TEST_PKT_CAP];
    sonar_ogg_lacing_t lacing_state;
    sonar_ogg_lacing_init(&lacing_state, pkt, sizeof(pkt));
    assert(sonar_ogg_lacing_begin(&lacing_state, &page) == SONAR_OGG_OK);

    packets_t got = {0};
    assert(sonar_ogg_lacing_feed(&lacing_state, page_buf + 27 + 1, 5, collect, &got) ==
           SONAR_OGG_OK);
    assert(got.count == 1);
    assert(got.len[0] == 5 && memcmp(packet_at(&got, 0), "hello", 5) == 0);
}

// 一页多个包:每个 255 续段、以及 0 长度段(包长正好是 255 的倍数)都要正确断包。
static void test_multi_segment_lacing(void) {
    uint8_t page_buf[1024];
    const uint8_t lacing[] = {3, 4, 0};
    const uint8_t body[] = "abcdefg";
    size_t page_len = build_page(page_buf, 0, lacing, 3, body, sizeof(body) - 1);

    sonar_ogg_page_t page;
    assert(sonar_ogg_parse_page(page_buf, page_len, &page) == SONAR_OGG_OK);
    assert(page.body_bytes == 7);

    uint8_t pkt[TEST_PKT_CAP];
    sonar_ogg_lacing_t state;
    sonar_ogg_lacing_init(&state, pkt, sizeof(pkt));
    assert(sonar_ogg_lacing_begin(&state, &page) == SONAR_OGG_OK);

    packets_t got = {0};
    // 包体分两块喂入,验证段内断点不影响断包。
    assert(sonar_ogg_lacing_feed(&state, page_buf + 27 + 3, 5, collect, &got) == SONAR_OGG_OK);
    assert(sonar_ogg_lacing_feed(&state, page_buf + 27 + 3 + 5, 2, collect, &got) == SONAR_OGG_OK);
    assert(got.count == 3);
    assert(got.len[0] == 3 && memcmp(packet_at(&got, 0), "abc", 3) == 0);
    assert(got.len[1] == 4 && memcmp(packet_at(&got, 1), "defg", 4) == 0);
    assert(got.len[2] == 0);  // 0 长度 lacing 本身就是一个空包

    // 255 + 0:正好 255 字节的包不能当成续包吞掉。
    uint8_t big[255];
    memset(big, 0x5a, sizeof(big));
    const uint8_t lacing2[] = {255, 0};
    size_t page2_len = build_page(page_buf, 0, lacing2, 2, big, sizeof(big));
    assert(sonar_ogg_parse_page(page_buf, page2_len, &page) == SONAR_OGG_OK);
    sonar_ogg_lacing_init(&state, pkt, sizeof(pkt));
    assert(sonar_ogg_lacing_begin(&state, &page) == SONAR_OGG_OK);
    packets_t got2 = {0};
    assert(sonar_ogg_lacing_feed(&state, page_buf + 27 + 2, 255, collect, &got2) == SONAR_OGG_OK);
    assert(got2.count == 1);
    assert(got2.len[0] == 255);
    assert(sonar_ogg_lacing_feed(&state, NULL, 0, collect, &got2) == SONAR_OGG_OK);
    assert(got2.count == 1);
    assert(state.len == 0);
}

// 包跨页:第一页以 255 结束(包未完成),第二页续页补尾。
static void test_packet_across_pages(void) {
    uint8_t page_buf[1024];
    uint8_t tail[255];
    for (size_t i = 0; i < sizeof(tail); i++) {
        tail[i] = (uint8_t)i;
    }
    const uint8_t lacing1[] = {255};
    size_t len1 = build_page(page_buf, 0, lacing1, 1, tail, sizeof(tail));

    uint8_t pkt[TEST_PKT_CAP];
    sonar_ogg_lacing_t state;
    sonar_ogg_lacing_init(&state, pkt, sizeof(pkt));
    sonar_ogg_page_t page;
    assert(sonar_ogg_parse_page(page_buf, len1, &page) == SONAR_OGG_OK);
    assert(sonar_ogg_lacing_begin(&state, &page) == SONAR_OGG_OK);

    packets_t got = {0};
    assert(sonar_ogg_lacing_feed(&state, page_buf + 27 + 1, 255, collect, &got) == SONAR_OGG_OK);
    assert(got.count == 0);   // 包还没结束
    assert(state.len == 255);

    uint8_t page_buf2[64];
    const uint8_t lacing2[] = {2};
    const uint8_t tail2[] = "hi";
    size_t len2 = build_page(page_buf2, 0x01, lacing2, 1, tail2, 2);
    assert(sonar_ogg_parse_page(page_buf2, len2, &page) == SONAR_OGG_OK);
    assert(page.continued);
    assert(sonar_ogg_lacing_begin(&state, &page) == SONAR_OGG_OK);
    assert(sonar_ogg_lacing_feed(&state, page_buf2 + 27 + 1, 2, collect, &got) == SONAR_OGG_OK);
    assert(got.count == 1);
    assert(got.len[0] == 257);
    assert(memcmp(packet_at(&got, 0), tail, 255) == 0);
    assert(memcmp(packet_at(&got, 0) + 255, "hi", 2) == 0);
    assert(state.len == 0);
}

// 续页标志与"未完成包"必须一致,否则拼接会错位。
static void test_page_continuation_mismatch(void) {
    uint8_t page_buf[512];
    uint8_t pkt[TEST_PKT_CAP];
    sonar_ogg_lacing_t state;
    sonar_ogg_lacing_init(&state, pkt, sizeof(pkt));

    const uint8_t lacing[] = {255};
    uint8_t body[255] = {0};
    size_t page_len = build_page(page_buf, 0, lacing, 1, body, sizeof(body));
    sonar_ogg_page_t page;
    assert(sonar_ogg_parse_page(page_buf, page_len, &page) == SONAR_OGG_OK);
    assert(sonar_ogg_lacing_begin(&state, &page) == SONAR_OGG_OK);
    assert(sonar_ogg_lacing_feed(&state, page_buf + 27 + 1, 255, NULL, NULL) == SONAR_OGG_OK);
    assert(state.len == 255);

    // 上一页留下未完成的包,新页却没标续页。
    const uint8_t lacing2[] = {1};
    const uint8_t body2[] = "x";
    size_t len2 = build_page(page_buf, 0, lacing2, 1, body2, 1);
    assert(sonar_ogg_parse_page(page_buf, len2, &page) == SONAR_OGG_OK);
    assert(sonar_ogg_lacing_begin(&state, &page) == SONAR_OGG_ERR_PAGE);

    // 反过来:没有未完成包却标了续页。
    sonar_ogg_lacing_init(&state, pkt, sizeof(pkt));
    size_t len3 = build_page(page_buf, 0x01, lacing2, 1, body2, 1);
    assert(sonar_ogg_parse_page(page_buf, len3, &page) == SONAR_OGG_OK);
    assert(sonar_ogg_lacing_begin(&state, &page) == SONAR_OGG_ERR_PAGE);
}

static void test_reject_bad_page(void) {
    uint8_t page_buf[128];
    const uint8_t lacing[] = {2};
    const uint8_t body[] = "ab";
    size_t page_len = build_page(page_buf, 0, lacing, 1, body, 2);

    sonar_ogg_page_t page;
    assert(sonar_ogg_parse_page(NULL, 0, &page) == SONAR_OGG_ERR_PARAM);
    assert(sonar_ogg_parse_page(page_buf, page_len, NULL) == SONAR_OGG_ERR_PARAM);

    uint8_t bad[128];
    memcpy(bad, page_buf, page_len);
    memcpy(bad, "OggX", 4);
    assert(sonar_ogg_parse_page(bad, page_len, &page) == SONAR_OGG_ERR_MAGIC);

    memcpy(bad, page_buf, page_len);
    bad[4] = 1;  // 版本必须为 0
    assert(sonar_ogg_parse_page(bad, page_len, &page) == SONAR_OGG_ERR_VERSION);

    memcpy(bad, page_buf, page_len);
    // 段表声明 5 段,实际只给了 1 段
    bad[26] = 5;
    assert(sonar_ogg_parse_page(bad, page_len, &page) == SONAR_OGG_ERR_TRUNCATED);
    // 只有 27 字节头,没有段表
    assert(sonar_ogg_parse_page(bad, 27, &page) == SONAR_OGG_ERR_TRUNCATED);
    // 连固定头都不全
    assert(sonar_ogg_parse_page(page_buf, 20, &page) == SONAR_OGG_ERR_TRUNCATED);
}

static void test_packet_overflow_and_extra_data(void) {
    uint8_t page_buf[128];
    const uint8_t lacing[] = {8};
    uint8_t body[8];
    memset(body, 1, sizeof(body));
    size_t page_len = build_page(page_buf, 0, lacing, 1, body, sizeof(body));

    sonar_ogg_page_t page;
    assert(sonar_ogg_parse_page(page_buf, page_len, &page) == SONAR_OGG_OK);

    uint8_t small[4];
    sonar_ogg_lacing_t state;
    sonar_ogg_lacing_init(&state, small, sizeof(small));
    assert(sonar_ogg_lacing_begin(&state, &page) == SONAR_OGG_OK);
    assert(sonar_ogg_lacing_feed(&state, page_buf + 27 + 1, 8, NULL, NULL) ==
           SONAR_OGG_ERR_PACKET);

    // 喂的字节比段表声明的多,说明页头/段表已损坏。
    uint8_t pkt[TEST_PKT_CAP];
    sonar_ogg_lacing_init(&state, pkt, sizeof(pkt));
    assert(sonar_ogg_lacing_begin(&state, &page) == SONAR_OGG_OK);
    assert(sonar_ogg_lacing_feed(&state, page_buf + 27 + 1, 10, NULL, NULL) ==
           SONAR_OGG_ERR_PAGE);
    assert(sonar_ogg_lacing_feed(NULL, page_buf, 1, NULL, NULL) == SONAR_OGG_ERR_PARAM);
}

static void test_callback_abort(void) {
    uint8_t page_buf[64];
    const uint8_t lacing[] = {2};
    const uint8_t body[] = "ab";
    size_t page_len = build_page(page_buf, 0, lacing, 1, body, 2);

    sonar_ogg_page_t page;
    assert(sonar_ogg_parse_page(page_buf, page_len, &page) == SONAR_OGG_OK);
    uint8_t pkt[TEST_PKT_CAP];
    sonar_ogg_lacing_t state;
    sonar_ogg_lacing_init(&state, pkt, sizeof(pkt));
    assert(sonar_ogg_lacing_begin(&state, &page) == SONAR_OGG_OK);
    assert(sonar_ogg_lacing_feed(&state, page_buf + 27 + 1, 2, abort_immediately, NULL) ==
           SONAR_OGG_ERR_CALLBACK);
}

static void test_parse_head(void) {
    uint8_t head_buf[19];
    sonar_ogg_head_t head;
    assert(build_opus_head(head_buf, 1, 312, 16000) == sizeof(head_buf));
    assert(sonar_ogg_parse_head(head_buf, sizeof(head_buf), &head) == SONAR_OGG_OK);
    assert(head.version == 1);
    assert(head.channels == 1);
    assert(head.pre_skip == 312);
    assert(head.input_rate == 16000);
    assert(head.mapping_family == 0);

    // 立体声也照样如实解析,是否接受由调用方决定。
    assert(build_opus_head(head_buf, 2, 3840, 48000) == sizeof(head_buf));
    assert(sonar_ogg_parse_head(head_buf, sizeof(head_buf), &head) == SONAR_OGG_OK);
    assert(head.channels == 2);
    assert(head.pre_skip == 3840);
    assert(head.input_rate == 48000);

    assert(sonar_ogg_parse_head(NULL, 19, &head) == SONAR_OGG_ERR_PARAM);
    assert(sonar_ogg_parse_head(head_buf, 19, NULL) == SONAR_OGG_ERR_PARAM);
    uint8_t bad[19];
    memset(bad, 0, sizeof(bad));
    memcpy(bad, "OpusHeaX", 8);
    assert(sonar_ogg_parse_head(bad, sizeof(bad), &head) == SONAR_OGG_ERR_HEAD);
    // 18 字节(缺映射族)按不合法处理
    assert(sonar_ogg_parse_head(bad, 18, &head) == SONAR_OGG_ERR_HEAD);
}

static void test_pre_skip_conversion(void) {
    // 48 kHz 采样数 ÷ 3 得到 16 kHz 采样数。
    assert(sonar_ogg_pre_skip_frames(0) == 0);
    assert(sonar_ogg_pre_skip_frames(3) == 1);
    assert(sonar_ogg_pre_skip_frames(312) == 104);
    assert(sonar_ogg_pre_skip_frames(313) == 104);   // 不足一个采样向下取整
    assert(sonar_ogg_pre_skip_frames(3840) == 1280); // 80 ms 的典型 pre-skip
    assert(sonar_ogg_pre_skip_frames(65535) == 21845);
}

int main(void) {
    test_is_ogg();
    test_single_page_single_packet();
    test_multi_segment_lacing();
    test_packet_across_pages();
    test_page_continuation_mismatch();
    test_reject_bad_page();
    test_packet_overflow_and_extra_data();
    test_callback_abort();
    test_parse_head();
    test_pre_skip_conversion();
    printf("test_sonar_ogg ok\n");
    return 0;
}
