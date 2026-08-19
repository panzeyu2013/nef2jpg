#include "cli.h"

#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

// 系统总内存（MB），用于并发数自动封顶
static long systemMemoryMB() {
#if defined(__APPLE__)
    uint64_t ram = 0;
    size_t sz = sizeof(ram);
    if (sysctlbyname("hw.memsize", &ram, &sz, NULL, 0) == 0)
        return (long)(ram / 1024 / 1024);
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && pageSize > 0)
        return (long)((double)pages * pageSize / 1024 / 1024);
#endif
    return 0;
}

// 严格解析大小："500K" / "1M" / "2.5M" / "500KB" / "1MB" / 纯数字(字节)
// 必须完整消费输入，拒绝 "1M2K"、"1.5.5M" 之类
bool parseSize(const std::string &s, int64_t *out) {
    if (s.empty()) return false;
    size_t i = 0;
    bool seenDot = false;
    while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) {
        if (s[i] == '.') {
            if (seenDot) return false;
            seenDot = true;
        }
        ++i;
    }
    if (i == 0) return false;
    std::string num = s.substr(0, i);
    char unit = 0;
    if (i < s.size()) {
        unit = (char)std::tolower((unsigned char)s[i++]);
        if (unit != 'k' && unit != 'm' && unit != 'g') return false;
        if (i < s.size() && (s[i] == 'b' || s[i] == 'B')) ++i; // 可选 B
        if (i != s.size()) return false; // 有剩余垃圾
    }
    char *end = nullptr;
    double v = std::strtod(num.c_str(), &end);
    if (end == num.c_str() || v < 0) return false;
    double mult;
    switch (unit) {
        case 'k': mult = 1024.0; break;
        case 'm': mult = 1024.0 * 1024.0; break;
        case 'g': mult = 1024.0 * 1024.0 * 1024.0; break;
        default:  mult = 1.0; break;
    }
    if (v * mult > (double)INT64_MAX) return false; // 溢出保护（含单位换算）
    *out = (int64_t)(v * mult);
    return true;
}

// 严格整数解析：必须完整消费输入
static bool readIntStrict(const char *s, int *out) {
    char *end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0') return false;
    if (v < INT_MIN || v > INT_MAX) return false;
    *out = (int)v;
    return true;
}

void printUsage(FILE *fp) {
    fprintf(fp,
        "nef2jpg %s - NEF to JPEG batch converter\n"
        "\n"
        "Usage: nef2jpg [options] <input>...\n"
        "\n"
        "输入\n"
        "  <input...>          一个或多个 .NEF 文件 / glob / 目录\n"
        "  -r, --recursive     递归扫描目录内的 .NEF\n"
        "      --dry-run       只预览任务，不执行\n"
        "\n"
        "输出\n"
        "  -o, --out-dir DIR   输出目录（默认：与输入同目录；不存在会自动创建）\n"
        "      --overwrite     覆盖已存在的输出（默认：跳过）\n"
        "      --suffix STR    输出文件名后缀（默认: _decoded）\n"
        "\n"
        "并发\n"
        "  -j, --jobs N        并发 worker 数（默认: min(CPU核数,8)，并按内存自动封顶；上限 64）\n"
        "\n"
        "JPEG 质量与大小（优先级: --max-size > --target-size > --quality）\n"
        "  -q, --quality N     JPEG 质量 1-100（默认: 90）。仅在没有 -s 时生效\n"
        "  -s, --target-size S 单张目标大小，如 500K / 1M / 2.5M。自动在质量上二分搜索逼近目标，\n"
        "                      此时 -q 作为二分起点（仍会被调整）\n"
        "      --max-size S    硬上限：超出则继续降质量直到满足或质量=1；无法满足时告警\n"
        "  -t, --tolerance PCT 目标大小容差 %%（默认: 5，上限 100）\n"
        "\n"
        "JPEG 编码\n"
        "      --progressive   渐进式编码：浏览器/看图软件可边下载边显示，文件稍大且部分\n"
        "                      老旧软件兼容性略差；默认关闭（基线式，兼容性最好）\n"
        "      --subsampling N 色度子采样 420 / 422 / 444（默认: 420）。\n"
        "                      420 体积最小，但精细彩色边缘（如红字、发丝）可能轻微渗色；\n"
        "                      444 无色度压缩、细节最锐、体积最大；422 折中。照片一般 420 即可\n"
        "      --no-icc        不嵌入色彩配置文件（默认嵌入 sRGB，保证看图软件颜色正确）\n"
        "      --icc FILE      嵌入自定义 ICC 配置文件（如 Adobe RGB / Display P3，\n"
        "                      文件需 1-65519 字节；与 --no-icc 互斥）\n"
        "\n"
        "图像\n"
        "  -w, --resize N      最大边缩到 N 像素（保持宽高比，只缩小不放大）\n"
        "      --no-auto-orient  不按 EXIF 方向自动转正（默认自动转正）\n"
        "\n"
        "其他\n"
        "      --backend NAME  解码后端：auto | sdk（默认: auto）\n"
        "  -v, --verbose       详细输出（含 open/decode/encode/exif 分阶段耗时）\n"
        "  -h, --help          显示本帮助\n"
        "\n"
        "退出码: 0 全部成功 / 1 部分失败 / 2 用法错误 / 130-143 被中断(Ctrl-C)\n"
        "\n"
        "示例:\n"
        "  nef2jpg photo.NEF\n"
        "  nef2jpg -j 4 -s 1M -o out/ *.NEF\n"
        "  nef2jpg -r -w 2000 --progressive -o out/ photos/\n"
        "  nef2jpg --icc DisplayP3.icc --q 95 photo.NEF\n",
        NEF2JPG_VERSION);
}

CliResult parseCli(int argc, char **argv, Options *opts, std::string *err) {
    auto fail = [&](const std::string &m) { *err = m; return CliResult::Error; };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto needValue = [&](const char *name) -> const char * {
            if (i + 1 < argc) return argv[++i];
            *err = std::string("missing value for ") + name;
            return nullptr;
        };

        if (a == "-h" || a == "--help") {
            printUsage(stdout);
            return CliResult::Help;
        } else if (a == "-o" || a == "--out-dir") {
            const char *v = needValue("--out-dir"); if (!v) return CliResult::Error;
            opts->out_dir = v;
        } else if (a == "-j" || a == "--jobs") {
            const char *v = needValue("--jobs"); if (!v) return CliResult::Error;
            if (!readIntStrict(v, &opts->jobs) || opts->jobs <= 0)
                return fail("invalid --jobs");
            if (opts->jobs > 64) return fail("--jobs too large (max 64)");
        } else if (a == "-q" || a == "--quality") {
            const char *v = needValue("--quality"); if (!v) return CliResult::Error;
            if (!readIntStrict(v, &opts->quality) || opts->quality < 1 || opts->quality > 100)
                return fail("quality must be 1..100");
        } else if (a == "-s" || a == "--target-size") {
            const char *v = needValue("--target-size"); if (!v) return CliResult::Error;
            if (!parseSize(v, &opts->target_size) || opts->target_size <= 0)
                return fail("invalid --target-size (e.g. 500K, 1M)");
        } else if (a == "--max-size") {
            const char *v = needValue("--max-size"); if (!v) return CliResult::Error;
            if (!parseSize(v, &opts->max_size) || opts->max_size <= 0)
                return fail("invalid --max-size");
        } else if (a == "-t" || a == "--tolerance") {
            const char *v = needValue("--tolerance"); if (!v) return CliResult::Error;
            if (!readIntStrict(v, &opts->tolerance_pct) || opts->tolerance_pct < 1 ||
                opts->tolerance_pct > 100)
                return fail("--tolerance must be 1..100");
        } else if (a == "--progressive") {
            opts->progressive = true;
        } else if (a == "--subsampling") {
            const char *v = needValue("--subsampling"); if (!v) return CliResult::Error;
            int n; if (!readIntStrict(v, &n)) return fail("invalid --subsampling");
            if (n != 420 && n != 422 && n != 444) return fail("--subsampling must be 420/422/444");
            opts->subsampling = n;
        } else if (a == "--no-icc") {
            opts->embed_icc = false;
        } else if (a == "--icc") {
            const char *v = needValue("--icc"); if (!v) return CliResult::Error;
            opts->icc_path = v;
        } else if (a == "-w" || a == "--resize") {
            const char *v = needValue("--resize"); if (!v) return CliResult::Error;
            if (!readIntStrict(v, &opts->resize_max_dim) || opts->resize_max_dim <= 0)
                return fail("invalid --resize");
        } else if (a == "--no-auto-orient") {
            opts->auto_orient = false;
        } else if (a == "--overwrite") {
            opts->overwrite = true;
        } else if (a == "--suffix") {
            const char *v = needValue("--suffix"); if (!v) return CliResult::Error;
            opts->suffix = v;
            if (opts->suffix.empty() || opts->suffix.find('/') != std::string::npos)
                return fail("--suffix must be non-empty and contain no '/'");
        } else if (a == "--backend") {
            const char *v = needValue("--backend"); if (!v) return CliResult::Error;
            std::string b = v;
            if (b != "auto" && b != "sdk")
                return fail("unknown --backend: " + b + " (allowed: auto, sdk)");
            opts->backend = b;
        } else if (a == "-r" || a == "--recursive") {
            opts->recursive = true;
        } else if (a == "--dry-run") {
            opts->dry_run = true;
        } else if (a == "-v" || a == "--verbose") {
            opts->verbose = true;
        } else if (!a.empty() && a[0] == '-') {
            return fail("unknown option: " + a);
        } else {
            opts->inputs.push_back(a);
        }
    }

    if (opts->inputs.empty())
        return fail("no input files specified (see -h)");

    // -j 默认值：min(CPU 核数, 8)，并按内存自动封顶（每 worker 约 512MB 峰值）
    if (opts->jobs == 0) {
        int hw = (int)std::thread::hardware_concurrency();
        if (hw <= 0) hw = 1;
        opts->jobs = std::min(hw, 8);
    }
    long memMB = systemMemoryMB();
    if (memMB > 0) {
        int memCap = (int)(memMB / 512);
        if (memCap < 1) memCap = 1;
        if (opts->jobs > memCap) opts->jobs = memCap;
    }

    // --icc 校验：与 --no-icc 互斥；文件存在且 ≤65519 字节（JPEG APP2 单 chunk 上限）
    if (!opts->icc_path.empty()) {
        if (!opts->embed_icc)
            return fail("--icc and --no-icc are mutually exclusive");
        FILE *f = fopen(opts->icc_path.c_str(), "rb");
        if (!f) return fail("--icc file not found: " + opts->icc_path);
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fclose(f);
        if (n <= 0 || n > 65519)
            return fail("--icc file must be 1..65519 bytes");
    }
    return CliResult::Ok;
}
