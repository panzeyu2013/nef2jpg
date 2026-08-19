#include "pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "decoder.h"
#include "exif.h"
#include "jpeg_encoder.h"
#include "resize.h"
#include "util.h"

namespace {

struct Job {
    std::string input;
    std::string output;
};

// 日志文件名转义：控制字符/ANSI 转义序列不可见化
std::string sanitizeName(const std::string &s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (u < 0x20 || u == 0x7f) {
            char b[8];
            snprintf(b, sizeof b, "\\x%02X", u);
            r += b;
        } else {
            r += c;
        }
    }
    return r;
}

void processJob(const Job &job, const Options &opts, Summary *summary,
                std::mutex *printMutex, const std::atomic<bool> *stopFlag) {
    (void)stopFlag; // 当前文件开始后执行到底（中断在任务边界生效）
    std::string err;

    auto fail = [&](const std::string &m) {
        {
            std::lock_guard<std::mutex> lock(*printMutex);
            summary->failed++;
            fprintf(stderr, "[FAIL] %s: %s\n", sanitizeName(job.input).c_str(), m.c_str());
        }
    };
    auto warn = [&](const std::string &m) {
        std::lock_guard<std::mutex> lock(*printMutex);
        fprintf(stderr, "[WARN] %s: %s\n", sanitizeName(job.input).c_str(), m.c_str());
    };

    std::string encPath, exifPath; // 临时文件路径（try 外声明，异常时 catch 可清理）
    try {
        using Clock = std::chrono::steady_clock;
        auto t0 = Clock::now();

        std::unique_ptr<Decoder> dec = Decoder::create(opts.backend, &err);
        if (!dec) { fail(err.empty() ? "no decoder available" : err); return; }

        if (!dec->open(job.input, &err)) { fail(err); return; }
        auto tOpen = Clock::now();

        DecodedImage img;
        if (!dec->decode(&img, opts.auto_orient, &err)) { fail(err); dec->close(); return; }
        dec->close();
        auto tDecode = Clock::now();

        // resize
        if (opts.resize_max_dim > 0) {
            int maxSide = std::max(img.width, img.height);
            if (maxSide > opts.resize_max_dim) {
                double scale = (double)opts.resize_max_dim / maxSide;
                int nw = std::max(1, (int)(img.width * scale + 0.5));
                int nh = std::max(1, (int)(img.height * scale + 0.5));
                std::vector<uint8_t> small((size_t)nw * nh * 3);
                resizeRgb(img.rgb.data(), img.width, img.height, small.data(), nw, nh);
                img.width = nw;
                img.height = nh;
                img.rgb.swap(small);
            }
        }

        // JPEG 编码（临时文件 + 原子 rename；EXIF 在副本上写，失败不毁原图）
        JpegOptions jopt;
        jopt.quality = opts.quality;
        jopt.progressive = opts.progressive;
        jopt.subsampling = opts.subsampling;
        jopt.embedIcc = opts.embed_icc;
        jopt.iccPath = opts.icc_path;
        jopt.targetSize = opts.target_size;
        jopt.maxSize = opts.max_size;
        jopt.tolerancePct = opts.tolerance_pct;

        // mkstemp 生成不可预测的临时文件名
        char tmpl[4096];
        snprintf(tmpl, sizeof tmpl, "%s.tmpXXXXXX", job.output.c_str());
        int fd = mkstemp(tmpl);
        if (fd < 0) { fail("cannot create temp file for output"); return; }
        close(fd);
        encPath = tmpl;                        // 编码产物
        exifPath = encPath + ".x";             // EXIF 副本

        if (!encodeJpeg(img, encPath, jopt, &err)) {
            unlink(encPath.c_str());
            fail(err);
            return;
        }
        auto tEncode = Clock::now();
        if (!err.empty()) warn(err); // encode 成功但带警告（如 max-size 无法满足）

        bool exifOk = false;
        {
            std::string exifErr;
            // 复制后写 EXIF：失败时丢弃副本，仍可用未污染的编码文件
            if (copyFile(encPath, exifPath, &exifErr)) {
                if (writeExif(exifPath, img, &exifErr)) {
                    exifOk = true;
                } else {
                    warn("exif: " + exifErr);
                    unlink(exifPath.c_str());
                }
            } else {
                warn("exif copy: " + exifErr);
                unlink(exifPath.c_str());
            }
        }
        auto tExif = Clock::now();

        const char *finalSrc = exifOk ? exifPath.c_str() : encPath.c_str();
        if (rename(finalSrc, job.output.c_str()) != 0) {
            fail("cannot finalize output: " + errnoMsg() + " (" + job.output + ")");
            unlink(encPath.c_str());
            unlink(exifPath.c_str());
            return;
        }
        unlink(encPath.c_str());
        if (exifOk) unlink(exifPath.c_str());

        struct stat st;
        int64_t bytes = 0;
        if (stat(job.output.c_str(), &st) == 0) bytes = (int64_t)st.st_size;

        {
            std::lock_guard<std::mutex> lock(*printMutex);
            summary->ok++;
            summary->totalBytes += bytes;
            if (opts.verbose) {
                auto ms = [](auto a, auto b) {
                    return std::chrono::duration<double, std::milli>(b - a).count();
                };
                fprintf(stdout,
                        "[OK] %s -> %s (%lld bytes, %dx%d) | open=%.0fms decode=%.0fms "
                        "encode=%.0fms exif=%.0fms total=%.0fms\n",
                        sanitizeName(job.input).c_str(), sanitizeName(job.output).c_str(),
                        (long long)bytes, img.width, img.height,
                        ms(t0, tOpen), ms(tOpen, tDecode), ms(tDecode, tEncode),
                        ms(tEncode, tExif), ms(t0, tExif));
            } else {
                fprintf(stdout, "[OK] %s -> %s (%lld bytes)\n",
                        sanitizeName(job.input).c_str(), sanitizeName(job.output).c_str(),
                        (long long)bytes);
            }
        }
    } catch (const std::exception &e) {
        if (!encPath.empty()) unlink(encPath.c_str());
        if (!exifPath.empty()) unlink(exifPath.c_str());
        fail(std::string("exception: ") + e.what());
    } catch (...) {
        if (!encPath.empty()) unlink(encPath.c_str());
        if (!exifPath.empty()) unlink(exifPath.c_str());
        fail("unknown exception");
    }
}

} // namespace

Summary runPipeline(const Options &opts, const std::vector<std::string> &files,
                    const std::atomic<bool> *stopFlag) {
    Summary summary;
    summary.total = (int)files.size();

    // 生成任务：输出路径去重 + 跳过已存在
    std::vector<Job> jobs;
    std::set<std::string> seenOutputs;
    for (const auto &f : files) {
        std::string out = outputPathFor(f, opts.out_dir, opts.suffix);
        if (!seenOutputs.insert(out).second) {
            summary.skipped++;
            if (opts.verbose)
                fprintf(stdout, "[WARN] output collision, skipped: %s\n",
                        sanitizeName(out).c_str());
            continue;
        }
        if (!opts.overwrite && fileExists(out)) {
            summary.skipped++;
            if (opts.verbose)
                fprintf(stdout, "[SKIP] %s (exists)\n", sanitizeName(out).c_str());
            continue;
        }
        jobs.push_back({f, out});
    }
    if (summary.skipped > 0 && opts.verbose)
        fprintf(stdout, "[INFO] skipped %d output(s)\n", summary.skipped);

    // 输出目录
    if (!opts.out_dir.empty() && !opts.dry_run && !makeDirs(opts.out_dir)) {
        fprintf(stderr, "[FATAL] cannot create output dir: %s\n",
                sanitizeName(opts.out_dir).c_str());
        summary.failed += (int)jobs.size();
        return summary;
    }

    auto t0 = std::chrono::steady_clock::now();

    if (opts.dry_run) {
        for (const auto &j : jobs)
            fprintf(stdout, "[DRY-RUN] %s -> %s\n",
                    sanitizeName(j.input).c_str(), sanitizeName(j.output).c_str());
        summary.elapsedSec = 0;
        return summary;
    }

    int nThreads = std::max(1, std::min(opts.jobs, (int)jobs.size()));
    std::atomic<size_t> next{0};
    std::mutex printMutex;

    auto worker = [&] {
        for (;;) {
            if (stopFlag && stopFlag->load())
                break;
            size_t idx = next.fetch_add(1);
            if (idx >= jobs.size()) break;
            processJob(jobs[idx], opts, &summary, &printMutex, stopFlag);
        }
    };

    if (nThreads == 1) {
        // 单任务/单线程时直接在主线程处理（尼康 SDK 在主线程更稳定）
        worker();
    } else {
        std::vector<std::thread> threads;
        for (int i = 0; i < nThreads; ++i) threads.emplace_back(worker);
        for (auto &t : threads) t.join();
    }

    auto t1 = std::chrono::steady_clock::now();
    summary.elapsedSec = std::chrono::duration<double>(t1 - t0).count();
    return summary;
}
