<p align="right">
  <a href="sonar-client.md">English</a> · <strong>简体中文</strong>
</p>

# 声呐客户端固件(ESP32-C3,按住说话)

本应用把 FoloToy AI Passport 变成声呐的语音终端:灵矽负责设备绑定、语音识别和
私人助手,Sonar 保存真人原声并管理问题、邀请与交付。设备用自己的扬声器和麦克风,
最长录 8 秒,收到的真人原声直接播放。

## 为什么是按住说话

ESP32-C3 没有 AEC(ESP-SR 的 AFE 不支持 C3),麦克风会听到自己的扬声器。所以本
固件是半双工:播放时不采集,播放中按 OK 是"停声"而不是"开始录音"。唤醒词和语音
打断不在本轮范围;停止是本地动作,断网也生效。

## 交互

| 操作 | 动作 |
| --- | --- |
| 按住 OK | 录音(最长 8 秒,由音频工作函数硬停) |
| 松开 OK | 结束录音 → 灵矽转写 → 按规则决定动作 |
| 单击 OK | 停播放,或确认发布待发草稿 |
| 单击 UP | 切换演示地点(模力社区 / 人工智能岛 19 号楼 / 中环线金科路)并刷新模拟位置 |
| 单击 DOWN | 切换安静(本地先静音,再同步 `PUT /v1/me/context`) |
| 长按 UP | 重新同步账号、地点与位置 |
| 长按 DOWN | 取消本人最后一个提问 |

屏幕显示阶段、地点、网络/服务/灵矽状态、待处理条数与电量。界面文字全部为 ASCII:
默认 LVGL 字体没有中文字形,本固件也未内置 CJK 子集。

## 决策规则

服务端没有"模型决策 → 设备执行"的通道,所以规则是确定性的(`sonar_decision.c`):

1. 安静或无发布意图 → 不发布;
2. 有已锁定的有效邀请 → 这句话是 `answer`;
3. 转写命中演示地点且是问句 → `question`;
4. 其他 → 私人助手(播放灵矽 TTS,不创建公开内容)。

提问 metadata 使用 `direction: "unknown"`(真实 POI 的要求);回答始终带上锁定邀请
的 `reply_to` 与 `invitation_id`。

## 服务端契约

以 `server/app/*.py` 与部署环境的 `/openapi.json` 为准,交接书只作设计意图。对固件
有影响的差异:

- 设备 Token 是按用户的 `SONAR_ALICE_TOKEN` / `SONAR_BOB_TOKEN`,没有统一的
  `SONAR_DEVICE_TOKEN`;
- `POST /v1/posts/{id}/confirm` 是唯一发布路径且请求体为空;回答还要求邀请已由
  Bamboo 批准且问题仍开放;
- 幂等指纹包含音频字节,重试必须复用同一个文件;
- `PUT /v1/me/location` 用 `android_mock` 时服务端需要 `SONAR_ALLOW_MOCK_LOCATION=true`;
- 原声固定 16 kHz 单声道 16-bit WAV,最多 128000 帧、300 KiB。

## 配置

凭证不提交。把 `main/local_secrets.h.example` 复制成 `main/local_secrets.h`(已忽略),
填入 Wi-Fi、Sonar 地址与设备 Token 后编译。首次启动把这些值写入 NVS,之后改 NVS
即可,不必重新编译。灵矽身份是 Wi-Fi MAC(Device-Id)+ 持久化 UUID v4(Client-Id),
用设备显示的激活码在灵矽智控台绑定。

首次创建 `main/local_secrets.h` 后要先 `touch main/sonar_config.c` 再编译:上一次编译时该头文件还不存在,Ninja 没有它的依赖,否则第一次构建不会把它编进去。

## 存储与内存

`partitions.csv` 保留 4 MB 应用分区,新增 SPIFFS `storage` 分区。录音和下载的原声
都流式落盘,任何时刻都不把整段音频放进 RAM。Sonar 业务走明文 HTTP(服务端地址是
IP,证书轮换代价高于实验期收益);TLS 只留给灵矽语音,入向分片 5120 字节(该服务的
证书链 4374 字节)、出向 2 KB,蓝牙关闭、Wi-Fi 收发缓冲压缩。

## 语音通道

灵矽语音是 `wss://xrobo-io.qiniuapi.com/v1/ws/`,由 `main/linx_ws.c` 自己实现:
mbedtls 直接握手 + RFC 6455 帧编解码(掩码、分片重组、ping/pong、close),不经
`esp_websocket_client`。两条约束决定了这个选择:

- 该服务的链末是 `DigiCert Global Root G2` 的**交叉签名**证书,与自签名 G2 同名同公钥,
mbedtls 组链以 `-0x2700` 失败;改为直接信任签发 leaf 的中间 CA
(`TrustAsia DV TLS RSA CA 2025`)后链长为 1,握手稳定。
- C3 同时只养得起一条 TLS 会话:握手后 ssl 上下文加收发缓冲约占 15 KB。

由此得到三条固件纪律:

1. 语音会话期间 `net_task` 停止所有 HTTP 轮询——录音是 20 ms 一帧的持续写,
   并发一次 HTTP 就会分配失败并把音频帧一起带崩;
2. 帧掩码用固定缓冲复用,发送路径上不做 malloc;
3. 握手完成后立刻释放 CA 与熵源(不重协商),关闭时按 `inited` 标记释放全部 mbedtls
   上下文——漏一次就是 10 KB 级的静态泄漏。

灵矽的 `stt` 通道返回什么就显示什么:账号额度用尽时服务端把提示语当作转写返回
(实测为「额度不足,请联系灵矽商务」)。

## 验证

```sh
./tools/validate.sh --static     # 仓库检查 + 主机测试(含纯逻辑)
./tools/validate.sh --firmware   # ESP-IDF 5.5.3 构建与合并镜像
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

主机测试覆盖 WAV 头与 RIFF 解析、URL 编码、RFC3339 换算、决策规则、播放门禁与退避。
录音电平、音量、真实灵矽 STT 时延与邀请时序仍需上板验证。
