#include "jpeg_encoder.h"

#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <jpeglib.h>

namespace {

// ------------------------------------------------------------------
// libjpeg 错误处理器：默认 error_exit 会 exit()，必须改为 longjmp
// ------------------------------------------------------------------
struct JpegErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf jmp;
    char msg[JMSG_LENGTH_MAX];
};

void jpegErrorExit(j_common_ptr cinfo) {
    JpegErrorMgr *e = (JpegErrorMgr *)cinfo->err;
    (*cinfo->err->format_message)(cinfo, e->msg);
    longjmp(e->jmp, 1);
}

// ------------------------------------------------------------------
// 内存输出目标（RAII：正常/异常路径都释放）
// ------------------------------------------------------------------
struct MemDest {
    jpeg_destination_mgr pub;
    std::vector<uint8_t> *buf;
    explicit MemDest(std::vector<uint8_t> *b) : buf(b) {
        pub.init_destination = memInit;
        pub.empty_output_buffer = memEmpty;
        pub.term_destination = memTerm;
    }

    static void memInit(j_compress_ptr cinfo) {
        MemDest *d = (MemDest *)cinfo->dest;
        d->buf->clear();
        d->buf->resize(65536);
        d->pub.next_output_byte = d->buf->data();
        d->pub.free_in_buffer = d->buf->size();
    }

    static boolean memEmpty(j_compress_ptr cinfo) {
        MemDest *d = (MemDest *)cinfo->dest;
        size_t old = d->buf->size();
        d->buf->resize(old + 65536);
        d->pub.next_output_byte = d->buf->data() + old;
        d->pub.free_in_buffer = 65536;
        return TRUE;
    }

    static void memTerm(j_compress_ptr cinfo) {
        MemDest *d = (MemDest *)cinfo->dest;
        size_t written = d->buf->size() - d->pub.free_in_buffer;
        d->buf->resize(written);
    }
};

void setSubsampling(j_compress_ptr cinfo, int ss) {
    jpeg_set_colorspace(cinfo, JCS_YCbCr);
    int h1 = 1, v1 = 1, h2 = 1, v2 = 1;
    switch (ss) {
        case 420: h1 = 2; v1 = 2; break;
        case 422: h1 = 2; break;
        case 444: break;
        default: break;
    }
    cinfo->comp_info[0].h_samp_factor = (JDIMENSION)h1;
    cinfo->comp_info[0].v_samp_factor = (JDIMENSION)v1;
    cinfo->comp_info[1].h_samp_factor = (JDIMENSION)h2;
    cinfo->comp_info[1].v_samp_factor = (JDIMENSION)v2;
    cinfo->comp_info[2].h_samp_factor = (JDIMENSION)h2;
    cinfo->comp_info[2].v_samp_factor = (JDIMENSION)v2;
}

// ------------------------------------------------------------------
// 编码一次到内存；成功返回字节数，失败返回 -1 并设置 err
// ------------------------------------------------------------------
long encodeOnce(const DecodedImage &img, int quality, const JpegOptions &opt,
                const std::vector<uint8_t> &icc, std::vector<uint8_t> *out,
                std::string *err) {
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    if (img.width <= 0 || img.height <= 0) {
        if (err) *err = "invalid image dimensions";
        return -1;
    }
    if ((size_t)img.width * img.height * 3 != img.rgb.size()) {
        if (err) *err = "image buffer size mismatch";
        return -1;
    }

    jpeg_compress_struct cinfo;
    JpegErrorMgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpegErrorExit;

    if (setjmp(jerr.jmp)) {
        // libjpeg 致命错误：清理后返回
        std::string m = jerr.msg;
        if (cinfo.dest) delete (MemDest *)cinfo.dest;
        jpeg_destroy_compress(&cinfo);
        if (err) *err = "jpeg encode failed: " + m;
        return -1;
    }

    jpeg_create_compress(&cinfo);
    MemDest *dest = new MemDest(out);
    cinfo.dest = &dest->pub;

    cinfo.image_width = (JDIMENSION)img.width;
    cinfo.image_height = (JDIMENSION)img.height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    setSubsampling(&cinfo, opt.subsampling);
    jpeg_set_quality(&cinfo, quality, TRUE);
    cinfo.progressive_mode = opt.progressive ? TRUE : FALSE;
    cinfo.dct_method = JDCT_ISLOW;

    jpeg_start_compress(&cinfo, TRUE);

    // 嵌入 sRGB ICC（APP2 marker，单 chunk ≤65519 字节）
    if (opt.embedIcc && !icc.empty()) {
        if (icc.size() + 14 <= 65533) {
            std::vector<JOCTET> chunk(14 + icc.size());
            memcpy(chunk.data(), "ICC_PROFILE\0", 12);
            chunk[12] = 1; // sequence
            chunk[13] = 1; // total
            memcpy(chunk.data() + 14, icc.data(), icc.size());
            jpeg_write_marker(&cinfo, JPEG_APP0 + 2, chunk.data(),
                              (unsigned int)chunk.size());
        }
    }

    // RGB 连续交错，直接按行传指针（libjpeg 不要求行对齐）
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW rowptr[1] = {
            (JSAMPLE *)(img.rgb.data() + (size_t)cinfo.next_scanline * img.width * 3)
        };
        jpeg_write_scanlines(&cinfo, rowptr, 1);
    }

    jpeg_finish_compress(&cinfo);
    long size = (long)out->size();

    delete (MemDest *)cinfo.dest;
    jpeg_destroy_compress(&cinfo);
    return size;
}

} // namespace

std::vector<uint8_t> loadSrgbIccProfile() {
    // 优先系统 profiles 目录；其次可执行文件旁的 Contents/Resources
    const char *sysPath = "/Library/Application Support/Nikon/Profiles/NKsRGB.icm";
    FILE *f = fopen(sysPath, "rb");
    if (!f) {
#if defined(__APPLE__)
        // 相对可执行文件定位
        char exe[4096];
        uint32_t len = sizeof(exe);
        if (_NSGetExecutablePath(exe, &len) == 0) {
            std::string p(exe);
            size_t slash = p.find_last_of('/');
            if (slash != std::string::npos) p = p.substr(0, slash);
            p += "/Contents/Resources/NKsRGB.icm";
            f = fopen(p.c_str(), "rb");
        }
#endif
    }
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> v;
    if (n > 0 && n < 65519) {
        v.resize((size_t)n);
        if (fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear();
    }
    fclose(f);
    return v;
}

bool encodeJpeg(const DecodedImage &img, const std::string &path,
                const JpegOptions &opt, std::string *err) {
    std::vector<uint8_t> icc = opt.embedIcc ? loadSrgbIccProfile() : std::vector<uint8_t>{};
    std::vector<uint8_t> out;

    int quality = opt.quality;
    long size = -1;
    std::vector<uint8_t> finalBuf; // target-size 模式下复用已探明的最终缓冲

    if (opt.targetSize > 0) {
        // 目标大小：二分搜索质量；命中容差直接复用该次缓冲
        int lo = 2, hi = 100;
        long bestSize = -1;
        int64_t target = opt.targetSize;
        int64_t tol = (int64_t)(target * opt.tolerancePct / 100);
        if (tol < 1) tol = 1;
        for (int iter = 0; iter < 7 && lo <= hi; ++iter) {
            int q = (lo + hi) / 2;
            long s = encodeOnce(img, q, opt, icc, &out, err);
            if (s < 0) return false;
            if (bestSize < 0 ||
                llabs((long long)s - target) < llabs((long long)bestSize - target)) {
                bestSize = s;
                finalBuf.swap(out);
            }
            if (llabs((long long)s - target) <= tol) break;
            if (s > target) hi = q - 1;
            else lo = q + 1;
        }
        size = bestSize;
        out.swap(finalBuf); // out 恢复为最接近目标质量的缓冲
    } else {
        size = encodeOnce(img, quality, opt, icc, &out, err);
        if (size < 0) return false;
    }

    // 硬上限：超出则持续降质量（q=1 仍超限时告警）
    if (opt.maxSize > 0 && size > opt.maxSize) {
        int lo = 1, hi = quality - 1;
        while (lo <= hi && size > opt.maxSize) {
            int q = (lo + hi) / 2;
            long s = encodeOnce(img, q, opt, icc, &out, err);
            if (s < 0) return false;
            size = s;
            quality = q;
            if (s > opt.maxSize) hi = q - 1;
            else lo = q + 1;
        }
        if (size > opt.maxSize && err)
            *err = "cannot reach max-size " + std::to_string(opt.maxSize) +
                   " bytes even at quality 1 (actual " + std::to_string(size) + ")";
    }

    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { if (err) *err = "cannot open output: " + path; return false; }
    size_t written = fwrite(out.data(), 1, out.size(), f);
    bool closeOk = (fclose(f) == 0);
    if (written != out.size() || !closeOk) {
        if (err) *err = "failed writing output: " + path;
        return false;
    }
    return true;
}
