// tests/test_sonar_text.c
// 地名归一化:ASR 把「模力」写成同音词时,发出去之前换成规范名。
// 主机测试,不依赖 ESP-IDF。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "sonar_text.h"

static const sonar_text_place_t PLACES[] = {
    {"模力社区附近",
     "模力社区|魔力社区|摩力社区|膜力社区|文丽社区|我去社区|莫力社区|磨力社区|茉莉社区|"
     "木里社区|社区|模力|魔力|茉莉"},
    {"人工智能岛19号楼附近", "人工智能岛|智能岛|19号楼|十九号楼|人工岛|岛|号楼"},
    {"中环线金科路路口", "金科路|华夏中路|中环线|金科|华夏路|路口"},
};

static void expect(const char *in, const char *want, size_t want_n) {
    char out[256];
    size_t n = sonar_text_normalize_place(PLACES, sizeof(PLACES) / sizeof(PLACES[0]), in, out,
                                          sizeof(out));
    if (strcmp(out, want) != 0 || n != want_n) {
        printf("FAIL: [%s] -> [%s] (n=%zu), 期望 [%s] (n=%zu)\n", in, out, n, want, want_n);
    }
    assert(strcmp(out, want) == 0 && n == want_n);
}

int main(void) {
    // 模力的同音写法都归一到「模力社区」
    expect("茉莉社区有地方可以停车吗？", "模力社区有地方可以停车吗？", 1);
    expect("魔力社区有什么地方可以停车吗？", "模力社区有什么地方可以停车吗？", 1);
    expect("文丽社区能停车吗", "模力社区能停车吗", 1);
    expect("上海的魔力社区。", "上海的模力社区。", 1);
    // 其他点位
    expect("智能岛那边堵不堵", "人工智能岛那边堵不堵", 1);
    // 已经正确时内容不变
    expect("金科路可以停车吗", "金科路可以停车吗", 1);
    // 泛词不替换:社区/路口 这类两字词会把正常句子改歪
    expect("前面路口可以停车吗", "前面路口可以停车吗", 0);
    expect("社区活动中心几点开门", "社区活动中心几点开门", 0);
    expect("今天天气怎么样", "今天天气怎么样", 0);
    // 命中后面紧跟规范名时不能出现重复词
    expect("华夏中路金科路路口堵吗", "金科路路口堵吗", 1);
    assert(sonar_text_normalize_place(NULL, 0, "x", NULL, 0) == 0);
    // 缓冲不足时安全截断,不越界(NUL 一定在缓冲内)
    char keep[8];
    assert(sonar_text_normalize_place(PLACES, sizeof(PLACES) / sizeof(PLACES[0]), "今天天气怎么样",
                                      keep, sizeof(keep)) == 0);
    assert(strlen(keep) < sizeof(keep));
    printf("test_sonar_text ok\n");
    return 0;
}
