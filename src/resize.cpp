#include "resize.h"

#include <algorithm>

void resizeRgb(const uint8_t *src, int sw, int sh,
               uint8_t *dst, int dw, int dh) {
    if (dw <= 0 || dh <= 0) return;
    if (dw == sw && dh == sh) {
        // 直接复制（调用方很少走这里，仅兜底）
        for (int y = 0; y < dh; ++y)
            memcpy(dst + (size_t)y * dw * 3, src + (size_t)y * sw * 3, (size_t)dw * 3);
        return;
    }

    // 双线性（缩小为主；放大时表现为最近邻式的简单插值）
    const double sx = (double)sw / dw;
    const double sy = (double)sh / dh;

    for (int y = 0; y < dh; ++y) {
        double fy = sy * (y + 0.5) - 0.5;
        if (fy < 0) fy = 0;
        int y0 = (int)fy;
        int y1 = std::min(y0 + 1, sh - 1);
        double wy = fy - y0;

        const uint8_t *row0 = src + (size_t)y0 * sw * 3;
        const uint8_t *row1 = src + (size_t)y1 * sw * 3;
        uint8_t *out = dst + (size_t)y * dw * 3;

        for (int x = 0; x < dw; ++x) {
            double fx = sx * (x + 0.5) - 0.5;
            if (fx < 0) fx = 0;
            int x0 = (int)fx;
            int x1 = std::min(x0 + 1, sw - 1);
            double wx = fx - x0;

            for (int c = 0; c < 3; ++c) {
                double v =
                    row0[x0 * 3 + c] * (1 - wx) * (1 - wy) +
                    row0[x1 * 3 + c] * wx * (1 - wy) +
                    row1[x0 * 3 + c] * (1 - wx) * wy +
                    row1[x1 * 3 + c] * wx * wy;
                out[x * 3 + c] = (uint8_t)(v + 0.5);
            }
        }
    }
}
