// main/sonar_policy.c
// 本地出声/发布门禁与退避。纯 C,由主机测试覆盖。
#include "sonar_policy.h"

#include <string.h>

uint32_t sonar_policy_retry_delay_ms(unsigned attempt) {
    // attempt 从 1 开始:1s, 2s, 4s, 8s, 之后固定 10s(轮询退避上限)。
    if (attempt == 0) {
        return 1000;
    }
    if (attempt >= 5) {
        return 10000;
    }
    uint32_t delay = 1000u << (attempt - 1);
    return delay > 10000u ? 10000u : delay;
}

sonar_gate_t sonar_policy_delivery_gate(bool actionable, const char *state, int64_t expires_at,
                                        int64_t now, int expected_revision,
                                        int delivery_revision, bool request_match, bool quiet,
                                        bool audio_busy) {
    // 静音优先:本地说安静之后,任何待播内容都不许出声,即使服务端仍说 actionable。
    if (quiet) {
        return SONAR_GATE_QUIET;
    }
    if (!actionable) {
        return SONAR_GATE_NOT_ACTIONABLE;
    }
    if (state != NULL && strcmp(state, "pending") != 0 && strcmp(state, "prompted") != 0) {
        return SONAR_GATE_STATE;
    }
    if (expires_at > 0 && expires_at <= now) {
        return SONAR_GATE_EXPIRED;
    }
    if (expected_revision > 0 && delivery_revision > 0 &&
        expected_revision != delivery_revision) {
        return SONAR_GATE_VERSION;
    }
    if (!request_match) {
        return SONAR_GATE_REQUEST;
    }
    if (audio_busy) {
        return SONAR_GATE_BUSY;
    }
    return SONAR_GATE_OK;
}

const char *sonar_policy_gate_reason(sonar_gate_t gate) {
    switch (gate) {
        case SONAR_GATE_OK:
            return "ok";
        case SONAR_GATE_QUIET:
            return "quiet";
        case SONAR_GATE_NOT_ACTIONABLE:
            return "not_actionable";
        case SONAR_GATE_STATE:
            return "state";
        case SONAR_GATE_EXPIRED:
            return "expired";
        case SONAR_GATE_VERSION:
            return "version";
        case SONAR_GATE_REQUEST:
            return "request";
        case SONAR_GATE_BUSY:
            return "busy";
        default:
            return "unknown";
    }
}

bool sonar_policy_should_auto_replay(bool attempted, bool completion_known) {
    (void)completion_known;
    // 只有从未尝试过的投递才会自动播放。尝试过但完成结果未知(ack 丢失/断电)时
    // 不重播、也不冒充成功;已完成的不再重复。
    return !attempted;
}

const char *sonar_policy_ack_for_stop(bool muted) {
    // 静音中止不写 played/skipped,保留待听;明确"停一下"才回 skipped。
    return muted ? NULL : "skipped";
}
