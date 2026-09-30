#include "image_doc.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

namespace {

constexpr int kMaxTextureSize = 16384;

// OpenCV channel index for a display-order channel.
int cvChannel(int channels, int displayIdx)
{
    if (channels >= 3) return displayIdx < 3 ? 2 - displayIdx : displayIdx;
    return displayIdx;
}

bool isNativeDepth(int d)
{
    return d == CV_8U || d == CV_8S || d == CV_16U || d == CV_16S || d == CV_32S || d == CV_32F || d == CV_64F;
}

template <typename T>
double at(const uchar* p) { return double(*reinterpret_cast<const T*>(p)); }

}  // namespace

void nominalRange(int depth, double& lo, double& hi)
{
    switch (depth) {
    case CV_8U: lo = 0; hi = 255; break;
    case CV_8S: lo = -128; hi = 127; break;
    case CV_16U: lo = 0; hi = 65535; break;
    case CV_16S: lo = -32768; hi = 32767; break;
    case CV_32S: lo = 0; hi = 65535; break;
    default: lo = 0; hi = 1; break;
    }
}

std::vector<const char*> channelLabels(int channels)
{
    switch (channels) {
    case 1: return {"Y"};
    case 2: return {"Y", "A"};
    case 3: return {"R", "G", "B"};
    case 4: return {"R", "G", "B", "A"};
    default: {
        static const char* n[] = {"c0", "c1", "c2", "c3", "c4", "c5", "c6", "c7"};
        std::vector<const char*> r;
        for (int i = 0; i < channels && i < 8; ++i) r.push_back(n[i]);
        return r;
    }
    }
}

std::vector<double> pixelValues(const cv::Mat& m, int x, int y)
{
    std::vector<double> v;
    if (x < 0 || y < 0 || x >= m.cols || y >= m.rows) return v;
    const int nch = m.channels();
    const uchar* px = m.ptr(y) + size_t(x) * m.elemSize();
    const size_t es = m.elemSize1();
    for (int i = 0; i < nch; ++i) {
        const uchar* p = px + size_t(cvChannel(nch, i)) * es;
        switch (m.depth()) {
        case CV_8U: v.push_back(at<uint8_t>(p)); break;
        case CV_8S: v.push_back(at<int8_t>(p)); break;
        case CV_16U: v.push_back(at<uint16_t>(p)); break;
        case CV_16S: v.push_back(at<int16_t>(p)); break;
        case CV_32S: v.push_back(at<int32_t>(p)); break;
        case CV_32F: v.push_back(at<float>(p)); break;
        case CV_64F: v.push_back(at<double>(p)); break;
        default: v.push_back(0); break;
        }
    }
    return v;
}

std::vector<ChannelStats> computeStats(const cv::Mat& m)
{
    std::vector<ChannelStats> out;
    cv::Mat ch;
    for (int i = 0; i < m.channels(); ++i) {
        cv::extractChannel(m, ch, cvChannel(m.channels(), i));
        ChannelStats s;
        cv::minMaxLoc(ch, &s.min, &s.max);
        cv::Scalar mean, dev;
        cv::meanStdDev(ch, mean, dev);
        s.mean = mean[0];
        s.stddev = dev[0];
        out.push_back(s);
    }
    return out;
}

void histogramRange(const cv::Mat& m, const std::vector<ChannelStats>& stats, double& lo, double& hi)
{
    switch (m.depth()) {
    case CV_8U: lo = 0; hi = 256; return;
    case CV_8S: lo = -128; hi = 128; return;
    case CV_16U: lo = 0; hi = 65536; return;
    default: break;
    }
    lo = stats.empty() ? 0 : stats[0].min;
    hi = stats.empty() ? 1 : stats[0].max;
    for (auto& s : stats) { lo = std::min(lo, s.min); hi = std::max(hi, s.max); }
    if (!(hi > lo)) hi = lo + 1;
    hi += (hi - lo) * 1e-6;  // calcHist's upper bound is exclusive
}

Histogram computeHistogram(const cv::Mat& m, double lo, double hi)
{
    Histogram h;
    h.lo = lo;
    h.hi = hi;
    h.channels = std::min(m.channels(), 4);
    const bool direct = m.depth() == CV_8U || m.depth() == CV_16U || m.depth() == CV_32F;
    cv::Mat ch;
    for (int i = 0; i < h.channels; ++i) {
        cv::extractChannel(m, ch, cvChannel(m.channels(), i));
        if (!direct) ch.convertTo(ch, CV_32F);
        const int histSize[] = {kHistBins};
        const float range[] = {float(lo), float(hi)};
        const float* ranges[] = {range};
        const int chan0[] = {0};
        cv::Mat hist;
        cv::calcHist(&ch, 1, chan0, cv::Mat(), hist, 1, histSize, ranges, true, false);
        h.bins[i].assign(hist.ptr<float>(), hist.ptr<float>() + kHistBins);
    }
    return h;
}

cv::Mat makeDisplayRGBA(const cv::Mat& m, const std::vector<ChannelStats>& stats, const DisplaySettings& s)
{
    const int nch = m.channels();
    const bool hasAlpha = nch == 2 || nch == 4;
    const int colorCh = nch >= 3 ? 3 : 1;

    double nlo, nhi;
    nominalRange(m.depth(), nlo, nhi);
    double lo = nlo, hi = nhi;
    if (s.normalize && !stats.empty()) {
        lo = stats[0].min;
        hi = stats[0].max;
        for (int i = 1; i < colorCh && i < int(stats.size()); ++i) {
            lo = std::min(lo, stats[i].min);
            hi = std::max(hi, stats[i].max);
        }
    }
    if (!(hi > lo)) hi = lo + 1;

    std::vector<cv::Mat> src;
    cv::split(m, src);
    auto conv = [](const cv::Mat& c, double l, double h) {
        cv::Mat o;
        const double a = 255.0 / (h - l);
        c.convertTo(o, CV_8U, a, -l * a);
        return o;
    };
    const cv::Mat opaque(m.size(), CV_8U, cv::Scalar(255));

    // Map requested channel to a display index that exists in this image.
    int sel = s.channel - 1;  // display index, -1 = all
    if (sel >= 0) {
        if (sel == 3) sel = hasAlpha ? nch - 1 : -1;   // A
        else if (colorCh == 1) sel = -1;               // R/G/B on gray: just show gray
    }

    std::vector<cv::Mat> rgba;
    if (sel >= 0) {
        const bool isAlpha = hasAlpha && sel == nch - 1;
        const cv::Mat g = isAlpha ? conv(src[cvChannel(nch, sel)], nlo, nhi) : conv(src[cvChannel(nch, sel)], lo, hi);
        rgba = {g, g, g, opaque};
    } else if (colorCh == 3) {
        rgba = {conv(src[2], lo, hi), conv(src[1], lo, hi), conv(src[0], lo, hi),
                hasAlpha ? conv(src[3], nlo, nhi) : opaque};
    } else {
        const cv::Mat g = conv(src[0], lo, hi);
        rgba = {g, g, g, hasAlpha ? conv(src[1], nlo, nhi) : opaque};
    }
    cv::Mat out;
    cv::merge(rgba, out);

    const int maxDim = std::max(out.cols, out.rows);
    if (maxDim > kMaxTextureSize) {
        const double f = double(kMaxTextureSize) / maxDim;
        cv::resize(out, out, cv::Size(), f, f, cv::INTER_AREA);
    }
    return out;
}

std::unique_ptr<LoadedData> loadImageFile(const std::string& path, const DisplaySettings& disp)
{
    auto r = std::make_unique<LoadedData>();
    const auto t0 = std::chrono::steady_clock::now();

    std::error_code ec;
    r->mtime = std::filesystem::last_write_time(path, ec);
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        r->error = "Cannot open file";
        return r;
    }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    r->fileSize = buf.size();
    r->format = detectFormat(buf);

    try {
        r->mat = cv::imdecode(buf, cv::IMREAD_UNCHANGED);
    } catch (const cv::Exception& e) {
        r->error = e.what();
        return r;
    }
    if (r->mat.empty()) {
        r->error = "OpenCV cannot decode this file (" + r->format + ")";
        return r;
    }
    r->origType = cv::typeToString(r->mat.type());
    if (!isNativeDepth(r->mat.depth())) r->mat.convertTo(r->mat, CV_MAKETYPE(CV_32F, r->mat.channels()));
    if (r->mat.channels() > 4) {
        std::vector<cv::Mat> ch;
        cv::split(r->mat, ch);
        ch.resize(4);
        cv::merge(ch, r->mat);
    }

    try {
        r->exif = parseExif(buf);
    } catch (...) {
    }
    buf.clear();
    buf.shrink_to_fit();

    r->stats = computeStats(r->mat);
    histogramRange(r->mat, r->stats, r->hist.lo, r->hist.hi);
    r->hist = computeHistogram(r->mat, r->hist.lo, r->hist.hi);
    r->rgba = makeDisplayRGBA(r->mat, r->stats, disp);
    r->rgbaSettings = disp;
    r->decodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    r->ok = true;
    return r;
}

Loader::Loader(unsigned threads)
{
    for (unsigned i = 0; i < std::max(1u, threads); ++i) threads_.emplace_back([this] { worker(); });
}

Loader::~Loader()
{
    {
        std::lock_guard lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

void Loader::enqueue(std::shared_ptr<ImageDoc> doc, DisplaySettings disp)
{
    if (doc->loading.exchange(true)) return;
    ++inFlight_;
    {
        std::lock_guard lk(m_);
        q_.emplace_back(std::move(doc), disp);
    }
    cv_.notify_one();
}

void Loader::worker()
{
    for (;;) {
        std::pair<std::shared_ptr<ImageDoc>, DisplaySettings> job;
        {
            std::unique_lock lk(m_);
            cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
            if (stop_) return;
            job = std::move(q_.front());
            q_.pop_front();
        }
        auto result = loadImageFile(job.first->path, job.second);
        {
            std::lock_guard lk(job.first->mtx);
            job.first->pending = std::move(result);
        }
        job.first->loading = false;
        --inFlight_;
    }
}
