// main/sonar_opus.c
#include "sonar_opus.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "opus.h"


static const char *TAG = "sonar_opus";
static OpusEncoder *s_enc;

esp_err_t sonar_opus_open(void) {
    if (s_enc != NULL) {
        return ESP_OK;
    }
    int err = 0;
    s_enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    if (s_enc == NULL || err != OPUS_OK) {
        ESP_LOGE(TAG, "创建编码器失败: %d", err);
        s_enc = NULL;
        return ESP_FAIL;
    }
    opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(SONAR_OPUS_BITRATE));
    opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(2));   // C3 单核,复杂度只求够用
    opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(s_enc, OPUS_SET_INBAND_FEC(1));
    ESP_LOGI(TAG, "编码器就绪 %d bps, 状态 %d 字节, heap=%u", SONAR_OPUS_BITRATE,
             opus_encoder_get_size(1), (unsigned)esp_get_free_heap_size());
    return ESP_OK;
}

void sonar_opus_close(void) {
    if (s_enc != NULL) {
        opus_encoder_destroy(s_enc);
        s_enc = NULL;
    }
}

bool sonar_opus_ready(void) {
    return s_enc != NULL;
}

int sonar_opus_encode(const int16_t *pcm, uint8_t *out, size_t out_len) {
    if (s_enc == NULL || pcm == NULL || out == NULL) {
        return -1;
    }
    int n = opus_encode(s_enc, pcm, SONAR_OPUS_FRAME_SAMPLES, out, (opus_int32)out_len);
    if (n < 0) {
        ESP_LOGW(TAG, "编码失败: %d", n);
    }
    return n;
}
