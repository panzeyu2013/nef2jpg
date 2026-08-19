// bench_sdk.cpp — 基准测试：SDK 在不同输出尺寸 / 分块下的解码耗时
#include <Carbon/Carbon.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sys/sysctl.h>
#include <unistd.h>
#include <vector>
#include "Nkfl_Interface.h"

static unsigned long sid = 0;

static double ms(std::chrono::steady_clock::time_point a,
                 std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// 解码指定输出尺寸（宽高按比例），返回耗时(ms)
// fast=true 时关闭降噪（RawDevelopment NR=OFF）
static double benchSize(const char *path, unsigned long outW, unsigned long outH,
                        bool fullRect, bool fast = false) {
    NkflImageInfoParam ii = {};
    ii.ulSize = sizeof(NkflImageInfoParam);
    ii.ulSessionID = sid;
    if (Nkfl_Entry(kNkfl_Cmd_GetImageInfo, &ii) != 0) return -1;

    if (fast) {
        NkflRawDevelopment_NR nr = {};
        nr.ulSize = sizeof(NkflRawDevelopment_NR);
        nr.ulNRType = kNkfl_NR_OFF;
        NkflRawDevelopmentParam rdp = {};
        rdp.ulSize = sizeof(NkflRawDevelopmentParam);
        rdp.ulSessionID = sid;
        rdp.ulRawDevelopment = kNkfl_RawDevelopment_NR;
        rdp.pData = &nr;
        Nkfl_Entry(kNkfl_Cmd_RawDevelopment, &rdp);
    }

    NkflImageInfoParam oi = ii;
    oi.ulWidth = outW;
    oi.ulHeight = outH;
    oi.ulByteDepth = 1;
    oi.ulColor = kNkfl_Color_RGB;
    oi.ulOrientation = kNkfl_Orientation_CW0;
    if (Nkfl_Entry(kNkfl_Cmd_SetImageInfo, &oi) != 0) return -1;

    NkflImageInfoParam out = {};
    out.ulSize = sizeof(NkflImageInfoParam);
    out.ulSessionID = sid;
    Nkfl_Entry(kNkfl_Cmd_GetImageInfo, &out);
    size_t w = out.ulWidth, h = out.ulHeight;

    std::vector<unsigned char> buf(w * h * 3);
    NkflImageParam ip = {};
    ip.ulSize = sizeof(NkflImageParam);
    ip.ulSessionID = sid;
    if (fullRect) { ip.rectArea.right = (short)w; ip.rectArea.bottom = (short)h; }
    else { ip.rectArea.right = (short)(w / 2); ip.rectArea.bottom = (short)(h / 2); }
    ip.ulDataSize = (unsigned long)buf.size();
    ip.pData = buf.data();

    auto t0 = std::chrono::steady_clock::now();
    unsigned long e = Nkfl_Entry(kNkfl_Cmd_GetImageData, &ip);
    auto t1 = std::chrono::steady_clock::now();
    if (e != 0) { printf("  GetImageData err 0x%08lX\n", e); return -1; }
    return ms(t0, t1);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <file.nef>\n", argv[0]); return 1; }
    const char *path = argv[1];

    NkflLibraryParam lib = {};
    lib.ulSize = sizeof(NkflLibraryParam);
    lib.ulVersion = 0x01000000;
    uint64_t ram = 0; size_t sz = sizeof(ram);
    sysctlbyname("hw.memsize", &ram, &sz, NULL, 0);
    lib.ulVMMemorySize = (unsigned long)((ram / 1024 / 1024) / 4);
    snprintf((char *)lib.VMFileInfo, MAX_PATH, "/tmp/NkImgSDK_%d.tmp", (int)getpid());
    unsigned long e = Nkfl_Entry(kNkfl_Cmd_OpenLibrary, &lib);
    if (e != 0) { printf("OpenLibrary failed 0x%08lX\n", e); return 1; }

    NkflSessionParam sess = {};
    sess.ulSize = sizeof(NkflSessionParam);
    sess.ulType = kNkfl_Source_FileName_UTF8;
    sess.pFileInfo = (void *)path;
    e = Nkfl_Entry(kNkfl_Cmd_OpenSession, &sess);
    if (e != 0) { printf("OpenSession failed 0x%08lX\n", e); return 1; }
    sid = sess.ulSessionID;

    // 与 decode_nef 一致：设置 sRGB 输出配置
    {
        NkflOutputProfileParam prof = {};
        prof.ulSize = sizeof(NkflOutputProfileParam);
        prof.ulSessionID = sid;
        prof.ulRenderingIntent = kNkfl_RenderingIntent_Relative;
        const char *srgb = "/Library/Application Support/Nikon/Profiles/NKsRGB.icm";
        memcpy(prof.OutputProfile, srgb, strlen(srgb) + 1);
        Nkfl_Entry(kNkfl_Cmd_SetOutputProfile_UTF8, &prof);
    }

    NkflImageInfoParam ii = {};
    ii.ulSize = sizeof(NkflImageInfoParam);
    ii.ulSessionID = sid;
    Nkfl_Entry(kNkfl_Cmd_GetImageInfo, &ii);
    printf("native: %lux%lu\n", ii.ulWidth, ii.ulHeight);

    // 全尺寸（多跑几次取稳定值）
    for (int i = 0; i < 2; ++i)
        printf("full  %lux%lu: %.0f ms\n", ii.ulWidth, ii.ulHeight,
               benchSize(path, ii.ulWidth, ii.ulHeight, true));
    printf("half  %lux%lu: %.0f ms\n", ii.ulWidth / 2, ii.ulHeight / 2,
           benchSize(path, ii.ulWidth / 2, ii.ulHeight / 2, true));
    printf("quart %lux%lu: %.0f ms\n", ii.ulWidth / 4, ii.ulHeight / 4,
           benchSize(path, ii.ulWidth / 4, ii.ulHeight / 4, true));
    // 1/4 tile（全尺寸下只取左上 1/4 区域）
    printf("tile1/4 (full-size rect 1/4): %.0f ms\n",
           benchSize(path, ii.ulWidth, ii.ulHeight, false));
    // 关闭降噪后的全尺寸耗时
    printf("full  no-NR: %.0f ms\n",
           benchSize(path, ii.ulWidth, ii.ulHeight, true, true));

    NkflSessionParam cs = {}; cs.ulSize = sizeof(NkflSessionParam); cs.ulSessionID = sid;
    Nkfl_Entry(kNkfl_Cmd_CloseSession, &cs);
    Nkfl_Entry(kNkfl_Cmd_CloseLibrary, NULL);
    return 0;
}
