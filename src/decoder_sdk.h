#pragma once
#if NEF2JPG_USE_SDK
#include "decoder.h"
#include <string>

// 尼康 Image SDK 后端（macOS only）
class SdkDecoder : public Decoder {
public:
    SdkDecoder() = default;
    ~SdkDecoder() override;

    bool open(const std::string &path, std::string *err) override;
    bool decode(DecodedImage *out, bool autoOrient, std::string *err) override;
    void close() override;
    const char *name() const override { return "sdk"; }

private:
    unsigned long sessionId_ = 0;
    std::string path_;
    bool opened_ = false;
    bool libraryOpen_ = false;
};
#endif // NEF2JPG_USE_SDK
