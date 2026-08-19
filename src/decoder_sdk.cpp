// macOS 尼康 Image SDK 解码后端。
// 整库只 OpenLibrary 一次（进程级引用计数），每个实例一个独立 session。
#if NEF2JPG_USE_SDK

#include "decoder_sdk.h"

#include <Carbon/Carbon.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/sysctl.h>
#include <unistd.h>

#include "Nkfl_Interface.h"

namespace {

// 进程级 SDK 库初始化（线程安全，引用计数）
std::mutex g_libMutex;
int g_libCount = 0;
std::string g_swapPath; // mkstemp 创建的 swap 文件路径，CloseLibrary 时清理

// SDK 警告码白名单（文档 0x0101-0x0115），显式枚举，不吞未来新错误码
bool isWarningCode(unsigned long code) {
    return code >= 0x0101 && code <= 0x0115;
}

std::string hexCode(unsigned long code) {
    char b[16];
    snprintf(b, sizeof b, "0x%08lX", code);
    return b;
}

unsigned long ensureLibraryOpen() {
    std::lock_guard<std::mutex> lock(g_libMutex);
    if (g_libCount > 0) {
        ++g_libCount;
        return kNkfl_Code_None;
    }
    NkflLibraryParam lib = {};
    lib.ulSize    = sizeof(NkflLibraryParam);
    lib.ulVersion = 0x01000000;
    uint64_t installedRAM = 0;
    size_t sz = sizeof(installedRAM);
    if (sysctlbyname("hw.memsize", &installedRAM, &sz, NULL, 0) == 0)
        lib.ulVMMemorySize = (unsigned long)((installedRAM / 1024 / 1024) / 4);
    else
        lib.ulVMMemorySize = 2048;

    // 用 mkstemp 生成不可预测的 swap 文件名（避免 /tmp 符号链接攻击）
    char tmpl[] = "/tmp/NkImgSDK_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) {
        close(fd);
        snprintf((char *)lib.VMFileInfo, MAX_PATH, "%s", tmpl);
        g_swapPath = tmpl;
    } else {
        snprintf((char *)lib.VMFileInfo, MAX_PATH, "/tmp/NkImgSDK_%d.tmp", (int)getpid());
    }

    unsigned long err = kNkfl_Code_Err_Unexpected;
    try {
        err = Nkfl_Entry(kNkfl_Cmd_OpenLibrary, &lib);
    } catch (...) {
        err = kNkfl_Code_Err_Unexpected;
    }
    if (err != kNkfl_Code_None) {
        if (!g_swapPath.empty()) { unlink(g_swapPath.c_str()); g_swapPath.clear(); }
        return err;
    }

    // 与官方示例一致：机内处理优先
    NkflDevelopColorMode mode = {};
    mode.ulSize = sizeof(NkflDevelopColorMode);
    mode.lDevelopColorMode = kNkfl_DevelopColorMode_AppliedInCamera;
    try { Nkfl_Entry(kNkfl_Cmd_SetDevelopColorMode, &mode); } catch (...) {}

    ++g_libCount;
    return kNkfl_Code_None;
}

void releaseLibrary() {
    std::lock_guard<std::mutex> lock(g_libMutex);
    if (g_libCount <= 0) return;
    if (--g_libCount == 0) {
        try { Nkfl_Entry(kNkfl_Cmd_CloseLibrary, NULL); } catch (...) {}
        if (!g_swapPath.empty()) {
            unlink(g_swapPath.c_str());
            g_swapPath.clear();
        }
    }
}

std::string tagString(unsigned long sid, unsigned long tagId) {
    // 两段式 GetTagData：先取长度/类型，再取数据
    NkflTagDataParam p = {};
    p.ulSize = sizeof(NkflTagDataParam);
    p.ulSessionID = sid;
    p.ulTagID = tagId;
    try {
        unsigned long e = Nkfl_Entry(kNkfl_Cmd_GetTagData, &p);
        if (e != kNkfl_Code_None || p.ulTagLength == 0 || p.ulTagLength > 1 << 20) return "";
        std::vector<char> buf(p.ulTagLength + 1, 0);
        p.pData = buf.data();
        e = Nkfl_Entry(kNkfl_Cmd_GetTagData, &p);
        if (e != kNkfl_Code_None) return "";
        return std::string(buf.data());
    } catch (...) { return ""; }
}

// DateTime 标签是 NkflTagParam_DateTime 结构体（不是字符串），单独解析
std::string dateTimeString(unsigned long sid) {
    NkflTagDataParam p = {};
    p.ulSize = sizeof(NkflTagDataParam);
    p.ulSessionID = sid;
    p.ulTagID = kNkfl_Tag_DateTime;
    p.ulTagType = kNkfl_TagType_DateTime;
    try {
        if (Nkfl_Entry(kNkfl_Cmd_GetTagData, &p) != kNkfl_Code_None) return "";
        if (p.ulTagLength != sizeof(NkflTagParam_DateTime)) return "";
        NkflTagParam_DateTime dt = {};
        p.pData = &dt;
        if (Nkfl_Entry(kNkfl_Cmd_GetTagData, &p) != kNkfl_Code_None) return "";
        if (dt.ulYear == 0 && dt.ulMonth == 0 && dt.ulDay == 0) return "";
        char b[64];
        // EXIF DateTime 标准格式不含小数秒：YYYY:MM:DD HH:MM:SS
        snprintf(b, sizeof b, "%04lu:%02lu:%02lu %02lu:%02lu:%02lu",
                 dt.ulYear, dt.ulMonth, dt.ulDay, dt.ulHour, dt.ulMinute,
                 (unsigned long)(dt.dbSecond + 0.5));
        return b;
    } catch (...) { return ""; }
}

// 镜头信息（kNkfl_Tag_NkLensInfo 结构）格式化为 "24-70mm f/2.8" 风格字符串
std::string lensString(unsigned long sid) {
    NkflTagDataParam p = {};
    p.ulSize = sizeof(NkflTagDataParam);
    p.ulSessionID = sid;
    p.ulTagID = kNkfl_Tag_NkLensInfo;
    p.ulTagType = kNkfl_TagType_LensInfo;
    try {
        if (Nkfl_Entry(kNkfl_Cmd_GetTagData, &p) != kNkfl_Code_None) return "";
        if (p.ulTagLength != sizeof(NkflLensInfo)) return "";
        NkflLensInfo li = {};
        p.pData = &li;
        if (Nkfl_Entry(kNkfl_Cmd_GetTagData, &p) != kNkfl_Code_None) return "";
        if (li.ulWideLength == 0 && li.ulTeleLength == 0) return "";
        char b[128];
        if (li.ulTeleLength == 0 || li.ulTeleLength == li.ulWideLength) {
            snprintf(b, sizeof b, "%lumm f/%.1f", li.ulWideLength, li.dbWideMaxAperture);
        } else {
            snprintf(b, sizeof b, "%lu-%lumm f/%.1f-%.1f",
                     li.ulWideLength, li.ulTeleLength,
                     li.dbWideMaxAperture, li.dbTeleMaxAperture);
        }
        return b;
    } catch (...) { return ""; }
}

} // namespace

SdkDecoder::~SdkDecoder() { close(); }

bool SdkDecoder::open(const std::string &path, std::string *err) {
    close();
    unsigned long e = ensureLibraryOpen();
    if (e != kNkfl_Code_None) {
        if (err) {
            if (e == kNkfl_Code_Err_Unexpected) {
                *err = "SDK initialization failed (" + hexCode(e) +
                       "). Usually the Nikon color profiles are missing: "
                       "install them with scripts/install_profiles.sh (see README)";
            } else {
                *err = "SDK initialization failed (" + hexCode(e) + ")";
            }
        }
        return false;
    }
    libraryOpen_ = true;

    NkflSessionParam sess = {};
    sess.ulSize    = sizeof(NkflSessionParam);
    sess.ulType    = kNkfl_Source_FileName_UTF8;
    sess.pFileInfo = (void *)path.c_str();
    unsigned long errCode = kNkfl_Code_Err_Unexpected;
    try {
        errCode = Nkfl_Entry(kNkfl_Cmd_OpenSession, &sess);
    } catch (...) {
        errCode = kNkfl_Code_Err_Unexpected;
    }
    if (errCode != kNkfl_Code_None && !isWarningCode(errCode)) {
        if (err) {
            switch (errCode) {
                case kNkfl_Code_Err_FileNotFound:
                    *err = "file not found: " + path;
                    break;
                case kNkfl_Code_Err_NotSupported:
                    *err = "format not supported by the SDK: " + path;
                    break;
                case kNkfl_Code_Err_Unexpected:
                case kNkfl_Code_Err_FileIO:
                    *err = "cannot open as NEF (" + hexCode(errCode) +
                           "): " + path +
                           " (not a NEF file, or the file is corrupt/empty)";
                    break;
                default:
                    *err = "SDK OpenSession failed (" + hexCode(errCode) + "): " + path;
                    break;
            }
        }
        releaseLibrary();
        libraryOpen_ = false;
        return false;
    }
    sessionId_ = sess.ulSessionID;
    opened_ = true;
    path_ = path;

    // 与官方示例一致：设置 sRGB 输出配置文件（缺失时 SDK 色彩转换可能退化/极慢）
    {
        NkflOutputProfileParam prof = {};
        prof.ulSize            = sizeof(NkflOutputProfileParam);
        prof.ulSessionID       = sessionId_;
        prof.ulRenderingIntent = kNkfl_RenderingIntent_Relative;
        const char *srgb = "/Library/Application Support/Nikon/Profiles/NKsRGB.icm";
        memcpy(prof.OutputProfile, srgb, strlen(srgb) + 1);
        try { Nkfl_Entry(kNkfl_Cmd_SetOutputProfile_UTF8, &prof); } catch (...) {}
    }
    return true;
}

bool SdkDecoder::decode(DecodedImage *out, bool autoOrient, std::string *err) {
    if (!opened_) { if (err) *err = "decoder not opened"; return false; }
    unsigned long sid = sessionId_;
    auto fail = [&](const std::string &m) { if (err) *err = m; return false; };

    // 图像信息
    NkflImageInfoParam ii = {};
    ii.ulSize = sizeof(NkflImageInfoParam);
    ii.ulSessionID = sid;
    try {
        if (Nkfl_Entry(kNkfl_Cmd_GetImageInfo, &ii) != kNkfl_Code_None)
            return fail("SDK GetImageInfo failed");
    } catch (...) { return fail("SDK GetImageInfo threw"); }

    // 请求 8-bit RGB；autoOrient 时输出正向（CW0）
    NkflImageInfoParam oi = ii;
    oi.ulByteDepth   = 1;
    oi.ulColor       = kNkfl_Color_RGB;
    oi.ulOrientation = autoOrient ? kNkfl_Orientation_CW0 : ii.ulOrientation;
    try {
        if (Nkfl_Entry(kNkfl_Cmd_SetImageInfo, &oi) != kNkfl_Code_None)
            return fail("SDK SetImageInfo failed");
        NkflImageInfoParam outInfo = {};
        outInfo.ulSize = sizeof(NkflImageInfoParam);
        outInfo.ulSessionID = sid;
        if (Nkfl_Entry(kNkfl_Cmd_GetImageInfo, &outInfo) != kNkfl_Code_None)
            return fail("SDK GetImageInfo(after set) failed");
        oi = outInfo;
    } catch (...) { return fail("SDK SetImageInfo threw"); }

    // 尺寸/格式校验（防损坏/伪造 NEF 触发越界或超大分配）
    if (oi.ulWidth == 0 || oi.ulHeight == 0)
        return fail("image has zero dimensions");
    if (oi.ulWidth > 32767 || oi.ulHeight > 32767)
        return fail("image dimensions too large for the SDK rect API");
    if (oi.ulByteDepth != 1)
        return fail("SDK did not honor 8-bit output request");
    if (oi.ulColor != kNkfl_Color_RGB)
        return fail("SDK did not honor RGB output request");

    size_t w = oi.ulWidth, h = oi.ulHeight;
    size_t dataLen = w * h * oi.ulByteDepth * 3;
    if (dataLen > (size_t)512 << 20) // 上限 512MB（与 -j 内存封顶假设一致，防恶意尺寸 OOM）
        return fail("decoded image buffer too large");

    out->rgb.resize(dataLen);

    NkflImageParam ip = {};
    ip.ulSize      = sizeof(NkflImageParam);
    ip.ulSessionID = sid;
    ip.rectArea.right  = (short)w;
    ip.rectArea.bottom = (short)h;
    ip.ulDataSize  = (unsigned long)dataLen;
    ip.pData       = out->rgb.data();

    unsigned long e = kNkfl_Code_Err_Unexpected;
    try {
        e = Nkfl_Entry(kNkfl_Cmd_GetImageData, &ip);
    } catch (...) { e = kNkfl_Code_Err_Unexpected; }
    if (e != kNkfl_Code_None) {
        return fail("SDK GetImageData failed (" + hexCode(e) + ")");
    }

    out->width  = (int)w;
    out->height = (int)h;
    // autoOrient 时像素已被 SDK 转正，EXIF 方向应写 1，避免看图软件二次旋转
    out->orientation = autoOrient ? 1 : (uint16_t)ii.ulOrientation;

    // 读取基本 EXIF 文本标签
    out->date_time = dateTimeString(sid);
    out->make      = tagString(sid, kNkfl_Tag_Make);
    out->model     = tagString(sid, kNkfl_Tag_Model);
    out->lens      = lensString(sid);
    return true;
}

void SdkDecoder::close() {
    if (opened_) {
        NkflSessionParam sess = {};
        sess.ulSize = sizeof(NkflSessionParam);
        sess.ulSessionID = sessionId_;
        try { Nkfl_Entry(kNkfl_Cmd_CloseSession, &sess); } catch (...) {}
        opened_ = false;
        sessionId_ = 0;
    }
    if (libraryOpen_) {
        releaseLibrary();
        libraryOpen_ = false;
    }
}

#endif // NEF2JPG_USE_SDK
