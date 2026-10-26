// main/sonar_ogg_mux.h
// 业务上传用的 Ogg Opus 封装:把录音阶段的裸 Opus 包重新装进 Ogg 容器。
//
// 两种封装不能混用:Linx WSS 走裸 Opus 包(WS 有自己的分帧),
// 这里的 Ogg 文件只给 HTTP multipart 业务上传用——服务端只认 PCM WAV(RIFF)
// 与 Ogg Opus(OggS),同内容的 Ogg 比 WAV 小一个数量级。
//
// 只依赖 stdio,不依赖 libopus 与 ESP-IDF 运行时,由主机测试覆盖。
// 一次只能封一个流(状态在模块内),调用顺序固定:
//   begin(fp) -> write(fp, ...) x N -> end(fp) -> fclose(fp)
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

// Opus 解码器要丢弃的前导采样数(48 kHz 采样)。312 是 libopus 16 kHz 输入的
// 典型 lookahead;它不参与 granule 累加,只写进 OpusHead 让播放器裁剪。
#define SONAR_OGG_MUX_PRE_SKIP 312

// 一个 20 ms 帧在 48 kHz 下的采样数:Ogg Opus 的 granule 一律按 48 kHz 计。
#define SONAR_OGG_MUX_FRAME_SAMPLES_48K 960

// 单页 payload 的目标上限。超过就先落盘一页,避免一次 fwrite 太大,
// 也避免整段录音的数据都攒在一页里。单个包大于该值时会独占一页
// (Ogg 页 payload 本身可到 65025 字节,这个值只是我们的写盘粒度)。
#define SONAR_OGG_MUX_PAGE_PAYLOAD 4096

// 写 OpusHead(BOS)与 OpusTags 两页,并重置封装状态。
// channels 是 Opus 声道数(本项目固定 1);pre_skip_48k 传 SONAR_OGG_MUX_PRE_SKIP;
// vendor 是 OpusTags 里的编码器标识,可为 NULL(写空串)。
esp_err_t sonar_ogg_mux_begin(FILE *fp, int channels, uint16_t pre_skip_48k, const char *vendor);

// 追加一个裸 Opus 包(不含任何长度前缀)。
// packet_samples_48k 是该包解码后的 48 kHz 采样数:20 ms 帧传
// SONAR_OGG_MUX_FRAME_SAMPLES_48K(960)。
esp_err_t sonar_ogg_mux_write(FILE *fp, const uint8_t *packet, size_t len,
                             uint32_t packet_samples_48k);

// 收尾:把缓冲里的包刷成最后一页并置 EOS(该页 granule = 总采样数),
// 然后 fflush(fp)。调用后若要再封一个流,重新从 begin 开始。
esp_err_t sonar_ogg_mux_end(FILE *fp);
