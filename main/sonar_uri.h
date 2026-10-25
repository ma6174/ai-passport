// main/sonar_uri.h
// URL 拼接与百分号编码。路径 ID 是不透明字符串(可能含 `:`、`/`(高德 POI id 形如
// `amap:<hex>`)),不能直接塞进路径;查询参数里的中文也必须编码。
// 纯 C,不依赖 ESP-IDF。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// `SONAR_BASE_URL` 末尾斜杠可有可无;本函数去掉尾部斜杠再拼接 path(必须以 '/' 开头)。
// 成功返回输出长度;缓冲不足或参数非法返回 0。
size_t sonar_url_join(const char *base, const char *path, char *out, size_t out_len);

// 按 RFC 3986 unreserved(A-Z a-z 0-9 - . _ ~)保留,其余字节转 %XX。
// 返回输出长度;缓冲不足返回 0(绝不截断成"看起来合法"的 URL)。
size_t sonar_uri_encode(const char *in, char *out, size_t out_len);

// base + path + "?" + key + "=" + encode(value)。用于 `/v1/places?query=...`。
size_t sonar_url_with_query(const char *base, const char *path, const char *key,
                            const char *value, char *out, size_t out_len);
