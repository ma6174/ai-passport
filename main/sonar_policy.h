// main/sonar_policy.h
// 出声/发布前的最后一道本地检查,以及轮询退避。纯 C,由主机测试覆盖。
//
// 这些判断和交接书里的硬要求一一对应:版本不符、请求号变化、过期、静音、
// 音频忙,任何一条不满足就不出声;已尝试但结果未知的投递重启后不自动重播。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SONAR_GATE_OK = 0,
    SONAR_GATE_QUIET,
    SONAR_GATE_NOT_ACTIONABLE,
    SONAR_GATE_STATE,
    SONAR_GATE_EXPIRED,
    SONAR_GATE_VERSION,
    SONAR_GATE_REQUEST,
    SONAR_GATE_BUSY,
} sonar_gate_t;

// 网络/接口重试退避:1s、2s、4s、8s,上限 10s(交接书:1/2/4 秒后报告失败,
// 轮询退避上限 10 秒)。
uint32_t sonar_policy_retry_delay_ms(unsigned attempt);

// 出声前复核。`state` 是服务端返回的 delivery.state(可为 NULL,视为未知)。
sonar_gate_t sonar_policy_delivery_gate(bool actionable, const char *state, int64_t expires_at,
                                        int64_t now, int expected_revision,
                                        int delivery_revision, bool request_match, bool quiet,
                                        bool audio_busy);

const char *sonar_policy_gate_reason(sonar_gate_t gate);

// 已尝试但完成结果未知(回执丢失/断电)时是否自动重播:一律不重播。
bool sonar_policy_should_auto_replay(bool attempted, bool completion_known);

// "停一下"/"安静"中断原声时的回执事件;静音不动 state,因此单独区分。
const char *sonar_policy_ack_for_stop(bool muted);
