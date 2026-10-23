#include "stdafx.h"
#include "extensionEngine.hpp"
#include <QBuffer>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QtEndian>

namespace hc::compare
{
    namespace
    {
        constexpr qint64 maxPixels = 4 * 1024 * 1024;
        QSize pngSize(const QByteArray& Data)
        {
            if (Data.size() < 33 || !Data.startsWith(QByteArray::fromHex("89504e470d0a1a0a")) || Data.mid(12, 4) != "IHDR" ||
                qFromBigEndian<quint32>(Data.constData() + 8) != 13) throw Failure("InvalidPng");
            const auto width = qFromBigEndian<quint32>(Data.constData() + 16), height = qFromBigEndian<quint32>(Data.constData() + 20);
            if (!width || !height || width > 8192 || height > 8192 || qint64(width) * height > maxPixels) throw Failure("ImagePixelLimit");
            if (quint8(Data[24]) == 16) throw Failure("Png16BitUnsupported");
            qsizetype offset = 8; bool ended = false;
            while (offset <= Data.size() - 12)
            {
                const auto length = qFromBigEndian<quint32>(Data.constData() + offset);
                if (length > quint64(Data.size() - offset - 12)) throw Failure("TruncatedPng");
                const auto type = Data.mid(offset + 4, 4);
                if (type == "acTL") throw Failure("AnimatedPngUnsupported");
                offset += qsizetype(length) + 12;
                if (type == "IEND") { if (length) throw Failure("InvalidPng"); ended = true; break; }
            }
            if (!ended || offset != Data.size()) throw Failure("TruncatedOrTrailingPng");
            return {int(width), int(height)};
        }
        QString digest(QByteArrayView Bytes)
        { return QString::fromLatin1(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex()); }
    }
    QJsonObject PngWorkerRequest(const QJsonObject& Payload)
    {
        const auto resources = Payload["resources"].toArray();
        if (resources.size() < 1 || resources.size() > 2) throw Failure("ImageArityUnsupported");
        qunsetenv("QT_IMAGEIO_MAXALLOC"); QImageReader::setAllocationLimit(64);
        QJsonArray documents; QVector<QImage> images; QVector<QByteArray> sources;
        Budget limits; limits.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        for (const auto& value : resources)
        {
            limits.Check(); const auto encoded = value.toString(); if (encoded.size() > 24 * 1024 * 1024) throw Failure("InputLimit");
            auto decoded = QByteArray::fromBase64Encoding(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (!decoded) throw Failure("InvalidInputEncoding");
            auto source = std::move(decoded.decoded); if (source.size() > 16 * 1024 * 1024) throw Failure("InputLimit");
            const auto size = pngSize(source); QBuffer buffer(&source); buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer, "png"); reader.setAutoDetectImageFormat(false); reader.setAutoTransform(false);
            auto image = reader.read(); if (image.isNull() || image.size() != size) throw Failure("PngDecodeFailed");
            image = image.convertToFormat(QImage::Format_RGBA8888); if (image.isNull()) throw Failure("PngDecodeFailed");
            QJsonObject document{{"width", image.width()}, {"height", image.height()}, {"bitDepth", int(quint8(source[24]))},
                {"rawSha256", digest(source)}, {"pixelSha256", digest(QByteArrayView(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()))},
                {"profileSha256", digest(image.colorSpace().iccProfile())}, {"colorProfilePresent", image.colorSpace().isValid()}};
            if (Payload["raster"].toBool())
            {
                auto view = image.colorSpace().isValid() ? image.convertedToColorSpace(QColorSpace::SRgb, QImage::Format_RGBA8888) : image;
                if (view.isNull()) throw Failure("ImageColorConversionFailed");
                document["rgba"] = QString::fromLatin1(QByteArrayView(reinterpret_cast<const char*>(view.constBits()), view.sizeInBytes()).toByteArray().toBase64());
            }
            documents.append(document); images.push_back(std::move(image)); sources.push_back(std::move(source));
        }
        QJsonArray pairs;
        if (images.size() == 2)
        {
            const auto& a = images[0]; const auto& b = images[1]; const auto width = std::min(a.width(), b.width()), height = std::min(a.height(), b.height());
            qint64 different = qint64(a.width()) * a.height() + qint64(b.width()) * b.height() - 2 * qint64(width) * height;
            int minX = std::max(a.width(), b.width()), minY = std::max(a.height(), b.height()), maxX = -1, maxY = -1;
            if (a.size() != b.size()) { minX = minY = 0; maxX = std::max(a.width(), b.width()) - 1; maxY = std::max(a.height(), b.height()) - 1; }
            for (int y = 0; y < height; ++y)
            {
                limits.Check(); const auto* left = a.constScanLine(y); const auto* right = b.constScanLine(y);
                for (int x = 0; x < width; ++x)
                    if (memcmp(left + x * 4, right + x * 4, 4)) { ++different; minX = std::min(minX, x); minY = std::min(minY, y); maxX = std::max(maxX, x); maxY = std::max(maxY, y); }
            }
            const QJsonObject rectangle = different ? QJsonObject{{"x", minX}, {"y", minY}, {"width", maxX - minX + 1}, {"height", maxY - minY + 1}} : QJsonObject{};
            QJsonArray locations;
            for (const auto& value : documents)
            {
                const auto document = value.toObject(); QJsonObject local;
                if (different)
                {
                    const auto x = std::min(minX, document["width"].toInt()), y = std::min(minY, document["height"].toInt());
                    local = {{"x", x}, {"y", y}, {"width", std::max(0, std::min(maxX + 1, document["width"].toInt()) - x)},
                        {"height", std::max(0, std::min(maxY + 1, document["height"].toInt()) - y)}};
                }
                locations.append(QJsonObject{{"schema", "image.rect.v1"}, {"version", 1}, {"resourceRevision", "sha256:" + document["rawSha256"].toString()}, {"payload", local}});
            }
            pairs.append(QJsonObject{{"inputs", QJsonArray{0, 1}}, {"differentPixels", double(different)}, {"pixelEqual", different == 0},
                {"rawEqual", sources[0] == sources[1]}, {"bounds", rectangle}, {"locations", locations}});
        }
        return {{"status", "Complete"}, {"payloadSchema", "image.rgba.v1"}, {"documents", documents}, {"pairs", pairs},
            {"pixelPolicy", "decoded RGBA8 before color conversion; transparent RGB included"},
            {"displayPolicy", "sRGB conversion when profile is valid; otherwise sRGB assumed"}, {"merge", false}};
    }
}
