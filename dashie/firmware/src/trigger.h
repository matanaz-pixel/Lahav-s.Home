#pragma once
#include <Arduino.h>
#include <math.h>

// ---------------------------------------------------------------------------
// "Double shout" voice trigger.
// Two loud bursts (200..700 ms each) whose gap ends within 1400 ms of the first
// burst's end. Threshold = adaptive noise floor + margin. Works in a noisy cabin
// without any ML model: say "Dashie! ... Dashie!" or clap twice.
// ---------------------------------------------------------------------------
class DoubleShout {
public:
  void holdoff(uint32_t now, uint32_t ms) { warmUntil_ = now + ms; haveFirst_ = false; inBurst_ = false; }
  float floorDb() const { return floor_; }

  // db: level of the latest frame, margin: dB above floor that counts as "loud".
  bool update(float db, uint32_t now, float margin) {
    if (!init_) { floor_ = db; init_ = true; }
    bool above = db > floor_ + margin;

    // Floor tracking: rises slowly, falls fast; sustained loudness becomes the new floor.
    if (!above) {
      floor_ += (db - floor_) * (db > floor_ ? 0.006f : 0.04f);
    } else if (inBurst_ && now - burstStart_ > 1500) {
      floor_ += (db - floor_) * 0.03f;
    }

    if (above) {
      lastAbove_ = now;
      if (!inBurst_) { inBurst_ = true; burstStart_ = now; }
    } else if (inBurst_ && now - lastAbove_ >= 80) {  // 80 ms hangover
      inBurst_ = false;
      uint32_t len = lastAbove_ - burstStart_ + 32;     // + one frame
      if (len >= 200 && len <= 700) {
        if (now < warmUntil_ || now < refractoryUntil_) {
          haveFirst_ = false;
        } else if (haveFirst_ && lastAbove_ - firstEnd_ <= 1400) {
          haveFirst_ = false;
          refractoryUntil_ = now + 3000;
          return true;
        } else {
          haveFirst_ = true;
          firstEnd_ = lastAbove_;
        }
      } else if (len > 700) {
        haveFirst_ = false;   // speech/noise, not a shout
      }                       // < 200 ms: click, ignored
    }
    if (haveFirst_ && now - firstEnd_ > 1400) haveFirst_ = false;
    return false;
  }

private:
  float floor_ = 30.0f;
  bool init_ = false, inBurst_ = false, haveFirst_ = false;
  uint32_t burstStart_ = 0, lastAbove_ = 0, firstEnd_ = 0;
  uint32_t refractoryUntil_ = 0, warmUntil_ = 0;
};

// ---------------------------------------------------------------------------
// Optional Edge Impulse keyword spotting hook.
// 1) Train a keyword model at studio.edgeimpulse.com, export "Arduino library",
//    add it to lib_deps, and #define USE_EI_KWS in config.h.
// 2) Implement the function below in a new src/kws_ei.cpp, e.g.:
//      #include <your_project_inference.h>
//      bool kwsInfer(const int16_t* s, size_t n) {
//        // append s[0..n) to a sliding window of EI_CLASSIFIER_RAW_SAMPLE_COUNT samples,
//        // call run_classifier(), return (result.classification[KW].value > 0.8f);
//      }
// It is called once per 32 ms audio frame while IDLE.
// ---------------------------------------------------------------------------
#ifdef USE_EI_KWS
bool kwsInfer(const int16_t* samples, size_t n);
#endif
