// main/sonar_time.c
// RFC3339 解析/生成与有效期计算。纯 C,由主机测试覆盖。
#include "sonar_time.h"

#include <stdio.h>
#include <string.h>

// Howard Hinnant 的 days_from_civil:1970-01-01 为 0,避免引入平台 timegm。
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);                        // [0, 399]
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1;  // [0, 365]
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;             // [0, 146096]
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, unsigned *m, unsigned *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yr = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yr + (*m <= 2);
}

static bool read_digits(const char *s, int count, int *out) {
    int value = 0;
    for (int i = 0; i < count; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        value = value * 10 + (s[i] - '0');
    }
    *out = value;
    return true;
}

int sonar_time_parse_rfc3339(const char *text, int64_t *epoch_out) {
    if (text == NULL || epoch_out == NULL) {
        return -1;
    }
    size_t len = strlen(text);
    // 至少 "YYYY-MM-DDTHH:MM:SSZ"
    if (len < 20) {
        return -1;
    }
    if (text[4] != '-' || text[7] != '-' || (text[10] != 'T' && text[10] != 't') ||
        text[13] != ':' || text[16] != ':') {
        return -1;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!read_digits(text, 4, &year) || !read_digits(text + 5, 2, &month) ||
        !read_digits(text + 8, 2, &day) || !read_digits(text + 11, 2, &hour) ||
        !read_digits(text + 14, 2, &minute) || !read_digits(text + 17, 2, &second)) {
        return -1;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 60) {
        return -1;
    }
    const char *rest = text + 19;
    // 可选小数秒
    if (*rest == '.') {
        ++rest;
        if (*rest < '0' || *rest > '9') {
            return -1;
        }
        while (*rest >= '0' && *rest <= '9') {
            ++rest;
        }
    }
    int64_t offset = 0;
    if (*rest == 'Z' || *rest == 'z') {
        ++rest;
    } else if (*rest == '+' || *rest == '-') {
        int sign = (*rest == '-') ? -1 : 1;
        ++rest;
        if (strlen(rest) < 5 || rest[2] != ':') {
            return -1;
        }
        int off_hour = 0;
        int off_min = 0;
        if (!read_digits(rest, 2, &off_hour) || !read_digits(rest + 3, 2, &off_min)) {
            return -1;
        }
        if (off_hour > 23 || off_min > 59) {
            return -1;
        }
        offset = sign * (int64_t)(off_hour * 3600 + off_min * 60);
        rest += 5;
    } else {
        return -1;
    }
    if (*rest != '\0') {
        return -1;
    }
    int64_t days = days_from_civil(year, (unsigned)month, (unsigned)day);
    *epoch_out = days * 86400 + hour * 3600 + minute * 60 + second - offset;
    return 0;
}

size_t sonar_time_format_rfc3339(int64_t epoch, char *out, size_t out_len) {
    if (out == NULL || out_len < 21) {
        return 0;
    }
    int64_t days = epoch / 86400;
    int64_t rem = epoch % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days, &year, &month, &day);
    int written = snprintf(out, out_len, "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ",
                           (long long)year, month, day, (long long)(rem / 3600),
                           (long long)((rem % 3600) / 60), (long long)(rem % 60));
    if (written <= 0 || (size_t)written >= out_len) {
        return 0;
    }
    return (size_t)written;
}

int64_t sonar_time_remaining(int64_t expires_at, int64_t now) {
    if (expires_at <= 0) {
        return 0;
    }
    int64_t left = expires_at - now;
    return left > 0 ? left : 0;
}

bool sonar_time_plausible(int64_t epoch) {
    // 2024-01-01 之后才可能是真实时间;1970 附近说明 SNTP 未完成。
    return epoch > 1704067200;
}
