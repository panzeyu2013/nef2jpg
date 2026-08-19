#include "decoder.h"

#if NEF2JPG_USE_SDK
#include "decoder_sdk.h"
#endif

std::unique_ptr<Decoder> Decoder::create(const std::string &backend, std::string *err) {
#if NEF2JPG_USE_SDK
    if (backend == "auto" || backend == "sdk")
        return std::make_unique<SdkDecoder>();
    *err = "unknown backend: " + backend;
    return nullptr;
#else
    (void)backend;
    if (err) *err = "no decode backend available on this platform yet "
                    "(Nikon SDK is macOS-only; libraw backend is planned)";
    return nullptr;
#endif
}
