#include "stdafx.h"
#include "avifCompare.hpp"
#include <QFile>
#include <QLibrary>
#include <avif/avif.h>
QImage CompareAvif(const QString& Path,bool Exact,int Threads)
{
    static QLibrary Library(qEnvironmentVariable("HC_AVIF_COMPARE_DLL"));
    if(!Library.isLoaded()&&!Library.load())return {};
#define HC_AVIF(Name) const auto Name=reinterpret_cast<decltype(&::Name)>(Library.resolve(#Name));if(!Name)return {}
    HC_AVIF(avifDecoderCreate);HC_AVIF(avifDecoderDestroy);HC_AVIF(avifDecoderReadMemory);
    HC_AVIF(avifImageCreateEmpty);HC_AVIF(avifImageDestroy);HC_AVIF(avifRGBImageSetDefaults);HC_AVIF(avifImageYUVToRGB);
#undef HC_AVIF
    QFile File(Path);if(!File.open(QIODevice::ReadOnly))return {};const auto Bytes=File.readAll();
    auto* Decoder=avifDecoderCreate();auto* Image=avifImageCreateEmpty();
    if(!Decoder||!Image){if(Decoder)avifDecoderDestroy(Decoder);if(Image)avifImageDestroy(Image);return {};}
    Decoder->maxThreads=Threads;
    Decoder->strictFlags&=~AVIF_STRICT_PIXI_REQUIRED;
    QImage Result;
    if(avifDecoderReadMemory(Decoder,Image,reinterpret_cast<const uint8_t*>(Bytes.constData()),size_t(Bytes.size()))==AVIF_RESULT_OK){
        Result=QImage(int(Image->width),int(Image->height),Image->depth>8?QImage::Format_RGBA64:QImage::Format_RGBA8888);
        avifRGBImage Rgb;avifRGBImageSetDefaults(&Rgb,Image);Rgb.format=AVIF_RGB_FORMAT_RGBA;Rgb.depth=Image->depth>8?16:8;
        Rgb.pixels=Result.bits();Rgb.rowBytes=uint32_t(Result.bytesPerLine());Rgb.maxThreads=Threads;
        Rgb.avoidLibYUV=Exact?AVIF_TRUE:AVIF_FALSE;Rgb.chromaUpsampling=AVIF_CHROMA_UPSAMPLING_BEST_QUALITY;
        if(Result.isNull()||avifImageYUVToRGB(Image,&Rgb)!=AVIF_RESULT_OK)Result={};
    }
    avifImageDestroy(Image);avifDecoderDestroy(Decoder);return Result;
}
