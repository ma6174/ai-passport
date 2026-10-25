// main/sonar_decision.c
// 语音→动作的确定性规则。纯 C,由主机测试覆盖。
#include "sonar_decision.h"

#include <string.h>

static bool text_contains(const char *text, const char *needle) {
    return text != NULL && needle != NULL && needle[0] != '\0' && strstr(text, needle) != NULL;
}

// 返回从 s 的第 index 个码点开始的字节偏移;index 越界返回 strlen(s)。
static size_t utf8_offset(const char *s, size_t codepoints) {
    size_t i = 0;
    size_t cp = 0;
    while (s[i] != '\0' && cp < codepoints) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0x80u) == 0) {
            i += 1;
        } else if ((c & 0xe0u) == 0xc0u) {
            i += 2;
        } else if ((c & 0xf0u) == 0xe0u) {
            i += 3;
        } else if ((c & 0xf8u) == 0xf0u) {
            i += 4;
        } else {
            i += 1;  // 非法字节按单字节推进,避免死循环
        }
        ++cp;
        if (i > strlen(s)) {
            return strlen(s);
        }
    }
    return i;
}

static size_t utf8_length(const char *s) {
    size_t cp = 0;
    size_t i = 0;
    while (s[i] != '\0') {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0x80u) == 0) {
            i += 1;
        } else if ((c & 0xe0u) == 0xc0u) {
            i += 2;
        } else if ((c & 0xf0u) == 0xe0u) {
            i += 3;
        } else if ((c & 0xf8u) == 0xf0u) {
            i += 4;
        } else {
            i += 1;
        }
        ++cp;
        if (i > strlen(s)) {
            return cp;
        }
    }
    return cp;
}

// 候选词在转写里出现,或"它的前缀(≥3 码点)"出现。后者让口语简称也能命中完整店名。
static bool hint_matches(const char *transcript, const char *candidate) {
    if (!text_contains(transcript, candidate)) {
        size_t total = utf8_length(candidate);
        if (total < 3) {
            return false;
        }
        // 从长到短试前缀,第一次命中即返回,避免把"中环"这类短词误当地点。
        for (size_t keep = total - 1; keep >= 3; --keep) {
            size_t bytes = utf8_offset(candidate, keep);
            if (bytes == 0 || bytes > strlen(candidate)) {
                break;
            }
            char prefix[64];
            if (bytes >= sizeof(prefix)) {
                continue;
            }
            memcpy(prefix, candidate, bytes);
            prefix[bytes] = '\0';
            if (text_contains(transcript, prefix)) {
                return true;
            }
        }
        return false;
    }
    return true;
}

// 别名表用 '|' 分隔:同一个地点在语音转写里会以不同写法出现
// (例如「模力社区」常被识别成「魔力社区」),逐个比对,任一命中即算。
static bool alias_list_matches(const char *transcript, const char *list) {
    if (transcript == NULL || list == NULL || list[0] == '\0') {
        return false;
    }
    const char *p = list;
    while (*p != '\0') {
        const char *bar = strchr(p, '|');
        size_t len = bar != NULL ? (size_t)(bar - p) : strlen(p);
        char token[64];
        if (len > 0 && len < sizeof(token)) {
            memcpy(token, p, len);
            token[len] = '\0';
            if (hint_matches(transcript, token)) {
                return true;
            }
        }
        if (bar == NULL) {
            break;
        }
        p = bar + 1;
    }
    return false;
}

int sonar_match_place(const char *transcript, const sonar_place_hint_t *hints, size_t count) {
    if (transcript == NULL || hints == NULL) {
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        if (hints[i].label != NULL && hint_matches(transcript, hints[i].label)) {
            return (int)i;
        }
    }
    // 别名只用于演示地点,放在第二轮,避免别名抢走更精确的完整名称。
    for (size_t i = 0; i < count; ++i) {
        if (alias_list_matches(transcript, hints[i].alias)) {
            return (int)i;
        }
    }
    return -1;
}

bool sonar_text_is_question(const char *text) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    if (text_contains(text, "?") || text_contains(text, "？")) {
        return true;
    }
    static const char *const markers[] = {
        "吗",   "呢",     "怎么",   "多少",   "能不能", "可不可以", "是否",
        "有没有", "堵不堵", "行不行", "好不好", "哪里",   "几点",     "多久",
    };
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
        if (text_contains(text, markers[i])) {
            return true;
        }
    }
    return false;
}

sonar_action_t sonar_decide(const sonar_decision_input_t *input, const char **reason) {
    if (input == NULL) {
        if (reason != NULL) {
            *reason = "no_input";
        }
        return SONAR_ACTION_NONE;
    }
    if (input->quiet) {
        if (reason != NULL) {
            *reason = "quiet";
        }
        return SONAR_ACTION_NONE;
    }
    if (input->has_invitation) {
        if (reason != NULL) {
            *reason = "invitation";
        }
        return SONAR_ACTION_ANSWER;
    }
    if (input->place_matched && input->question_like) {
        if (reason != NULL) {
            *reason = "question";
        }
        return SONAR_ACTION_QUESTION;
    }
    if (reason != NULL) {
        *reason = input->place_matched ? "not_question" : "no_place";
    }
    return SONAR_ACTION_PRIVATE;
}

const char *sonar_action_name(sonar_action_t action) {
    switch (action) {
        case SONAR_ACTION_ANSWER:
            return "answer";
        case SONAR_ACTION_QUESTION:
            return "question";
        case SONAR_ACTION_PRIVATE:
            return "private";
        case SONAR_ACTION_NONE:
        default:
            return "none";
    }
}
