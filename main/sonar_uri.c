// main/sonar_uri.c
// URL 拼接与百分号编码。纯 C,由主机测试覆盖。
#include "sonar_uri.h"

#include <stdio.h>
#include <string.h>

static bool is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

size_t sonar_uri_encode(const char *in, char *out, size_t out_len) {
    if (in == NULL || out == NULL || out_len == 0) {
        return 0;
    }
    size_t used = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; ++p) {
        size_t need = is_unreserved(*p) ? 1u : 3u;
        if (used + need >= out_len) {  // 预留结尾 '\0'
            return 0;
        }
        if (need == 3u) {
            static const char hex[] = "0123456789ABCDEF";
            out[used++] = '%';
            out[used++] = hex[*p >> 4];
            out[used++] = hex[*p & 0x0f];
        } else {
            out[used++] = (char)*p;
        }
    }
    out[used] = '\0';
    return used;
}

size_t sonar_url_join(const char *base, const char *path, char *out, size_t out_len) {
    if (base == NULL || path == NULL || out == NULL || out_len == 0 || path[0] != '/') {
        return 0;
    }
    size_t base_len = strlen(base);
    while (base_len > 0 && base[base_len - 1] == '/') {
        --base_len;
    }
    if (base_len == 0) {
        return 0;
    }
    size_t path_len = strlen(path);
    if (base_len + path_len >= out_len) {
        return 0;
    }
    memcpy(out, base, base_len);
    memcpy(out + base_len, path, path_len);
    out[base_len + path_len] = '\0';
    return base_len + path_len;
}

size_t sonar_url_with_query(const char *base, const char *path, const char *key,
                            const char *value, char *out, size_t out_len) {
    if (key == NULL || value == NULL || out == NULL || out_len == 0) {
        return 0;
    }
    char joined[512];
    if (sonar_url_join(base, path, joined, sizeof(joined)) == 0) {
        return 0;
    }
    char encoded[512];
    if (sonar_uri_encode(value, encoded, sizeof(encoded)) == 0) {
        return 0;
    }
    int n = snprintf(out, out_len, "%s?%s=%s", joined, key, encoded);
    if (n < 0 || (size_t)n >= out_len) {
        return 0;
    }
    return (size_t)n;
}

// 把不透明 ID 安全地放进路径段(高德 POI id 形如 `amap:1a2b...`,冒号需编码)。
size_t sonar_path_with_id(const char *base, const char *prefix, const char *id, const char *suffix,
                          char *out, size_t out_len) {
    if (prefix == NULL || id == NULL || suffix == NULL) {
        return 0;
    }
    char encoded[256];
    if (sonar_uri_encode(id, encoded, sizeof(encoded)) == 0) {
        return 0;
    }
    char path[512];
    int n = snprintf(path, sizeof(path), "%s%s%s", prefix, encoded, suffix);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return 0;
    }
    return sonar_url_join(base, path, out, out_len);
}
