#include "exif.h"

#ifdef NEF2JPG_WITH_EXIF
#include <exiv2/exiv2.hpp>
#endif

bool writeExif(const std::string &jpegPath, const DecodedImage &img, std::string *err) {
#ifdef NEF2JPG_WITH_EXIF
    try {
        auto image = Exiv2::ImageFactory::open(jpegPath);
        if (!image.get()) { if (err) *err = "exiv2: cannot open " + jpegPath; return false; }
        image->readMetadata();

        Exiv2::ExifData &ed = image->exifData();
        if (!img.make.empty())  ed["Exif.Image.Make"] = img.make;
        if (!img.model.empty()) ed["Exif.Image.Model"] = img.model;
        if (!img.date_time.empty()) {
            ed["Exif.Photo.DateTimeOriginal"] = img.date_time;
            ed["Exif.Image.DateTime"] = img.date_time;
        }
        if (!img.lens.empty()) ed["Exif.Photo.LensModel"] = img.lens;
        if (img.orientation >= 1 && img.orientation <= 8)
            ed["Exif.Image.Orientation"] = (uint16_t)img.orientation;
        ed["Exif.Image.Software"] = "nef2jpg " NEF2JPG_VERSION;
        image->writeMetadata();
        return true;
    } catch (const std::exception &e) {
        if (err) *err = std::string("exiv2: ") + e.what();
        return false;
    }
#else
    (void)jpegPath; (void)img; (void)err;
    return true; // 未启用 EXIF 编译
#endif
}
