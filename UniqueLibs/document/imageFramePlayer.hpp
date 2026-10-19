#pragma once
#include <QObject>
#include <QImage>
#include <QTimer>
#include <memory>
namespace hc::document
{
class ImageFramePlayer final : public QObject
{
    Q_OBJECT
  public:
    explicit ImageFramePlayer(QObject *Parent = nullptr);
    ~ImageFramePlayer() override;
    void Load(QByteArray Bytes);
    void Clear();
    bool IsBusy() const
    {
        return busy_;
    }
    void Play(bool Enabled);
    void Step(int Direction);
    bool Playing() const
    {
        return playing_;
    }
    bool Supported() const
    {
        return supported_;
    }
  signals:
    void Frame(const QImage &Image);
    void Changed();

  private:
    struct Decoder;
    void decode(int Index);
    std::shared_ptr<Decoder> decoder_;
    QTimer timer_;
    bool playing_ = false, busy_ = false, supported_ = false;
    int index_ = -1, loops_ = 0;
    quint64 generation_ = 0;
};
} // namespace hc::document
