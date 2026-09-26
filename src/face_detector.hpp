#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct FaceResult {
  float center_x = 0.5f;
  float center_y = 0.5f;
  float width = 0.0f;
  float height = 0.0f;
  float left_eye_x = 0.42f;
  float left_eye_y = 0.45f;
  float right_eye_x = 0.58f;
  float right_eye_y = 0.45f;
  float confidence = 0.0f;
};

constexpr size_t kMaxFaces = 3;

struct DetectionResult {
  std::array<FaceResult, kMaxFaces> faces{};
  size_t count = 0;
  uint64_t generation = 0;
};

class FaceDetector {
public:
  explicit FaceDetector(std::string model_path);
  ~FaceDetector();

  FaceDetector(const FaceDetector &) = delete;
  FaceDetector &operator=(const FaceDetector &) = delete;

  void submit_rgba(std::vector<uint8_t> pixels, int width, int height);
  DetectionResult latest() const;
  bool ready() const { return ready_.load(); }

private:
  struct PendingFrame {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    bool available = false;
  };

  void run();

  std::string model_path_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  PendingFrame pending_;
  DetectionResult result_;
  std::thread worker_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> ready_{false};
};
