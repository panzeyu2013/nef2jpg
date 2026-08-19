#pragma once
#include "decoder.h"
#include <cstdint>
#include <string>

struct JpegOptions {
    int quality = 90;           // 1..100
    bool progressive = false;
    int subsampling = 420;      // 420 / 422 / 444
    bool embedIcc = true;
    int64_t targetSize = 0;     // 0 = off
    int64_t maxSize = 0;        // 0 = off
    int tolerancePct = 5;
};

// 编码 DecodedImage 为 JPEG 写入 path。
// 返回 false 时 err 为失败原因；返回 true 时 err 可能携带警告（如 max-size 无法满足）。
bool encodeJpeg(const DecodedImage &img, const std::string &path,
                const JpegOptions &opt, std::string *err);

// 把 sRGB ICC 配置文件读入内存（找不到返回空）
std::vector<uint8_t> loadSrgbIccProfile();
