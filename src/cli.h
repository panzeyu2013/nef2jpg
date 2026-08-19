#pragma once
#include <cstdint>
#include <string>
#include <vector>

// 命令行参数
struct Options {
    // inputs
    std::vector<std::string> inputs; // files / globs / dirs
    bool recursive = false;
    bool dry_run = false;

    // output
    std::string out_dir;
    bool overwrite = false;
    std::string suffix = "_decoded";

    // concurrency
    int jobs = 0; // 0 = auto (hardware concurrency)

    // jpeg
    int quality = 90;            // 1..100
    int64_t target_size = 0;     // 0 = off
    int64_t max_size = 0;        // 0 = off
    int tolerance_pct = 5;       // target-size tolerance
    bool progressive = false;
    int subsampling = 420;       // 420 / 422 / 444
    bool embed_icc = true;

    // image processing
    int resize_max_dim = 0;      // 0 = off
    bool auto_orient = true;

    // misc
    std::string backend = "auto"; // v1: auto | sdk
    bool verbose = false;
};

enum class CliResult { Ok, Help, Error };

// 解析命令行；错误信息写入 err。返回 Ok 表示可执行，Help 表示打印帮助后退出。
CliResult parseCli(int argc, char **argv, Options *opts, std::string *err);

// 解析 "500K" / "1M" / "2.5M" / 纯数字(字节)
bool parseSize(const std::string &s, int64_t *out);

void printUsage(FILE *fp);
