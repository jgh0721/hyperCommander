#include "stdafx.h"
#include "gpu.hpp"
#include "avifCompare.hpp"
#include "UniqueLibs/document/imageResampler.hpp"
#include <QGuiApplication>
#include <QImageReader>
#include <QBuffer>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QThread>
#include <QtConcurrent>
#include <windows.h>
#include <psapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <zimg.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <atomic>
#include <thread>
#include <QScopeGuard>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
using namespace hc::document;
using Microsoft::WRL::ComPtr;
namespace hc::document { ImageRenderResult ResampleReference(const ImageRenderRequest &); }
namespace
{
QImage Wic(const QImage &I,QSize Size)
{
    ComPtr<IWICImagingFactory> Factory;ComPtr<IWICBitmap> Bitmap;ComPtr<IWICBitmapScaler> Scaler;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&Factory))))return {};
    if(FAILED(Factory->CreateBitmapFromMemory(I.width(),I.height(),GUID_WICPixelFormat32bppRGBA,UINT(I.bytesPerLine()),UINT(I.sizeInBytes()),const_cast<BYTE*>(I.constBits()),&Bitmap)))return {};
    if(FAILED(Factory->CreateBitmapScaler(&Scaler))||FAILED(Scaler->Initialize(Bitmap.Get(),Size.width(),Size.height(),WICBitmapInterpolationModeFant)))return {};
    QImage Out(Size,QImage::Format_RGBA8888);if(FAILED(Scaler->CopyPixels(nullptr,UINT(Out.bytesPerLine()),UINT(Out.sizeInBytes()),Out.bits())))return {};return Out;
}
QImage WicDecode(const QString& Path,QSize Scaled={})
{
    ComPtr<IWICImagingFactory> Factory;ComPtr<IWICBitmapDecoder> Decoder;ComPtr<IWICBitmapFrameDecode> Frame;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&Factory)))||
        FAILED(Factory->CreateDecoderFromFilename(Path.toStdWString().c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&Decoder))||FAILED(Decoder->GetFrame(0,&Frame)))return {};
    ComPtr<IWICBitmapSource> Source=Frame;ComPtr<IWICMetadataQueryReader> Metadata;ComPtr<IWICBitmapFlipRotator> Rotation;ComPtr<IWICBitmap> CachedFrame;
    if(SUCCEEDED(Frame->GetMetadataQueryReader(&Metadata))){PROPVARIANT V{};
        if(SUCCEEDED(Metadata->GetMetadataByName(L"/app1/ifd/{ushort=274}",&V))&&V.vt==VT_UI2&&V.uiVal>=2&&V.uiVal<=8){
            const WICBitmapTransformOptions Options[]={WICBitmapTransformRotate0,WICBitmapTransformRotate0,WICBitmapTransformFlipHorizontal,WICBitmapTransformRotate180,WICBitmapTransformFlipVertical,
                WICBitmapTransformOptions(WICBitmapTransformRotate90|WICBitmapTransformFlipHorizontal),WICBitmapTransformRotate90,WICBitmapTransformOptions(WICBitmapTransformRotate270|WICBitmapTransformFlipHorizontal),WICBitmapTransformRotate270};
            // Rotating a demand-decoded JPEG by 90 degrees otherwise repeatedly
            // decodes source scanlines for column reads. Include materialization
            // in the measured decode boundary, rather than hiding it in warmup.
            if(FAILED(Factory->CreateBitmapFromSource(Frame.Get(),WICBitmapCacheOnLoad,&CachedFrame))){PropVariantClear(&V);return {};}
            if(SUCCEEDED(Factory->CreateBitmapFlipRotator(&Rotation))&&SUCCEEDED(Rotation->Initialize(CachedFrame.Get(),Options[V.uiVal])))Source=Rotation;
        }PropVariantClear(&V);
    }
    ComPtr<IWICBitmapScaler> Scaler;
    if(!Scaled.isEmpty()){if(FAILED(Factory->CreateBitmapScaler(&Scaler))||FAILED(Scaler->Initialize(Source.Get(),Scaled.width(),Scaled.height(),WICBitmapInterpolationModeFant)))return {};Source=Scaler;}
    WICPixelFormatGUID Format;UINT Width=0,Height=0,Bits=8,Channels=1;Source->GetSize(&Width,&Height);Source->GetPixelFormat(&Format);
    ComPtr<IWICComponentInfo> Component;ComPtr<IWICPixelFormatInfo> Info;
    if(SUCCEEDED(Factory->CreateComponentInfo(Format,&Component))&&SUCCEEDED(Component.As(&Info))){Info->GetBitsPerPixel(&Bits);Info->GetChannelCount(&Channels);}
    const bool Deep=Channels&&Bits/Channels>8;
    ComPtr<IWICFormatConverter> Converter;const auto Target=Deep?GUID_WICPixelFormat64bppRGBA:GUID_WICPixelFormat32bppRGBA;
    if(FAILED(Factory->CreateFormatConverter(&Converter))||FAILED(Converter->Initialize(Source.Get(),Target,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return {};
    QImage Out(Width,Height,Deep?QImage::Format_RGBA64:QImage::Format_RGBA8888);
    if(Out.isNull()||FAILED(Converter->CopyPixels(nullptr,UINT(Out.bytesPerLine()),UINT(Out.sizeInBytes()),Out.bits())))return {};return Out;
}
QImage Zimg(const QImage &I,QSize Size,const QString &Filter)
{
    auto Tagged=I;if(!Tagged.colorSpace().isValid())Tagged.setColorSpace(QColorSpace::SRgb);
    const auto Linear=Tagged.convertedToColorSpace(QColorSpace(QColorSpace::SRgbLinear),QImage::Format_RGBA32FPx4);
    zimg_image_format In{},Out{};zimg_image_format_default(&In,ZIMG_API_VERSION);zimg_image_format_default(&Out,ZIMG_API_VERSION);
    In.width=I.width();In.height=I.height();In.pixel_type=ZIMG_PIXEL_FLOAT;In.color_family=ZIMG_COLOR_RGB;In.depth=32;In.pixel_range=ZIMG_RANGE_FULL;In.alpha=ZIMG_ALPHA_PREMULTIPLIED;
    Out=In;Out.width=Size.width();Out.height=Size.height();
    zimg_graph_builder_params P{};zimg_graph_builder_params_default(&P,ZIMG_API_VERSION);
    P.resample_filter=Filter=="spline16"?ZIMG_RESIZE_SPLINE16:Filter=="spline36"?ZIMG_RESIZE_SPLINE36:Filter=="spline64"?ZIMG_RESIZE_SPLINE64:Filter=="bilinear"?ZIMG_RESIZE_BILINEAR:(Filter=="bicubicCatmullRom"||Filter=="mitchellNetravali")?ZIMG_RESIZE_BICUBIC:ZIMG_RESIZE_LANCZOS;
    if(P.resample_filter==ZIMG_RESIZE_LANCZOS)P.filter_param_a=3;
    if(P.resample_filter==ZIMG_RESIZE_BICUBIC){P.filter_param_a=Filter=="bicubicCatmullRom"?0:1./3;P.filter_param_b=Filter=="bicubicCatmullRom"?.5:1./3;}
    auto* Graph=zimg_filter_graph_build(&In,&Out,&P);if(!Graph)return {};
    std::vector<void*> Allocations;
    auto Allocate=[&](size_t N){void* V=_aligned_malloc(N,64);Allocations.push_back(V);return V;};
    zimg_image_buffer_const Src{};Src.version=ZIMG_API_VERSION;zimg_image_buffer Dst{};Dst.version=ZIMG_API_VERSION;
    const auto Stride=[](int Width){return (size_t(Width)*sizeof(float)+63)&~size_t(63);};
    for(int C=0;C<4;++C){Src.plane[C]={Allocate(Stride(I.width())*I.height()),ptrdiff_t(Stride(I.width())),ZIMG_BUFFER_MAX};Dst.plane[C]={Allocate(Stride(Size.width())*Size.height()),ptrdiff_t(Stride(Size.width())),ZIMG_BUFFER_MAX};}
    for(int Y=0;Y<I.height();++Y){const float* Row=(const float*)Linear.constScanLine(Y);for(int C=0;C<4;++C){auto* D=(float*)((char*)Src.plane[C].data+Y*Src.plane[C].stride);for(int X=0;X<I.width();++X)D[X]=Row[4*X+C]*(C<3?Row[4*X+3]:1.f);}}
    size_t Bytes=0;zimg_filter_graph_get_tmp_size(Graph,&Bytes);auto Code=zimg_filter_graph_process(Graph,&Src,&Dst,Allocate(Bytes?Bytes:64),nullptr,nullptr,nullptr,nullptr);
    QImage Result;
    if(!Code){QImage F(Size,QImage::Format_RGBA32FPx4);F.setColorSpace(QColorSpace::SRgbLinear);for(int Y=0;Y<Size.height();++Y){float* Row=(float*)F.scanLine(Y);for(int X=0;X<Size.width();++X){const float A=((float*)((char*)Dst.plane[3].data+Y*Dst.plane[3].stride))[X];Row[4*X+3]=std::clamp(A,0.f,1.f);for(int C=0;C<3;++C)Row[4*X+C]=A>1e-8?std::clamp(((float*)((char*)Dst.plane[C].data+Y*Dst.plane[C].stride))[X]/A,0.f,1.f):0;}}Result=F.convertedToColorSpace(QColorSpace(QColorSpace::SRgb),QImage::Format_RGBA8888);}
    zimg_filter_graph_free(Graph);for(auto* V:Allocations)_aligned_free(V);return Result;
}
int Validate(const QString& Root)
{
    QJsonArray Rows;int Failures=0;
    QImage Source(71,53,QImage::Format_RGBA8888);Source.setColorSpace(QColorSpace::SRgb);
    for(int Y=0;Y<Source.height();++Y)for(int X=0;X<Source.width();++X)Source.setPixelColor(X,Y,QColor((X*11+Y*7)%256,(X*3+Y*29)%256,(X*19+Y*2)%256,(X*23+Y*13)%256));
    for(const auto& Filter:ImageFilterRegistry::Filters())for(int Variant=0;Variant<7;++Variant){
        ImageRenderRequest R;R.source_=Source;R.filter_=Filter.id_;R.outputSize_={87,61};
        if(Variant==0)R.sourceToOutput_.scale(87./71,61./53);
        if(Variant==1){R.outputSize_={23,17};R.sourceToOutput_.scale(23./71,17./53);}
        if(Variant==2){R.sourceToOutput_.translate(85,0);R.sourceToOutput_.scale(-1.2,.9);}
        if(Variant==3){R.sourceToOutput_.translate(-5,9);R.sourceToOutput_.scale(.7,1.1);R.transparentBoundary_=true;}
        if(Variant==4){R.sourceToOutput_.rotate(17);R.sourceToOutput_.scale(.8,.9);R.transparentBoundary_=true;}
        if(Variant==5){R.sourceToOutput_.scale(1.2,.8);R.tile_={11,7,37,23};}
        if(Variant==6){R.source_=Source.convertedToColorSpace(QColorSpace(QColorSpace::DisplayP3),QImage::Format_RGBA64);R.targetColor_=QColorSpace::SRgb;R.sourceToOutput_.scale(1.2,.8);}
        const auto A=ResampleReference(R),B=ResampleImage(R);int Max=0;double Total=0;
        if(A.image_.size()!=B.image_.size()||!B.error_.isEmpty())Max=999;
        else for(int Y=0;Y<A.image_.height();++Y)for(int X=0;X<A.image_.width();++X){const auto P=A.image_.pixelColor(X,Y),Q=B.image_.pixelColor(X,Y);for(int D:{std::abs(P.red()-Q.red()),std::abs(P.green()-Q.green()),std::abs(P.blue()-Q.blue()),std::abs(P.alpha()-Q.alpha())}){Max=std::max(Max,D);Total+=D;}}
        const bool Passed=Max<=2;if(!Passed)++Failures;
        Rows.append(QJsonObject{{"filter",Filter.id_},{"variant",Variant},{"max_channel",Max},{"absolute_sum",Total},{"pass",Passed},{"error",B.error_}});
    }
    ImageRenderRequest R;R.source_=Source;R.filter_="spline36";R.outputSize_={120,90};R.sourceToOutput_.scale(120./71,90./53);
    R.source_.setColorSpace(QColorSpace(QColorSpace::Primaries::AdobeRgb,QColorSpace::TransferFunction::Gamma,2.17));
    R.targetColor_=QColorSpace::SRgb;std::vector<QFuture<ImageRenderResult>> Futures;
    for(int I=0;I<24;++I)Futures.push_back(QtConcurrent::run([R]{return ResampleImage(R);}));
    for(auto& F:Futures)F.waitForFinished();const auto Expected=ResampleImage(R).image_;
    bool Concurrent=true;for(auto& F:Futures)Concurrent=Concurrent&&F.result().image_==Expected;
    if(!Concurrent)++Failures;Rows.append(QJsonObject{{"case","concurrent-color-and-filter"},{"pass",Concurrent}});
    std::stop_source Stop;Stop.request_stop();R.stop_=Stop.get_token();const bool Cancelled=ResampleImage(R).error_=="image-render-cancelled";if(!Cancelled)++Failures;
    Rows.append(QJsonObject{{"case","pre-cancelled"},{"pass",Cancelled}});
    struct KernelState {std::atomic<int> active_{0},overlap_{0};};const auto State=std::make_shared<KernelState>();
    ImageFilterRegistry::Register({"serialized-test","Serialized test",1,false,[State](double X){if(State->active_.fetch_add(1)>0)++State->overlap_;std::this_thread::yield();--State->active_;return std::max(0.,1-std::abs(X));}});
    R.stop_={};R.filter_="serialized-test";R.outputSize_={8,7};R.sourceToOutput_=QTransform::fromScale(8./71,7./53);Futures.clear();
    for(int I=0;I<8;++I)Futures.push_back(QtConcurrent::run([R]{return ResampleImage(R);}));for(auto& F:Futures)F.waitForFinished();
    const bool Serialized=State->overlap_==0;if(!Serialized)++Failures;Rows.append(QJsonObject{{"case","custom-kernel-serialization"},{"pass",Serialized}});
    QFile Report(Root+"/validation.json");if(!Report.open(QIODevice::WriteOnly))return 3;Report.write(QJsonDocument(QJsonObject{{"failures",Failures},{"checks",Rows}}).toJson());
    return Failures?1:0;
}
}
int main(int Argc,char** Argv)
{
    QGuiApplication App(Argc,Argv);const auto ComStatus=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    const auto ComCleanup=qScopeGuard([ComStatus]{if(SUCCEEDED(ComStatus))CoUninitialize();});
    const auto Args=App.arguments();if(Args.size()<4)return 2;
    const QString Root=Args[1],Tag=Args[2],Operation=Args[3];
    const QString Engine=Tag.startsWith("gpu-")?"gpu":Tag.startsWith("native-")?"native":Tag.startsWith("reference-")?"reference":Tag=="wic-scaled"?"wic":Tag;
    for(const auto* Name:{"libde265.dll","dav1d.dll","heif.dll","avif.dll"}){
        const auto Path=QDir(qEnvironmentVariable("HC_IMAGE_CODEC_RUNTIME",QStringLiteral(HC_IMAGE_CODEC_RUNTIME))).absoluteFilePath(QString::fromLatin1(Name));
        LoadLibraryExW(Path.toStdWString().c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    }
    if(Operation=="validate")return Validate(Root);
    QFile Matrix(Root+"/matrix.json");if(!Matrix.open(QIODevice::ReadOnly))return 3;
    QFile Report(Root+"/results/"+Tag+"-"+Operation+".jsonl");if(!Report.open(QIODevice::WriteOnly|QIODevice::Truncate))return 4;
    QElapsedTimer Init;Init.start();
    std::unique_ptr<GpuResizer> Gpu;
    if(Engine=="gpu"||Engine=="warp")Gpu=std::make_unique<GpuResizer>(Engine=="warp");
    const double RendererInit=Gpu?Init.nsecsElapsed()/1e6:0;
    const auto Cases=QJsonDocument::fromJson(Matrix.readAll()).array();
    for(const auto &Value:Cases){const auto C=Value.toObject();if(C["operation"].toString()!=Operation)continue;
        const QString Id=C["id"].toString(),Filter=C["filter"].toString("lanczos3"),Path=Root+"/corpus/"+C["file"].toString();
        if(Args.size()>4&&!Args[4].isEmpty()&&!Id.contains(Args[4]))continue;
        const QSize Size(C["width"].toInt(),C["height"].toInt());
        if(Engine=="reference"&&!C["reference"].toBool(true))continue;
        if(Engine.startsWith("avif-")&&!Path.endsWith(".avif",Qt::CaseInsensitive))continue;
        const QImage Input=Operation=="resize"?QImage(Path).convertToFormat(QImage::Format_RGBA8888):QImage{};
        if(Engine=="reference"&&Input.width()*qint64(Input.height())>3000000&&!Id.contains("-0.25-lanczos3"))continue;
        if(Engine=="warp"&&Input.width()*qint64(Input.height())>310000)continue;
        if((Engine=="zimg")&&Filter.startsWith("ewa"))continue;
        if((Engine=="gpu"||Engine=="warp")&&Filter!="lanczos3"&&Filter!="spline36"&&Filter!="ewaRobidouxSharp"&&Filter!="bilinear")continue;
        if((Engine=="qt"||Engine=="wic"||Engine=="stb")&&Filter!="lanczos3")continue;
        std::vector<double> Times;QImage Result;QString Error,Adapter;double Upload=0,Compute=0,Readback=0,FirstOperation=0;
        const int Runs=C["runs"].toInt(5);
        for(int N=-1;N<Runs;++N){QElapsedTimer T;T.start();
            if(Engine.startsWith("avif-"))Result=CompareAvif(Path,Engine.contains("exact"),std::max(1,qEnvironmentVariableIntValue("HC_IMAGE_CODEC_THREADS")));
            else if(Engine=="wic"&&Operation=="decode")Result=WicDecode(Path,Tag=="wic-scaled"?Size:QSize{});
            else if(Operation=="decode") {QImageReader Reader(Path);Reader.setAutoTransform(true);if(Engine=="qt-scaled"){auto DecodeSize=Size;if(Reader.transformation()&QImageIOHandler::TransformationRotate90)DecodeSize.transpose();Reader.setScaledSize(DecodeSize);}Result=Reader.read();Error=Reader.errorString();if(!Result.isNull())Error.clear();}
            else if(Engine=="qt")Result=Input.scaled(Size,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
            else if(Engine=="wic")Result=Wic(Input,Size);
            else if(Engine=="stb"){Result=QImage(Size,QImage::Format_RGBA8888);if(!stbir_resize_uint8_srgb(Input.constBits(),Input.width(),Input.height(),Input.bytesPerLine(),Result.bits(),Size.width(),Size.height(),Result.bytesPerLine(),STBIR_RGBA))Result={};}
            else if(Engine=="zimg")Result=Zimg(Input,Size,Filter);
            else if(Engine=="gpu"||Engine=="warp"){auto R=Gpu->Resize(Input,Size,Filter);Result=R.image_;Error=R.error_;Adapter=R.adapter_;Upload=R.uploadMs_;Compute=R.processMs_;Readback=R.readbackMs_;}
            else {ImageRenderRequest R;R.source_=Input;R.outputSize_=Size;R.filter_=Filter;R.sourceToOutput_.scale(double(Size.width())/Input.width(),double(Size.height())/Input.height());auto V=Engine=="reference"?ResampleReference(R):ResampleImage(R);Result=V.image_;Error=V.error_;}
            if(N>=0)Times.push_back(T.nsecsElapsed()/1e6);else FirstOperation=T.nsecsElapsed()/1e6;if(Result.isNull())break;
        }
        std::sort(Times.begin(),Times.end());PROCESS_MEMORY_COUNTERS Mem{};GetProcessMemoryInfo(GetCurrentProcess(),&Mem,sizeof(Mem));
        QJsonArray Samples;for(double T:Times)Samples.append(T);
        QJsonObject Row{{"id",Id},{"engine",Tag},{"operation",Operation},{"filter",Filter},{"samples_ms",Samples},{"p50_ms",Times.empty()?0:Times[Times.size()/2]},{"p95_ms",Times.empty()?0:Times[size_t(std::ceil(Times.size()*.95))-1]},{"peak_working_set",double(Mem.PeakWorkingSetSize)},{"memory_scope","process-cumulative"},{"error",Result.isNull()?(Error.isEmpty()?"empty-output":Error):QString{}},{"adapter",Adapter},{"shared_renderer_init_ms",RendererInit},{"first_operation_ms",FirstOperation},{"upload_ms",Upload},{"dispatch_sync_transfer_ms",Compute},{"readback_copy_ms",Readback},{"width",Result.width()},{"height",Result.height()},{"output_bits_per_pixel",Result.depth()}};
        Report.write(QJsonDocument(Row).toJson(QJsonDocument::Compact)+"\n");Report.flush();
        if(!Result.isNull())Result.save(Root+"/results/"+Tag+"-"+Id+".png");
    }
    return 0;
}
