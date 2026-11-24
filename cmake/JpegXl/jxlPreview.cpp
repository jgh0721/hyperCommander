#include "stdafx.h"
#include "jxlPreview.hpp"

/// 외부 라이브러리
#include <jxl/decode.h>

namespace
{
    class JxlPreview
    {
    public:
        explicit JxlPreview(QSize Bounds) : bounds_(Bounds), decoder_(JxlDecoderCreate(nullptr), JxlDecoderDestroy) {}

        bool Initialize() const
        {
            return decoder_ && JxlDecoderSetUnpremultiplyAlpha(decoder_.get(), JXL_TRUE) == JXL_DEC_SUCCESS
                && JxlDecoderSubscribeEvents(decoder_.get(),
                JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME_PROGRESSION | JXL_DEC_FULL_IMAGE) == JXL_DEC_SUCCESS;
        }

        bool Update(QByteArrayView Bytes, QImage& Image)
        {
            if (Bytes.size() <= consumed_) return true;
            const auto Tail = Bytes.sliced(consumed_);
            if (JxlDecoderSetInput(decoder_.get(), reinterpret_cast<const uint8_t*>(Tail.data()), Tail.size()) != JXL_DEC_SUCCESS)
                return false;
            const auto ReleaseInput = qScopeGuard([&] { consumed_ = Bytes.size() - qsizetype(JxlDecoderReleaseInput(decoder_.get())); });
            for (;;) {
                switch (JxlDecoderProcessInput(decoder_.get())) {
                case JXL_DEC_BASIC_INFO:
                    if (!prepareImage()) return false;
                    break;
                case JXL_DEC_COLOR_ENCODING:
                    setColorSpace();
                    break;
                case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
                    if (JxlDecoderSetImageOutCallback(decoder_.get(), &PixelFormat, writePixels, this) != JXL_DEC_SUCCESS)
                        return false;
                    break;
                case JXL_DEC_FRAME_PROGRESSION:
                    if (JxlDecoderFlushImage(decoder_.get()) == JXL_DEC_SUCCESS && hasPixels_) {
                        Image = std::move(image_);
                        return false;
                    }
                    break;
                case JXL_DEC_NEED_MORE_INPUT:
                    return true;
                default:
                    return false;
                }
            }
        }

    private:
        bool prepareImage()
        {
            JxlBasicInfo Info{};
            if (JxlDecoderGetBasicInfo(decoder_.get(), &Info) != JXL_DEC_SUCCESS || Info.have_animation
                || Info.num_color_channels != 3 || Info.exponent_bits_per_sample != 0
                || !Info.xsize || !Info.ysize || quint64(Info.xsize) * Info.ysize > 64ULL * 1024 * 1024)
                return false;
            const auto Width = size_t(Info.xsize), Height = size_t(Info.ysize);
            const auto HorizontalStep = (Width + bounds_.width() - 1) / bounds_.width();
            const auto VerticalStep = (Height + bounds_.height() - 1) / bounds_.height();
            step_ = std::max(HorizontalStep, VerticalStep);
            image_ = QImage(int((Width + step_ - 1) / step_), int((Height + step_ - 1) / step_), QImage::Format_RGBA8888);
            if (image_.isNull()) return false;
            image_.fill(Qt::transparent);
            return true;
        }

        void setColorSpace()
        {
            size_t Size = 0;
            if (JxlDecoderGetICCProfileSize(decoder_.get(), JXL_COLOR_PROFILE_TARGET_DATA, &Size) != JXL_DEC_SUCCESS
                || !Size || Size > 4 * 1024 * 1024) return;
            QByteArray Profile(qsizetype(Size), Qt::Uninitialized);
            if (JxlDecoderGetColorAsICCProfile(decoder_.get(), JXL_COLOR_PROFILE_TARGET_DATA,
                reinterpret_cast<uint8_t*>(Profile.data()), Size) == JXL_DEC_SUCCESS)
                image_.setColorSpace(QColorSpace::fromIccProfile(Profile));
        }

        static void writePixels(void* State, size_t X, size_t Y, size_t Count, const void* Pixels)
        {
            auto& Preview = *static_cast<JxlPreview*>(State);
            const auto Step = Preview.step_;
            if (Y % Step || Y / Step >= size_t(Preview.image_.height())) return;
            auto* Row = Preview.image_.scanLine(int(Y / Step));
            const auto* Input = static_cast<const uchar*>(Pixels);
            for (auto Column = (X + Step - 1) / Step * Step;
                Column < X + Count && Column / Step < size_t(Preview.image_.width()); Column += Step) {
                std::memcpy(Row + (Column / Step) * 4, Input + (Column - X) * 4, 4);
                Preview.hasPixels_ = true;
            }
        }

        static constexpr JxlPixelFormat PixelFormat{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
        QSize bounds_;
        std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> decoder_;
        QImage image_;
        qsizetype consumed_ = 0;
        size_t step_ = 1;
        bool hasPixels_ = false;
    };
}

extern "C" void* HC_CreateJxlPreview(const QSize* Bounds)
{
    if (!Bounds || Bounds->isEmpty()) return nullptr;
    auto Preview = std::make_unique<JxlPreview>(Bounds->boundedTo(QSize(4096, 4096)));
    return Preview->Initialize() ? Preview.release() : nullptr;
}

extern "C" void HC_DestroyJxlPreview(void* State)
{
    delete static_cast<JxlPreview*>(State);
}

extern "C" bool HC_UpdateJxlPreview(void* State, const QByteArray* Bytes, QImage* Image)
{
    return static_cast<JxlPreview*>(State)->Update(*Bytes, *Image);
}
