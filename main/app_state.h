// main/app_state.h
// 声呐设备业务入口:轮询、按住说话、发布、自动播放、本地停/安静。
#pragma once

#include "esp_err.h"

esp_err_t app_state_start(void);
