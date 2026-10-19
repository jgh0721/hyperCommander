#include <QImageIOPlugin>
#include <QImageIOHandler>
#include <QImage>
#include <QColorSpace>
#include <QIODevice>
#include <QBuffer>
#include <QTransform>
#include <QDebug>
#include <QThread>
#include <QtEndian>
#include <libheif/heif.h>
#include <avif/avif.h>
#include <cstring>
#include <limits>
#include <memory>

namespace
{
QColorSpace ColorSpace(int Primaries, int Transfer)
{
    const auto P = Primaries == 9    ? QColorSpace::Primaries::Bt2020
                   : Primaries == 12 ? QColorSpace::Primaries::DciP3D65
                                     : QColorSpace::Primaries::SRgb;
    const auto T = Transfer == 16                                      ? QColorSpace::TransferFunction::St2084
                   : Transfer == 18                                    ? QColorSpace::TransferFunction::Hlg
                   : Transfer == 8                                     ? QColorSpace::TransferFunction::Linear
                   : Transfer == 1 || Transfer == 14 || Transfer == 15 ? QColorSpace::TransferFunction::Bt2020
                                                                       : QColorSpace::TransferFunction::SRgb;
    return QColorSpace(P, T);
}
bool Avif(QIODevice *Device)
{
    if (!Device)
        return false;
    const auto Bytes = Device->peek(256);
    avifROData Raw{reinterpret_cast<const uint8_t *>(Bytes.constData()), size_t(Bytes.size())};
    return avifPeekCompatibleFileType(&Raw);
}
bool Heif(QIODevice *Device)
{
    if (!Device)
        return false;
    const auto Bytes = Device->peek(256);
    if (Bytes.size() < 12)
        return false;
    const auto Type = heif_check_filetype(reinterpret_cast<const uint8_t *>(Bytes.constData()), int(Bytes.size()));
    if (Type == heif_filetype_yes_supported)
        return true;
    if (Type != heif_filetype_maybe || Bytes.mid(4, 4) != "ftyp")
        return false;
    const auto BoxSize = qFromBigEndian<quint32>(Bytes.constData());
    for (int At = 8; At + 4 <= Bytes.size() && quint32(At + 4) <= BoxSize; At += 4)
    {
        if (At == 12)
            continue;
        const auto Brand = Bytes.mid(At, 4);
        if (Brand == "heic" || Brand == "heix" || Brand == "hevc" || Brand == "hevx")
            return true;
    }
    return false;
}
class ModernHandler final : public QImageIOHandler
{
  public:
    ~ModernHandler() override
    {
        if (handle_)
            heif_image_handle_release(handle_);
        if (heif_)
            heif_context_free(heif_);
        if (avif_)
            avifDecoderDestroy(avif_);
    }
    bool canRead() const override
    {
        return parsed_ ? !size_.isEmpty() : device() && (Avif(device()) || Heif(device()));
    }
    bool supportsOption(ImageOption Option) const override
    {
        return Option == Size || Option == ImageFormat || Option == ImageTransformation || Option == Animation;
    }
    QVariant option(ImageOption Option) const override
    {
        if (Option == Animation)
            return false;
        if (Option == ImageTransformation)
            return int(TransformationNone);
        auto *Self = const_cast<ModernHandler *>(this);
        if (!Self->parse())
            return {};
        if (Option == Size)
            return size_;
        if (Option == ImageFormat)
            return int(depth_ > 8 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        return {};
    }
    bool read(QImage *Output) override
    {
        if (!Output || !parse())
            return false;
        if (avif_)
        {
            // Four threads saturated the large-image comparison; retain spare
            // cores for navigation and filters, and avoid overhead on small files.
            const int Threads=qMin(qMax(1,QThread::idealThreadCount()/4),size_.width()*qint64(size_.height())>=1000000?4:2);
            avif_->maxThreads=Threads;
            const auto DecodeResult = avifDecoderNextImage(avif_);
            if (DecodeResult != AVIF_RESULT_OK)
            {
                qWarning() << "AVIF decode:" << avifResultToString(DecodeResult) << avif_->diag.error;
                return false;
            }
            const auto *Image = avif_->image;
            QImage Result(int(Image->width), int(Image->height),
                          depth_ > 8 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
            if (Result.isNull())
                return false;
            avifRGBImage RGB;
            avifRGBImageSetDefaults(&RGB, Image);
            RGB.format = AVIF_RGB_FORMAT_RGBA;
            RGB.depth = depth_ > 8 ? 16 : 8;
            RGB.pixels = Result.bits();
            RGB.rowBytes = uint32_t(Result.bytesPerLine());
            RGB.maxThreads = Threads;
            RGB.chromaUpsampling = AVIF_CHROMA_UPSAMPLING_BEST_QUALITY;
            // Preserve the exact high-bit conversion path. The measured 8-bit
            // SIMD conversion differs by at most two channel levels in the corpus.
            RGB.avoidLibYUV = depth_ > 8 ? AVIF_TRUE : AVIF_FALSE;
            const auto ConvertResult = avifImageYUVToRGB(Image, &RGB);
            if (ConvertResult != AVIF_RESULT_OK)
            {
                qWarning() << "AVIF RGB:" << avifResultToString(ConvertResult);
                return false;
            }
            auto Profile = Image->icc.size
                               ? QColorSpace::fromIccProfile(QByteArray(reinterpret_cast<const char *>(Image->icc.data),
                                                                        qsizetype(Image->icc.size)))
                               : ColorSpace(Image->colorPrimaries, Image->transferCharacteristics);
            if (Profile.isValid())
                Result.setColorSpace(Profile);
            if (Image->transformFlags & AVIF_TRANSFORM_CLAP)
            {
                avifCropRect Crop;
                avifDiagnostics Diagnostics;
                if (!avifCropRectConvertCleanApertureBox(&Crop, &Image->clap, Image->width, Image->height,
                                                         Image->yuvFormat, &Diagnostics))
                    return false;
                Result = Result.copy(int(Crop.x), int(Crop.y), int(Crop.width), int(Crop.height));
            }
            if (Image->transformFlags & AVIF_TRANSFORM_IROT)
                Result = Result.transformed(QTransform().rotate(-90 * Image->irot.angle));
            if (Image->transformFlags & AVIF_TRANSFORM_IMIR)
                Result = Result.flipped(Image->imir.axis == 0 ? Qt::Vertical : Qt::Horizontal);
            if (avif_->imageCount > 1)
                Result.setText("ImagePreview", "Animated AVIF: first frame only");
            Result.setText("SourceBitDepth", QString::number(depth_));
            *Output = std::move(Result);
            return true;
        }
        heif_image *Decoded = nullptr;
        auto Options = heif_decoding_options_alloc();
        if (!Options)
            return false;
        Options->convert_hdr_to_8bit = 0;
        const auto Chroma = depth_ > 8 ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RGBA;
        const auto Error = heif_decode_image(handle_, &Decoded, heif_colorspace_RGB, Chroma, Options);
        heif_decoding_options_free(Options);
        if (Error.code != heif_error_Ok || !Decoded)
            return false;
        std::unique_ptr<heif_image, decltype(&heif_image_release)> Owner(Decoded, heif_image_release);
        const int Width = heif_image_get_width(Decoded, heif_channel_interleaved),
                  Height = heif_image_get_height(Decoded, heif_channel_interleaved);
        QImage Result(Width, Height, depth_ > 8 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        if (Result.isNull())
            return false;
        int Stride = 0;
        const auto *Pixels = heif_image_get_plane_readonly(Decoded, heif_channel_interleaved, &Stride);
        const qsizetype RowBytes = qsizetype(Width) * (depth_ > 8 ? 8 : 4);
        if (!Pixels || Stride < RowBytes)
            return false;
        for (int Y = 0; Y < Height; ++Y)
            std::memcpy(Result.scanLine(Y), Pixels + qsizetype(Y) * Stride, RowBytes);
        if (depth_ > 8)
        {
            const int Bits = heif_image_get_bits_per_pixel_range(Decoded, heif_channel_interleaved);
            if (Bits > 8 && Bits < 16)
            {
                const quint32 Max = (1u << Bits) - 1;
                for (int Y = 0; Y < Height; ++Y)
                {
                    auto *Row = reinterpret_cast<quint16 *>(Result.scanLine(Y));
                    for (int X = 0; X < Width * 4; ++X)
                        Row[X] = quint16(qMin(quint32(Row[X]), Max) * 65535u / Max);
                }
            }
        }
        // libheif applies irot/imir/clap during decode; report no further Qt transform.
        const auto ProfileSize = heif_image_handle_get_raw_color_profile_size(handle_);
        QColorSpace Profile;
        if (ProfileSize && ProfileSize <= size_t(std::numeric_limits<int>::max()))
        {
            QByteArray Data(qsizetype(ProfileSize), Qt::Uninitialized);
            if (heif_image_handle_get_raw_color_profile(handle_, Data.data()).code == heif_error_Ok)
                Profile = QColorSpace::fromIccProfile(Data);
        }
        if (!Profile.isValid())
        {
            heif_color_profile_nclx *Nclx = nullptr;
            if (heif_image_handle_get_nclx_color_profile(handle_, &Nclx).code == heif_error_Ok && Nclx)
            {
                Profile = ColorSpace(Nclx->color_primaries, Nclx->transfer_characteristics);
                heif_nclx_color_profile_free(Nclx);
            }
        }
        if (Profile.isValid())
            Result.setColorSpace(Profile);
        Result.setText("SourceBitDepth", QString::number(depth_));
        *Output = std::move(Result);
        return true;
    }

  private:
    bool parse()
    {
        if (parsed_)
            return !size_.isEmpty();
        parsed_ = true;
        if (!device())
            return false;
        const bool IsAvif = Avif(device());
        // The document loader already owns immutable compressed bytes. Reuse
        // QBuffer storage instead of copying the whole HEIC/AVIF payload again.
        if(auto* Buffer=qobject_cast<QBuffer*>(device());Buffer&&Buffer->pos()==0){bytes_=Buffer->data();Buffer->seek(Buffer->size());}
        else bytes_ = device()->readAll();
        if (bytes_.isEmpty())
            return false;
        if (IsAvif)
        {
            avif_ = avifDecoderCreate();
            if (!avif_)
                return false;
            avif_->maxThreads = 2;
            avif_->strictFlags &= ~AVIF_STRICT_PIXI_REQUIRED;
            if (avifDecoderSetIOMemory(avif_, reinterpret_cast<const uint8_t *>(bytes_.constData()),
                                       size_t(bytes_.size())) != AVIF_RESULT_OK)
                return false;
            const auto ParseResult = avifDecoderParse(avif_);
            if (ParseResult != AVIF_RESULT_OK)
            {
                qWarning() << "AVIF parse:" << avifResultToString(ParseResult) << avif_->diag.error;
                return false;
            }
            if (avif_->image->width > uint32_t(std::numeric_limits<int>::max()) ||
                avif_->image->height > uint32_t(std::numeric_limits<int>::max()))
                return false;
            size_ = QSize(int(avif_->image->width), int(avif_->image->height));
            depth_ = avif_->image->depth;
            return true;
        }
        heif_ = heif_context_alloc();
        if (!heif_)
            return false;
        if (heif_context_read_from_memory_without_copy(heif_, bytes_.constData(), size_t(bytes_.size()), nullptr)
                .code != heif_error_Ok)
            return false;
        if (heif_context_get_primary_image_handle(heif_, &handle_).code != heif_error_Ok)
            return false;
        size_ = QSize(heif_image_handle_get_width(handle_), heif_image_handle_get_height(handle_));
        depth_ = heif_image_handle_get_luma_bits_per_pixel(handle_);
        return !size_.isEmpty();
    }
    bool parsed_ = false;
    QByteArray bytes_;
    QSize size_;
    int depth_ = 8;
    heif_context *heif_ = nullptr;
    heif_image_handle *handle_ = nullptr;
    avifDecoder *avif_ = nullptr;
};
} // namespace
class ModernImagePlugin final : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "modernImagePlugin.json")
  public:
    Capabilities capabilities(QIODevice *Device, const QByteArray &Format) const override
    {
        if (!Format.isEmpty())
            return Format == "heic" || Format == "heif" || Format == "avif" ? CanRead : Capabilities{};
        return Avif(Device) || Heif(Device) ? CanRead : Capabilities{};
    }
    QImageIOHandler *create(QIODevice *Device, const QByteArray &Format) const override
    {
        auto *Handler = new ModernHandler;
        Handler->setDevice(Device);
        Handler->setFormat(Format);
        return Handler;
    }
};
#include "modernImagePlugin.moc"
