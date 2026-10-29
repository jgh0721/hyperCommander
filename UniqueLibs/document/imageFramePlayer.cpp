#include "imageFramePlayer.hpp"
#include <QBuffer>
#include <QFutureWatcher>
#include <QImageReader>
#include <QtConcurrent>
namespace hc::document
{
struct ImageFramePlayer::Decoder
{
    QByteArray bytes_;
    QBuffer buffer_;
    std::unique_ptr<QImageReader> reader_;
    int index_ = -1;
    explicit Decoder(QByteArray Bytes) : bytes_(std::move(Bytes)), buffer_(&bytes_)
    {
        reset();
    }
    void reset()
    {
        reader_.reset();
        buffer_.close();
        buffer_.open(QIODevice::ReadOnly);
        reader_ = std::make_unique<QImageReader>(&buffer_);
        reader_->setAutoTransform(true);
        index_ = -1;
    }
};
namespace
{
struct FrameResult
{
    QImage image_;
    int delay_ = 100, loops_ = 0, count_ = 0;
    bool supported_ = false, end_ = false;
};
} // namespace
ImageFramePlayer::ImageFramePlayer(QObject *Parent) : QObject(Parent)
{
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] { decode(index_ + 1); });
}
ImageFramePlayer::~ImageFramePlayer()
{
    timer_.stop();
    ++generation_;
}
void ImageFramePlayer::Load(QByteArray Bytes)
{
    timer_.stop();
    playing_ = false;
    decoder_ = std::make_shared<Decoder>(std::move(Bytes));
    index_ = -1;
    supported_ = false;
    frameCount_ = 0;
    loops_ = 0;
    ++generation_;
    decode(0);
}
void ImageFramePlayer::Clear()
{
    timer_.stop();
    playing_ = false;
    decoder_.reset();
    index_ = -1;
    supported_ = false;
    loops_ = 0;
    ++generation_;
    emit Changed();
}
void ImageFramePlayer::Play(bool Enabled)
{
    playing_ = Enabled;
    if (!Enabled)
        timer_.stop();
    else if (!busy_)
        decode(index_ + 1);
    emit Changed();
}
void ImageFramePlayer::Step(int Direction)
{
    Play(false);
    if (!busy_)
        decode(qMax(0, index_ + Direction));
}
void ImageFramePlayer::decode(int Index)
{
    if (!decoder_ || busy_)
        return;
    busy_ = true;
    const auto Decoder = decoder_;
    const auto Generation = generation_;
    auto *Watcher = new QFutureWatcher<FrameResult>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this,
            [this, Watcher, Generation, Index]
            {
                auto Result = Watcher->result();
                Watcher->deleteLater();
                busy_ = false;
                if (Generation != generation_)
                {
                    decode(0);
                    return;
                }
                supported_ = Result.supported_;
                frameCount_ = Result.count_;
                if (Result.end_)
                {
                    if (playing_ && (Result.loops_ < 0 || loops_ < Result.loops_))
                    {
                        ++loops_;
                        index_ = -1;
                        decode(0);
                    }
                    else
                        Play(false);
                    return;
                }
                index_ = Index;
                if (!Result.image_.isNull())
                    emit Frame(Result.image_);
                if (playing_ && supported_)
                    timer_.start(qBound(10, Result.delay_, 60000));
                else if (!supported_)
                    playing_ = false;
                emit Changed();
            });
    Watcher->setFuture(QtConcurrent::run(
        [Decoder, Index]
        {
            if (Index <= Decoder->index_)
                Decoder->reset();
            FrameResult Result;
            Result.supported_ = Decoder->reader_->supportsAnimation();
            Result.count_ = Decoder->reader_->imageCount();
            Result.loops_ = Decoder->reader_->loopCount();
            while (Decoder->index_ < Index)
            {
                auto Image = Decoder->reader_->read();
                if (Image.isNull())
                {
                    Result.end_ = true;
                    return Result;
                }
                ++Decoder->index_;
                Result.image_ = std::move(Image);
            }
            Result.delay_ = Decoder->reader_->nextImageDelay();
            return Result;
        }));
}
} // namespace hc::document
