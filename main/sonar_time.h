// main/sonar_time.h
// Sonar 的 `server_time` / `expires_at` 是 UTC RFC3339 字符串(如 `2026-10-24T02:00:00Z`),
// 与 Linx OTA 的毫秒 timestamp 对象不是一回事。设备侧统一换算成 epoch 秒再比较,
// 避免把两种时间混用。纯 C,不依赖 ESP-IDF。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 解析 RFC3339:`YYYY-MM-DDTHH:MM:SS[.fff](Z|±HH:MM)`。
// 成功返回 0 并写入 epoch 秒;格式非法返回负值。
int sonar_time_parse_rfc3339(const char *text, int64_t *epoch_out);

// 生成 UTC RFC3339(`YYYY-MM-DDTHH:MM:SSZ`)。用于上报 `recorded_at` / `sampled_at`。
// 成功返回输出长度;缓冲不足返回 0。
size_t sonar_time_format_rfc3339(int64_t epoch, char *out, size_t out_len);

// 剩余有效期(秒)。已过期返回 0,`expires_at` 无法解析返回 0。
int64_t sonar_time_remaining(int64_t expires_at, int64_t now);

// 设备是否有可信时钟。1970 年附近的值说明 SNTP 还没成功。
bool sonar_time_plausible(int64_t epoch);
