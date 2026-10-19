#include "imageResampler.hpp"
#include "imagePerformance.hpp"
#include "imageColor.hpp"
#include <QColorSpace>
#include <QCryptographicHash>
#include <QMutex>
#include <QMutexLocker>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <cstring>
#include <mutex>
#include <atomic>
#include <thread>
#include <exception>
namespace hc::document
{
    namespace
    {
        double Cubic(double X, double B, double C)
        {
            X = std::abs(X);
            if (X < 1) return ((12-9*B-6*C)*X*X*X+(-18+12*B+6*C)*X*X+(6-2*B))/6;
            if (X < 2) return ((-B-6*C)*X*X*X+(6*B+30*C)*X*X+(-12*B-48*C)*X+(8*B+24*C))/6;
            return 0;
        }
        double Sinc(double X) { return std::abs(X)<1e-12 ? 1 : std::sin(std::numbers::pi*X)/(std::numbers::pi*X); }
        QMutex RegistryMutex;
        double Spline(double X,int Radius)
        {
            X=std::abs(X);if(X>=Radius)return 0;
            if(Radius==2){if(X<1)return ((X-9./5)*X-1./5)*X+1;X-=1;return ((-X/3+4./5)*X-7./15)*X;}
            if(Radius==3){if(X<1)return ((13./11*X-453./209)*X-3./209)*X+1;if(X<2){X-=1;return ((-6./11*X+270./209)*X-156./209)*X;}X-=2;return ((X/11-45./209)*X+26./209)*X;}
            if(X<1)return ((49./41*X-6387./2911)*X-3./2911)*X+1;
            if(X<2){X-=1;return ((-24./41*X+4032./2911)*X-2328./2911)*X;}
            if(X<3){X-=2;return ((6./41*X-1008./2911)*X+582./2911)*X;}X-=3;return ((-X/41+168./2911)*X-97./2911)*X;
        }
        // Only additional worker threads consume these slots. Independent renders
        // never wait for each other, and a saturated pool executes on its caller.
        std::atomic<int> ParallelWorkers{0};
        QMutex CustomKernelMutex;
        template<class Function> bool ParallelBands(int Count, std::stop_token Stop, Function Work, bool AllowParallel = true)
        {
            const int Hardware=std::max(1u,std::thread::hardware_concurrency());
            const int Requested=qEnvironmentVariableIntValue("HC_IMAGE_RENDER_THREADS");
            const int Limit=!AllowParallel?1:Requested>0?std::clamp(Requested,1,Hardware):std::clamp(Hardware/2,1,4);
            std::atomic<int> Next{0};std::atomic<bool> Failed{false};
            auto Run=[&]{try{for(;;){const int I=Next.fetch_add(1);if(I>=Count||Stop.stop_requested()||Failed.load())break;Work(I);}}catch(...){Failed=true;}};
            std::vector<std::jthread> Threads;
            for(int I=1;I<std::min(Count,Limit);++I){
                int Active=ParallelWorkers.load();bool Acquired=false;
                while(Active<std::max(0,Hardware-std::clamp(Hardware/4,1,4)-1)){if(ParallelWorkers.compare_exchange_weak(Active,Active+1)){Acquired=true;break;}}
                if(!Acquired)break;
                try{Threads.emplace_back([&]{Run();--ParallelWorkers;});}catch(...){--ParallelWorkers;break;}
            }
            Run();for(auto& Thread:Threads)Thread.join();return !Failed;
        }
        struct AxisWeights { std::vector<int> positions_;std::vector<double> values_;double total_=0; };
        std::vector<AxisWeights> Weights(int Begin,int Count,double InverseScale,double Offset,double Radius,double Scale,
            const ImageFilter& Filter)
        {
            std::vector<AxisWeights> Result(Count);
            for(int I=0;I<Count;++I){auto& W=Result[I];const double Center=(Begin+I+.5)*InverseScale+Offset-.5;
                for(int P=int(std::ceil(Center-Radius));P<=int(std::floor(Center+Radius));++P){const double V=Filter.kernel_((P-Center)/Scale);
                    if(std::abs(V)<1e-15)continue;W.positions_.push_back(P);W.values_.push_back(V);W.total_+=V;}}
            return Result;
        }
        ImageRenderResult AxisAligned(const ImageRenderRequest& Request,const ImageFilter& Filter,const QTransform& Inverse,const QRect& Tile)
        {
            const double SX=std::max(1.,std::abs(Inverse.m11())),SY=std::max(1.,std::abs(Inverse.m22()));
            const double RX=SX*Filter.support_,RY=SY*Filter.support_;
            if(!std::isfinite(RX)||!std::isfinite(RY)||RX>512||RY>512)return {{},"image-render-footprint-budget"};
            const auto Mapped=Inverse.mapRect(QRectF(Tile));
            for(double Coordinate:{Mapped.left(),Mapped.top(),Mapped.right(),Mapped.bottom()})if(!std::isfinite(Coordinate)||std::abs(Coordinate)>1e8)return {{},"image-render-coordinate"};
            const auto X=Weights(Tile.x(),Tile.width(),Inverse.m11(),Inverse.dx(),RX,SX,Filter);
            const auto Y=Weights(Tile.y(),Tile.height(),Inverse.m22(),Inverse.dy(),RY,SY,Filter);
            const auto Profile=ImageWorkingColorSpace(Request.source_);
            const auto Linear=Profile.withTransferFunction(QColorSpace::TransferFunction::Linear);
            const auto EncodedProfile=ImageEncodedColorSpace(Request.source_,Profile,Request.targetColor_);
            QImage Output(Tile.size(),QImage::Format_RGBA8888);if(Output.isNull())return {{},"image-render-allocation"};
            auto* OutputBits=Output.bits();const auto OutputStride=Output.bytesPerLine();
            // Bound temporary floating-point storage by strips, even when fitting
            // a multi-megapixel original into a much smaller viewport.
            constexpr int Strip=16;std::atomic<bool> Invalid{false};
            const bool Success=ParallelBands((Tile.height()-1)/Strip+1,Request.stop_,[&](int Band){
                const int Start=Band*Strip,End=Start+std::min(Strip,Tile.height()-Start);
                int Left=Request.source_.width()-1,Right=0,Top=Request.source_.height()-1,Bottom=0;
                for(const auto& W:X)for(int P:W.positions_){Left=std::min(Left,std::clamp(P,0,Request.source_.width()-1));Right=std::max(Right,std::clamp(P,0,Request.source_.width()-1));}
                for(int I=Start;I<End;++I)for(int P:Y[I].positions_){Top=std::min(Top,std::clamp(P,0,Request.source_.height()-1));Bottom=std::max(Bottom,std::clamp(P,0,Request.source_.height()-1));}
                QImage Source=ImageColorSource(Request.source_.copy(QRect(QPoint(Left,Top),QPoint(Right,Bottom))));
                Source=Source.convertedToColorSpace(Linear,QImage::Format_RGBA32FPx4);
                if(Source.isNull()){Invalid=true;return;}
                std::vector<std::array<double,4>> Horizontal(size_t(Source.height())*Tile.width());
                for(int Row=0;Row<Source.height();++Row){if(Request.stop_.stop_requested())return;const auto* Pixels=reinterpret_cast<const float*>(Source.constScanLine(Row));
                    for(int Col=0;Col<Tile.width();++Col){auto& Sum=Horizontal[size_t(Row)*Tile.width()+Col];const auto& W=X[Col];
                        for(size_t K=0;K<W.positions_.size();++K){const int P=W.positions_[K];if(Request.transparentBoundary_&&(P<0||P>=Request.source_.width()))continue;
                            const auto* V=Pixels+4*(std::clamp(P,0,Request.source_.width()-1)-Left);const double Weight=W.values_[K];
                            for(int C=0;C<3;++C)Sum[C]+=Weight*V[C]*V[3];Sum[3]+=Weight*V[3];}}
                }
                QImage Rows(Tile.width(),End-Start,QImage::Format_RGBA32FPx4);if(Rows.isNull()){Invalid=true;return;}Rows.setColorSpace(Linear);
                for(int Row=Start;Row<End;++Row){if(Request.stop_.stop_requested())return;auto* Pixels=reinterpret_cast<float*>(Rows.scanLine(Row-Start));const auto& W=Y[Row];
                    for(int Col=0;Col<Tile.width();++Col){std::array<double,4> Sum{};
                        for(size_t K=0;K<W.positions_.size();++K){const int P=W.positions_[K];if(Request.transparentBoundary_&&(P<0||P>=Request.source_.height()))continue;
                            const auto& V=Horizontal[size_t(std::clamp(P,0,Request.source_.height()-1)-Top)*Tile.width()+Col];
                            for(int C=0;C<4;++C)Sum[C]+=W.values_[K]*V[C];}
                        const double Weight=W.total_*X[Col].total_,Alpha=std::abs(Weight)>1e-12?std::clamp(Sum[3]/Weight,0.,1.):0.;
                        auto* V=Pixels+4*Col;V[3]=float(Alpha);for(int C=0;C<3;++C)V[C]=Alpha>1e-8&&std::abs(Weight)>1e-12?float(std::clamp(Sum[C]/Weight/Alpha,0.,1.)):0.f;}
                }
                const auto Encoded=Rows.convertedToColorSpace(EncodedProfile,QImage::Format_RGBA8888);if(Encoded.isNull()){Invalid=true;return;}
                for(int Row=Start;Row<End;++Row)std::memcpy(OutputBits+Row*OutputStride,Encoded.constScanLine(Row-Start),size_t(Tile.width())*4);
            },Filter.threadSafe_);
            if(Request.stop_.stop_requested())return {{},"image-render-cancelled"};
            if(!Success||Invalid)return {{},"image-render-allocation-or-color"};
            Output.setColorSpace(EncodedProfile);Output.setDotsPerMeterX(Request.source_.dotsPerMeterX());Output.setDotsPerMeterY(Request.source_.dotsPerMeterY());
            if(Request.targetColor_.isValid())Output=Output.convertedToColorSpace(Request.targetColor_);
            return Output.isNull()?ImageRenderResult{{},"image-display-color"}:ImageRenderResult{Output,{}};
        }
        std::vector<ImageFilter>& Registry()
        {
            static std::vector<ImageFilter> Items{
                {"bilinear", "Bilinear", 1, false, [](double X){ return std::max(0.0,1-std::abs(X)); }},
                {"bicubicCatmullRom", "Bicubic Catmull-Rom", 2, false, [](double X){return Cubic(X,0,.5);}},
                {"mitchellNetravali", "Mitchell-Netravali", 2, false, [](double X){return Cubic(X,1.0/3,1.0/3);}},
                {"lanczos3", "Lanczos3", 3, false, [](double X){return std::abs(X)<3 ? Sinc(X)*Sinc(X/3) : 0;}},
                {"ewaRobidoux", "EWA Robidoux", 2, true, [](double X){constexpr double B=0.3782157550939987;return Cubic(X,B,(1-B)/2);}},
                {"spline16", "Spline16", 2, false, [](double X){return Spline(X,2);}},
                {"spline36", "Spline36", 3, false, [](double X){return Spline(X,3);}},
                {"spline64", "Spline64", 4, false, [](double X){return Spline(X,4);}},
                {"ewaRobidouxSharp", "EWA RobidouxSharp", 2, true, [](double X){const double B=6/(13+7*std::sqrt(2.)),C=7/(2+12*std::sqrt(2.));return Cubic(X,B,C);}}
            };
            static const bool Marked=[] {for(auto& Item:Items)Item.threadSafe_=true;return true;}();Q_UNUSED(Marked);return Items;
        }
        double Linear(double X) { return X<=.04045 ? X/12.92 : std::pow((X+.055)/1.055,2.4); }
        double Gamma(double X) { X=std::clamp(X,0.0,1.0);return X<=.0031308 ? 12.92*X : 1.055*std::pow(X,1/2.4)-.055; }
    }
    std::shared_ptr<QThreadPool> ImageWorkerPool()
    {
        // Views own the pool and drain it before the DLL can be unloaded.
        // A static pool destructor would join threads under the Windows loader lock.
        static QMutex Mutex;static std::weak_ptr<QThreadPool> Weak;
        QMutexLocker Guard(&Mutex);auto Pool=Weak.lock();
        if(!Pool){Pool=std::make_shared<QThreadPool>();Pool->setMaxThreadCount(std::clamp(int(std::thread::hardware_concurrency())/4,1,4));Weak=Pool;}
        return Pool;
    }
    std::vector<ImageFilter> ImageFilterRegistry::Filters() { QMutexLocker Guard(&RegistryMutex); return Registry(); }
    bool ImageFilterRegistry::Register(ImageFilter Filter)
    {
        if (Filter.id_.isEmpty() || !Filter.kernel_ || !std::isfinite(Filter.support_) || Filter.support_<=0 || Filter.support_>8) return false;
        QMutexLocker Guard(&RegistryMutex);
        for (const auto& Item : Registry()) if (Item.id_==Filter.id_) return false;
        Registry().push_back(std::move(Filter)); return true;
    }
    ImageFilter ImageFilterRegistry::Resolve(const QString& Id, const QTransform& Transform, QSize SourceSize)
    {
        const double X=std::hypot(Transform.m11(),Transform.m12()),Y=std::hypot(Transform.m21(),Transform.m22());
        // Viewport dimensions are rounded to whole pixels. Treat that rounding
        // as uniform scaling so ordinary fit-to-window uses the separable path.
        const double Tolerance=SourceSize.isEmpty()?0:std::min(.01*std::max(X,Y),.5/SourceSize.width()+.5/SourceSize.height());
        const bool NonUniform=!qFuzzyCompare(X,Y)&&std::abs(X-Y)>Tolerance;
        const auto Chosen=Id=="auto" ? (NonUniform||std::abs(Transform.m12())>1e-9||std::abs(Transform.m21())>1e-9
            ? QString("ewaRobidoux") : X<1 ? QString("lanczos3") : QString("mitchellNetravali")) : Id;
        for (const auto& Filter:Filters()) if(Filter.id_==Chosen) return Filter;
        return {};
    }
    QString ImageCacheKey(std::uint64_t Revision, const ImageRenderRequest& Request, qreal Dpr)
    {
        QByteArray Key=QByteArray::number(Revision)+":"+Request.filter_.toUtf8()+":"+QByteArray::number(Dpr,'g',17);
        for (const auto X:{Request.sourceToOutput_.m11(),Request.sourceToOutput_.m12(),Request.sourceToOutput_.m21(),
            Request.sourceToOutput_.m22(),Request.sourceToOutput_.dx(),Request.sourceToOutput_.dy()}) Key+=":"+QByteArray::number(X,'g',17);
        Key+=":"+QByteArray::number(Request.outputSize_.width())+":"+QByteArray::number(Request.outputSize_.height());
        Key+=":"+QByteArray::number(Request.tile_.x())+":"+QByteArray::number(Request.tile_.y())+":"+QByteArray::number(Request.tile_.width())+":"+QByteArray::number(Request.tile_.height());
        Key+=":linear-premultiplied-v1";Key+=Request.source_.colorSpace().iccProfile();Key+=Request.targetColor_.iccProfile(); Key+=Request.transparentBoundary_ ? ":transparent" : ":clamp";
        return QString::fromLatin1(QCryptographicHash::hash(Key,QCryptographicHash::Sha256).toHex());
    }
    ImageRenderResult ResampleImage(const ImageRenderRequest& Request) try
    {
        ImageTraceSpan Trace("image.resample",Request.traceSource_);
        if(Request.stop_.stop_requested()) return {{},"image-render-cancelled"};
        const auto Size=Request.outputSize_;
        if(Request.source_.isNull() || Size.isEmpty() || !Request.sourceToOutput_.isAffine()) return {{},"image-render-budget-or-transform"};
        bool Invertible=false; const auto Inverse=Request.sourceToOutput_.inverted(&Invertible);
        if(!Invertible) return {{},"image-render-singular-transform"};
        const auto Filter=ImageFilterRegistry::Resolve(Request.filter_,Request.sourceToOutput_,Request.source_.size());
        if(!Filter.kernel_) return {{},"image-filter-unknown"};
        const QRect Full(QPoint(0,0),Size),Tile=Request.tile_.isEmpty()?Full:Request.tile_.intersected(Full);
        if(Tile.isEmpty()) return {{},"image-render-empty-tile"};
        // Exact pixel identity avoids any color conversion or interpolation.
        if(Request.sourceToOutput_.isIdentity()&&Size==Request.source_.size()) {
            auto Image=Request.source_.copy(Tile);
            if(Request.targetColor_.isValid()){Image=ImageColorSource(Image);Image=Image.convertedToColorSpace(Request.targetColor_);}
            return {Image,{}};
        }
        std::unique_lock<QMutex> KernelGuard;
        if(!Filter.threadSafe_){while(!CustomKernelMutex.tryLock(20))if(Request.stop_.stop_requested())return {{},"image-render-cancelled"};KernelGuard=std::unique_lock<QMutex>(CustomKernelMutex,std::adopt_lock);}
        if(!Filter.elliptical_&&std::abs(Inverse.m12())<1e-12&&std::abs(Inverse.m21())<1e-12)
            return AxisAligned(Request,Filter,Inverse,Tile);
        struct Pixel { float r_,g_,b_,a_; };
        // Ellipse covariance J*J^T, with singular values clamped to one for
        // reconstruction. This preserves anisotropy without undersampling shrink.
        const double A=Inverse.m11(), B=Inverse.m21(), C=Inverse.m12(), D=Inverse.m22();
        double CovX=A*A+B*B,CovY=C*C+D*D,CovXY=A*C+B*D;
        const double Disc=std::hypot(CovX-CovY,2*CovXY),L1=(CovX+CovY+Disc)/2,L2=(CovX+CovY-Disc)/2;
        const double Angle=.5*std::atan2(2*CovXY,CovX-CovY),Cos=std::cos(Angle),Sin=std::sin(Angle);
        const double V1=std::max(1.0,L1),V2=std::max(1.0,L2);
        CovX=V1*Cos*Cos+V2*Sin*Sin;CovY=V1*Sin*Sin+V2*Cos*Cos;CovXY=(V1-V2)*Sin*Cos;
        const double Det=CovX*CovY-CovXY*CovXY,InvX=CovY/Det,InvY=CovX/Det,InvXY=-CovXY/Det;
        const double ScaleX=std::max(1.0,std::hypot(A,B)),ScaleY=std::max(1.0,std::hypot(C,D));
        const double RadiusX=Filter.support_*(Filter.elliptical_?std::sqrt(CovX):ScaleX);
        const double RadiusY=Filter.support_*(Filter.elliptical_?std::sqrt(CovY):ScaleY);
        if(!std::isfinite(RadiusX)||!std::isfinite(RadiusY)||RadiusX>512||RadiusY>512) return {{},"image-render-footprint-budget"};
        const QSize OriginalSize=Request.source_.size();
        const auto Profile=ImageWorkingColorSpace(Request.source_);
        const auto LinearProfile=Profile.withTransferFunction(QColorSpace::TransferFunction::Linear);
        const auto EncodedProfile=ImageEncodedColorSpace(Request.source_,Profile,Request.targetColor_);
        QImage Output(Tile.size(),QImage::Format_RGBA8888);
        if(Output.isNull()) return {{},"image-render-allocation"};
        auto* OutputBits=Output.bits();const auto OutputStride=Output.bytesPerLine();std::atomic<bool> Invalid{false};
        const bool Success=ParallelBands((Tile.height()-1)/16+1,Request.stop_,[&](int Band){
            const int First=Band*16,Last=First+std::min(16,Tile.height()-First);
            // Keep the full sampling halo for this strip, including rotated and
            // elliptical footprints, without expanding the entire source to float.
            const auto Bounds=Inverse.mapRect(QRectF(Tile.x(),Tile.y()+First,Tile.width(),Last-First)).adjusted(-RadiusX-2,-RadiusY-2,RadiusX+2,RadiusY+2);
            for(double Coordinate:{Bounds.left(),Bounds.top(),Bounds.right(),Bounds.bottom()})
                if(!std::isfinite(Coordinate)||std::abs(Coordinate)>1e8){Invalid=true;return;}
            const int Left=int(std::clamp(std::floor(Bounds.left()),0.,double(OriginalSize.width()-1)));
            const int Top=int(std::clamp(std::floor(Bounds.top()),0.,double(OriginalSize.height()-1)));
            const int Right=int(std::clamp(std::ceil(Bounds.right()),double(Left),double(OriginalSize.width()-1)));
            const int Bottom=int(std::clamp(std::ceil(Bounds.bottom()),double(Top),double(OriginalSize.height()-1)));
            QImage Source=ImageColorSource(Request.source_.copy(QRect(QPoint(Left,Top),QPoint(Right,Bottom))));
            Source=Source.convertedToColorSpace(LinearProfile,QImage::Format_RGBA32FPx4);
            if(Source.isNull()){Invalid=true;return;}
            QImage LinearRows(Tile.width(),Last-First,QImage::Format_RGBA32FPx4);LinearRows.setColorSpace(LinearProfile);
            if(LinearRows.isNull()){Invalid=true;return;}
            for(int Y=First;Y<Last;++Y) {
            if(Request.stop_.stop_requested())return;
            auto* Row=reinterpret_cast<float*>(LinearRows.scanLine(Y-First));
            for(int X=0;X<Tile.width();++X) {
                const auto Center=Inverse.map(QPointF(Tile.x()+X+.5,Tile.y()+Y+.5))-QPointF(.5,.5);
                if(!std::isfinite(Center.x())||!std::isfinite(Center.y())||std::abs(Center.x())>1e8||std::abs(Center.y())>1e8) {Invalid=true;return;}
                std::array<double,4> Sum{};double Weight=0;
                for(int SY=int(std::ceil(Center.y()-RadiusY));SY<=int(std::floor(Center.y()+RadiusY));++SY)
                for(int SX=int(std::ceil(Center.x()-RadiusX));SX<=int(std::floor(Center.x()+RadiusX));++SX) {
                    const double DX=SX-Center.x(),DY=SY-Center.y();
                    const double W=Filter.elliptical_ ? Filter.kernel_(std::sqrt(std::max(0.0,InvX*DX*DX+2*InvXY*DX*DY+InvY*DY*DY)))
                        : Filter.kernel_(DX/ScaleX)*Filter.kernel_(DY/ScaleY);
                    if(std::abs(W)<1e-15) continue; Weight+=W;
                    if(Request.transparentBoundary_&&(SX<0||SY<0||SX>=OriginalSize.width()||SY>=OriginalSize.height())) continue;
                    const auto& P=reinterpret_cast<const Pixel*>(Source.constScanLine(std::clamp(SY-Top,0,Source.height()-1)))[std::clamp(SX-Left,0,Source.width()-1)];
                    Sum[0]+=W*P.r_*P.a_;Sum[1]+=W*P.g_*P.a_;Sum[2]+=W*P.b_*P.a_;Sum[3]+=W*P.a_;
                }
                auto* P=Row+4*X;
                const double Alpha=std::abs(Weight)>1e-12?std::clamp(Sum[3]/Weight,0.0,1.0):0;
                P[3]=float(Alpha);
                for(int Channel=0;Channel<3;++Channel) P[Channel]=Alpha>1e-8&&std::abs(Weight)>1e-12
                    ? float(std::clamp(Sum[Channel]/Weight/Alpha,0.0,1.0)) : 0;
            }
            }
            const auto Encoded=LinearRows.convertedToColorSpace(EncodedProfile,QImage::Format_RGBA8888);
            if(Encoded.isNull()){Invalid=true;return;}
            for(int Y=First;Y<Last;++Y)std::memcpy(OutputBits+Y*OutputStride,Encoded.constScanLine(Y-First),std::size_t(Tile.width())*4);
        },Filter.threadSafe_);
        if(Request.stop_.stop_requested())return {{},"image-render-cancelled"};
        if(!Success||Invalid)return {{},"image-render-allocation-or-coordinate"};
        Output.setColorSpace(EncodedProfile);
        Output.setDotsPerMeterX(Request.source_.dotsPerMeterX());Output.setDotsPerMeterY(Request.source_.dotsPerMeterY());
        if(Request.targetColor_.isValid())Output=Output.convertedToColorSpace(Request.targetColor_);
        return Output.isNull()?ImageRenderResult{{},"image-display-color"}:ImageRenderResult{Output,{}};
    }
    catch(const std::bad_alloc&){return {{},"image-render-allocation"};}
    catch(...){return {{},"image-render-failed"};}
}
