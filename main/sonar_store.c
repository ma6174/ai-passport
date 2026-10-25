// main/sonar_store.c
// SPIFFS 文件与 NVS journal。
#include "sonar_store.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs.h"

static const char *TAG = "sonar_store";
static const char *NS = "sonar_journal";

#define JOURNAL_MAX_ITEMS 12
#define JOURNAL_ID_MAX 32
#define JS_KEY_ATTEMPTED "attempted"
#define JS_KEY_LAST_Q "last_q"
#define JS_KEY_DRAFT "draft"

typedef struct {
    uint8_t count;
    char ids[JOURNAL_MAX_ITEMS][JOURNAL_ID_MAX];
} attempted_blob_t;

esp_err_t sonar_store_mount(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载 SPIFFS 失败: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS %u/%u 字节已用", (unsigned)used, (unsigned)total);
    }
    return ESP_OK;
}

esp_err_t sonar_store_remove(const char *path) {
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (remove(path) == 0) {
        return ESP_OK;
    }
    return ESP_OK;  // 不存在也当作已清理
}

long sonar_store_file_size(const char *path) {
    struct stat st;
    if (path == NULL || stat(path, &st) != 0) {
        return -1;
    }
    return (long)st.st_size;
}

long sonar_store_free_bytes(void) {
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("storage", &total, &used) != ESP_OK) {
        return -1;
    }
    return (long)(total - used);
}

static esp_err_t load_attempted(nvs_handle_t h, attempted_blob_t *blob) {
    size_t len = sizeof(*blob);
    memset(blob, 0, sizeof(*blob));
    esp_err_t err = nvs_get_blob(h, JS_KEY_ATTEMPTED, blob, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        memset(blob, 0, sizeof(*blob));
    }
    if (blob->count > JOURNAL_MAX_ITEMS) {
        blob->count = 0;
    }
    return ESP_OK;
}

bool sonar_journal_attempted(const char *delivery_id) {
    if (delivery_id == NULL || delivery_id[0] == '\0') {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    attempted_blob_t blob;
    load_attempted(h, &blob);
    nvs_close(h);
    for (uint8_t i = 0; i < blob.count; ++i) {
        if (strncmp(blob.ids[i], delivery_id, JOURNAL_ID_MAX) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t sonar_journal_mark_attempted(const char *delivery_id) {
    if (delivery_id == NULL || delivery_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    attempted_blob_t blob;
    load_attempted(h, &blob);
    for (uint8_t i = 0; i < blob.count; ++i) {
        if (strncmp(blob.ids[i], delivery_id, JOURNAL_ID_MAX) == 0) {
            nvs_close(h);
            return ESP_OK;
        }
    }
    if (blob.count >= JOURNAL_MAX_ITEMS) {
        // 环形覆盖最旧一条,避免无限增长。
        memmove(blob.ids[0], blob.ids[1], sizeof(blob.ids[0]) * (JOURNAL_MAX_ITEMS - 1));
        blob.count = JOURNAL_MAX_ITEMS - 1;
    }
    snprintf(blob.ids[blob.count], JOURNAL_ID_MAX, "%s", delivery_id);
    blob.count++;
    err = nvs_set_blob(h, JS_KEY_ATTEMPTED, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

void sonar_journal_set_last_question(const char *question_id) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, JS_KEY_LAST_Q, question_id == NULL ? "" : question_id);
    nvs_commit(h);
    nvs_close(h);
}

bool sonar_journal_last_question(char *out, size_t out_len) {
    if (out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, JS_KEY_LAST_Q, out, &len);
    nvs_close(h);
    return err == ESP_OK && out[0] != '\0';
}

void sonar_journal_set_draft(const char *post_id, const char *client_request_id) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, JS_KEY_DRAFT, post_id == NULL ? "" : post_id);
    nvs_set_str(h, "draft_crid", client_request_id == NULL ? "" : client_request_id);
    nvs_commit(h);
    nvs_close(h);
}

bool sonar_journal_draft(char *post_id, size_t post_len, char *client_request_id,
                         size_t crid_len) {
    if (post_id == NULL || post_len == 0 || client_request_id == NULL || crid_len == 0) {
        return false;
    }
    post_id[0] = '\0';
    client_request_id[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = post_len;
    esp_err_t err = nvs_get_str(h, JS_KEY_DRAFT, post_id, &len);
    size_t clen = crid_len;
    if (err == ESP_OK) {
        nvs_get_str(h, "draft_crid", client_request_id, &clen);
    }
    nvs_close(h);
    return err == ESP_OK && post_id[0] != '\0';
}
