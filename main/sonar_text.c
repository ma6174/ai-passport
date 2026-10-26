// main/sonar_text.c
// 见 sonar_text.h。纯逻辑,可主机测试。
#include "sonar_text.h"

#include <stdio.h>
#include <string.h>

// 只替换"足够具体"的别名:至少 3 个汉字(9 字节)。像「社区」「路口」「岛」
// 这类泛词替换会把正常句子改歪,所以跳过。
#define NORMALIZE_MIN_BYTES 9

size_t sonar_text_normalize_place(const sonar_text_place_t *places, size_t count, const char *in,
                                  char *out, size_t out_len) {
    if (places == NULL || count == 0) {
        return 0;
    }
    if (in == NULL || out == NULL || out_len == 0) {
        return 0;
    }
    size_t best_pos = (size_t)-1;
    size_t best_len = 0;
    char best_name[32] = {0};
    for (size_t i = 0; i < count; i++) {
        const char *alias = places[i].alias;
        if (alias == NULL) {
            continue;
        }
        // 第一个别名就是该地点的规范说法(「模力社区」「人工智能岛」「金科路」)。
        const char *sep0 = strchr(alias, '|');
        size_t canon_len = sep0 != NULL ? (size_t)(sep0 - alias) : strlen(alias);
        char canon[32];
        if (canon_len == 0 || canon_len >= sizeof(canon)) {
            continue;
        }
        memcpy(canon, alias, canon_len);
        canon[canon_len] = '\0';
        while (alias != NULL && *alias != '\0') {
            const char *sep = strchr(alias, '|');
            size_t len = sep != NULL ? (size_t)(sep - alias) : strlen(alias);
            if (len >= NORMALIZE_MIN_BYTES && len < sizeof(canon)) {
                char token[32];
                memcpy(token, alias, len);
                token[len] = '\0';
                for (const char *hit = strstr(in, token); hit != NULL; hit = strstr(hit + 1, token)) {
                    size_t pos = (size_t)(hit - in);
                    if (pos < best_pos || (pos == best_pos && len > best_len)) {
                        best_pos = pos;
                        best_len = len;
                        snprintf(best_name, sizeof(best_name), "%s", canon);
                    }
                }
            }
            alias = sep != NULL ? sep + 1 : NULL;
        }
    }
    if (best_pos == (size_t)-1) {
        snprintf(out, out_len, "%s", in);
        return 0;
    }
    // 别造出重复词:命中后面紧跟规范名时(「华夏中路」+「金科路」),只保留一份。
    size_t tail_skip = 0;
    size_t name_len = strlen(best_name);
    if (strncmp(in + best_pos + best_len, best_name, name_len) == 0) {
        tail_skip = name_len;
    }
    snprintf(out, out_len, "%.*s%s%s", (int)best_pos, in, best_name,
             in + best_pos + best_len + tail_skip);
    return 1;
}

