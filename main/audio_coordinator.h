// main/audio_coordinator.h
// 音频唯一持有者:录音、播放文件、播放内存素材,全部串行。
//
// 半双工是硬约束(ESP32-C3 无 AEC):说话时不播放,播放时不采集。所有出声都排队
// 经过这里,业务层只提交请求和读结果,不直接碰 I2S/codec。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    AUDIO_STATE_IDLE = 0,
    AUDIO_STATE_RECORDING,
    AUDIO_STATE_PLAYING,
} audio_state_t;

esp_err_t audio_init(void);

// 开始录音到 `path`(16 kHz/16-bit/单声道)。已有播放会先被停掉。
esp_err_t audio_record_start(const char *path);

// 停止录音并回填 WAV 头。`pcm_bytes`/`truncated` 可为 NULL。
esp_err_t audio_record_stop(uint32_t *pcm_bytes, bool *truncated, uint32_t timeout_ms);

bool audio_record_active(void);

// 异步播放磁盘上的 WAV(按 RIFF chunk 解析,不假设 44 字节头)。
esp_err_t audio_play_file(const char *path);

// 异步播放 Flash 里的 WAV 素材(提示音/滴)。
esp_err_t audio_play_mem(const void *data, size_t len);

// 等当前播放结束;`completed` 表示是否播完(被 stop 打断为 false)。
esp_err_t audio_wait_playback_done(uint32_t timeout_ms, bool *completed);

// 上一次播放任务是否真的把缓冲写完了(被打断/写失败都是 false)。
bool audio_last_play_completed(void);

// 立即停:清队列、停 I2S,返回后不再有声音。
void audio_stop(void);

bool audio_idle(void);

audio_state_t audio_state(void);

const char *audio_state_name(void);
