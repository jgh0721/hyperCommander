#include "imageResampler.hpp"
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
        QMutex RenderMutex;
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
            }; return Items;
        }
        double Linear(double X) { return X<=.04045 ? X/12.92 : std::pow((X+.055)/1.055,2.4); }
        double Gamma(double X) { X=std::clamp(X,0.0,1.0);return X<=.0031308 ? 12.92*X : 1.055*std::pow(X,1/2.4)-.055; }
    }
    std::vector<ImageFilter> ImageFilterRegistry::Filters() { QMutexLocker Guard(&RegistryMutex); return Registry(); }
    bool ImageFilterRegistry::Register(ImageFilter Filter)
    {
        if (Filter.id_.isEmpty() || !Filter.kernel_ || !std::isfinite(Filter.support_) || Filter.support_<=0 || Filter.support_>8) return false;
        QMutexLocker Guard(&RegistryMutex);
        for (const auto& Item : Registry()) if (Item.id_==Filter.id_) return false;
        Registry().push_back(std::move(Filter)); return true;
    }
    ImageFilter ImageFilterRegistry::Resolve(const QString& Id, const QTransform& Transform)
    {
        const double X=std::hypot(Transform.m11(),Transform.m12()),Y=std::hypot(Transform.m21(),Transform.m22());
        const auto Chosen=Id=="auto" ? (!qFuzzyCompare(X,Y)||std::abs(Transform.m12())>1e-9||std::abs(Transform.m21())>1e-9
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
    ImageRenderResult ResampleImage(const ImageRenderRequest& Request)
    {
        if(Request.stop_.stop_requested()) return {{},"image-render-cancelled"};
        const auto Size=Request.outputSize_;
        if(Request.source_.isNull() || Size.isEmpty() || !Request.sourceToOutput_.isAffine()) return {{},"image-render-budget-or-transform"};
        bool Invertible=false; const auto Inverse=Request.sourceToOutput_.inverted(&Invertible);
        if(!Invertible) return {{},"image-render-singular-transform"};
        const auto Filter=ImageFilterRegistry::Resolve(Request.filter_,Request.sourceToOutput_);
        if(!Filter.kernel_) return {{},"image-filter-unknown"};
        const QRect Full(QPoint(0,0),Size),Tile=Request.tile_.isEmpty()?Full:Request.tile_.intersected(Full);
        if(Tile.isEmpty()) return {{},"image-render-empty-tile"};
        // Exact pixel identity avoids any color conversion or interpolation.
        if(Request.sourceToOutput_.isIdentity()&&Size==Request.source_.size()) {
            auto Image=Request.source_.copy(Tile);
            if(Request.targetColor_.isValid()){if(!Image.colorSpace().isValid())Image.setColorSpace(QColorSpace::SRgb);Image=Image.convertedToColorSpace(Request.targetColor_);}
            return {Image,{}};
        }
        // Serialize color conversion work; only the tile footprint is converted below.
        while(!RenderMutex.tryLock(20))if(Request.stop_.stop_requested())return {{},"image-render-cancelled"};
        std::unique_lock<QMutex> RenderGuard(RenderMutex, std::adopt_lock);
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
        const auto Bounds=Inverse.mapRect(QRectF(Tile)).adjusted(-RadiusX-2,-RadiusY-2,RadiusX+2,RadiusY+2);
        if(!std::isfinite(Bounds.left())||!std::isfinite(Bounds.top())||!std::isfinite(Bounds.right())||!std::isfinite(Bounds.bottom()))return {{},"image-render-coordinate"};
        const int Left=int(std::clamp(std::floor(Bounds.left()),0.,double(OriginalSize.width()-1)));
        const int Top=int(std::clamp(std::floor(Bounds.top()),0.,double(OriginalSize.height()-1)));
        const int Right=int(std::clamp(std::ceil(Bounds.right()),double(Left),double(OriginalSize.width()-1)));
        const int Bottom=int(std::clamp(std::ceil(Bounds.bottom()),double(Top),double(OriginalSize.height()-1)));
        QImage Source=Request.source_.copy(QRect(QPoint(Left,Top),QPoint(Right,Bottom)));
        const auto Profile=Source.colorSpace().isValid()?Source.colorSpace():QColorSpace(QColorSpace::SRgb);
        const auto LinearProfile=Profile.withTransferFunction(QColorSpace::TransferFunction::Linear);
        if(!Source.colorSpace().isValid())Source.setColorSpace(QColorSpace::SRgb);
        Source=Source.convertedToColorSpace(LinearProfile,QImage::Format_RGBA32FPx4);
        if(Source.isNull())return {{},"image-color-profile-unavailable"};
        QImage Output(Tile.size(),QImage::Format_RGBA8888);
        QImage LinearRow(Tile.width(),1,QImage::Format_RGBA32FPx4);LinearRow.setColorSpace(LinearProfile);
        if(Output.isNull()) return {{},"image-render-allocation"};
        for(int Y=0;Y<Tile.height();++Y) {
            if(Request.stop_.stop_requested()) return {{},"image-render-cancelled"};
            auto* Row=reinterpret_cast<float*>(LinearRow.scanLine(0));
            for(int X=0;X<Tile.width();++X) {
                const auto Center=Inverse.map(QPointF(Tile.x()+X+.5,Tile.y()+Y+.5))-QPointF(.5,.5);
                if(!std::isfinite(Center.x())||!std::isfinite(Center.y())||std::abs(Center.x())>1e8||std::abs(Center.y())>1e8) return {{},"image-render-coordinate"};
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
            const auto EncodedRow=LinearRow.convertedToColorSpace(Profile,QImage::Format_RGBA8888);
            if(EncodedRow.isNull())return {{},"image-color-profile-unavailable"};
            std::memcpy(Output.scanLine(Y),EncodedRow.constScanLine(0),std::size_t(Tile.width())*4);
        }
        Output.setColorSpace(Profile);
        Output.setDotsPerMeterX(Request.source_.dotsPerMeterX());Output.setDotsPerMeterY(Request.source_.dotsPerMeterY());
        if(Request.targetColor_.isValid())Output=Output.convertedToColorSpace(Request.targetColor_);
        return Output.isNull()?ImageRenderResult{{},"image-display-color"}:ImageRenderResult{Output,{}};
    }
}
