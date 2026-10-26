#pragma once
#include <QBuffer>
#include "imagePngPreview.hpp"
#include <QCoreApplication>
#include <QLibrary>
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
            if(Bytes.startsWith(QByteArray("\x89PNG\r\n\x1a\n",8)))return DecodePngPreview(Bytes);
            const bool Webp=Bytes.startsWith("RIFF")&&Bytes.mid(8,4)=="WEBP",Avif=Bytes.mid(4,4)=="ftyp";
            if(Webp||Avif){
                struct Plugin {
                    using Decode=bool(*)(const QByteArray*,QImage*);
                    QLibrary library_;Decode webp_=nullptr,avif_=nullptr;
                    Plugin():library_(QCoreApplication::applicationDirPath()+"/Libs/imageformats/qhcmodern.dll"){
                        library_.setLoadHints(QLibrary::ResolveAllSymbolsHint|QLibrary::PreventUnloadHint);
                        if(library_.load()){webp_=reinterpret_cast<Decode>(library_.resolve("HC_ReadWebpPreview"));avif_=reinterpret_cast<Decode>(library_.resolve("HC_ReadAvifPreview"));}
                    }
                };
                static Plugin Codec;const auto Decode=Webp?Codec.webp_:Codec.avif_;QImage Image;
                return Decode&&Decode(&Bytes,&Image)?Image:QImage{};
            }
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
