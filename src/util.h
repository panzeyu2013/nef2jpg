#pragma once
#include <string>
#include <vector>

// 文件工具：glob 展开、目录递归扫描、mkdir -p、存在性检查
bool fileExists(const std::string &path);
bool makeDirs(const std::string &path); // mkdir -p（路径为目录）
std::string errnoMsg();                 // strerror(errno)（线程安全）
bool copyFile(const std::string &src, const std::string &dst, std::string *err);

// 收集输入：文件直接加入（缺失则告警并跳过）；含通配符先按字面量匹配、再 glob；
// 目录则递归/非递归扫描 .NEF。缺失文件/非 .NEF 的警告直接打到 stderr。
bool collectInputs(const std::vector<std::string> &inputs, bool recursive,
                   std::vector<std::string> *files, std::string *err);

// 由输入文件路径 + out_dir + suffix 计算输出路径
std::string outputPathFor(const std::string &inputPath, const std::string &outDir,
                          const std::string &suffix);
