#pragma once
#include <QImage>
#include <QColorSpace>
#include <QTransform>
#include <QString>
#include <QRect>
#include <functional>
#include <stop_token>
#include <vector>
namespace hc::document
{
    struct ImageFilter
    {
        QString id_, label_;
        double support_;
        bool elliptical_ = false;
        std::function<double(double)> kernel_;
    };
    class ImageFilterRegistry
    {
    public:
        static std::vector<ImageFilter> Filters();
        static bool Register(ImageFilter Filter);
        static ImageFilter Resolve(const QString& Id, const QTransform& Transform);
    };
    struct ImageRenderRequest
    {
        QImage source_;
        QSize outputSize_;
        QTransform sourceToOutput_;
        QString filter_ = "auto";
        // Tiles use global output coordinates. Every sample reads the full halo
        // from the immutable source, so a tile edge never becomes an image edge.
        QRect tile_;
        std::stop_token stop_;
        bool transparentBoundary_ = false;
        QColorSpace targetColor_;
    };
    struct ImageRenderResult { QImage image_; QString error_; };
    ImageRenderResult ResampleImage(const ImageRenderRequest& Request);
    QString ImageCacheKey(std::uint64_t Revision, const ImageRenderRequest& Request, qreal Dpr);
}
