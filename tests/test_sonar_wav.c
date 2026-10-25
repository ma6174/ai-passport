// tests/test_sonar_wav.c
// Sonar WAV 头生成、前缀解析与规格校验。主机测试,不依赖 ESP-IDF。
#include <assert.h>
#include <string.h>

#include "sonar_wav.h"

static void test_header_roundtrip(void) {
    uint8_t header[SONAR_WAV_HEADER_BYTES];
    sonar_wav_header(header, 32000);
    assert(memcmp(header, "RIFF", 4) == 0);
    assert(memcmp(header + 8, "WAVE", 4) == 0);

    sonar_wav_info_t info;
    assert(sonar_wav_parse(header, sizeof(header), &info) == SONAR_WAV_OK);
    assert(info.format == 1);
    assert(info.channels == 1);
    assert(info.bits == 16);
    assert(info.sample_rate == SONAR_WAV_RATE);
    assert(info.data_offset == 44);
    assert(info.data_bytes == 32000);
    assert(sonar_wav_validate(&info) == SONAR_WAV_OK);
    assert(sonar_wav_frames(&info) == 16000);
}

// data chunk 前面常有 LIST/INFO 等附加 chunk,不能假设头永远是 44 字节。
static void test_parse_with_extra_chunk(void) {
    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "RIFF", 4);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(buf + 16, &fmt_size, 4);
    uint16_t pcm = 1, channels = 1, bits = 16;
    uint32_t rate = SONAR_WAV_RATE;
    memcpy(buf + 20, &pcm, 2);
    memcpy(buf + 22, &channels, 2);
    memcpy(buf + 24, &rate, 4);
    memcpy(buf + 34, &bits, 2);
    // LIST chunk,长度 10(偶数),占 8+10 字节
    memcpy(buf + 36, "LIST", 4);
    uint32_t list_size = 10;
    memcpy(buf + 40, &list_size, 4);
    memset(buf + 44, 'x', 10);
    // data chunk 头从 54 开始
    memcpy(buf + 54, "data", 4);
    uint32_t data_size = 4800;
    memcpy(buf + 58, &data_size, 4);

    sonar_wav_info_t info;
    assert(sonar_wav_parse(buf, 62, &info) == SONAR_WAV_OK);
    assert(info.data_offset == 62);
    assert(info.data_bytes == 4800);
}

static void test_reject_bad_format(void) {
    uint8_t header[SONAR_WAV_HEADER_BYTES];
    sonar_wav_header(header, 32000);
    sonar_wav_info_t info;
    assert(sonar_wav_parse(header, sizeof(header), &info) == SONAR_WAV_OK);
    info.sample_rate = 8000;
    assert(sonar_wav_validate(&info) == SONAR_WAV_ERR_FORMAT);
    info.sample_rate = SONAR_WAV_RATE;
    info.channels = 2;
    assert(sonar_wav_validate(&info) == SONAR_WAV_ERR_FORMAT);
    info.channels = 1;
    info.data_bytes = 0;
    assert(sonar_wav_validate(&info) == SONAR_WAV_ERR_SIZE);
    info.data_bytes = SONAR_WAV_MAX_PCM_BYTES + 2;
    assert(sonar_wav_validate(&info) == SONAR_WAV_ERR_SIZE);
    info.data_bytes = SONAR_WAV_MAX_PCM_BYTES;
    assert(sonar_wav_validate(&info) == SONAR_WAV_OK);
}

static void test_reject_truncated_and_garbage(void) {
    uint8_t header[SONAR_WAV_HEADER_BYTES];
    sonar_wav_header(header, 32000);
    sonar_wav_info_t info;
    assert(sonar_wav_parse(header, 10, &info) == SONAR_WAV_ERR_RIFF);
    assert(sonar_wav_parse(NULL, 0, &info) == SONAR_WAV_ERR_PARAM);
    assert(sonar_wav_parse(header, 20, &info) == SONAR_WAV_ERR_FMT);
    uint8_t no_data[12];
    memcpy(no_data, "RIFF", 4);
    memcpy(no_data + 8, "WAVE", 4);
    assert(sonar_wav_parse(no_data, sizeof(no_data), &info) == SONAR_WAV_ERR_FMT);
}

int main(void) {
    test_header_roundtrip();
    test_parse_with_extra_chunk();
    test_reject_bad_format();
    test_reject_truncated_and_garbage();
    return 0;
}
