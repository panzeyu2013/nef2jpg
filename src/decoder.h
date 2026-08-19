#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// 解码后的图像（8-bit 交错 RGB）
struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgb; // width*height*3
    // EXIF orientation (1..8) as reported by the source; 0 = unknown
    uint16_t orientation = 0;
    // basic EXIF text tags (filled when available)
    std::string date_time;   // "YYYY:MM:DD HH:MM:SS"
    std::string make;
    std::string model;
    std::string lens;
};

class Decoder {
public:
    virtual ~Decoder() = default;

    // 打开文件并解析；失败时 err 给出原因。返回 false 表示失败。
    virtual bool open(const std::string &path, std::string *err) = 0;

    // 解码为 8-bit RGB；autoOrient=true 时输出按 EXIF 正向。
    // 失败返回 false，err 给出原因。
    virtual bool decode(DecodedImage *out, bool autoOrient, std::string *err) = 0;

    virtual void close() = 0;

    // 后端名：sdk / stub
    virtual const char *name() const = 0;

    // 创建后端实例；backend = "auto" 时选当前平台可用后端。
    static std::unique_ptr<Decoder> create(const std::string &backend, std::string *err);
};
