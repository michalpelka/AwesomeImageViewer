#include "exif.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <unordered_map>

namespace {

enum class Ifd { Main, Exif, Gps, Interop };

const std::unordered_map<uint16_t, const char*>& mainTagNames()
{
    static const std::unordered_map<uint16_t, const char*> t = {
        {0x00FE, "NewSubfileType"}, {0x0100, "ImageWidth"}, {0x0101, "ImageHeight"},
        {0x0102, "BitsPerSample"}, {0x0103, "Compression"}, {0x0106, "PhotometricInterpretation"},
        {0x010E, "ImageDescription"}, {0x010F, "Make"}, {0x0110, "Model"}, {0x0112, "Orientation"},
        {0x0115, "SamplesPerPixel"}, {0x011A, "XResolution"}, {0x011B, "YResolution"},
        {0x011C, "PlanarConfiguration"}, {0x0128, "ResolutionUnit"}, {0x0131, "Software"},
        {0x0132, "DateTime"}, {0x013B, "Artist"}, {0x013E, "WhitePoint"},
        {0x013F, "PrimaryChromaticities"}, {0x0211, "YCbCrCoefficients"}, {0x0213, "YCbCrPositioning"},
        {0x0214, "ReferenceBlackWhite"}, {0x02BC, "XMP"}, {0x8298, "Copyright"},
        {0x829A, "ExposureTime"}, {0x829D, "FNumber"}, {0x83BB, "IPTC"}, {0x8773, "ICCProfile"},
        {0x8822, "ExposureProgram"}, {0x8824, "SpectralSensitivity"}, {0x8827, "ISO"},
        {0x8830, "SensitivityType"}, {0x8832, "RecommendedExposureIndex"}, {0x9000, "ExifVersion"},
        {0x9003, "DateTimeOriginal"}, {0x9004, "DateTimeDigitized"}, {0x9010, "OffsetTime"},
        {0x9011, "OffsetTimeOriginal"}, {0x9012, "OffsetTimeDigitized"},
        {0x9101, "ComponentsConfiguration"}, {0x9102, "CompressedBitsPerPixel"},
        {0x9201, "ShutterSpeedValue"}, {0x9202, "ApertureValue"}, {0x9203, "BrightnessValue"},
        {0x9204, "ExposureBiasValue"}, {0x9205, "MaxApertureValue"}, {0x9206, "SubjectDistance"},
        {0x9207, "MeteringMode"}, {0x9208, "LightSource"}, {0x9209, "Flash"}, {0x920A, "FocalLength"},
        {0x9214, "SubjectArea"}, {0x927C, "MakerNote"}, {0x9286, "UserComment"},
        {0x9290, "SubSecTime"}, {0x9291, "SubSecTimeOriginal"}, {0x9292, "SubSecTimeDigitized"},
        {0xA000, "FlashpixVersion"}, {0xA001, "ColorSpace"}, {0xA002, "PixelXDimension"},
        {0xA003, "PixelYDimension"}, {0xA004, "RelatedSoundFile"}, {0xA20E, "FocalPlaneXResolution"},
        {0xA20F, "FocalPlaneYResolution"}, {0xA210, "FocalPlaneResolutionUnit"},
        {0xA215, "ExposureIndex"}, {0xA217, "SensingMethod"}, {0xA300, "FileSource"},
        {0xA301, "SceneType"}, {0xA401, "CustomRendered"}, {0xA402, "ExposureMode"},
        {0xA403, "WhiteBalance"}, {0xA404, "DigitalZoomRatio"}, {0xA405, "FocalLengthIn35mmFilm"},
        {0xA406, "SceneCaptureType"}, {0xA407, "GainControl"}, {0xA408, "Contrast"},
        {0xA409, "Saturation"}, {0xA40A, "Sharpness"}, {0xA40C, "SubjectDistanceRange"},
        {0xA420, "ImageUniqueID"}, {0xA430, "CameraOwnerName"}, {0xA431, "BodySerialNumber"},
        {0xA432, "LensSpecification"}, {0xA433, "LensMake"}, {0xA434, "LensModel"},
        {0xA435, "LensSerialNumber"}, {0xA500, "Gamma"},
        {0xC612, "DNGVersion"}, {0xC614, "UniqueCameraModel"}, {0xC621, "ColorMatrix1"},
        {0xC622, "ColorMatrix2"}, {0xC628, "AsShotNeutral"}, {0xC65A, "CalibrationIlluminant1"},
        {0xC65B, "CalibrationIlluminant2"},
    };
    return t;
}

const std::unordered_map<uint16_t, const char*>& gpsTagNames()
{
    static const std::unordered_map<uint16_t, const char*> t = {
        {0x00, "GPSVersionID"}, {0x01, "GPSLatitudeRef"}, {0x02, "GPSLatitude"},
        {0x03, "GPSLongitudeRef"}, {0x04, "GPSLongitude"}, {0x05, "GPSAltitudeRef"},
        {0x06, "GPSAltitude"}, {0x07, "GPSTimeStamp"}, {0x08, "GPSSatellites"},
        {0x09, "GPSStatus"}, {0x0A, "GPSMeasureMode"}, {0x0B, "GPSDOP"}, {0x0C, "GPSSpeedRef"},
        {0x0D, "GPSSpeed"}, {0x0E, "GPSTrackRef"}, {0x0F, "GPSTrack"},
        {0x10, "GPSImgDirectionRef"}, {0x11, "GPSImgDirection"}, {0x12, "GPSMapDatum"},
        {0x1B, "GPSProcessingMethod"}, {0x1D, "GPSDateStamp"}, {0x1F, "GPSHPositioningError"},
    };
    return t;
}

const char* lookup(const std::unordered_map<uint16_t, const char*>& m, int v)
{
    auto it = m.find(uint16_t(v));
    return it == m.end() ? nullptr : it->second;
}

std::string fmt(const char* f, double v)
{
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

std::string enumValue(uint16_t tag, int v)
{
    static const std::unordered_map<uint16_t, const char*> orientation = {
        {1, "Normal"}, {2, "Mirror horizontal"}, {3, "Rotate 180"}, {4, "Mirror vertical"},
        {5, "Mirror horizontal + rotate 270 CW"}, {6, "Rotate 90 CW"},
        {7, "Mirror horizontal + rotate 90 CW"}, {8, "Rotate 270 CW"}};
    static const std::unordered_map<uint16_t, const char*> program = {
        {0, "Not defined"}, {1, "Manual"}, {2, "Program AE"}, {3, "Aperture priority"},
        {4, "Shutter priority"}, {5, "Creative"}, {6, "Action"}, {7, "Portrait"}, {8, "Landscape"}};
    static const std::unordered_map<uint16_t, const char*> metering = {
        {0, "Unknown"}, {1, "Average"}, {2, "Center-weighted"}, {3, "Spot"}, {4, "Multi-spot"},
        {5, "Multi-segment"}, {6, "Partial"}, {255, "Other"}};
    static const std::unordered_map<uint16_t, const char*> resUnit = {{1, "None"}, {2, "inch"}, {3, "cm"}};
    static const std::unordered_map<uint16_t, const char*> photometric = {
        {0, "WhiteIsZero"}, {1, "BlackIsZero"}, {2, "RGB"}, {3, "Palette"}, {6, "YCbCr"},
        {32803, "CFA"}, {34892, "LinearRaw"}};
    static const std::unordered_map<uint16_t, const char*> compression = {
        {1, "Uncompressed"}, {5, "LZW"}, {6, "JPEG (old)"}, {7, "JPEG"}, {8, "Deflate"},
        {32773, "PackBits"}, {34892, "Lossy JPEG"}};

    const char* s = nullptr;
    switch (tag) {
    case 0x0112: s = lookup(orientation, v); break;
    case 0x8822: s = lookup(program, v); break;
    case 0x9207: s = lookup(metering, v); break;
    case 0x0128:
    case 0xA210: s = lookup(resUnit, v); break;
    case 0x0106: s = lookup(photometric, v); break;
    case 0x0103: s = lookup(compression, v); break;
    case 0xA001: s = v == 1 ? "sRGB" : v == 2 ? "Adobe RGB" : v == 0xFFFF ? "Uncalibrated" : nullptr; break;
    case 0xA402: s = v == 0 ? "Auto" : v == 1 ? "Manual" : v == 2 ? "Auto bracket" : nullptr; break;
    case 0xA403: s = v == 0 ? "Auto" : v == 1 ? "Manual" : nullptr; break;
    case 0xA406: s = v == 0 ? "Standard" : v == 1 ? "Landscape" : v == 2 ? "Portrait" : v == 3 ? "Night" : nullptr; break;
    case 0x9209: return (v & 1) ? "Fired" : "Did not fire";
    default: break;
    }
    return s ? s : std::to_string(v);
}

class TiffReader {
public:
    TiffReader(const uint8_t* d, size_t n) : d_(d), n_(n) { le_ = n >= 2 && d[0] == 'I'; }

    std::vector<MetaEntry> run()
    {
        if (n_ < 8) return {};
        uint16_t magic = u16(2);
        if (magic != 42 && magic != 0x55 /*RW2*/ && magic != 0x4F52 /*ORF*/) return {};
        parseIfd(u32(4), Ifd::Main, 0);
        if (hasLat_ && hasLon_) {
            double lat = latSign_ * lat_, lon = lonSign_ * lon_;
            char b[96];
            std::snprintf(b, sizeof b, "%.6f, %.6f", lat, lon);
            out_.push_back({"GPS", "Position (decimal)", b});
        }
        return std::move(out_);
    }

private:
    const uint8_t* d_;
    size_t n_;
    bool le_;
    std::vector<MetaEntry> out_;
    std::set<uint32_t> visited_;
    double lat_ = 0, lon_ = 0;
    int latSign_ = 1, lonSign_ = 1;
    bool hasLat_ = false, hasLon_ = false;

    bool ok(size_t off, size_t len) const { return off <= n_ && len <= n_ - off; }
    uint16_t u16(size_t o) const
    {
        if (!ok(o, 2)) return 0;
        return le_ ? uint16_t(d_[o] | d_[o + 1] << 8) : uint16_t(d_[o] << 8 | d_[o + 1]);
    }
    uint32_t u32(size_t o) const
    {
        if (!ok(o, 4)) return 0;
        return le_ ? uint32_t(d_[o]) | uint32_t(d_[o + 1]) << 8 | uint32_t(d_[o + 2]) << 16 | uint32_t(d_[o + 3]) << 24
                   : uint32_t(d_[o]) << 24 | uint32_t(d_[o + 1]) << 16 | uint32_t(d_[o + 2]) << 8 | uint32_t(d_[o + 3]);
    }

    static int typeSize(int type)
    {
        switch (type) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: case 13: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
        }
    }

    double num(int type, size_t o) const
    {
        switch (type) {
        case 1: case 7: return d_[o];
        case 6: return int8_t(d_[o]);
        case 3: return u16(o);
        case 8: return int16_t(u16(o));
        case 4: case 13: return u32(o);
        case 9: return int32_t(u32(o));
        case 5: { uint32_t a = u32(o), b = u32(o + 4); return b ? double(a) / b : 0.0; }
        case 10: { int32_t a = int32_t(u32(o)), b = int32_t(u32(o + 4)); return b ? double(a) / b : 0.0; }
        case 11: { uint32_t v = u32(o); float f; std::memcpy(&f, &v, 4); return f; }
        case 12: {
            uint64_t v = le_ ? (uint64_t(u32(o + 4)) << 32 | u32(o)) : (uint64_t(u32(o)) << 32 | u32(o + 4));
            double f; std::memcpy(&f, &v, 8); return f;
        }
        default: return 0;
        }
    }

    std::string rawValue(int type, uint32_t count, size_t o) const
    {
        if (type == 2) {
            std::string s(reinterpret_cast<const char*>(d_ + o), count);
            while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
            return s;
        }
        if ((type == 7 || type == 1) && count > 16) return "<" + std::to_string(count) + " bytes>";
        std::string r;
        uint32_t shown = count < 8 ? count : 8;
        for (uint32_t i = 0; i < shown; ++i) {
            if (i) r += ' ';
            size_t eo = o + size_t(i) * typeSize(type);
            if (type == 5 || type == 10) {
                uint32_t a = u32(eo), b = u32(eo + 4);
                if (b == 1 || b == 0) r += std::to_string(type == 10 ? int32_t(a) : int64_t(a));
                else r += fmt("%.6g", num(type, eo));
            } else if (type == 11 || type == 12) {
                r += fmt("%.6g", num(type, eo));
            } else {
                r += std::to_string(int64_t(num(type, eo)));
            }
        }
        if (count > shown) r += " … (" + std::to_string(count) + ")";
        return r;
    }

    std::string pretty(Ifd ifd, uint16_t tag, int type, uint32_t count, size_t o)
    {
        auto first = [&] { return num(type, o); };
        if (ifd == Ifd::Gps) {
            if ((tag == 2 || tag == 4) && count == 3 && (type == 5 || type == 10)) {
                double deg = num(type, o) + num(type, o + 8) / 60.0 + num(type, o + 16) / 3600.0;
                (tag == 2 ? lat_ : lon_) = deg;
                (tag == 2 ? hasLat_ : hasLon_) = true;
                return fmt("%.6f°", deg);
            }
            if ((tag == 1 || tag == 3) && type == 2 && count >= 1) {
                bool neg = d_[o] == 'S' || d_[o] == 'W';
                (tag == 1 ? latSign_ : lonSign_) = neg ? -1 : 1;
            }
            if (tag == 6 && count == 1) return fmt("%.1f m", first());
            if (tag == 7 && count == 3)
                return fmt("%02.0f:", num(type, o)) + fmt("%02.0f:", num(type, o + 8)) + fmt("%05.2f UTC", num(type, o + 16));
            return rawValue(type, count, o);
        }
        switch (tag) {
        case 0x829A: {
            double t = first();
            if (t > 0 && t < 1) return fmt("1/%.0f s", 1.0 / t) + fmt("  (%.6g)", t);
            return fmt("%.6g s", t);
        }
        case 0x829D: return fmt("f/%.1f", first());
        case 0x9202: case 0x9205: return fmt("f/%.1f", std::pow(2.0, first() / 2.0));
        case 0x9201: return fmt("1/%.0f s", std::pow(2.0, first()));
        case 0x920A: return fmt("%.1f mm", first());
        case 0xA405: return fmt("%.0f mm", first());
        case 0x9204: return fmt("%+.2f EV", first());
        case 0x9206: return fmt("%.2f m", first());
        case 0x9000: case 0xA000: case 0xC612:
            if (type == 7 || type == 1) {
                std::string s;
                for (uint32_t i = 0; i < count && i < 8; ++i)
                    s += (type == 7 && d_[o + i] >= 32) ? std::string(1, char(d_[o + i]))
                                                        : std::to_string(d_[o + i]) + (i + 1 < count ? "." : "");
                return s;
            }
            break;
        case 0xA432:
            if (count == 4)
                return fmt("%.0f-", num(type, o)) + fmt("%.0f mm ", num(type, o + 8)) + fmt("f/%.1f-", num(type, o + 16)) + fmt("%.1f", num(type, o + 24));
            break;
        case 0x0112: case 0x8822: case 0x9207: case 0x0128: case 0xA210: case 0x0106:
        case 0x0103: case 0xA001: case 0xA402: case 0xA403: case 0xA406: case 0x9209:
            if (count == 1 && (type == 3 || type == 4)) return enumValue(tag, int(first()));
            break;
        case 0x02BC: case 0x927C: case 0x8773: case 0x83BB: case 0x9286:
            return "<" + std::to_string(count * typeSize(type)) + " bytes>";
        default: break;
        }
        return rawValue(type, count, o);
    }

    static const char* groupFor(Ifd ifd, uint16_t tag)
    {
        switch (ifd) {
        case Ifd::Gps: return "GPS";
        case Ifd::Interop: return "Interop";
        default: break;
        }
        switch (tag) {
        case 0x010F: case 0x0110: case 0xA430: case 0xA431: case 0xA432: case 0xA433:
        case 0xA434: case 0xA435: case 0x0131: case 0x0132: case 0x9003: case 0x9004:
        case 0x013B: case 0x8298: case 0x010E: case 0xC614:
            return "Camera";
        case 0x829A: case 0x829D: case 0x8827: case 0x8822: case 0x9201: case 0x9202:
        case 0x9204: case 0x9205: case 0x9207: case 0x9209: case 0x920A: case 0xA405:
        case 0xA402: case 0xA403: case 0xA406: case 0x9208: case 0x9206: case 0x8832:
            return "Exposure";
        default:
            return ifd == Ifd::Exif ? "Exif" : "Image";
        }
    }

    void parseIfd(uint32_t off, Ifd ifd, int depth)
    {
        if (depth > 4 || off == 0 || !ok(off, 2) || !visited_.insert(off).second) return;
        uint16_t n = u16(off);
        if (!ok(off + 2, size_t(n) * 12)) return;
        const auto& names = ifd == Ifd::Gps ? gpsTagNames() : mainTagNames();
        for (uint16_t i = 0; i < n; ++i) {
            size_t e = off + 2 + size_t(i) * 12;
            uint16_t tag = u16(e);
            uint16_t type = u16(e + 2);
            uint32_t count = u32(e + 4);
            int ts = typeSize(type);
            if (!ts) continue;
            uint64_t bytes = uint64_t(ts) * count;
            size_t vo = bytes <= 4 ? e + 8 : u32(e + 8);
            if (bytes > n_ || !ok(vo, size_t(bytes))) continue;

            if (ifd != Ifd::Gps) {
                if (tag == 0x8769) { parseIfd(u32(vo), Ifd::Exif, depth + 1); continue; }
                if (tag == 0x8825) { parseIfd(u32(vo), Ifd::Gps, depth + 1); continue; }
                if (tag == 0xA005) continue;  // interop: not interesting
                if (tag == 0x014A) continue;  // SubIFDs (RAW previews)
            }
            // Skip strip/tile layout noise.
            if (tag == 0x0111 || tag == 0x0117 || tag == 0x0144 || tag == 0x0145 || tag == 0x0201 || tag == 0x0202) continue;

            const char* nm = lookup(names, tag);
            char hex[16];
            std::snprintf(hex, sizeof hex, "0x%04X", tag);
            out_.push_back({groupFor(ifd, tag), nm ? nm : hex, pretty(ifd, tag, type, count, vo)});
        }
    }
};

bool startsWith(const std::vector<uint8_t>& f, size_t at, const char* s, size_t len)
{
    return f.size() >= at + len && std::memcmp(f.data() + at, s, len) == 0;
}

// Locate the TIFF header of the EXIF block. Returns {offset, size} or size 0.
std::pair<size_t, size_t> findTiff(const std::vector<uint8_t>& f)
{
    const size_t n = f.size();
    if (n < 12) return {0, 0};
    // TIFF based (TIFF, DNG, NEF, CR2, ARW, ...)
    if (startsWith(f, 0, "II*\0", 4) || startsWith(f, 0, "MM\0*", 4) || startsWith(f, 0, "IIRO", 4) || startsWith(f, 0, "IIU\0", 4))
        return {0, n};
    // JPEG
    if (f[0] == 0xFF && f[1] == 0xD8) {
        size_t p = 2;
        while (p + 4 <= n && f[p] == 0xFF) {
            uint8_t marker = f[p + 1];
            if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) { p += 2; continue; }
            if (marker == 0xDA || marker == 0xD9) break;
            size_t len = size_t(f[p + 2]) << 8 | f[p + 3];
            if (len < 2 || p + 2 + len > n) break;
            if (marker == 0xE1 && len >= 8 && startsWith(f, p + 4, "Exif\0\0", 6))
                return {p + 10, len - 8};
            p += 2 + len;
        }
        return {0, 0};
    }
    // PNG eXIf chunk
    if (startsWith(f, 0, "\x89PNG", 4)) {
        size_t p = 8;
        while (p + 12 <= n) {
            size_t len = size_t(f[p]) << 24 | size_t(f[p + 1]) << 16 | size_t(f[p + 2]) << 8 | f[p + 3];
            if (p + 12 + len > n) break;
            if (startsWith(f, p + 4, "eXIf", 4)) return {p + 8, len};
            if (startsWith(f, p + 4, "IEND", 4)) break;
            p += 12 + len;
        }
        return {0, 0};
    }
    // WebP EXIF chunk
    if (startsWith(f, 0, "RIFF", 4) && startsWith(f, 8, "WEBP", 4)) {
        size_t p = 12;
        while (p + 8 <= n) {
            size_t len = size_t(f[p + 4]) | size_t(f[p + 5]) << 8 | size_t(f[p + 6]) << 16 | size_t(f[p + 7]) << 24;
            if (p + 8 + len > n) break;
            if (startsWith(f, p, "EXIF", 4)) {
                size_t o = p + 8;
                if (startsWith(f, o, "Exif\0\0", 6)) return {o + 6, len - 6};
                return {o, len};
            }
            p += 8 + len + (len & 1);
        }
        return {0, 0};
    }
    // Anything else (HEIF/AVIF/JXL box containers...): scan for an Exif header.
    size_t lim = n < (4u << 20) ? n : (4u << 20);
    for (size_t p = 0; p + 10 < lim; ++p) {
        if (f[p] == 'E' && startsWith(f, p, "Exif\0\0", 6) && (startsWith(f, p + 6, "II*\0", 4) || startsWith(f, p + 6, "MM\0*", 4)))
            return {p + 6, n - p - 6};
    }
    return {0, 0};
}

}  // namespace

std::vector<MetaEntry> parseExif(const std::vector<uint8_t>& file)
{
    auto [off, size] = findTiff(file);
    if (!size) return {};
    return TiffReader(file.data() + off, size).run();
}

std::string detectFormat(const std::vector<uint8_t>& f)
{
    if (f.size() < 12) return "?";
    if (f[0] == 0xFF && f[1] == 0xD8) return "JPEG";
    if (startsWith(f, 0, "\x89PNG", 4)) return "PNG";
    if (startsWith(f, 0, "RIFF", 4) && startsWith(f, 8, "WEBP", 4)) return "WebP";
    if (startsWith(f, 0, "II*\0", 4) || startsWith(f, 0, "MM\0*", 4)) return "TIFF";
    if (startsWith(f, 0, "\x76\x2F\x31\x01", 4)) return "OpenEXR";
    if (startsWith(f, 0, "BM", 2)) return "BMP";
    if (startsWith(f, 4, "ftyp", 4)) return "ISO-BMFF (" + std::string(reinterpret_cast<const char*>(f.data() + 8), 4) + ")";
    if (f[0] == 'P' && f[1] >= '1' && f[1] <= '7') return "PNM";
    if (startsWith(f, 0, "#?RADIANCE", 10) || startsWith(f, 0, "#?RGBE", 6)) return "Radiance HDR";
    if (startsWith(f, 0, "\xFF\x0A", 2) || startsWith(f, 4, "JXL ", 4)) return "JPEG XL";
    if (startsWith(f, 0, "\x00\x00\x00\x0CjP  ", 8)) return "JPEG 2000";
    return "?";
}
