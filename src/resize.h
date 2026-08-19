#pragma once
#include <cstdint>

// 双线性缩放 8-bit 交错 RGB；仅支持缩小的场景（dw<=sw, dh<=sh），
// 若目标更大则退化为最近邻。
void resizeRgb(const uint8_t *src, int sw, int sh,
               uint8_t *dst, int dw, int dh);
