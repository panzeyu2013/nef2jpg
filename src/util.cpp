#include "util.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <glob.h>
#include <sys/stat.h>
#include <sys/types.h>

std::string errnoMsg() {
    char buf[256];
#if defined(__APPLE__) || defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE)
    if (strerror_r(errno, buf, sizeof buf) == 0) return std::string(buf);
    return "unknown error";
#else
    return std::string(strerror_r(errno, buf, sizeof buf));
#endif
}

bool copyFile(const std::string &src, const std::string &dst, std::string *err) {
    FILE *in = fopen(src.c_str(), "rb");
    if (!in) { if (err) *err = errnoMsg() + ": " + src; return false; }
    FILE *out = fopen(dst.c_str(), "wb");
    if (!out) {
        fclose(in);
        if (err) *err = errnoMsg() + ": " + dst;
        return false;
    }
    char buf[1 << 16];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    if (fclose(out) != 0) ok = false;
    fclose(in);
    if (!ok) {
        if (err) *err = "copy failed: " + src + " -> " + dst;
        return false;
    }
    return true;
}

bool fileExists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool makeDirs(const std::string &path) {
    if (path.empty()) return true;
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return true;
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        char c = (i < path.size()) ? path[i] : '/';
        if (c == '/' || i == path.size()) {
            if (!cur.empty() && cur != "/") {
                if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
            }
            cur += c;
        } else {
            cur += c;
        }
    }
    struct stat st2;
    return stat(path.c_str(), &st2) == 0 && S_ISDIR(st2.st_mode);
}

static bool endsWithIgnoreCase(const std::string &s, const char *suffix) {
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != std::tolower((unsigned char)suffix[i]))
            return false;
    return true;
}

static bool isDir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static const int kMaxScanDepth = 64;

static void scanDir(const std::string &dir, bool recursive, int depth,
                    std::vector<std::string> *out) {
    if (depth > kMaxScanDepth) return;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dir + "/" + name;

        bool is_directory = false;
        switch (e->d_type) {
            case DT_DIR: is_directory = true; break;
            case DT_LNK: {
                // 符号链接：按目标类型处理
                struct stat st;
                if (stat(full.c_str(), &st) == 0) is_directory = S_ISDIR(st.st_mode);
                break;
            }
            case DT_REG: is_directory = false; break;
            case DT_UNKNOWN: {
                // 不提供 d_type 的文件系统（NFS 等）：回退 stat（与 DT_LNK 一致，跟随链接）
                struct stat st;
                if (stat(full.c_str(), &st) == 0) {
                    if (S_ISDIR(st.st_mode)) is_directory = true;
                    else if (!S_ISREG(st.st_mode)) continue; // 其它类型跳过
                } else {
                    continue;
                }
                break;
            }
            default: continue;
        }

        if (is_directory) {
            if (recursive) scanDir(full, recursive, depth + 1, out);
        } else {
            if (endsWithIgnoreCase(name, ".nef")) out->push_back(full);
        }
    }
    closedir(d);
}

static void warn(const std::string &m) { fprintf(stderr, "nef2jpg: %s\n", m.c_str()); }

bool collectInputs(const std::vector<std::string> &inputs, bool recursive,
                   std::vector<std::string> *files, std::string *err) {
    (void)err; // 当前实现不产生致命错误，缺失输入走告警
    for (const auto &in : inputs) {
        if (isDir(in)) {
            scanDir(in, recursive, 0, files);
            continue;
        }
        bool hasGlob = in.find_first_of("*?[") != std::string::npos;

        if (hasGlob) {
            // 字面量优先：真实文件名含 *?[ 时按字面使用
            if (fileExists(in)) {
                files->push_back(in);
                continue;
            }
            glob_t g;
            memset(&g, 0, sizeof(g));
            if (glob(in.c_str(), GLOB_TILDE, nullptr, &g) == 0) {
                for (size_t i = 0; i < g.gl_pathc; ++i) {
                    if (!isDir(g.gl_pathv[i])) files->push_back(g.gl_pathv[i]);
                }
            } else {
                warn("no match for: " + in);
            }
            globfree(&g);
        } else {
            if (!fileExists(in)) {
                warn("input not found (skipped): " + in);
                continue;
            }
            if (!endsWithIgnoreCase(in, ".nef"))
                warn("input does not look like a .NEF file: " + in);
            files->push_back(in);
        }
    }
    // 去重并排序
    std::sort(files->begin(), files->end());
    files->erase(std::unique(files->begin(), files->end()), files->end());
    return true;
}

std::string outputPathFor(const std::string &inputPath, const std::string &outDir,
                          const std::string &suffix) {
    std::string base = inputPath;
    size_t slash = base.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? "." : base.substr(0, slash);
    std::string name = (slash == std::string::npos) ? base : base.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    std::string out = outDir.empty() ? dir : outDir;
    if (!out.empty() && out.back() != '/') out += "/";
    return out + name + suffix + ".jpg";
}
