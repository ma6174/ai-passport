// main/sonar_text.h
// 文本后处理:把 ASR 听错的地名换成规范名。纯逻辑,不依赖 ESP-IDF(便于主机测试)。
#pragma once

#include <stddef.h>

// 演示地点(与 sonar_config.h 的 sonar_place_preset_t 字段一致的最小子集)。
typedef struct {
    const char *label;  // 展示名
    const char *alias;  // 口语别名,'|' 分隔;第一个是该地点的规范说法
} sonar_text_place_t;

// 最左最长命中,最多替换一处;只处理"足够具体"的别名(>= 3 个汉字),
// 避免把「社区」「路口」这类泛词改歪。返回替换次数。
size_t sonar_text_normalize_place(const sonar_text_place_t *places, size_t count, const char *in,
                                  char *out, size_t out_len);
