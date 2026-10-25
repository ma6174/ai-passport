// main/sonar_session.h
// Sonar "一次会话多轮"接口:exchange session + 每句话一个 utterance + 会话收件箱。
//
// 契约来源:部署环境 http://122.195.131.106/openapi.json
//   POST /v1/exchange/sessions                      启动会话(ExchangeStart)
//   GET  /v1/exchange/sessions/{sid}                取会话(revision/state/capabilities)
//   POST /v1/exchange/sessions/{sid}/utterances     multipart:metadata + audio
//   GET  /v1/exchange/sessions/{sid}/inbox          会话收件箱(含真人原声)
//   PUT  /v1/exchange/sessions/{sid}/context        上报"我实际听到了哪条原声"
//   POST /v1/tasks/{tid}/actions                    resolve / cancel
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sonar_client.h"
#include "sonar_config.h"

#define SONAR_SESSION_MAX_ITEMS 2

typedef struct {
    char audio_url[SONAR_PATH_MAX];  // 服务端给的相对或绝对地址,不做拼接假设
    char post_id[SONAR_ID_MAX];
    char message_id[SONAR_ID_MAX];
    char kind[24];
    char transcript[SONAR_TEXT_MAX];
    uint32_t duration_s;
} sonar_voice_item_t;

// 一次 utterance 的结构化结果。voice_action 优先于 agent_response.text。
typedef struct {
    char turn_id[SONAR_ID_MAX];
    char status[24];   // ok / clarifying / deferred / failed
    char action[24];   // answer / clarify / defer / publish / chat...
    char reason[48];   // MODEL_TIMEOUT / MODEL_UNCERTAIN ...
    char task_id[SONAR_ID_MAX];
    char conversation_id[SONAR_ID_MAX];
    char chat_id[SONAR_ID_MAX];
    char message_id[SONAR_ID_MAX];
    char agent_text[SONAR_TEXT_MAX];  // agent_response.text
    char agent_kind[32];
    char voice_type[24];  // play_audio 表示 items 里是原声
    size_t item_count;
    sonar_voice_item_t items[SONAR_SESSION_MAX_ITEMS];
} sonar_turn_t;

// 建立/复用会话。place_id 为远程地点 ID;auto_publish 决定服务端是否直接发布提问。
esp_err_t sonar_session_ensure(sonar_client_t *client, const sonar_config_t *cfg,
                               const char *place_id);
// 丢掉本地会话记录(下次 ensure 会重新建立)。
void sonar_session_forget(void);
const char *sonar_session_id(void);
int sonar_session_revision(void);

// 上传一句话(wav + 转写),拿到结构化结果。409 时会刷新 revision 重试一次。
esp_err_t sonar_session_utterance(sonar_client_t *client, const char *wav_path, size_t wav_len,
                                  const char *transcript, sonar_turn_t *out);

// 会话收件箱:与 /v1/inbox 同构(items[].payload.*)。
esp_err_t sonar_session_inbox(sonar_client_t *client, sonar_delivery_t *out, size_t max,
                              size_t *count);

// 原声真的开始播放后,把"听到的这条"写回会话,后续追问才关联得上。
esp_err_t sonar_session_context(sonar_client_t *client, const char *post_id);

// 按服务端返回的 audio_url 下载原声(chat/answer 各自路径都支持)。
esp_err_t sonar_session_download(sonar_client_t *client, const char *audio_url, const char *path,
                                 size_t *out_len);

// 结构化动作:action 只能是 resolve / cancel(服务端 TaskAction 枚举)。
esp_err_t sonar_session_task_action(sonar_client_t *client, const char *task_id,
                                    const char *action);
