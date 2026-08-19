#pragma once
#include "decoder.h"
#include <cstdint>
#include <string>

struct JpegOptions {
    int quality = 90;           // 1..100；-s 时作为二分起点
    bool progressive = false;
    int subsampling = 420;      // 420 / 422 / 444
    bool embedIcc = true;
    std::string iccPath;        // 自定义 ICC 文件（空 = 默认 sRGB）
    int64_t targetSize = 0;     // 0 = off
    int64_t maxSize = 0;        // 0 = off
    int tolerancePct = 5;
};

// 编码 DecodedImage 为 JPEG 写入 path。
// 返回 false 时 err 为失败原因；返回 true 时 err 可能携带警告（如 max-size 无法满足）。
bool encodeJpeg(const DecodedImage &img, const std::string &path,
                const JpegOptions &opt, std::string *err);

// 读取 ICC 配置文件：path 非空时读该文件；为空时回退默认 sRGB（系统 profiles 目录，
// 其次可执行文件旁 Contents/Resources）。找不到返回空。
std::vector<uint8_t> loadIccProfile(const std::string &path);
