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
        "  Inputs: one or more .NEF files, globs, or directories (-r to recurse)\n"
        "\n"
        "  Output\n"
        "    -o, --out-dir DIR     output directory (default: same as input)\n"
        "        --overwrite       overwrite existing files (default: skip)\n"
        "        --suffix STR      output suffix (default: _decoded)\n"
        "\n"
        "  Concurrency\n"
        "    -j, --jobs N          worker threads (default: min(CPU cores, 8); "
        "auto-capped by RAM)\n"
        "\n"
        "  JPEG\n"
        "    -q, --quality 0-100   JPEG quality (default 90)\n"
        "    -s, --target-size S   per-image target size, e.g. 500K / 1M / 2.5M\n"
        "        --max-size S      hard size cap\n"
        "    -t, --tolerance PCT   target-size tolerance %% (default 5, max 100)\n"
        "        --progressive     progressive JPEG\n"
        "        --subsampling N   420 / 422 / 444 (default 420)\n"
        "        --no-icc          do not embed sRGB ICC profile\n"
        "\n"
        "  Image\n"
        "    -w, --resize MAXDIM   downscale so max side <= MAXDIM px\n"
        "        --no-auto-orient  do not auto-rotate by EXIF orientation\n"
        "\n"
        "  Misc\n"
        "        --backend NAME    decode backend: auto | sdk (v1)\n"
        "    -r, --recursive       scan directories recursively\n"
        "        --dry-run         print jobs without processing\n"
        "    -v, --verbose         verbose output\n"
        "    -h, --help            this help\n"
        "\n"
        "Exit codes: 0 all ok, 1 partial failure, 2 usage error, 130 interrupted\n",
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
    return CliResult::Ok;
}
