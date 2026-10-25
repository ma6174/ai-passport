// tests/test_sonar_time.c
// RFC3339 解析/生成。主机测试,不依赖 ESP-IDF。
#include <assert.h>
#include <string.h>

#include "sonar_time.h"

static void test_parse(void) {
    int64_t epoch = 0;
    assert(sonar_time_parse_rfc3339("2026-10-24T02:00:00Z", &epoch) == 0);
    // 2026-10-24T02:00:00Z = 1792807200
    assert(epoch == 1792807200);
    // 带小数秒
    assert(sonar_time_parse_rfc3339("2026-10-24T02:00:00.123Z", &epoch) == 0);
    assert(epoch == 1792807200);
    // 带时区偏移:02:00+08:00 == 前一天 18:00Z
    assert(sonar_time_parse_rfc3339("2026-10-24T02:00:00+08:00", &epoch) == 0);
    assert(epoch == 1792807200 - 8 * 3600);
    assert(sonar_time_parse_rfc3339("2026-10-24T02:00:00", &epoch) != 0);
    assert(sonar_time_parse_rfc3339("2026-13-24T02:00:00Z", &epoch) != 0);
    assert(sonar_time_parse_rfc3339(NULL, &epoch) != 0);
}

static void test_format_roundtrip(void) {
    char buf[32];
    assert(sonar_time_format_rfc3339(1792807200, buf, sizeof(buf)) > 0);
    assert(strcmp(buf, "2026-10-24T02:00:00Z") == 0);
    int64_t epoch = 0;
    assert(sonar_time_parse_rfc3339(buf, &epoch) == 0);
    assert(epoch == 1792807200);
    // 跨世纪/闰年边界
    assert(sonar_time_format_rfc3339(951782400, buf, sizeof(buf)) > 0);
    assert(strcmp(buf, "2000-02-29T00:00:00Z") == 0);
    assert(sonar_time_format_rfc3339(0, buf, 8) == 0);
}

static void test_remaining_and_plausible(void) {
    assert(sonar_time_remaining(100, 40) == 60);
    assert(sonar_time_remaining(100, 140) == 0);
    assert(sonar_time_remaining(0, 140) == 0);
    assert(!sonar_time_plausible(0));
    assert(!sonar_time_plausible(1704067200));
    assert(sonar_time_plausible(1792807200));
}

int main(void) {
    test_parse();
    test_format_roundtrip();
    test_remaining_and_plausible();
    return 0;
}
