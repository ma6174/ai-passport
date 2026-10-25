// tests/test_sonar_policy.c
// 出声门禁与退避。主机测试,不依赖 ESP-IDF。
#include <assert.h>
#include <string.h>

#include "sonar_policy.h"

static void test_retry_delay(void) {
    assert(sonar_policy_retry_delay_ms(0) == 1000);
    assert(sonar_policy_retry_delay_ms(1) == 1000);
    assert(sonar_policy_retry_delay_ms(2) == 2000);
    assert(sonar_policy_retry_delay_ms(3) == 4000);
    assert(sonar_policy_retry_delay_ms(4) == 8000);
    assert(sonar_policy_retry_delay_ms(5) == 10000);
    assert(sonar_policy_retry_delay_ms(99) == 10000);
}

static void test_gate(void) {
    const int64_t now = 1000;
    const int64_t future = 1100;
    // 正常可播
    assert(sonar_policy_delivery_gate(true, "pending", future, now, 3, 3, true, false, false) ==
           SONAR_GATE_OK);
    // 静音优先
    assert(sonar_policy_delivery_gate(true, "pending", future, now, 3, 3, true, true, false) ==
           SONAR_GATE_QUIET);
    assert(sonar_policy_delivery_gate(false, "pending", future, now, 3, 3, true, false, false) ==
           SONAR_GATE_NOT_ACTIONABLE);
    // 已终结状态不能出声
    assert(sonar_policy_delivery_gate(true, "played", future, now, 3, 3, true, false, false) ==
           SONAR_GATE_STATE);
    assert(sonar_policy_delivery_gate(true, NULL, future, now, 3, 3, true, false, false) ==
           SONAR_GATE_OK);
    // 过期
    assert(sonar_policy_delivery_gate(true, "pending", now, now, 3, 3, true, false, false) ==
           SONAR_GATE_EXPIRED);
    // 版本/请求号变化
    assert(sonar_policy_delivery_gate(true, "pending", future, now, 4, 3, true, false, false) ==
           SONAR_GATE_VERSION);
    assert(sonar_policy_delivery_gate(true, "pending", future, now, 3, 3, false, false, false) ==
           SONAR_GATE_REQUEST);
    // 音频忙
    assert(sonar_policy_delivery_gate(true, "pending", future, now, 3, 3, true, false, true) ==
           SONAR_GATE_BUSY);
    // prompted 仍可播(邀请已提示过,但回答入口保留)
    assert(sonar_policy_delivery_gate(true, "prompted", future, now, 3, 3, true, false, false) ==
           SONAR_GATE_OK);
}

static void test_reason_strings(void) {
    assert(strcmp(sonar_policy_gate_reason(SONAR_GATE_OK), "ok") == 0);
    assert(strcmp(sonar_policy_gate_reason(SONAR_GATE_QUIET), "quiet") == 0);
    assert(strcmp(sonar_policy_gate_reason(SONAR_GATE_VERSION), "version") == 0);
    assert(strcmp(sonar_policy_gate_reason(SONAR_GATE_BUSY), "busy") == 0);
}

static void test_replay_and_ack(void) {
    assert(sonar_policy_should_auto_replay(false, false));
    assert(!sonar_policy_should_auto_replay(true, false));
    assert(!sonar_policy_should_auto_replay(true, true));
    assert(sonar_policy_should_auto_replay(false, true));
    assert(sonar_policy_ack_for_stop(false) != NULL);
    assert(strcmp(sonar_policy_ack_for_stop(false), "skipped") == 0);
    // 静音中止不写 played/skipped,保留待听
    assert(sonar_policy_ack_for_stop(true) == NULL);
}

int main(void) {
    test_retry_delay();
    test_gate();
    test_reason_strings();
    test_replay_and_ack();
    return 0;
}
