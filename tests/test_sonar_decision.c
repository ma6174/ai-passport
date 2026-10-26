// tests/test_sonar_decision.c
// 语音→动作规则。主机测试,不依赖 ESP-IDF。
#include <assert.h>
#include <string.h>

#include "sonar_decision.h"

static void test_question_detection(void) {
    assert(sonar_text_is_question("模力社区可以停车吗？"));
    assert(sonar_text_is_question("现在堵不堵"));
    assert(sonar_text_is_question("那边怎么走"));
    assert(sonar_text_is_question("人多吗"));
    assert(!sonar_text_is_question("金科路中环路下面堵死了。"));
    assert(!sonar_text_is_question("我再说一句"));
    assert(!sonar_text_is_question(""));
    assert(!sonar_text_is_question(NULL));
}

static void test_place_match(void) {
    const sonar_place_hint_t hints[] = {
        {"模力社区附近", "模力社区|魔力社区|摩力社区"},
        {"人工智能岛19号楼附近", "人工智能岛"},
        {"中环线金科路路口", "金科路"},
    };
    assert(sonar_match_place("模力社区可以停车吗？", hints, 3) == 0);
    // 别名表用 '|' 分隔,同音写法(ASR 常把「模力」写成「魔力」)也要命中
    assert(sonar_match_place("魔力社区可以停车吗？", hints, 3) == 0);
    assert(sonar_match_place("摩力社区那边堵吗", hints, 3) == 0);
    // 口语简称命中完整名称
    assert(sonar_match_place("人工智能岛那边能停车吗", hints, 3) == 1);
    assert(sonar_match_place("中环线金科路路口堵不堵", hints, 3) == 2);
    // 只说"金科路"命中别名
    assert(sonar_match_place("金科路现在堵吗", hints, 3) == 2);
    assert(sonar_match_place("今天天气不错", hints, 3) == -1);
    assert(sonar_match_place(NULL, hints, 3) == -1);
    assert(sonar_match_place("模力社区", NULL, 0) == -1);
}

static void test_decide(void) {
    const char *reason = NULL;
    sonar_decision_input_t in = {0};
    assert(sonar_decide(&in, &reason) == SONAR_ACTION_PRIVATE);
    assert(strcmp(reason, "no_place") == 0);

    in.place_matched = true;
    in.question_like = true;
    assert(sonar_decide(&in, &reason) == SONAR_ACTION_QUESTION);
    assert(strcmp(reason, "question") == 0);

    // 邀请优先于发问:避免把人声发到错误对象
    in.has_invitation = true;
    assert(sonar_decide(&in, &reason) == SONAR_ACTION_ANSWER);
    assert(strcmp(reason, "invitation") == 0);

    // 安静优先于一切
    in.quiet = true;
    assert(sonar_decide(&in, &reason) == SONAR_ACTION_NONE);
    assert(strcmp(reason, "quiet") == 0);

    in.quiet = false;
    in.has_invitation = false;
    in.question_like = false;
    assert(sonar_decide(&in, &reason) == SONAR_ACTION_PRIVATE);
    assert(strcmp(reason, "not_question") == 0);
    assert(sonar_decide(NULL, &reason) == SONAR_ACTION_NONE);
}

static void test_action_names(void) {
    assert(strcmp(sonar_action_name(SONAR_ACTION_ANSWER), "answer") == 0);
    assert(strcmp(sonar_action_name(SONAR_ACTION_QUESTION), "question") == 0);
    assert(strcmp(sonar_action_name(SONAR_ACTION_PRIVATE), "private") == 0);
    assert(strcmp(sonar_action_name(SONAR_ACTION_NONE), "none") == 0);
}

int main(void) {
    test_question_detection();
    test_place_match();
    test_decide();
    test_action_names();
    return 0;
}
