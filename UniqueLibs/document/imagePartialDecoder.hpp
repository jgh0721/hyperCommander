#pragma once
#include <QBuffer>
#include <QImageReader>
#include <functional>

namespace hc::document
{
    // Preview pixels are provisional and never enter the decoded-source cache.
    class ImagePartialDecoder
    {
    public:
        static QImage Decode(const QByteArray& Bytes)
        {
            if(!Bytes.startsWith(QByteArray("\xff\xd8",2)))return {};
            QBuffer Input;Input.setData(Bytes);Input.open(QIODevice::ReadOnly);
            QImageReader Reader(&Input,"jpeg");Reader.setAutoTransform(true);
            const auto Size=Reader.size();if(!Size.isValid())return {};
            Reader.setScaledSize(Size.scaled(1024,1024,Qt::KeepAspectRatio));
            return Reader.read();
        }
        void Update(const QByteArray& Bytes,const std::function<void(QImage)>& Publish)
        {
            if(!Publish || Bytes.size()<next_ || next_>32*1024*1024)return;
            next_=qMax(next_*2,Bytes.size()*2);
            auto Image=Decode(Bytes);if(!Image.isNull())Publish(std::move(Image));
        }
    private:
        qsizetype next_=512*1024;
    };
}
