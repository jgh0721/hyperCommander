#pragma once
#include <QBuffer>
#include "imagePngPreview.hpp"
#include <QCoreApplication>
#include <QLibrary>
#include <QImageReader>
#include <QElapsedTimer>
#include "imagePerformance.hpp"
#include "cmake/JpegXl/jxlPreview.hpp"
#include <functional>
#include <algorithm>
#include <memory>

namespace hc::document
{
    // Preview pixels are provisional and never enter the decoded-source cache.
    class ImagePartialDecoder
    {
    public:
        explicit ImagePartialDecoder(QSize Bounds = {1024, 1024}) : bounds_(Bounds) {}

        static QImage DecodeCompletePreview(const QByteArray& Bytes, QSize Bounds=QSize(1024,1024))
        {
            QBuffer Input;Input.setData(Bytes);Input.open(QIODevice::ReadOnly);
            QImageReader Reader(&Input);Reader.setAutoTransform(true);
            const auto Size=Reader.size();
            // Ask the codec, not the filename. Codecs without native scaled
            // decoding use the single full decode below, never a second full decode.
            if(!Size.isValid()||Bounds.isEmpty()||!Reader.supportsOption(QImageIOHandler::ScaledSize))return {};
            const auto Effective=Reader.effectiveSize();
            const auto DisplaySize=Effective.isValid()?Effective:Size;
            if(DisplaySize.width()<=Bounds.width()&&DisplaySize.height()<=Bounds.height())return {};
            ImageTraceSpan Trace("image.decode-preview");
            const double Scale=std::min(double(Bounds.width())/DisplaySize.width(),double(Bounds.height())/DisplaySize.height());
            Reader.setScaledSize((Size*std::min(1.,Scale)).expandedTo(QSize(1,1)));
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
            if (!Publish) return;
            if(!elapsed_.isValid())elapsed_.start();
            if (Bytes.startsWith(QByteArrayView("\xff\x0a", 2))
                || Bytes.startsWith(QByteArrayView("\0\0\0\x0cJXL \x0d\x0a\x87\x0a", 12))) {
                updateJxl(Bytes, Publish);
                return;
            }
            // Fast local reads should reach EOF once; repeated partial decodes
            // at 512 KiB/1 MiB/... otherwise delay every supported codec.
            if(elapsed_.elapsed()<100)return;
            if(Bytes.size()<next_ || next_>32*1024*1024)return;
            next_=qMax(next_*2,Bytes.size()*2);
            elapsed_.restart();
            auto Image=Decode(Bytes);if(!Image.isNull())Publish(std::move(Image));
        }
    private:
        void updateJxl(const QByteArray& Bytes, const std::function<void(QImage)>& Publish)
        {
            if (jxlDone_ || (!jxl_ && elapsed_.elapsed() < 100)) return;
            struct Codec
            {
                QLibrary library_{QCoreApplication::applicationDirPath() + "/Libs/imageformats/kimg_jxl.dll"};
                decltype(&HC_CreateJxlPreview) create_ = nullptr;
                decltype(&HC_DestroyJxlPreview) destroy_ = nullptr;
                decltype(&HC_UpdateJxlPreview) update_ = nullptr;
                Codec()
                {
                    library_.setLoadHints(QLibrary::ResolveAllSymbolsHint | QLibrary::PreventUnloadHint);
                    if (!library_.load()) return;
                    create_ = reinterpret_cast<decltype(create_)>(library_.resolve("HC_CreateJxlPreview"));
                    destroy_ = reinterpret_cast<decltype(destroy_)>(library_.resolve("HC_DestroyJxlPreview"));
                    update_ = reinterpret_cast<decltype(update_)>(library_.resolve("HC_UpdateJxlPreview"));
                }
            };
            static Codec Plugin;
            if (!jxl_) {
                if (Plugin.create_ && Plugin.destroy_ && Plugin.update_)
                    jxl_ = decltype(jxl_)(Plugin.create_(&bounds_), Plugin.destroy_);
                if (!jxl_) { jxlDone_ = true; return; }
            }
            QImage Image;
            if (!Plugin.update_(jxl_.get(), &Bytes, &Image)) { jxl_.reset(); jxlDone_ = true; }
            if (!Image.isNull()) Publish(std::move(Image));
        }

        QSize bounds_;
        QElapsedTimer elapsed_;
        qsizetype next_=512*1024;
        std::unique_ptr<void, decltype(&HC_DestroyJxlPreview)> jxl_{nullptr, nullptr};
        bool jxlDone_ = false;
    };
}
