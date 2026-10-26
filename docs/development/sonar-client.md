<p align="right">
  <strong>English</strong> · <a href="sonar-client.zh_CN.md">简体中文</a>
</p>

# Sonar client firmware (ESP32-C3, push-to-talk)

This application turns the FoloToy AI Passport board into the Sonar voice terminal.
Linx provides device binding, speech-to-text and the private assistant; Sonar stores
the real human voices and manages questions, invitations and deliveries. The device
keeps its own speaker and microphone, records up to eight seconds and plays the
voices it receives.

## Why push-to-talk

The ESP32-C3 has no AEC (ESP-SR AFE does not target C3), so the microphone hears the
board's own speaker. The application is therefore half duplex: it never records while
playing, and pressing OK during playback stops the sound instead of starting a
recording. Wake words and voice barge-in are out of scope for this board; stopping is
a local action that works offline.

## Interaction

| Input | Action |
| --- | --- |
| Hold OK | Record (maximum 8 s, hard stop by the audio worker) |
| Release OK | Stop recording, run Linx STT, then follow the decision rules |
| Click OK | Stop playback, or publish the pending draft |
| Click UP | Switch demo place (Moli / AI Island 19 / Jinke Rd) and refresh mock location |
| Click DOWN | Toggle quiet (local silence first, then `PUT /v1/me/context`) |
| Long UP | Re-sync account, places and location |
| Long DOWN | Cancel the last question |

The screen shows phase, place, network/Sonar/Linx state, pending count and battery.
All on-screen text is ASCII on purpose: the default LVGL fonts carry no CJK glyphs,
and this build ships no CJK subset.

## Decision rules

`sonar_decision.c` is deterministic, because the service exposes no channel for a
model decision to reach the device:

1. quiet, or no publishing intent → nothing is published;
2. a locked invitation is valid → the utterance becomes an `answer`;
3. a demo place appears in the transcript and the sentence is a question → `question`;
4. otherwise → private assistant (Linx TTS is played, nothing is published).

Question metadata uses `direction: "unknown"`, required for real POIs. Answers always
carry `reply_to` and `invitation_id` from the locked invitation.

## Server contract

`server/app/*.py` and the deployed `/openapi.json` are authoritative; the handoff
document describes intent only. Differences that matter to the firmware:

- device tokens are per user (`SONAR_ALICE_TOKEN`, `SONAR_BOB_TOKEN`), not a single
  `SONAR_DEVICE_TOKEN`;
- `POST /v1/posts/{id}/confirm` is the only publish path and takes an empty body;
  answers additionally require the invitation to be approved (Bamboo) and still open;
- idempotency fingerprints include the audio bytes, so retries must reuse the file;
- `PUT /v1/me/location` needs `SONAR_ALLOW_MOCK_LOCATION=true` for `android_mock`;
- audio is 16 kHz mono 16-bit WAV, at most 128000 frames and 300 KiB.

## Configuration

Credentials are never committed. Copy `main/local_secrets.h.example` to
`main/local_secrets.h` (git-ignored), fill Wi-Fi, Sonar base URL and the device token,
then build. The firmware seeds NVS on first boot; later changes can be made in NVS
without rebuilding. Linx identity is the Wi-Fi MAC (Device-Id) plus a persistent UUID
v4 (Client-Id). Binding happens on the Linx console with the activation code the
device shows.

Create `main/local_secrets.h` once, then `touch main/sonar_config.c` before building: a header that did not exist during the previous compile is not in Ninja's dependency graph yet, so the first build after creating it would otherwise skip seeding.

## Storage and memory

`partitions.csv` keeps a 4 MB factory app and adds a SPIFFS `storage` partition.
Recording and downloaded voices are streamed to files; nothing keeps a whole clip in
RAM. Sonar traffic is plain HTTP (the server is addressed by IP, and certificate
rotation costs more than it buys during the experiment); TLS is reserved for the Linx
voice link: 5120-byte inbound fragments (the service's certificate chain is 4374 bytes)
and 2 KB outbound, with Bluetooth disabled and the Wi-Fi pools reduced.

## Voice link

The Linx voice session is `wss://xrobo-io.qiniuapi.com/v1/ws/`, implemented in
`main/linx_ws.c`: an mbedtls handshake plus RFC 6455 framing (masking, fragment
reassembly, ping/pong, close), without `esp_websocket_client`. Two constraints drive
that choice:

- the chain ends in the **cross-signed** `DigiCert Global Root G2`, which shares subject
  and key with the self-signed G2 and makes mbedtls chain building fail with `-0x2700`;
  trusting the intermediate that issued the leaf (`TrustAsia DV TLS RSA CA 2025`) keeps
  the chain one hop long and verifies reliably;
- a C3 can afford exactly one TLS session at a time: context plus I/O buffers take
  about 15 KB after the handshake.

Three firmware rules follow:

1. `net_task` issues no HTTP polling while a voice session is active — the uplink is a
   continuous 20 ms-frame write, and one concurrent request breaks it with an
   allocation failure;
2. frame masking reuses a fixed buffer; the send path never calls malloc;
3. the CA and entropy source are released right after the handshake (no renegotiation),
   and shutdown frees every mbedtls context behind an `inited` flag — missing one leaks
   ~10 KB per session.

The device displays whatever the `stt` channel returns: when the account quota is
exhausted the server sends that notice as the transcript (observed: a Chinese
"quota exhausted, contact Linx sales" notice).

## Verification

```sh
./tools/validate.sh --static     # repo checks + host tests (pure logic included)
./tools/validate.sh --firmware   # ESP-IDF 5.5.3 build + merged image
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Host tests cover the WAV header/RIFF parsing, URL encoding, RFC3339 arithmetic, the
decision rules and the playback gate/backoff. Device behaviour (recording level,
speaker volume, real Linx STT latency, invitation timing) still needs the board.
