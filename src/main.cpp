#include <atomic>
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#include "cli.h"
#include "pipeline.h"
#include "util.h"

namespace {
std::atomic<bool> g_interrupted{false};

void onSignal(int) {
    // 只置标志；worker 在任务边界检查（信号处理器内不做任何非异步安全调用）
    g_interrupted.store(true);
}
} // namespace

int main(int argc, char **argv) {
    Options opts;
    std::string err;
    CliResult r = parseCli(argc, argv, &opts, &err);
    if (r == CliResult::Help) return 0;
    if (r == CliResult::Error) {
        fprintf(stderr, "nef2jpg: %s\n\n", err.c_str());
        printUsage(stderr);
        return 2;
    }

    // 收集输入（缺失文件告警并跳过，不中止）
    std::vector<std::string> files;
    if (!collectInputs(opts.inputs, opts.recursive, &files, &err)) {
        fprintf(stderr, "nef2jpg: %s\n", err.c_str());
        return 2;
    }
    if (files.empty()) {
        fprintf(stderr, "nef2jpg: no .NEF files found in the given inputs\n");
        return 2;
    }

    if (opts.verbose) {
        fprintf(stdout, "nef2jpg %s | inputs: %zu | jobs: %d | backend: %s\n",
                NEF2JPG_VERSION, files.size(), opts.jobs, opts.backend.c_str());
        fprintf(stdout, "  quality=%d target=%lld max=%lld progressive=%d subsampling=%d icc=%d\n",
                opts.quality, (long long)opts.target_size, (long long)opts.max_size,
                opts.progressive ? 1 : 0, opts.subsampling, opts.embed_icc ? 1 : 0);
    }

    // Ctrl-C / SIGTERM：置停止标志，worker 完成当前文件后退出
    struct sigaction sa = {};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    Summary s = runPipeline(opts, files, &g_interrupted);

    fprintf(stdout,
            "\nSummary: %d total, %d ok, %d failed, %d skipped, "
            "%lld bytes output, %.2fs elapsed%s\n",
            s.total, s.ok, s.failed, s.skipped,
            (long long)s.totalBytes, s.elapsedSec,
            g_interrupted.load() ? " (interrupted)" : "");

    if (g_interrupted.load()) return 130;      // 128 + SIGINT
    if (s.failed > 0) return 1;
    return 0;
}
