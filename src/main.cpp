// PhotoViewer — a personal image inspection tool for image processing work.
// raylib (OpenGL) + Dear ImGui (docking) + OpenCV decoding.

#include "image_doc.hpp"
#include "mac_platform.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <raylib.h>
#include <rlImGui.h>
#include <rlgl.h>

#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <future>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------

struct View {
    double cx = 0, cy = 0;  // image coordinate at canvas centre
    float scale = 1;        // screen points per image pixel
    bool fitted = false;
};

// Dense optical flow from the previous image in the list to the current one.
struct FlowResult {
    cv::Mat flow;      // CV_32FC2, per-pixel (dx, dy) at `scale` × the current image's resolution
    cv::Mat mag, ang;  // CV_32F magnitude in image pixels, direction in degrees (0 = +x, 90 = +y / down)
    float scale = 1;   // flow pixels per image pixel
    float maxMag = 0;  // in image pixels
    float p99Mag = 0;  // 99th percentile magnitude, the automatic colour range
};

enum FlowMode { FlowDirection, FlowMagnitude };

struct Flow {
    const LoadedData* from = nullptr;  // inputs of the current/pending result (change on reload)
    const LoadedData* to = nullptr;
    std::future<FlowResult> pending;
    FlowResult res;
    const LoadedData* resFrom = nullptr;  // inputs `res` was computed from
    const LoadedData* resTo = nullptr;
    Texture2D tex{};
    bool hasTex = false;
    int texMode = -1;  // visualisation the texture was built with
    float texRange = 0;
};

struct App {
    std::vector<std::shared_ptr<ImageDoc>> docs;
    int current = -1;
    std::unique_ptr<Loader> loader;
    DisplaySettings disp;
    View view;
    ImVec2 canvasSize{1, 1};

    bool showLoupe = true, showGrid = true, showValues = true;
    bool scrollPans = true;  // trackpad: scroll pans, pinch / Cmd+scroll zooms
    float pinch = 0;         // magnification accumulated this frame (macOS)
    bool keepView = true, autoReload = true, histLog = false, showDemo = false;
    int loupePixels = 15;
    float loupeSize = 220;

    bool hoverValid = false;
    int hoverX = 0, hoverY = 0;
    bool pinValid = false;
    int pinX = 0, pinY = 0;

    bool roiDragging = false;
    ImGuiMouseButton roiButton = ImGuiMouseButton_Right;
    double roiX0 = 0, roiY0 = 0;

    bool blend = false;      // cross-fade the two open images (only offered with exactly two)
    float blendT = 0.5f;     // 0 = first image, 1 = second
    bool showFlow = false;   // optical flow from the previous image to the current one
    bool flowArrows = true;
    float flowAlpha = 0.6f;
    int flowMode = FlowDirection;
    bool flowAutoRange = true;  // colour range = 99th percentile magnitude
    float flowRange = 10;       // manual colour range, image pixels
    float arrowScale = 1;       // arrow length multiplier
    float arrowSpacing = 28;    // screen points between arrows
    Flow flow;

    Texture2D checker{};
    double lastWatch = 0;
    std::string lastTitle;
    bool resetLayout = false, quit = false;
};

ImageDoc* currentDoc(App& a)
{
    return a.current >= 0 && a.current < int(a.docs.size()) ? a.docs[a.current].get() : nullptr;
}

float dpiScale()
{
    return std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x);
}

ImTextureID texId(const Texture2D& t) { return ImTextureID(uintptr_t(t.id)); }

// ----------------------------------------------------------------------------
// Small helpers
// ----------------------------------------------------------------------------

bool isIntegerDepth(int d) { return d != CV_32F && d != CV_64F; }

std::string fmtVal(int depth, double v)
{
    char b[48];
    if (isIntegerDepth(depth)) std::snprintf(b, sizeof b, "%.0f", v);
    else std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

std::string fmtBytes(double n)
{
    const char* u[] = {"B", "KB", "MB", "GB"};
    int i = 0;
    while (n >= 1024 && i < 3) { n /= 1024; ++i; }
    char b[32];
    std::snprintf(b, sizeof b, i ? "%.2f %s" : "%.0f %s", n, u[i]);
    return b;
}

std::string fmtTime(fs::file_time_type t)
{
#ifdef _MSC_VER
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
#else
    auto sys = std::chrono::file_clock::to_sys(t);
#endif
    std::time_t tt = std::chrono::system_clock::to_time_t(std::chrono::time_point_cast<std::chrono::system_clock::duration>(sys));
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char b[64];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

std::string shellQuote(const std::string& s)
{
    std::string r = "'";
    for (char c : s) r += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return r + "'";
}

#if defined(__APPLE__)
constexpr const char* kRevealLabel = "Reveal in Finder";
#elif defined(_WIN32)
constexpr const char* kRevealLabel = "Show in Explorer";
#else
constexpr const char* kRevealLabel = "Open containing folder";
#endif

void revealInFileManager(const std::string& path)
{
#if defined(__APPLE__)
    std::system(("open -R " + shellQuote(path)).c_str());
#elif defined(_WIN32)
    std::system(("explorer /select,\"" + path + "\"").c_str());
#else
    std::system(("xdg-open " + shellQuote(fs::path(path).parent_path().string()) + " &").c_str());
#endif
}

ImU32 channelColor(int channels, int i, int alpha = 255)
{
    if (channels >= 3) {
        switch (i) {
        case 0: return IM_COL32(255, 90, 90, alpha);
        case 1: return IM_COL32(90, 230, 90, alpha);
        case 2: return IM_COL32(90, 140, 255, alpha);
        default: return IM_COL32(190, 190, 190, alpha);
        }
    }
    return i == 0 ? IM_COL32(230, 230, 230, alpha) : IM_COL32(150, 150, 150, alpha);
}

// Value mapped to [0,1] the same way the display texture maps it.
void displayRange(const LoadedData& d, const DisplaySettings& s, double& lo, double& hi)
{
    nominalRange(d.mat.depth(), lo, hi);
    if (s.normalize && !d.stats.empty()) {
        const int colorCh = d.mat.channels() >= 3 ? 3 : 1;
        lo = d.stats[0].min;
        hi = d.stats[0].max;
        for (int i = 1; i < colorCh; ++i) { lo = std::min(lo, d.stats[i].min); hi = std::max(hi, d.stats[i].max); }
    }
    if (!(hi > lo)) hi = lo + 1;
}

ImVec4 displayColor(const LoadedData& d, const DisplaySettings& s, const std::vector<double>& v)
{
    double lo, hi;
    displayRange(d, s, lo, hi);
    auto n = [&](double x) { return float(std::clamp((x - lo) / (hi - lo), 0.0, 1.0)); };
    if (v.size() >= 3) return {n(v[0]), n(v[1]), n(v[2]), 1};
    if (!v.empty()) return {n(v[0]), n(v[0]), n(v[0]), 1};
    return {0, 0, 0, 1};
}

void rgbToHsv(float r, float g, float b, float& h, float& s, float& v)
{
    float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0) { h = 0; return; }
    if (mx == r) h = 60 * std::fmod((g - b) / d, 6.0f);
    else if (mx == g) h = 60 * ((b - r) / d + 2);
    else h = 60 * ((r - g) / d + 4);
    if (h < 0) h += 360;
}

// ----------------------------------------------------------------------------
// Document management
// ----------------------------------------------------------------------------

bool looksLikeImage(const fs::path& p)
{
    static const char* exts[] = {".jpg", ".jpeg", ".png", ".tif", ".tiff", ".bmp", ".webp", ".exr", ".hdr", ".pic",
                                 ".pgm", ".ppm", ".pbm", ".pnm", ".pfm", ".jp2", ".j2k", ".jxl", ".avif", ".dib",
                                 ".sr", ".ras", ".gif", ".dng"};
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return std::find(std::begin(exts), std::end(exts), e) != std::end(exts);
}

void reload(App& a, ImageDoc& d)
{
    for (auto& sp : a.docs)
        if (sp.get() == &d) a.loader->enqueue(sp, a.disp);
}

void addPaths(App& a, const std::vector<std::string>& in)
{
    std::vector<std::string> files;
    for (const auto& p : in) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            std::vector<std::string> dir;
            for (auto& e : fs::directory_iterator(p, ec))
                if (e.is_regular_file(ec) && looksLikeImage(e.path())) dir.push_back(e.path().string());
            std::sort(dir.begin(), dir.end());
            files.insert(files.end(), dir.begin(), dir.end());
        } else {
            files.push_back(p);
        }
    }
    int firstNew = -1;
    for (const auto& f : files) {
        std::error_code ec;
        std::string canon = fs::weakly_canonical(f, ec).string();
        if (ec) canon = f;
        auto it = std::find_if(a.docs.begin(), a.docs.end(), [&](auto& d) { return d->path == canon; });
        if (it != a.docs.end()) {  // dropping an open file again reloads it
            reload(a, **it);
            if (firstNew < 0) firstNew = int(it - a.docs.begin());
            continue;
        }
        auto d = std::make_shared<ImageDoc>();
        d->path = canon;
        d->name = fs::path(canon).filename().string();
        a.docs.push_back(d);
        a.loader->enqueue(d, a.disp);
        if (firstNew < 0) firstNew = int(a.docs.size()) - 1;
    }
    if (firstNew >= 0) {
        a.current = firstNew;
        if (!a.keepView) a.view.fitted = false;
    }
}

void uploadTexture(ImageDoc& d)
{
    if (d.hasTex) UnloadTexture(d.tex);
    d.hasTex = false;
    const cv::Mat& m = d.data->rgba;
    if (m.empty()) return;
    Image img{};
    img.data = m.data;
    img.width = m.cols;
    img.height = m.rows;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    d.tex = LoadTextureFromImage(img);
    GenTextureMipmaps(&d.tex);
    // Smooth when zoomed out, crisp pixels when zoomed in (and in the loupe).
    rlTextureParameters(d.tex.id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_MIP_LINEAR);
    rlTextureParameters(d.tex.id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_NEAREST);
    rlTextureParameters(d.tex.id, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
    rlTextureParameters(d.tex.id, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
    d.hasTex = true;
    d.data->rgba.release();  // no longer needed on the CPU side
}

void updateRoi(ImageDoc& d)
{
    if (!d.data || !d.data->ok) { d.hasRoi = false; return; }
    d.roi &= cv::Rect(0, 0, d.data->mat.cols, d.data->mat.rows);
    if (d.roi.empty()) { d.hasRoi = false; return; }
    const cv::Mat sub = d.data->mat(d.roi);
    d.roiStats = computeStats(sub);
    d.roiHist = computeHistogram(sub, d.data->hist.lo, d.data->hist.hi);
    d.hasRoi = true;
}

void pollDocs(App& a)
{
    for (auto& sp : a.docs) {
        ImageDoc& d = *sp;
        std::unique_ptr<LoadedData> p;
        {
            std::lock_guard lk(d.mtx);
            p = std::move(d.pending);
        }
        if (!p) continue;
        d.watchedMtime = p->mtime;
        // A failed *re*load (e.g. file caught mid-write) keeps the last good image.
        if (!p->ok && d.data && d.data->ok) continue;
        d.data = std::move(p);
        if (d.data->ok) {
            if (d.data->rgbaSettings != a.disp) {
                d.data->rgba = makeDisplayRGBA(d.data->mat, d.data->stats, a.disp);
                d.data->rgbaSettings = a.disp;
            }
            uploadTexture(d);
            if (d.hasRoi) updateRoi(d);
        }
    }
}

void ensureDisplaySettings(App& a, ImageDoc& d)
{
    if (!d.data || !d.data->ok || d.data->rgbaSettings == a.disp) return;
    d.data->rgba = makeDisplayRGBA(d.data->mat, d.data->stats, a.disp);
    d.data->rgbaSettings = a.disp;
    uploadTexture(d);
}

// ----------------------------------------------------------------------------
// Blend / optical flow
// ----------------------------------------------------------------------------

bool canBlend(const App& a) { return a.docs.size() == 2; }

bool docReady(const ImageDoc& d) { return d.data && d.data->ok && d.hasTex; }

// Inputs for flow: previous image in the list -> current one, or false if unavailable.
bool flowInputs(App& a, const LoadedData*& from, const LoadedData*& to)
{
    if (!a.showFlow || a.current < 1 || a.current >= int(a.docs.size())) return false;
    const ImageDoc& p = *a.docs[a.current - 1];
    const ImageDoc& c = *a.docs[a.current];
    if (!p.data || !p.data->ok || !c.data || !c.data->ok) return false;
    from = p.data.get();
    to = c.data.get();
    return true;
}

// 8-bit grayscale at w×h, mapped by the depth's nominal range so brightness is comparable between frames.
cv::Mat flowGray(const cv::Mat& m, int w, int h)
{
    cv::Mat f, g;
    m.convertTo(f, CV_32F);
    if (f.channels() == 3) cv::cvtColor(f, g, cv::COLOR_BGR2GRAY);
    else if (f.channels() == 4) cv::cvtColor(f, g, cv::COLOR_BGRA2GRAY);
    else if (f.channels() == 2) cv::extractChannel(f, g, 0);
    else g = f;
    double lo, hi;
    nominalRange(m.depth(), lo, hi);
    cv::Mat g8;
    g.convertTo(g8, CV_8U, 255.0 / (hi - lo), -lo * 255.0 / (hi - lo));
    if (g8.cols != w || g8.rows != h) cv::resize(g8, g8, cv::Size(w, h), 0, 0, cv::INTER_AREA);
    return g8;
}

FlowResult computeFlow(cv::Mat prev, cv::Mat cur)
{
    constexpr int kMaxSide = 1024;  // Farneback is slow; flow is smooth enough to compute downscaled
    FlowResult r;
    r.scale = std::min(1.0f, float(kMaxSide) / std::max(cur.cols, cur.rows));
    const int w = std::max(1, int(std::lround(cur.cols * r.scale)));
    const int h = std::max(1, int(std::lround(cur.rows * r.scale)));
    cv::calcOpticalFlowFarneback(flowGray(prev, w, h), flowGray(cur, w, h), r.flow, 0.5, 5, 15, 3, 5, 1.2, 0);

    cv::Mat xy[2];
    cv::split(r.flow, xy);
    cv::cartToPolar(xy[0], xy[1], r.mag, r.ang, true);
    r.mag *= 1.0 / r.scale;
    // The 99th percentile is the default colour range, so a few outliers don't wash everything out.
    std::vector<float> m(r.mag.begin<float>(), r.mag.end<float>());
    auto k = m.begin() + std::ptrdiff_t(m.size() * 0.99);
    std::nth_element(m.begin(), k, m.end());
    r.p99Mag = std::max(*k, 1e-3f);
    double mx;
    cv::minMaxLoc(r.mag, nullptr, &mx);
    r.maxMag = float(mx);
    return r;
}

float flowColorRange(const App& a, const FlowResult& r) { return a.flowAutoRange ? r.p99Mag : std::max(a.flowRange, 1e-3f); }

// Flow visualisation as RGBA: direction (hue = angle, brightness = magnitude) or magnitude (Turbo colormap).
cv::Mat flowRGBA(const FlowResult& r, int mode, float range)
{
    cv::Mat rgb, rgba;
    if (mode == FlowMagnitude) {
        cv::Mat m8, bgr;
        r.mag.convertTo(m8, CV_8U, 255.0 / range);
        cv::applyColorMap(m8, bgr, cv::COLORMAP_TURBO);
        cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
        return rgba;
    }
    cv::Mat hsv[3], hsvImg;
    r.ang.convertTo(hsv[0], CV_8U, 0.5);  // OpenCV 8-bit hue is 0..180
    hsv[1] = cv::Mat(r.mag.size(), CV_8U, cv::Scalar(255));
    r.mag.convertTo(hsv[2], CV_8U, 255.0 / range);
    cv::merge(hsv, 3, hsvImg);
    cv::cvtColor(hsvImg, rgb, cv::COLOR_HSV2RGB);
    cv::cvtColor(rgb, rgba, cv::COLOR_RGB2RGBA);
    return rgba;
}

// Turbo colormap sampled for the legend, index 0..255.
const std::vector<ImU32>& turboLut()
{
    static std::vector<ImU32> lut;
    if (lut.empty()) {
        cv::Mat ramp(1, 256, CV_8U), bgr;
        for (int i = 0; i < 256; ++i) ramp.at<uchar>(i) = uchar(i);
        cv::applyColorMap(ramp, bgr, cv::COLORMAP_TURBO);
        for (int i = 0; i < 256; ++i) {
            const cv::Vec3b c = bgr.at<cv::Vec3b>(i);
            lut.push_back(IM_COL32(c[2], c[1], c[0], 255));
        }
    }
    return lut;
}

// Collects finished flow results and starts a new computation when the inputs changed.
void updateFlow(App& a)
{
    Flow& f = a.flow;
    if (f.pending.valid() && f.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        f.res = f.pending.get();
        f.resFrom = f.from;
        f.resTo = f.to;
        f.texMode = -1;
    }
    // (Re)build the texture when the result or the visualisation settings changed.
    if (f.resTo && !f.res.mag.empty()) {
        const float range = flowColorRange(a, f.res);
        if (f.texMode != a.flowMode || f.texRange != range) {
            const cv::Mat rgba = flowRGBA(f.res, a.flowMode, range);
            if (f.hasTex && f.tex.width == rgba.cols && f.tex.height == rgba.rows) {
                UpdateTexture(f.tex, rgba.data);
            } else {
                if (f.hasTex) UnloadTexture(f.tex);
                Image img{rgba.data, rgba.cols, rgba.rows, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                f.tex = LoadTextureFromImage(img);
                rlTextureParameters(f.tex.id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
                rlTextureParameters(f.tex.id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
                f.hasTex = true;
            }
            f.texMode = a.flowMode;
            f.texRange = range;
        }
    }
    const LoadedData *from, *to;
    if (f.pending.valid() || !flowInputs(a, from, to) || (from == f.from && to == f.to)) return;
    f.from = from;
    f.to = to;
    f.pending = std::async(std::launch::async, computeFlow, from->mat, to->mat);  // Mats share (ref-counted) pixels
}

// The flow result for the current pair, if it has been computed.
const FlowResult* activeFlow(App& a)
{
    const LoadedData *from, *to;
    if (!flowInputs(a, from, to) || !a.flow.hasTex || a.flow.resFrom != from || a.flow.resTo != to) return nullptr;
    return &a.flow.res;
}

// Flow at image pixel (x, y) in image pixels.
bool flowAt(const FlowResult& r, double x, double y, float& dx, float& dy)
{
    const int fx = int(x * r.scale), fy = int(y * r.scale);
    if (fx < 0 || fy < 0 || fx >= r.flow.cols || fy >= r.flow.rows) return false;
    const cv::Vec2f v = r.flow.at<cv::Vec2f>(fy, fx);
    dx = v[0] / r.scale;
    dy = v[1] / r.scale;
    return true;
}

void watchFiles(App& a)
{
    if (!a.autoReload || GetTime() - a.lastWatch < 1.0) return;
    a.lastWatch = GetTime();
    for (auto& sp : a.docs) {
        if (!sp->data || sp->loading) continue;
        std::error_code ec;
        auto t = fs::last_write_time(sp->path, ec);
        if (!ec && t != sp->watchedMtime) {
            sp->watchedMtime = t;
            a.loader->enqueue(sp, a.disp);
        }
    }
}

void closeDoc(App& a, int i)
{
    if (i < 0 || i >= int(a.docs.size())) return;
    if (a.docs[i]->hasTex) UnloadTexture(a.docs[i]->tex);
    a.docs[i]->hasTex = false;
    a.docs.erase(a.docs.begin() + i);
    if (a.current >= int(a.docs.size())) a.current = int(a.docs.size()) - 1;
    if (a.docs.empty()) a.view.fitted = false;
}

void select(App& a, int i)
{
    if (a.docs.empty()) return;
    a.current = (i % int(a.docs.size()) + int(a.docs.size())) % int(a.docs.size());
    if (!a.keepView) a.view.fitted = false;
}

// ----------------------------------------------------------------------------
// View helpers
// ----------------------------------------------------------------------------

void fitView(App& a, int w, int h)
{
    a.view.scale = std::min(a.canvasSize.x / w, a.canvasSize.y / h) * 0.97f;
    a.view.cx = w * 0.5;
    a.view.cy = h * 0.5;
    a.view.fitted = true;
}

void zoomAt(App& a, ImVec2 canvasCenter, ImVec2 screen, float newScale)
{
    newScale = std::clamp(newScale, 0.002f, 1024.0f);
    const double ix = a.view.cx + (screen.x - canvasCenter.x) / a.view.scale;
    const double iy = a.view.cy + (screen.y - canvasCenter.y) / a.view.scale;
    a.view.scale = newScale;
    a.view.cx = ix - (screen.x - canvasCenter.x) / newScale;
    a.view.cy = iy - (screen.y - canvasCenter.y) / newScale;
}

// ----------------------------------------------------------------------------
// Widgets
// ----------------------------------------------------------------------------

void pixelInfo(App& a, const ImageDoc& d, int x, int y, const char* id)
{
    const LoadedData& ld = *d.data;
    const auto v = pixelValues(ld.mat, x, y);
    if (v.empty()) { ImGui::TextDisabled("(%d, %d) outside image", x, y); return; }
    const auto labels = channelLabels(ld.mat.channels());
    const int depth = ld.mat.depth();

    ImVec4 c = displayColor(ld, a.disp, v);
    ImGui::PushID(id);
    ImGui::ColorButton("##swatch", c, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(44, 44));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("x %d   y %d", x, y);
    ImGui::Text("#%02X%02X%02X", int(c.x * 255 + 0.5f), int(c.y * 255 + 0.5f), int(c.z * 255 + 0.5f));
    if (v.size() >= 3) {
        float h, s, val;
        rgbToHsv(c.x, c.y, c.z, h, s, val);
        ImGui::TextDisabled("HSV %.0f° %.3f %.3f", h, s, val);
    }
    ImGui::EndGroup();

    double nlo, nhi;
    nominalRange(depth, nlo, nhi);
    if (ImGui::BeginTable("##px", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Ch");
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Norm");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < v.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(channelColor(ld.mat.channels(), int(i))), "%s", labels[i]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmtVal(depth, v[i]).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", (v[i] - nlo) / (nhi - nlo));
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void drawHistogram(const Histogram& h, int depth, bool logScale, const std::vector<double>* marks, float height)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = std::max(50.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 p1(p0.x + w, p0.y + height);
    ImGui::InvisibleButton("##hist", ImVec2(w, height));
    dl->AddRectFilled(p0, p1, IM_COL32(20, 20, 22, 255), 3);
    if (h.channels == 0) return;

    auto val = [&](float b) { return logScale ? std::log1p(b) : b; };
    // Normalise by interior bins so clipped 0/max spikes don't squash the plot.
    float mx = 0, mxAll = 0;
    for (int c = 0; c < h.channels; ++c)
        for (int i = 0; i < kHistBins; ++i) {
            float b = val(h.bins[c][i]);
            mxAll = std::max(mxAll, b);
            if (i > 0 && i < kHistBins - 1) mx = std::max(mx, b);
        }
    if (mx <= 0) mx = mxAll;
    if (mx <= 0) mx = 1;

    const float bw = w / kHistBins;
    for (int c = 0; c < h.channels; ++c) {
        ImVec2 pts[kHistBins];
        for (int i = 0; i < kHistBins; ++i) {
            float t = std::min(1.0f, val(h.bins[c][i]) / mx);
            float y = p1.y - t * (height - 2);
            float x = p0.x + i * bw;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, p1.y), channelColor(h.channels, c, 55));
            pts[i] = ImVec2(x + bw * 0.5f, y);
        }
        dl->AddPolyline(pts, kHistBins, channelColor(h.channels, c, 230), 0, 1.0f);
    }
    if (marks)
        for (size_t c = 0; c < marks->size() && int(c) < h.channels; ++c) {
            float x = p0.x + float(((*marks)[c] - h.lo) / (h.hi - h.lo)) * w;
            if (x >= p0.x && x <= p1.x) dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), channelColor(h.channels, int(c), 200), 1.5f);
        }
    dl->AddRect(p0, p1, IM_COL32(70, 70, 75, 255), 3);

    if (ImGui::IsItemHovered()) {
        int b = std::clamp(int((ImGui::GetIO().MousePos.x - p0.x) / bw), 0, kHistBins - 1);
        const double step = (h.hi - h.lo) / kHistBins;
        dl->AddLine(ImVec2(p0.x + (b + 0.5f) * bw, p0.y), ImVec2(p0.x + (b + 0.5f) * bw, p1.y), IM_COL32(255, 255, 255, 60));
        ImGui::BeginTooltip();
        if (isIntegerDepth(depth) && step >= 1) ImGui::Text("Bin %d: [%.0f, %.0f]", b, h.lo + b * step, h.lo + (b + 1) * step - 1);
        else ImGui::Text("Bin %d: [%.5g, %.5g)", b, h.lo + b * step, h.lo + (b + 1) * step);
        const auto labels = channelLabels(h.channels);
        for (int c = 0; c < h.channels; ++c)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(channelColor(h.channels, c)), "%s  %.0f", labels[c], h.bins[c][b]);
        ImGui::EndTooltip();
    }
    const bool intRange = isIntegerDepth(depth) && (h.hi - h.lo) >= kHistBins;
    ImGui::TextDisabled("%.5g", h.lo);
    char hiTxt[32];
    std::snprintf(hiTxt, sizeof hiTxt, "%.5g", intRange ? h.hi - 1 : h.hi);
    ImGui::SameLine(ImGui::GetCursorPosX() + w - ImGui::CalcTextSize(hiTxt).x - ImGui::GetStyle().ItemSpacing.x);
    ImGui::TextDisabled("%s", hiTxt);
}

// ----------------------------------------------------------------------------
// Windows
// ----------------------------------------------------------------------------

void drawLoupe(App& a, const ImageDoc& d, int hx, int hy)
{
    const int W = d.data->mat.cols, H = d.data->mat.rows;
    const int n = a.loupePixels | 1;
    const float size = a.loupeSize;
    const float cell = size / n;
    const int x0 = hx - n / 2, y0 = hy - n / 2;

    ImGui::BeginTooltip();
    const ImVec2 lp = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(lp, ImVec2(lp.x + size, lp.y + size), IM_COL32(12, 12, 14, 255));

    const int cx0 = std::max(x0, 0), cy0 = std::max(y0, 0);
    const int cx1 = std::min(x0 + n, W), cy1 = std::min(y0 + n, H);
    if (cx1 > cx0 && cy1 > cy0) {
        ImVec2 q0(lp.x + (cx0 - x0) * cell, lp.y + (cy0 - y0) * cell);
        ImVec2 q1(lp.x + (cx1 - x0) * cell, lp.y + (cy1 - y0) * cell);
        const int nch = d.data->mat.channels();
        if (nch == 2 || nch == 4) dl->AddImage(texId(a.checker), q0, q1, ImVec2(0, 0), ImVec2((q1.x - q0.x) / 16, (q1.y - q0.y) / 16));
        dl->AddImage(texId(d.tex), q0, q1, ImVec2(float(cx0) / W, float(cy0) / H), ImVec2(float(cx1) / W, float(cy1) / H));
    }
    if (cell >= 6)
        for (int i = 1; i < n; ++i) {
            dl->AddLine(ImVec2(lp.x + i * cell, lp.y), ImVec2(lp.x + i * cell, lp.y + size), IM_COL32(0, 0, 0, 60));
            dl->AddLine(ImVec2(lp.x, lp.y + i * cell), ImVec2(lp.x + size, lp.y + i * cell), IM_COL32(0, 0, 0, 60));
        }
    const float half = float(n / 2) * cell;  // n is odd: centre cell index
    const ImVec2 c0(lp.x + half, lp.y + half);
    const ImVec2 c1(c0.x + cell, c0.y + cell);
    dl->AddRect(ImVec2(c0.x - 1, c0.y - 1), ImVec2(c1.x + 1, c1.y + 1), IM_COL32(0, 0, 0, 255), 0.0f, 2);
    dl->AddRect(c0, c1, IM_COL32(255, 255, 255, 255), 0.0f, 1);
    dl->AddRect(lp, ImVec2(lp.x + size, lp.y + size), IM_COL32(90, 90, 95, 255));

    ImGui::PushItemWidth(size);
    pixelInfo(a, d, hx, hy, "loupe");
    ImGui::PopItemWidth();
    ImGui::TextDisabled("%dx%d px · release Shift to hide", n, n);
    ImGui::EndTooltip();
}

// Colour key for the flow overlay: a direction wheel or a magnitude colour bar, labelled in image pixels.
void drawFlowLegend(const App& a, const FlowResult& r)
{
    const float range = flowColorRange(a, r);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text), dimCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float fs = ImGui::GetFontSize();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;

    if (a.flowMode == FlowMagnitude) {
        const float barH = fs * 0.9f;
        const auto& lut = turboLut();
        const int seg = 64;
        for (int i = 0; i < seg; ++i) {
            const float x0 = p.x + w * i / seg, x1 = p.x + w * (i + 1) / seg;
            const ImU32 c0 = lut[i * 255 / seg], c1 = lut[(i + 1) * 255 / seg];
            dl->AddRectFilledMultiColor(ImVec2(x0, p.y), ImVec2(x1, p.y + barH), c0, c1, c1, c0);
        }
        dl->AddRect(p, ImVec2(p.x + w, p.y + barH), IM_COL32(0, 0, 0, 160));
        // Ticks at 0, 1/4, ..., 1 of the range.
        for (int t = 0; t <= 4; ++t) {
            char b[32];
            std::snprintf(b, sizeof b, t == 4 ? "%.3g px" : "%.3g", range * t / 4);
            const float x = p.x + w * t / 4;
            const float tw = ImGui::CalcTextSize(b).x;
            const float tx = std::clamp(x - tw / 2, p.x, p.x + w - tw);
            dl->AddLine(ImVec2(x, p.y + barH), ImVec2(x, p.y + barH + 3), dimCol);
            dl->AddText(ImVec2(tx, p.y + barH + 3), textCol, b);
        }
        ImGui::Dummy(ImVec2(w, barH + 3 + fs));
        ImGui::TextDisabled("|flow|, values above the range are clipped");
        return;
    }

    // Direction wheel: hue = direction the pixel moved (screen orientation, y down), brightness = magnitude.
    const float R = fs * 2.2f;
    const ImVec2 c(p.x + R + 2, p.y + R + 2);
    const int rings = 6, sectors = 48;
    for (int ri = 0; ri < rings; ++ri)
        for (int si = 0; si < sectors; ++si) {
            const float r0 = R * ri / rings, r1 = R * (ri + 1) / rings;
            const float a0 = 2 * IM_PI * si / sectors, a1 = 2 * IM_PI * (si + 1) / sectors;
            float cr, cg, cb;
            ImGui::ColorConvertHSVtoRGB((si + 0.5f) / sectors, 1, (ri + 0.5f) / rings, cr, cg, cb);
            const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(cr, cg, cb, 1));
            const ImVec2 q[4] = {{c.x + r0 * std::cos(a0), c.y + r0 * std::sin(a0)}, {c.x + r1 * std::cos(a0), c.y + r1 * std::sin(a0)},
                                 {c.x + r1 * std::cos(a1), c.y + r1 * std::sin(a1)}, {c.x + r0 * std::cos(a1), c.y + r0 * std::sin(a1)}};
            dl->AddConvexPolyFilled(q, 4, col);
        }
    dl->AddCircle(c, R, IM_COL32(0, 0, 0, 160), 48);
    ImGui::Dummy(ImVec2(2 * R + 4, 2 * R + 4));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted("hue = direction of motion");
    ImGui::TextDisabled("right red · down yellow-green");
    ImGui::TextDisabled("left cyan · up blue-violet");
    ImGui::Text("brightness = |flow|, full at %.3g px", range);
    ImGui::EndGroup();
}

// Floating controls at the bottom of the canvas for blend / flow.
void drawComparePanel(App& a, ImVec2 anchor, float canvasW)
{
    const bool blend = a.blend && canBlend(a);
    const bool flow = a.showFlow && a.docs.size() >= 2;
    if (!blend && !flow) return;
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(0.5f, 1));
    ImGui::SetNextWindowSize(ImVec2(std::min(420.0f, canvasW - 24), 0));
    ImGui::SetNextWindowBgAlpha(0.8f);
    const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin("##compare", nullptr, fl)) { ImGui::End(); return; }
    const float w = ImGui::GetContentRegionAvail().x;
    if (blend) {
        const std::string& na = a.docs[0]->name;
        const std::string& nb = a.docs[1]->name;
        ImGui::TextUnformatted(na.c_str());
        const float tw = ImGui::CalcTextSize(nb.c_str()).x;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + w - tw));
        ImGui::TextUnformatted(nb.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##blend", &a.blendT, 0, 1, "blend %.2f");
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) a.blendT = 0.5f;
    }
    if (flow) {
        if (blend) ImGui::Separator();
        if (a.current < 1) ImGui::TextDisabled("Optical flow: first image has no previous frame");
        else {
            ImGui::Text("Flow  %s -> %s", a.docs[a.current - 1]->name.c_str(), a.docs[a.current]->name.c_str());
            if (const FlowResult* r = activeFlow(a)) ImGui::TextDisabled("max %.1f px · p99 %.1f px · computed at %.0f%%", r->maxMag, r->p99Mag, r->scale * 100);
            else ImGui::TextDisabled("computing…");
        }
        ImGui::RadioButton("Direction", &a.flowMode, FlowDirection);
        ImGui::SameLine();
        ImGui::RadioButton("Magnitude", &a.flowMode, FlowMagnitude);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##flowalpha", &a.flowAlpha, 0, 1, "opacity %.2f");

        ImGui::Checkbox("Auto range", &a.flowAutoRange);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Colour range = 99th percentile magnitude");
        ImGui::SameLine();
        ImGui::BeginDisabled(a.flowAutoRange);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##flowrange", &a.flowRange, 0.1f, 500, "range %.1f px", ImGuiSliderFlags_Logarithmic);
        ImGui::EndDisabled();
        if (const FlowResult* r = activeFlow(a)) drawFlowLegend(a, *r);

        ImGui::Checkbox("Arrows", &a.flowArrows);
        ImGui::BeginDisabled(!a.flowArrows);
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
        ImGui::SetNextItemWidth(half);
        ImGui::SliderFloat("##arrowscale", &a.arrowScale, 0.1f, 50, "length ×%.2f", ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) a.arrowScale = 1;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(half);
        ImGui::SliderFloat("##arrowspacing", &a.arrowSpacing, 8, 160, "spacing %.0f pt", ImGuiSliderFlags_Logarithmic);
        ImGui::EndDisabled();
    }
    ImGui::End();
}

void drawViewer(App& a)
{
    ImGuiWindowClass wc;
    wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&wc);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewer", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 csz(std::max(avail.x, 32.0f), std::max(avail.y, 32.0f));
    a.canvasSize = csz;
    const ImVec2 p1(p0.x + csz.x, p0.y + csz.y);
    const ImVec2 cc(p0.x + csz.x * 0.5f, p0.y + csz.y * 0.5f);

    ImGui::InvisibleButton("##canvas", csz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(p0, p1, IM_COL32(28, 28, 31, 255));

    a.hoverValid = false;
    ImageDoc* d = currentDoc(a);
    const bool ready = d && d->data && d->data->ok && d->hasTex;

    if (!ready) {
        a.view.fitted = a.view.fitted && d;
        const char* msg = !d ? "Drop images or folders here  ·  Help menu lists controls"
                        : (d->data && !d->data->ok) ? d->data->error.c_str()
                        : "Loading…";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(cc.x - ts.x / 2, cc.y - ts.y / 2), IM_COL32(150, 150, 155, 255), msg);
    } else {
        ensureDisplaySettings(a, *d);
        const cv::Mat& mat = d->data->mat;
        const int W = mat.cols, H = mat.rows;
        if (!a.view.fitted) fitView(a, W, H);

        auto toScreen = [&](double ix, double iy) {
            return ImVec2(float(cc.x + (ix - a.view.cx) * a.view.scale), float(cc.y + (iy - a.view.cy) * a.view.scale));
        };
        auto toImage = [&](ImVec2 s, double& ix, double& iy) {
            ix = a.view.cx + (s.x - cc.x) / a.view.scale;
            iy = a.view.cy + (s.y - cc.y) / a.view.scale;
        };

        // ---- input ----
        if (hovered && (io.MouseWheel != 0 || io.MouseWheelH != 0)) {
            const bool zoomMod = io.KeyCtrl || io.KeySuper;
            if (a.scrollPans != zoomMod) {
                // Two-finger trackpad scroll: GLFW reports ~10 points of finger travel per unit.
                a.view.cx -= io.MouseWheelH * 10.0 / a.view.scale;
                a.view.cy -= io.MouseWheel * 10.0 / a.view.scale;
            } else if (io.MouseWheel != 0) {
                zoomAt(a, cc, io.MousePos, a.view.scale * std::pow(1.2f, io.MouseWheel));
            }
        }
        if (hovered && a.pinch != 0) zoomAt(a, cc, io.MousePos, a.view.scale * (1.0f + a.pinch));
        if (active && !a.roiDragging && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1)) && !io.KeyAlt) {
            a.view.cx -= io.MouseDelta.x / a.view.scale;
            a.view.cy -= io.MouseDelta.y / a.view.scale;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) fitView(a, W, H);

        double mx, my;
        toImage(io.MousePos, mx, my);
        if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) {
            a.roiDragging = true;
            a.roiButton = ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? ImGuiMouseButton_Right : ImGuiMouseButton_Left;
            a.roiX0 = std::clamp(mx, 0.0, double(W));
            a.roiY0 = std::clamp(my, 0.0, double(H));
        }
        if (a.roiDragging) {
            const double x1 = std::clamp(mx, 0.0, double(W)), y1 = std::clamp(my, 0.0, double(H));
            const int rx0 = int(std::floor(std::min(a.roiX0, x1))), ry0 = int(std::floor(std::min(a.roiY0, y1)));
            const int rx1 = int(std::ceil(std::max(a.roiX0, x1))), ry1 = int(std::ceil(std::max(a.roiY0, y1)));
            if (!ImGui::IsMouseDown(a.roiButton)) {
                a.roiDragging = false;
                if (io.MouseDragMaxDistanceSqr[a.roiButton] < 9) d->hasRoi = false;  // plain click clears the ROI
                else { d->roi = cv::Rect(rx0, ry0, rx1 - rx0, ry1 - ry0); updateRoi(*d); }
            } else {
                d->roi = cv::Rect(rx0, ry0, rx1 - rx0, ry1 - ry0);
            }
        }
        if (hovered && !io.KeyAlt && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 9) {
            a.pinValid = true;
            a.pinX = int(std::floor(mx));
            a.pinY = int(std::floor(my));
        }

        // ---- image ----
        // Each image is drawn at its own pixel size from the origin, so pixel coordinates line up.
        auto drawDoc = [&](const ImageDoc& doc, float alpha) {
            const ImVec2 q0 = toScreen(0, 0), q1 = toScreen(doc.data->mat.cols, doc.data->mat.rows);
            const ImU32 tint = IM_COL32(255, 255, 255, int(alpha * 255 + 0.5f));
            if (alpha >= 1 && (doc.data->mat.channels() == 2 || doc.data->mat.channels() == 4))
                dl->AddImage(texId(a.checker), q0, q1, ImVec2(0, 0), ImVec2((q1.x - q0.x) / 16, (q1.y - q0.y) / 16));
            dl->AddImage(texId(doc.tex), q0, q1, ImVec2(0, 0), ImVec2(1, 1), tint);
        };
        const ImVec2 i0 = toScreen(0, 0), i1 = toScreen(W, H);
        if (a.blend && canBlend(a) && docReady(*a.docs[0]) && docReady(*a.docs[1])) {
            for (auto& sp : a.docs) ensureDisplaySettings(a, *sp);
            drawDoc(*a.docs[0], 1);
            drawDoc(*a.docs[1], a.blendT);
        } else {
            drawDoc(*d, 1);
        }
        const FlowResult* flow = activeFlow(a);
        if (flow && a.flowAlpha > 0) dl->AddImage(texId(a.flow.tex), i0, i1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, int(a.flowAlpha * 255 + 0.5f)));

        // Visible pixel range.
        double vx0, vy0, vx1, vy1;
        toImage(p0, vx0, vy0);
        toImage(p1, vx1, vy1);
        const int gx0 = std::max(0, int(std::floor(vx0))), gy0 = std::max(0, int(std::floor(vy0)));
        const int gx1 = std::min(W, int(std::ceil(vx1))), gy1 = std::min(H, int(std::ceil(vy1)));
        const float s = a.view.scale;

        if (a.showGrid && s >= 12)
            for (int x = gx0; x <= gx1; ++x) dl->AddLine(toScreen(x, gy0), toScreen(x, gy1), IM_COL32(128, 128, 128, 60));
        if (a.showGrid && s >= 12)
            for (int y = gy0; y <= gy1; ++y) dl->AddLine(toScreen(gx0, y), toScreen(gx1, y), IM_COL32(128, 128, 128, 60));

        // Flow vectors on a coarse grid (spacing fixed in screen points).
        if (flow && a.flowArrows) {
            const int step = std::max(1, int(std::ceil(a.arrowSpacing / s)));
            if (double(gx1 - gx0) * (gy1 - gy0) / (double(step) * step) < 20000)
                for (int y = gy0 - gy0 % step + step / 2; y < gy1; y += step)
                    for (int x = gx0 - gx0 % step + step / 2; x < gx1; x += step) {
                        float fx, fy;
                        if (!flowAt(*flow, x + 0.5, y + 0.5, fx, fy)) continue;
                        const ImVec2 o = toScreen(x + 0.5, y + 0.5), e = toScreen(x + 0.5 + fx * a.arrowScale, y + 0.5 + fy * a.arrowScale);
                        const float len = std::hypot(e.x - o.x, e.y - o.y);
                        if (len < 1.5f) { dl->AddRectFilled(ImVec2(o.x - 1, o.y - 1), ImVec2(o.x + 1, o.y + 1), IM_COL32(255, 255, 255, 140)); continue; }
                        const float ux = (e.x - o.x) / len, uy = (e.y - o.y) / len, hl = std::min(6.0f, len * 0.4f);
                        const ImVec2 h1(e.x - hl * (ux - uy * 0.5f), e.y - hl * (uy + ux * 0.5f));
                        const ImVec2 h2(e.x - hl * (ux + uy * 0.5f), e.y - hl * (uy - ux * 0.5f));
                        for (int pass = 0; pass < 2; ++pass) {
                            const ImU32 col = pass ? IM_COL32(255, 255, 255, 230) : IM_COL32(0, 0, 0, 160);
                            const float th = pass ? 1.2f : 3.0f;
                            dl->AddLine(o, e, col, th);
                            dl->AddLine(e, h1, col, th);
                            dl->AddLine(e, h2, col, th);
                        }
                    }
        }

        // Pixel values printed inside cells at high zoom.
        if (a.showValues) {
            const int nch = mat.channels();
            const float lineH = ImGui::GetFontSize();
            const float needW = ImGui::CalcTextSize(isIntegerDepth(mat.depth()) ? "-00000" : "0.00000").x + 6;
            const float needH = lineH * nch + 4;
            if (s >= needW && s >= needH && (gx1 - gx0) * (gy1 - gy0) < 4000) {
                double lo, hi;
                displayRange(*d->data, a.disp, lo, hi);
                for (int y = gy0; y < gy1; ++y)
                    for (int x = gx0; x < gx1; ++x) {
                        const auto v = pixelValues(mat, x, y);
                        double lum = nch >= 3 ? 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2] : v[0];
                        lum = (lum - lo) / (hi - lo);
                        const ImU32 fg = lum > 0.5 ? IM_COL32(0, 0, 0, 220) : IM_COL32(255, 255, 255, 220);
                        const ImVec2 cp = toScreen(x + 0.5, y + 0.5);
                        float ty = cp.y - needH / 2 + 2;
                        for (int c = 0; c < nch; ++c) {
                            const std::string t = fmtVal(mat.depth(), v[c]);
                            const float tw = ImGui::CalcTextSize(t.c_str()).x;
                            dl->AddText(ImVec2(cp.x - tw / 2, ty), fg, t.c_str());
                            ty += lineH;
                        }
                    }
            }
        }

        // Hovered / pinned pixel markers.
        if (hovered && mx >= 0 && my >= 0 && mx < W && my < H) {
            a.hoverValid = true;
            a.hoverX = int(std::floor(mx));
            a.hoverY = int(std::floor(my));
            if (s >= 4) dl->AddRect(toScreen(a.hoverX, a.hoverY), toScreen(a.hoverX + 1, a.hoverY + 1), IM_COL32(255, 255, 255, 200));
        }
        if (a.pinValid && a.pinX >= 0 && a.pinY >= 0 && a.pinX < W && a.pinY < H) {
            ImVec2 q0 = toScreen(a.pinX, a.pinY), q1 = toScreen(a.pinX + 1, a.pinY + 1);
            const ImVec2 c((q0.x + q1.x) / 2, (q0.y + q1.y) / 2);
            const float r = std::max(6.0f, (q1.x - q0.x) / 2 + 3);
            dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), IM_COL32(0, 220, 255, 255), 0.0f, 1.5f);
            dl->AddLine(ImVec2(c.x - r - 5, c.y), ImVec2(c.x - r + 2, c.y), IM_COL32(0, 220, 255, 255), 1.5f);
            dl->AddLine(ImVec2(c.x + r - 2, c.y), ImVec2(c.x + r + 5, c.y), IM_COL32(0, 220, 255, 255), 1.5f);
        }
        if (d->hasRoi || a.roiDragging) {
            ImVec2 r0 = toScreen(d->roi.x, d->roi.y), r1 = toScreen(d->roi.x + d->roi.width, d->roi.y + d->roi.height);
            dl->AddRectFilled(r0, r1, IM_COL32(255, 210, 0, 25));
            dl->AddRect(r0, r1, IM_COL32(255, 210, 0, 255), 0.0f, 1.5f);
            char b[64];
            std::snprintf(b, sizeof b, "%d×%d", d->roi.width, d->roi.height);
            dl->AddText(ImVec2(r0.x + 3, r0.y - ImGui::GetFontSize() - 2), IM_COL32(255, 210, 0, 255), b);
        }
    }
    dl->PopClipRect();

    if (ready && a.showLoupe && io.KeyShift && a.hoverValid && !active && !a.roiDragging) drawLoupe(a, *d, a.hoverX, a.hoverY);

    ImGui::End();
    drawComparePanel(a, ImVec2(cc.x, p1.y - 12), csz.x);
}

std::vector<MetaEntry> metadataRows(const ImageDoc& d)
{
    const LoadedData& ld = *d.data;
    std::vector<MetaEntry> rows;
    rows.push_back({"File", "Name", d.name});
    rows.push_back({"File", "Folder", fs::path(d.path).parent_path().string()});
    rows.push_back({"File", "Format", ld.format});
    rows.push_back({"File", "Size", fmtBytes(double(ld.fileSize))});
    rows.push_back({"File", "Modified", fmtTime(ld.mtime)});
    if (ld.ok) {
        const cv::Mat& m = ld.mat;
        char b[128];
        std::snprintf(b, sizeof b, "%d × %d  (%.2f MP)", m.cols, m.rows, m.cols * double(m.rows) / 1e6);
        rows.push_back({"Image", "Dimensions", b});
        rows.push_back({"Image", "Channels", std::to_string(m.channels())});
        rows.push_back({"Image", "Decoded type", ld.origType});
        const std::string memType = cv::typeToString(m.type());
        if (memType != ld.origType) rows.push_back({"Image", "In-memory type", memType});
        rows.push_back({"Image", "Memory", fmtBytes(double(m.total() * m.elemSize()))});
        if (d.hasTex && (d.tex.width != m.cols || d.tex.height != m.rows))
            rows.push_back({"Image", "Display texture", std::to_string(d.tex.width) + " × " + std::to_string(d.tex.height) + " (downscaled)"});
        std::snprintf(b, sizeof b, "%.1f ms", ld.decodeMs);
        rows.push_back({"Image", "Load time", b});
        if (ld.exif.empty()) rows.push_back({"Image", "EXIF", "none"});
    } else {
        rows.push_back({"File", "Error", ld.error});
    }
    for (const auto& e : ld.exif) rows.push_back(e);
    return rows;
}

std::string metadataText(const ImageDoc& d)
{
    std::string t;
    for (const auto& r : metadataRows(d)) t += r.group + "\t" + r.name + "\t" + r.value + "\n";
    return t;
}

// Narrow-sidebar pixel readout: swatch + coords, then one row per channel.
void pixelCompact(App& a, const ImageDoc& d, int x, int y, const char* id)
{
    const LoadedData& ld = *d.data;
    const auto v = pixelValues(ld.mat, x, y);
    if (v.empty()) return;
    const auto labels = channelLabels(ld.mat.channels());
    double nlo, nhi;
    nominalRange(ld.mat.depth(), nlo, nhi);
    const ImVec4 c = displayColor(ld, a.disp, v);

    ImGui::PushID(id);
    const float fs = ImGui::GetFontSize();
    ImGui::ColorButton("##sw", c, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(fs, fs));
    if (ImGui::IsItemHovered()) {
        float h, sat, val;
        rgbToHsv(c.x, c.y, c.z, h, sat, val);
        ImGui::SetTooltip("#%02X%02X%02X\nHSV %.0f° %.3f %.3f", int(c.x * 255 + 0.5f), int(c.y * 255 + 0.5f), int(c.z * 255 + 0.5f), h, sat, val);
    }
    ImGui::SameLine();
    ImGui::Text("%s %d, %d", id, x, y);
    if (ImGui::BeginTable("##v", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthFixed, fs);
        for (size_t i = 0; i < v.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(channelColor(ld.mat.channels(), int(i))), "%s", labels[i]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(fmtVal(ld.mat.depth(), v[i]).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.3f", (v[i] - nlo) / (nhi - nlo));
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

// Min / max / mean per channel; std-dev in the row tooltip.
void statsCompact(const std::vector<ChannelStats>& st, int channels, int depth, const char* id)
{
    const auto labels = channelLabels(channels);
    if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) return;
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize());
    ImGui::TableSetupColumn("min");
    ImGui::TableSetupColumn("max");
    ImGui::TableSetupColumn("mean");
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < st.size(); ++i) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(channelColor(channels, int(i))), "%s", labels[i]);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(fmtVal(depth, st[i].min).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(fmtVal(depth, st[i].max).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.4g", st[i].mean);
        if (ImGui::TableGetHoveredRow() == int(i) + 1)
            ImGui::SetTooltip("%s  min %s  max %s\nmean %.6g  std %.6g", labels[i], fmtVal(depth, st[i].min).c_str(),
                              fmtVal(depth, st[i].max).c_str(), st[i].mean, st[i].stddev);
    }
    ImGui::EndTable();
}

void drawInfoTab(App& a)
{
    ImageDoc* d = currentDoc(a);
    if (!d || !d->data || !d->data->ok) { ImGui::TextDisabled("No image"); return; }
    const LoadedData& ld = *d->data;

    if (a.hoverValid) pixelCompact(a, *d, a.hoverX, a.hoverY, "@");
    else ImGui::TextDisabled("hover image");
    if (const FlowResult* fr = activeFlow(a); fr && a.hoverValid) {
        float fx, fy;
        if (flowAt(*fr, a.hoverX + 0.5, a.hoverY + 0.5, fx, fy))
            ImGui::Text("flow %+.2f %+.2f  |%.2f|", fx, fy, std::hypot(fx, fy));
    }
    if (a.pinValid) {
        ImGui::Spacing();
        pixelCompact(a, *d, a.pinX, a.pinY, "pin");
    }
    ImGui::Separator();

    const bool roi = d->hasRoi && !d->roiHist.bins[0].empty();
    std::vector<double> marks;
    if (a.hoverValid) marks = pixelValues(ld.mat, a.hoverX, a.hoverY);
    drawHistogram(roi ? d->roiHist : ld.hist, ld.mat.depth(), a.histLog, a.hoverValid ? &marks : nullptr, 90);
    if (ImGui::BeginPopupContextItem("##histctx")) {
        ImGui::MenuItem("Log scale", nullptr, &a.histLog);
        ImGui::EndPopup();
    }
    ImGui::Separator();

    statsCompact(ld.stats, ld.mat.channels(), ld.mat.depth(), "##stats");
    if (d->hasRoi) {
        ImGui::TextColored(ImVec4(1, 0.82f, 0, 1), "ROI %d,%d  %d×%d", d->roi.x, d->roi.y, d->roi.width, d->roi.height);
        statsCompact(d->roiStats, ld.mat.channels(), ld.mat.depth(), "##roistats");
    }
}

void drawMetaTab(App& a)
{
    ImageDoc* d = currentDoc(a);
    if (!d || !d->data) { ImGui::TextDisabled("No image"); return; }
    static ImGuiTextFilter filter;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##filter", "filter", filter.InputBuf, IM_ARRAYSIZE(filter.InputBuf))) filter.Build();

    const auto rows = metadataRows(*d);
    const char* groups[] = {"File", "Image", "Camera", "Exposure", "Exif", "GPS", "Interop"};
    auto pass = [&](const MetaEntry& r) { return filter.PassFilter(r.name.c_str()) || filter.PassFilter(r.value.c_str()); };
    ImGui::BeginChild("##meta");
    for (const char* g : groups) {
        if (std::none_of(rows.begin(), rows.end(), [&](auto& r) { return r.group == g && pass(r); })) continue;
        if (!ImGui::CollapsingHeader(g, ImGuiTreeNodeFlags_DefaultOpen)) continue;
        for (const auto& r : rows) {
            if (r.group != g || !pass(r)) continue;
            ImGui::TextDisabled("%s", r.name.c_str());
            ImGui::Indent(8);
            ImGui::TextWrapped("%s", r.value.c_str());
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ImGui::SetClipboardText(r.value.c_str());
            ImGui::Unindent(8);
        }
    }
    ImGui::EndChild();
}

void drawFilesTab(App& a)
{
    const float th = 36, tw = 48;
    int toClose = -1;
    ImGui::BeginChild("##list");
    for (int i = 0; i < int(a.docs.size()); ++i) {
        ImageDoc& d = *a.docs[i];
        ImGui::PushID(i);
        if (ImGui::Selectable("##item", a.current == i, 0, ImVec2(0, th))) select(a, i);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.path.c_str());
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Reload")) a.loader->enqueue(a.docs[i], a.disp);
            if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(d.path.c_str());
            if (ImGui::MenuItem(kRevealLabel)) revealInFileManager(d.path);
            if (ImGui::MenuItem("Close")) toClose = i;
            ImGui::EndPopup();
        }
        const ImVec2 r0 = ImGui::GetItemRectMin();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (d.hasTex) {
            const float sc = std::min(tw / d.tex.width, th / d.tex.height);
            const float w = d.tex.width * sc, h = d.tex.height * sc;
            const ImVec2 q0(r0.x + (tw - w) / 2, r0.y + (th - h) / 2);
            dl->AddImage(texId(d.tex), q0, ImVec2(q0.x + w, q0.y + h));
        }
        const float tx = r0.x + tw + 6;
        dl->PushClipRect(ImVec2(tx, r0.y), ImGui::GetItemRectMax(), true);
        dl->AddText(ImVec2(tx, r0.y + 2), ImGui::GetColorU32(ImGuiCol_Text), d.name.c_str());
        std::string sub;
        if (d.data && d.data->ok) sub = std::to_string(d.data->mat.cols) + "×" + std::to_string(d.data->mat.rows) + " " + d.data->origType;
        else if (d.data) sub = d.data->error;
        if (d.loading) sub = "loading…";
        dl->AddText(ImVec2(tx, r0.y + 4 + ImGui::GetFontSize()),
                    d.data && !d.data->ok ? IM_COL32(255, 110, 110, 255) : ImGui::GetColorU32(ImGuiCol_TextDisabled), sub.c_str());
        dl->PopClipRect();
        ImGui::PopID();
    }
    if (a.docs.empty()) ImGui::TextDisabled("Drop files here");
    ImGui::EndChild();
    if (toClose >= 0) closeDoc(a, toClose);
}

void drawSidebar(App& a)
{
    ImGuiWindowClass wc;
    wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&wc);
    ImGui::Begin("Sidebar");
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Info")) { drawInfoTab(a); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Meta")) { drawMetaTab(a); ImGui::EndTabItem(); }
        char files[32];
        std::snprintf(files, sizeof files, "Files %d###files", int(a.docs.size()));
        if (ImGui::BeginTabItem(files)) { drawFilesTab(a); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void drawMenuBar(App& a)
{
    if (!ImGui::BeginMainMenuBar()) return;
    ImageDoc* d = currentDoc(a);
    const bool ready = d && d->data && d->data->ok;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Reload", "R", false, d)) reload(a, *d);
        if (ImGui::MenuItem("Reload all", nullptr, false, !a.docs.empty()))
            for (auto& sp : a.docs) a.loader->enqueue(sp, a.disp);
        ImGui::MenuItem("Auto-reload changed files", nullptr, &a.autoReload);
        ImGui::Separator();
        if (ImGui::MenuItem("Copy path", nullptr, false, d)) ImGui::SetClipboardText(d->path.c_str());
        if (ImGui::MenuItem("Copy metadata", nullptr, false, d && d->data)) ImGui::SetClipboardText(metadataText(*d).c_str());
        if (ImGui::MenuItem(kRevealLabel, nullptr, false, d)) revealInFileManager(d->path);
        ImGui::Separator();
        if (ImGui::MenuItem("Close", "Del", false, d)) closeDoc(a, a.current);
        if (ImGui::MenuItem("Close all", nullptr, false, !a.docs.empty()))
            while (!a.docs.empty()) closeDoc(a, 0);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Cmd+Q")) a.quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Fit", "F")) a.view.fitted = false;
        const char* zl[] = {"100%", "200%", "400%", "800%"};
        const char* zk[] = {"1", "2", "3", "4"};
        for (int i = 0; i < 4; ++i)
            if (ImGui::MenuItem(zl[i], zk[i])) { a.view.scale = float(1 << i) / dpiScale(); a.view.fitted = true; }
        ImGui::Separator();
        ImGui::MenuItem("Normalize min..max", "N", &a.disp.normalize);
        if (ImGui::BeginMenu("Channel")) {
            const char* chans[] = {"All", "R", "G", "B", "A"};
            for (int i = 0; i < 5; ++i)
                if (ImGui::MenuItem(chans[i], i == 0 ? "C cycles" : nullptr, a.disp.channel == i)) a.disp.channel = i;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem("Blend two images", "B", &a.blend, canBlend(a));
        ImGui::MenuItem("Optical flow from previous", "O", &a.showFlow, a.docs.size() >= 2);
        ImGui::Separator();
        ImGui::MenuItem("Pixel grid", "G", &a.showGrid);
        ImGui::MenuItem("Values in cells", "V", &a.showValues);
        if (ImGui::BeginMenu("Loupe (hold Shift)")) {
            ImGui::MenuItem("Enabled", nullptr, &a.showLoupe);
            ImGui::SetNextItemWidth(140);
            ImGui::SliderInt("Pixels", &a.loupePixels, 5, 65);
            ImGui::SetNextItemWidth(140);
            ImGui::SliderFloat("Size", &a.loupeSize, 120, 480, "%.0f");
            ImGui::EndMenu();
        }
        ImGui::MenuItem("Histogram log scale", nullptr, &a.histLog);
        ImGui::Separator();
        ImGui::MenuItem("Scroll pans (trackpad)", nullptr, &a.scrollPans);
        ImGui::MenuItem("Keep view when switching", nullptr, &a.keepView);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) a.resetLayout = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Image")) {
        if (ImGui::MenuItem("Next", "→", false, a.docs.size() > 1)) select(a, a.current + 1);
        if (ImGui::MenuItem("Previous", "←", false, a.docs.size() > 1)) select(a, a.current - 1);
        ImGui::Separator();
        if (ImGui::MenuItem("Clear ROI", "Esc", false, d && d->hasRoi)) d->hasRoi = false;
        if (ImGui::MenuItem("Copy ROI as cv::Rect", nullptr, false, d && d->hasRoi)) {
            char b[96];
            std::snprintf(b, sizeof b, "cv::Rect(%d, %d, %d, %d)", d->roi.x, d->roi.y, d->roi.width, d->roi.height);
            ImGui::SetClipboardText(b);
        }
        if (ImGui::MenuItem("Unpin pixel", "Esc", false, a.pinValid)) a.pinValid = false;
        if (!a.docs.empty()) {
            ImGui::Separator();
            for (int i = 0; i < int(a.docs.size()) && i < 40; ++i)
                if (ImGui::MenuItem(a.docs[i]->name.c_str(), nullptr, a.current == i)) select(a, i);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        const char* lines[] = {
            "Drop files or folders onto the window",
            "Two-finger scroll    pan",
            "Pinch / Cmd+scroll   zoom",
            "Drag                 pan",
            "Double-click / F     fit",
            "Hold Shift           loupe",
            "Click                pin pixel",
            "Right / Option-drag  ROI",
            "Left / Right         previous / next",
            "N  normalize   C  channel",
            "G  grid        V  cell values",
            "B  blend (2 images)   O  optical flow",
            "R  reload      Del  close",
        };
        for (const char* l : lines) ImGui::TextUnformatted(l);
        ImGui::EndMenu();
    }

    // Right-aligned status: image, size, type, zoom.
    std::string info;
    if (a.loader->busy()) info += "loading…   ";
    if (ready) {
        char b[256];
        std::snprintf(b, sizeof b, "%d/%d  %s   %d×%d %s   %.0f%%", a.current + 1, int(a.docs.size()), d->name.c_str(),
                      d->data->mat.cols, d->data->mat.rows, d->data->origType.c_str(), a.view.scale * dpiScale() * 100);
        info += b;
    }
    const float w = ImGui::CalcTextSize(info.c_str()).x;
    const float x = ImGui::GetWindowWidth() - w - ImGui::GetStyle().ItemSpacing.x * 2;
    if (x > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(x);
        ImGui::TextDisabled("%s", info.c_str());
    }
    ImGui::EndMainMenuBar();
}

void handleShortcuts(App& a)
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    auto pressed = [](ImGuiKey k, bool repeat = false) { return ImGui::IsKeyPressed(k, repeat); };
    const ImVec2 cc(a.canvasSize.x, a.canvasSize.y);  // only used as a relative origin below
    ImageDoc* d = currentDoc(a);

    if (pressed(ImGuiKey_RightArrow, true) || pressed(ImGuiKey_PageDown, true) || pressed(ImGuiKey_Space, true)) select(a, a.current + 1);
    if (pressed(ImGuiKey_LeftArrow, true) || pressed(ImGuiKey_PageUp, true) || pressed(ImGuiKey_Backspace, true)) select(a, a.current - 1);
    if (pressed(ImGuiKey_Home)) select(a, 0);
    if (pressed(ImGuiKey_End)) select(a, int(a.docs.size()) - 1);
    if (pressed(ImGuiKey_F) || pressed(ImGuiKey_0)) a.view.fitted = false;
    const ImGuiKey zk[] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4};
    for (int i = 0; i < 4; ++i)
        if (pressed(zk[i])) { a.view.scale = float(1 << i) / dpiScale(); a.view.fitted = true; }
    if (pressed(ImGuiKey_Equal, true) || pressed(ImGuiKey_KeypadAdd, true)) zoomAt(a, cc, cc, a.view.scale * 1.25f);
    if (pressed(ImGuiKey_Minus, true) || pressed(ImGuiKey_KeypadSubtract, true)) zoomAt(a, cc, cc, a.view.scale / 1.25f);
    if (pressed(ImGuiKey_N)) a.disp.normalize = !a.disp.normalize;
    if (pressed(ImGuiKey_C)) a.disp.channel = (a.disp.channel + 1) % 5;
    if (pressed(ImGuiKey_G)) a.showGrid = !a.showGrid;
    if (pressed(ImGuiKey_B) && canBlend(a)) a.blend = !a.blend;
    if (pressed(ImGuiKey_O) && a.docs.size() >= 2) a.showFlow = !a.showFlow;
    if (pressed(ImGuiKey_V)) a.showValues = !a.showValues;
    if (pressed(ImGuiKey_R) && d) reload(a, *d);
    if (pressed(ImGuiKey_Escape)) {
        if (d && d->hasRoi) d->hasRoi = false;
        else a.pinValid = false;
    }
    if (pressed(ImGuiKey_Delete)) closeDoc(a, a.current);
    if (pressed(ImGuiKey_F1)) a.showDemo = !a.showDemo;
}

void buildDefaultLayout(ImGuiID dock)
{
    const ImVec2 size = ImGui::GetMainViewport()->WorkSize;
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock, size);
    ImGuiID center = dock;
    // ~10% of the width, but never so narrow that the pixel table wraps.
    const float ratio = std::max(0.10f, 190.0f / std::max(size.x, 1.0f));
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, ratio, nullptr, &center);
    ImGui::DockBuilderDockWindow("Viewer", center);
    ImGui::DockBuilderDockWindow("Sidebar", right);
    ImGui::DockBuilderFinish(dock);
}

}  // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    if (!std::getenv("OPENCV_IO_ENABLE_OPENEXR")) _putenv_s("OPENCV_IO_ENABLE_OPENEXR", "1");
#else
    setenv("OPENCV_IO_ENABLE_OPENEXR", "1", 0);
#endif

    std::vector<std::string> initial;
    std::string shotPath;  // --shot out.png: render a few frames, save a screenshot and exit (for testing)
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--shot" && i + 1 < argc) shotPath = argv[++i];
        else if (s.rfind("-psn_", 0) == 0) continue;  // old macOS Finder launch argument
        else {
            // Resolve now: inside an .app bundle GLFW changes the working directory to Resources/.
            std::error_code ec;
            fs::path abs = fs::absolute(s, ec);
            initial.push_back(ec ? s : abs.string());
        }
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI | FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    macInstallOpenHandler();  // before InitWindow: the launch "open file" event arrives during glfwInit
    InitWindow(1600, 1000, "PhotoViewer");
    {
        // macOS silently shrinks windows taller than the screen, but raylib keeps the requested
        // size, pushing the top of the UI (the menu bar) out of view. Fit to the monitor instead.
        const int mon = GetCurrentMonitor();
        const int w = std::min(1600, int(GetMonitorWidth(mon) * 0.9f));
        const int h = std::min(1000, int(GetMonitorHeight(mon) * 0.85f));
        SetWindowSize(w, h);
        SetWindowPosition((GetMonitorWidth(mon) - w) / 2, (GetMonitorHeight(mon) - h) / 2);
    }
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    rlImGuiSetup(true);
    macInstallGestures();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    static std::string iniPath;
    if (const char* home = std::getenv("HOME")) iniPath = std::string(home) + "/.photoviewer.ini";
    else iniPath = "photoviewer.ini";
    io.IniFilename = iniPath.c_str();
    const bool firstRun = !fs::exists(iniPath);
    ImGui::GetStyle().WindowRounding = 0;

    App a;
    a.loader = std::make_unique<Loader>(std::max(2u, std::thread::hardware_concurrency() / 2));
    {
        Image ck = GenImageChecked(16, 16, 8, 8, Color{90, 90, 94, 255}, Color{60, 60, 64, 255});
        a.checker = LoadTextureFromImage(ck);
        UnloadImage(ck);
    }
    if (!initial.empty()) addPaths(a, initial);

    bool layoutDone = false;
    int frame = 0;
    while (!WindowShouldClose() && !a.quit) {
        if (IsFileDropped()) {
            FilePathList list = LoadDroppedFiles();
            std::vector<std::string> paths(list.paths, list.paths + list.count);
            UnloadDroppedFiles(list);
            addPaths(a, paths);
        }
        if (auto opened = macTakeOpenedFiles(); !opened.empty()) addPaths(a, opened);
        pollDocs(a);
        a.pinch = macConsumeMagnify();
        watchFiles(a);
        updateFlow(a);

        std::string title = "PhotoViewer";
        if (ImageDoc* d = currentDoc(a)) title = d->name + " — PhotoViewer";
        if (title != a.lastTitle) { SetWindowTitle(title.c_str()); a.lastTitle = title; }

        BeginDrawing();
        ClearBackground(Color{28, 28, 31, 255});
        rlImGuiBegin();

        drawMenuBar(a);
        const ImGuiID dock = ImGui::GetID("MainDock");
        if (!layoutDone || a.resetLayout) {
            if (firstRun || a.resetLayout || !ImGui::DockBuilderGetNode(dock)) buildDefaultLayout(dock);
            layoutDone = true;
            a.resetLayout = false;
        }
        ImGui::DockSpaceOverViewport(dock, ImGui::GetMainViewport());

        drawViewer(a);
        drawSidebar(a);
        if (a.showDemo) ImGui::ShowDemoWindow(&a.showDemo);
        handleShortcuts(a);

        rlImGuiEnd();
        // raylib's TakeScreenshot mis-sizes HiDPI framebuffers, so read the back buffer ourselves.
        const bool shoot = !shotPath.empty() && ++frame > 20 && !a.loader->busy();
        if (shoot) {
            rlDrawRenderBatchActive();
            const int w = GetRenderWidth(), h = GetRenderHeight();
            unsigned char* px = rlReadScreenPixels(w, h);
            Image shot{px, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            ExportImage(shot, shotPath.c_str());
            RL_FREE(px);
        }
        EndDrawing();
        if (shoot) break;
        if (!shotPath.empty() && frame == 10) SetMousePosition(GetScreenWidth() / 2, GetScreenHeight() / 2);
    }

    for (auto& d : a.docs)
        if (d->hasTex) UnloadTexture(d->tex);
    a.docs.clear();
    a.loader.reset();
    if (a.flow.pending.valid()) a.flow.pending.wait();
    if (a.flow.hasTex) UnloadTexture(a.flow.tex);
    UnloadTexture(a.checker);
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
