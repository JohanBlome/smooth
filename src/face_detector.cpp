#include "face_detector.hpp"

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>

namespace {
float clamp01(float value) { return std::max(0.0f, std::min(1.0f, value)); }
}

FaceDetector::FaceDetector(std::string model_path) : model_path_(std::move(model_path)), worker_(&FaceDetector::run, this) {}

FaceDetector::~FaceDetector()
{
  stopping_.store(true);
  wake_.notify_one();
  if (worker_.joinable())
    worker_.join();
}

void FaceDetector::submit_rgba(std::vector<uint8_t> pixels, int width, int height)
{
  std::lock_guard<std::mutex> lock(mutex_);
  pending_.pixels = std::move(pixels);
  pending_.width = width;
  pending_.height = height;
  pending_.available = true;
  wake_.notify_one();
}

DetectionResult FaceDetector::latest() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return result_;
}

void FaceDetector::run()
{
  cv::Ptr<cv::FaceDetectorYN> detector;
  try {
    detector = cv::FaceDetectorYN::create(model_path_, "", cv::Size(320, 180), 0.72f, 0.3f, 5000);
    ready_.store(true);
  } catch (const cv::Exception &) {
    ready_.store(false);
    return;
  }

  uint64_t generation = 0;
  while (!stopping_.load()) {
    PendingFrame frame;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_.load() || pending_.available; });
      if (stopping_.load())
        break;
      frame = std::move(pending_);
      pending_.available = false;
    }

    DetectionResult next;
    next.generation = ++generation;
    try {
      cv::Mat rgba(frame.height, frame.width, CV_8UC4, frame.pixels.data());
      cv::Mat bgr;
      cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);
      detector->setInputSize(bgr.size());
      cv::Mat faces;
      detector->detect(bgr, faces);

      struct Candidate {
        int row;
        float area;
      };
      std::vector<Candidate> candidates;
      candidates.reserve(static_cast<size_t>(faces.rows));
      for (int row = 0; row < faces.rows; ++row) {
        const float area = faces.at<float>(row, 2) * faces.at<float>(row, 3);
        candidates.push_back({row, area});
      }
      std::sort(candidates.begin(), candidates.end(),
                [](const Candidate &a, const Candidate &b) { return a.area > b.area; });

      next.count = std::min(kMaxFaces, candidates.size());
      for (size_t index = 0; index < next.count; ++index) {
        FaceResult &result = next.faces[index];
        const float *face = faces.ptr<float>(candidates[index].row);
        const float inv_w = 1.0f / static_cast<float>(frame.width);
        const float inv_h = 1.0f / static_cast<float>(frame.height);
        const float expanded_w = face[2] * 1.18f;
        const float expanded_h = face[3] * 1.24f;
        result.center_x = clamp01((face[0] + face[2] * 0.5f) * inv_w);
        result.center_y = clamp01((face[1] + face[3] * 0.52f) * inv_h);
        result.width = clamp01(expanded_w * inv_w);
        result.height = clamp01(expanded_h * inv_h);
        // YuNet returns right eye first, then left eye.
        result.right_eye_x = clamp01(face[4] * inv_w);
        result.right_eye_y = clamp01(face[5] * inv_h);
        result.left_eye_x = clamp01(face[6] * inv_w);
        result.left_eye_y = clamp01(face[7] * inv_h);
        result.confidence = clamp01(face[14]);
      }
    } catch (const cv::Exception &) {
      next.count = 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    result_ = next;
  }
}
