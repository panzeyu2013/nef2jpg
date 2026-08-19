#pragma once
#include "decoder.h"
#include <string>

// 向已编码的 JPEG 写入 EXIF（日期/相机/镜头/方向等）。
// 返回 true 表示成功或本构建未启用 EXIF；false 表示写入失败（调用方可降级为警告）。
bool writeExif(const std::string &jpegPath, const DecodedImage &img, std::string *err);
