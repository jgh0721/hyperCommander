#pragma once

/// 플랫폼/프레임워크 헤더파일
#include <QColorSpace>
#include <QImage>
#include <QJsonObject>
#include <utility>

namespace hc::document
{
    inline constexpr int LoadingImageExtent = 192;
    inline constexpr int LoadingImagePixelBytes = 4;

    inline QJsonObject EncodeLoadingImageFrame(const QImage& Image)
    {
        const QSize Bounds(LoadingImageExtent, LoadingImageExtent);
        auto Thumb = Image.width() > Bounds.width() || Image.height() > Bounds.height()
            ? Image.scaled(Bounds, Qt::KeepAspectRatio, Qt::SmoothTransformation) : Image;
        if (Thumb.colorSpace().isValid()) {
            auto Converted = Thumb.convertedToColorSpace(QColorSpace(QColorSpace::SRgb), QImage::Format_RGBA8888);
            if (!Converted.isNull()) Thumb = std::move(Converted);
        }
        Thumb = Thumb.convertToFormat(QImage::Format_RGBA8888);
        if (Thumb.isNull()) return {};
        // At most 192 KiB after base64. Only the active provisional frame travels with DocumentState.
        const auto Data = QByteArray::fromRawData(reinterpret_cast<const char*>(Thumb.constBits()), Thumb.sizeInBytes());
        return {{"width", Thumb.width()}, {"height", Thumb.height()}, {"rgba", QString::fromLatin1(Data.toBase64())}};
    }

    inline QImage DecodeLoadingImageFrame(const QJsonObject& Frame)
    {
        const auto Width = Frame["width"].toInt(), Height = Frame["height"].toInt();
        if (Width < 1 || Width > LoadingImageExtent || Height < 1 || Height > LoadingImageExtent) return {};
        const auto Expected = Width * Height * LoadingImagePixelBytes;
        const auto Text = Frame["rgba"].toString();
        if (Text.size() != ((Expected + 2) / 3) * 4) return {};
        const auto Data = QByteArray::fromBase64Encoding(Text.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!Data || Data.decoded.size() != Expected) return {};
        const QImage View(reinterpret_cast<const uchar*>(Data.decoded.constData()), Width, Height,
            Width * LoadingImagePixelBytes, QImage::Format_RGBA8888);
        return View.copy();
    }
}
