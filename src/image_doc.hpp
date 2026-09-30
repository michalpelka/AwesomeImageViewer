#pragma once
#include "exif.hpp"

#include <opencv2/core.hpp>
#include <raylib.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

constexpr int kHistBins = 256;

struct ChannelStats {
    double min = 0, max = 0, mean = 0, stddev = 0;
};

struct Histogram {
    int channels = 0;                                  // in display order (R,G,B,A / Gray)
    std::array<std::vector<float>, 4> bins;            // kHistBins each
    double lo = 0, hi = 1;                             // value range covered by the bins
};

// How the pixel data is mapped to the 8-bit display texture.
struct DisplaySettings {
    bool normalize = false;  // stretch min..max of the data instead of the type's nominal range
    int channel = 0;         // 0 = all, 1 = R, 2 = G, 3 = B, 4 = A (shown as grayscale)
    bool operator==(const DisplaySettings&) const = default;
};

struct LoadedData {
    bool ok = false;
    std::string error;
    cv::Mat mat;               // pixel data, OpenCV channel order (BGR/BGRA); exotic depths promoted to 32F
    cv::Mat rgba;              // 8-bit RGBA display copy (possibly downscaled to fit GPU limits)
    DisplaySettings rgbaSettings;
    std::string origType;      // type as decoded, e.g. "16UC3"
    std::string format;        // container, e.g. "JPEG"
    std::vector<MetaEntry> exif;
    std::vector<ChannelStats> stats;  // display order
    Histogram hist;
    uintmax_t fileSize = 0;
    std::filesystem::file_time_type mtime{};
    double decodeMs = 0;
};

struct ImageDoc {
    std::string path, name;

    // Worker -> main thread hand-off.
    std::mutex mtx;
    std::unique_ptr<LoadedData> pending;
    std::atomic<bool> loading{false};

    // Main thread only.
    std::unique_ptr<LoadedData> data;
    Texture2D tex{};
    bool hasTex = false;
    std::filesystem::file_time_type watchedMtime{};

    bool hasRoi = false;
    cv::Rect roi;
    std::vector<ChannelStats> roiStats;
    Histogram roiHist;
};

// Nominal value range for a depth (0..255, 0..65535, 0..1 for float, ...).
void nominalRange(int depth, double& lo, double& hi);
// Channel values at (x,y) in display order (R,G,B,A / Gray / Gray,A).
std::vector<double> pixelValues(const cv::Mat& m, int x, int y);
// Channel labels in display order.
std::vector<const char*> channelLabels(int channels);
std::vector<ChannelStats> computeStats(const cv::Mat& m);
Histogram computeHistogram(const cv::Mat& m, double lo, double hi);
// Histogram range used for a whole image (nominal for 8/16 bit, data min..max otherwise).
void histogramRange(const cv::Mat& m, const std::vector<ChannelStats>& stats, double& lo, double& hi);
cv::Mat makeDisplayRGBA(const cv::Mat& m, const std::vector<ChannelStats>& stats, const DisplaySettings& s);

std::unique_ptr<LoadedData> loadImageFile(const std::string& path, const DisplaySettings& disp);

// Small fixed pool that decodes images off the UI thread.
class Loader {
public:
    explicit Loader(unsigned threads);
    ~Loader();
    void enqueue(std::shared_ptr<ImageDoc> doc, DisplaySettings disp);
    bool busy() const { return inFlight_.load() > 0; }

private:
    void worker();
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::pair<std::shared_ptr<ImageDoc>, DisplaySettings>> q_;
    std::vector<std::thread> threads_;
    std::atomic<int> inFlight_{0};
    bool stop_ = false;
};
