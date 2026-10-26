// tests/test_sonar_uri.c
// URL 拼接与百分号编码。主机测试,不依赖 ESP-IDF。
#include <assert.h>
#include <string.h>

#include "sonar_uri.h"

static void test_join(void) {
    char out[128];
    assert(sonar_url_join("https://122.195.131.106", "/v1/me", out, sizeof(out)) > 0);
    assert(strcmp(out, "https://122.195.131.106/v1/me") == 0);
    // 末尾斜杠可有可无,不能拼出双斜杠
    assert(sonar_url_join("https://122.195.131.106/", "/v1/inbox", out, sizeof(out)) > 0);
    assert(strcmp(out, "https://122.195.131.106/v1/inbox") == 0);
    // 缓冲不足必须失败,不能截断成看起来合法的 URL
    assert(sonar_url_join("https://122.195.131.106", "/v1/inbox", out, 10) == 0);
}

static void test_encode(void) {
    char out[64];
    assert(sonar_uri_encode("amap:abc", out, sizeof(out)) > 0);
    assert(strcmp(out, "amap%3Aabc") == 0);
    assert(sonar_uri_encode("模力社区", out, sizeof(out)) > 0);
    assert(strcmp(out, "%E6%A8%A1%E5%8A%9B%E7%A4%BE%E5%8C%BA") == 0);
    assert(sonar_uri_encode("a-b_c.d~e", out, sizeof(out)) > 0);
    assert(strcmp(out, "a-b_c.d~e") == 0);
    assert(sonar_uri_encode("abc", out, 3) == 0);
}

static void test_query(void) {
    char out[128];
    assert(sonar_url_with_query("https://h", "/v1/places", "query", "金科路", out, sizeof(out)) >
           0);
    assert(strcmp(out, "https://h/v1/places?query=%E9%87%91%E7%A7%91%E8%B7%AF") == 0);
}

int main(void) {
    test_join();
    test_encode();
    test_query();
    return 0;
}
