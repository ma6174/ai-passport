// main/sonar_store.h
// 本地存储:SPIFFS 上的单槽录音/下载文件,NVS 上的小型 journal。
//
// 录音与下载都走文件,不在堆里放 256 KB 的整段音频;NVS 只放状态与 ID,
// 不做高频音频 blob(交接书要求)。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define SONAR_REC_PATH "/spiffs/rec.wav"
#define SONAR_OPUS_PATH "/spiffs/rec.opus"  // 录音时同步生成的 Opus 裸包流
#define SONAR_PLAY_PCM_PATH "/spiffs/play16k.wav"  // Ogg 原声解码后的 PCM
#define SONAR_ASSISTANT_PATH "/spiffs/assistant.wav"  // 首次语音调用自带的助手回复
#define SONAR_PLAY_PATH "/spiffs/play.wav"   // 收到/下载的真人原声
#define SONAR_Q_PATH "/spiffs/q.wav"         // 邀请里附带的问题原声

esp_err_t sonar_store_mount(void);

// 删除文件;文件不存在视为成功。
esp_err_t sonar_store_remove(const char *path);

// 文件大小;失败返回 -1。
long sonar_store_file_size(const char *path);

// 剩余空间(字节);失败返回 -1。
long sonar_store_free_bytes(void);

// ---- journal ----
// 记录"已尝试出声但完成结果未知"的投递:重启后不自动重播,也不冒充成功。
bool sonar_journal_attempted(const char *delivery_id);
esp_err_t sonar_journal_mark_attempted(const char *delivery_id);

// 最近一次本人发起的问题,用于重启恢复查询。
void sonar_journal_set_last_question(const char *question_id);
bool sonar_journal_last_question(char *out, size_t out_len);

// 最近一次公开草稿(post_id + client_request_id),断线重试沿用同一 UUID。
void sonar_journal_set_draft(const char *post_id, const char *client_request_id);
bool sonar_journal_draft(char *post_id, size_t post_len, char *client_request_id,
                         size_t crid_len);
