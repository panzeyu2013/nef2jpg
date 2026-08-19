#pragma once
#include "cli.h"
#include <atomic>
#include <string>
#include <vector>

struct Summary {
    int total = 0;
    int ok = 0;
    int failed = 0;
    int skipped = 0;
    int64_t totalBytes = 0; // 成功输出字节数
    double elapsedSec = 0;
};

// 执行批处理；files 为已收集的输入文件列表。
// stopFlag 非空时，worker 在任务边界检查并停止（用于 Ctrl-C）。
Summary runPipeline(const Options &opts, const std::vector<std::string> &files,
                    const std::atomic<bool> *stopFlag = nullptr);
