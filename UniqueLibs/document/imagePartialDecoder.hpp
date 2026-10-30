#pragma once
#include <QBuffer>
#include "imagePngPreview.hpp"
#include <QCoreApplication>
#include <QLibrary>
#include <QImageReader>
#include <QElapsedTimer>
#include "imagePerformance.hpp"
#include <functional>

namespace hc::document
{
    // Preview pixels are provisional and never enter the decoded-source cache.
    class ImagePartialDecoder
    {
    public:
        static QImage DecodeCompletePreview(const QByteArray& Bytes, QSize Bounds=QSize(1024,1024))
        {
            QBuffer Input;Input.setData(Bytes);Input.open(QIODevice::ReadOnly);
            QImageReader Reader(&Input);Reader.setAutoTransform(true);
            const auto Size=Reader.size();
            // Ask the codec, not the filename. Codecs without native scaled
            // decoding use the single full decode below, never a second full decode.
            if(!Size.isValid() || (Size.width()<=Bounds.width() && Size.height()<=Bounds.height())
                || !Reader.supportsOption(QImageIOHandler::ScaledSize))return {};
            ImageTraceSpan Trace("image.decode-preview");
            Reader.setScaledSize(Size.scaled(Bounds,Qt::KeepAspectRatio));
            return Reader.read();
        }
        static bool HasCompleteJpeg(const QByteArray& Bytes)
        {
            if (!Bytes.startsWith(QByteArray("\xff\xd8", 2))) return false;
            qsizetype Offset = 2;
            bool Scan = false;
            while (Offset < Bytes.size()) {
                if (Scan) {
                    Offset = Bytes.indexOf(char(0xff), Offset);
                    if (Offset < 0) return false;
                } else if (static_cast<unsigned char>(Bytes[Offset]) != 0xff) return false;
                while (Offset < Bytes.size() && static_cast<unsigned char>(Bytes[Offset]) == 0xff) ++Offset;
                if (Offset == Bytes.size()) return false;
                const auto Marker = static_cast<unsigned char>(Bytes[Offset++]);
                if (Marker == 0x00) { if (!Scan) return false; continue; }
                if (Marker == 0xd9) return true;
                if (Marker == 0xd8) return false;
                if (Marker >= 0xd0 && Marker <= 0xd7) { if (!Scan) return false; continue; }
                if (Marker == 0x01) continue;
                if (Bytes.size() - Offset < 2) return false;
                const auto Length = (int(static_cast<unsigned char>(Bytes[Offset])) << 8)
                    | int(static_cast<unsigned char>(Bytes[Offset + 1]));
                if (Length < 2 || Length > Bytes.size() - Offset) return false;
                // Skip metadata, including embedded thumbnails and their EOI.
                // After the real EOI, cameras may append motion-photo payloads.
                Offset += Length;
                Scan = Marker == 0xda || (Scan && Marker == 0xdc);
            }
            return false;
        }
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
            if(!elapsed_.isValid())elapsed_.start();
            // Fast local reads should reach EOF once; repeated partial decodes
            // at 512 KiB/1 MiB/... otherwise delay every supported codec.
            if(elapsed_.elapsed()<100)return;
            if(!Publish || Bytes.size()<next_ || next_>32*1024*1024)return;
            next_=qMax(next_*2,Bytes.size()*2);
            elapsed_.restart();
            auto Image=Decode(Bytes);if(!Image.isNull())Publish(std::move(Image));
        }
    private:
        QElapsedTimer elapsed_;
        qsizetype next_=512*1024;
    };
}
