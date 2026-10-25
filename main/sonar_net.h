// main/sonar_net.h
// Wi-Fi STA 连接与校时。
//
// TLS 需要可信时间(否则证书"尚未生效"),所以顺序是:联网 → SNTP → 再访问 Sonar/Linx。
// 如果 SNTP 一时失败,可在拿到任一 `server_time` 后用它校准偏移,保证 expires_at
// 判断不会被 1970 年带偏。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sonar_config.h"

esp_err_t sonar_net_start(const sonar_config_t *cfg);

bool sonar_net_connected(void);

// 当前 epoch 秒(已叠加服务器时间偏移)。
int64_t sonar_net_now(void);

// 等 SNTP 完成;超时返回 ESP_ERR_TIMEOUT(时间不可信)。
esp_err_t sonar_net_wait_time(uint32_t timeout_ms);

// 用服务端返回的时间校准(只在本地时钟明显不可信或偏差较大时生效,避免来回抖动)。
void sonar_net_apply_server_time(int64_t server_epoch);

// 触发一次重连(按键"立即同步"用)。
void sonar_net_reconnect(void);
