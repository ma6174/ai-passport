// main/sonar_decision.h
// 本轮语音该变成什么动作。Sonar 服务端没有"模型决策→设备执行"的通道,设备只能用
// 确定性规则把一句话落到具体业务动作上;这里就是那套规则,纯 C,由主机测试覆盖。
//
// 规则(与交接书一致的最小集):
//   1. 本地安静/未授权  → 只保留私有,不发布;
//   2. 有已锁定的有效邀请 → answer(回答优先于发问,避免把人声发到错误对象);
//   3. 句子像问题且命中地点 → question;
//   4. 其他 → 私人助手对话(Linx 回复可播放,但不创建公开内容)。
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SONAR_ACTION_NONE = 0,  // 不发布,也不播放助手回复
    SONAR_ACTION_ANSWER,    // 回答锁定的邀请
    SONAR_ACTION_QUESTION,  // 公开提问(需要真实地点)
    SONAR_ACTION_PRIVATE,   // 私人助手对话
} sonar_action_t;

// 地点提示:label 来自 `/v1/places` 的返回值;alias 是演示用的口语别名(可为 NULL)。
typedef struct {
    const char *label;
    const char *alias;
} sonar_place_hint_t;

typedef struct {
    bool has_invitation;  // 已锁定且未过期的邀请
    bool place_matched;   // 转写命中某个地点
    bool question_like;   // 转写是问句
    bool quiet;           // 本地安静或自动发布关闭
} sonar_decision_input_t;

// 按 UTF-8 码点取前缀长度比较:先整串 label,再逐步缩短到 3 个码点,
// 这样"人工智能岛"能命中"人工智能岛19号楼附近"这类完整名称。
int sonar_match_place(const char *transcript, const sonar_place_hint_t *hints, size_t count);

// 中文问句判定(？/吗/呢/怎么/多少/能不能/可以…吗/堵不堵 等)。
bool sonar_text_is_question(const char *text);

sonar_action_t sonar_decide(const sonar_decision_input_t *input, const char **reason);

const char *sonar_action_name(sonar_action_t action);
