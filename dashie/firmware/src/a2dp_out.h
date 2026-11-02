#pragma once
// ---------------------------------------------------------------------------
// A2DP *source* output to a car hands-free kit. Compiled only with -DUSE_A2DP_OUT
// (env core2_a2dp; ESP32 classic only - the S3 has no Bluetooth Classic).
//
// One "session" = bring BT up -> connect -> stream the reply -> tear everything down,
// all in a worker task so the face keeps animating. Between sessions Bluetooth is
// off (controller idle, Bluedroid deinit), so the kit stays free for the phone.
// NOT verified on hardware: see firmware/README.md.
// ---------------------------------------------------------------------------
#ifdef USE_A2DP_OUT
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_bt_main.h>
#include <esp_bt.h>
#include <BluetoothA2DPSource.h>
#include "config.h"

namespace a2o {

enum class Res : uint8_t { NONE, OK, NOT_FOUND, TIMEOUT, NO_MEM, FAIL };
static constexpr int MAX_FOUND = 8;

namespace detail {
static BluetoothA2DPSource* bt = nullptr;
static volatile bool active = false, abortReq = false, streaming = false, ended = false, seen = false;
static volatile Res result = Res::NONE;
static const int16_t* volatile pcm = nullptr;
static size_t nSamp = 0;
static uint32_t rateIn = 16000;
static uint64_t phase = 0, step = 0;
static volatile size_t played = 0;
static volatile uint32_t lead = 0;
static int16_t* ownTone = nullptr;
static bool skipSaved = false, discoveryOnly = false;
static uint32_t scanMs = A2DP_CONNECT_TIMEOUT_MS;
static char target[40];
static esp_bd_addr_t hitAddr;
static char foundName[MAX_FOUND][33];
static esp_bd_addr_t foundAddr[MAX_FOUND];
static volatile int foundN = 0;

static void loadTarget() {
  Preferences p; p.begin("dashie_bt", true);
  String n = p.getString("name", "");
  p.end();
  strlcpy(target, n.length() ? n.c_str() : A2DP_TARGET_NAME, sizeof(target));
}
static bool loadAddr(esp_bd_addr_t out) {
  Preferences p; p.begin("dashie_bt", true);
  bool ok = p.getBytesLength("addr") == ESP_BD_ADDR_LEN && p.getBytes("addr", out, ESP_BD_ADDR_LEN) == ESP_BD_ADDR_LEN;
  p.end();
  return ok;
}
static void saveAddr(const uint8_t* a) { Preferences p; p.begin("dashie_bt", false); p.putBytes("addr", a, ESP_BD_ADDR_LEN); p.end(); }

// Called by the library for every audio-capable device found while scanning.
static bool ssidCb(const char* ssid, esp_bd_addr_t addr, int rssi) {
  bool dup = false;
  for (int i = 0; i < foundN; i++) if (memcmp(foundAddr[i], addr, ESP_BD_ADDR_LEN) == 0) { dup = true; break; }
  if (!dup && foundN < MAX_FOUND && ssid && ssid[0]) {
    strlcpy(foundName[foundN], ssid, sizeof(foundName[0]));
    memcpy(foundAddr[foundN], addr, ESP_BD_ADDR_LEN);
    foundN = foundN + 1;
  }
  if (target[0] && strncmp(ssid, target, strlen(target)) == 0) {
    seen = true; memcpy(hitAddr, addr, ESP_BD_ADDR_LEN);
    return true;
  }
  return false;
}

// 16 kHz mono -> 44.1 kHz stereo, linear interpolation, with gain headroom.
static int32_t dataCb(Frame* f, int32_t frames) {
  const int16_t* p = pcm;
  if (!p) return 0;
  streaming = true;
  int32_t i = 0;
  for (; i < frames; i++) {
    int16_t v = 0;
    if (lead) { lead = lead - 1; }                 // short silence: some kits clip the first ~200 ms
    else {
      size_t idx = (size_t)(phase >> 16);
      if (idx >= nSamp) { ended = true; break; }
      int32_t s0 = p[idx], s1 = (idx + 1 < nSamp) ? p[idx + 1] : s0;
      int32_t s = s0 + (int32_t)(((int64_t)(s1 - s0) * (int64_t)(phase & 0xFFFF)) >> 16);
      v = (int16_t)((s * A2DP_GAIN_Q15) >> 15);
      phase += step; played = idx;
    }
    f[i].channel1 = v; f[i].channel2 = v;
  }
  return i;
}

static void task(void*) {
  Res r = Res::FAIL;
  bool started = false, usedSaved = false;
  esp_bd_addr_t saved;
  do {
    size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    Serial.printf("a2dp: free internal %u, largest %u (need %u)\n", (unsigned)freeInt,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)A2DP_MIN_FREE_HEAP);
    if (freeInt < A2DP_MIN_FREE_HEAP) { r = Res::NO_MEM; break; }
    if (!bt) { bt = new BluetoothA2DPSource(); if (!bt) { r = Res::NO_MEM; break; } }
#if A2DP_RELEASE_BLE
    static bool bleReleased = false;
    if (!bleReleased) { esp_bt_controller_mem_release(ESP_BT_MODE_BLE); bleReleased = true; }
#endif
    loadTarget();
    usedSaved = !discoveryOnly && !skipSaved && loadAddr(saved);
    if (usedSaved) bt->set_auto_reconnect(saved); else bt->set_auto_reconnect(false);
    bt->set_ssid_callback(ssidCb);
    bt->set_data_callback_in_frames(dataCb);
    bt->set_volume(110);
    started = true;
    bt->start(target);

    uint32_t t0 = millis(); bool conn = false;
    uint32_t limit = usedSaved ? (uint32_t)A2DP_CONNECT_TIMEOUT_MS : scanMs;
    while (!abortReq && millis() - t0 < limit) { if (bt->is_connected()) { conn = true; break; } vTaskDelay(pdMS_TO_TICKS(50)); }
    if (abortReq) { r = Res::FAIL; break; }
    if (!conn) { r = (!usedSaved && !seen) ? Res::NOT_FOUND : Res::TIMEOUT; break; }
    if (seen) saveAddr(hitAddr);

    // The library only polls "source ready" on a 10 s heartbeat; kick it so audio starts quickly.
    uint32_t t1 = millis(), lastKick = 0;
    while (!abortReq && !streaming && millis() - t1 < A2DP_STREAM_START_TIMEOUT_MS) {
      if (millis() - lastKick >= 1000) { esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY); lastKick = millis(); }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!streaming) { r = abortReq ? Res::FAIL : Res::TIMEOUT; break; }
    while (!abortReq && !ended && bt->is_connected()) vTaskDelay(pdMS_TO_TICKS(50));
    if (abortReq) { r = Res::FAIL; break; }
    if (ended) { vTaskDelay(pdMS_TO_TICKS(600)); r = Res::OK; }   // let the last audio drain
    else r = Res::FAIL;                                            // link dropped mid-reply
  } while (false);

  pcm = nullptr;
  if (started && bt) {
    bt->end(false);                                                // disconnect + stop A2DP/AVRC
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) esp_bluedroid_disable();
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) esp_bluedroid_deinit();
    // controller stays initialised (idle): a full release would make a restart impossible until reboot
  }
  if (r == Res::OK) skipSaved = false;
  else if (usedSaved) skipSaved = true;                            // next try: discover instead
  if (ownTone) { heap_caps_free(ownTone); ownTone = nullptr; }
  Serial.printf("a2dp: session result %d\n", (int)r);
  result = r;
  active = false;
  vTaskDelete(nullptr);
}

static bool launch(bool discover, uint32_t scan) {
  active = true; abortReq = false; streaming = false; ended = false; seen = false;
  result = Res::NONE; played = 0; foundN = 0; phase = 0; lead = 44100 * 3 / 10;
  discoveryOnly = discover; scanMs = scan;
  step = ((uint64_t)rateIn << 16) / 44100;
  if (xTaskCreatePinnedToCore(task, "a2dp", 8192, nullptr, 2, nullptr, 0) != pdPASS) {
    active = false; pcm = nullptr; result = Res::NO_MEM; return false;
  }
  return true;
}
}  // namespace detail

// ---- public API (call from the main loop) ----
inline bool busy() { return detail::active; }
inline Res result() { return detail::result; }
inline bool streaming() { return detail::active && detail::streaming && !detail::ended; }
inline size_t played() { return detail::played; }            // index of the input sample being sent
inline void abort() { if (detail::active) detail::abortReq = true; }
inline int foundCount() { return detail::foundN; }
inline const char* foundName(int i) { return (i >= 0 && i < detail::foundN) ? detail::foundName[i] : ""; }

// Stream 16-bit mono PCM (buffer must stay valid until busy() is false). false = busy / no task.
inline bool play(const int16_t* pcm, size_t n, uint32_t rate) {
  if (detail::active || !pcm || !n) return false;
  detail::pcm = pcm; detail::nSamp = n; detail::rateIn = rate;
  return detail::launch(false, A2DP_CONNECT_TIMEOUT_MS);
}

// Settings > Test kit: connect (scanning long enough to fill the pick list) and play a short tone.
inline bool test() {
  if (detail::active) return false;
  const size_t N = 11200;                                        // 0.7 s @16k
  detail::ownTone = (int16_t*)heap_caps_malloc(N * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!detail::ownTone) return false;
  for (size_t i = 0; i < N; i++) {
    float f = i < N / 2 ? 659.0f : 880.0f;
    size_t seg = i < N / 2 ? i : i - N / 2, segN = N / 2;
    float env = min(1.0f, min(seg, segN - 1 - seg) / 320.0f);
    detail::ownTone[i] = (int16_t)(9000.0f * env * sinf(2.0f * PI * f * i / 16000.0f));
  }
  detail::pcm = detail::ownTone; detail::nSamp = N; detail::rateIn = 16000;
  if (!detail::launch(false, A2DP_SCAN_MS)) { heap_caps_free(detail::ownTone); detail::ownTone = nullptr; return false; }
  return true;
}

// Persist the picked scan entry (name overrides config.h; address makes later connects scan-free).
inline void selectFound(int i) {
  if (detail::active || i < 0 || i >= detail::foundN) return;
  Preferences p; p.begin("dashie_bt", false);
  p.putString("name", detail::foundName[i]);
  p.putBytes("addr", detail::foundAddr[i], ESP_BD_ADDR_LEN);
  p.end();
  detail::skipSaved = false;
}

}  // namespace a2o
#endif  // USE_A2DP_OUT
