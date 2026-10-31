#pragma once
#include <QImage>
#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QSize>
#include <QString>
#include <cmath>

namespace hc::document {
struct ImageReadingSample {
    int page_ = 0;
    QSize size_;
    QImage preview_;
    bool edgeOnly_ = false;
};
struct ImageReadingAnalysis {
    static constexpr int Version = 1;
    int mode_ = -1, samples_ = 0, longPages_ = 0, joinedEdges_ = 0;
    double confidence_ = 0, longFlowShare_ = 0;
    QString reason_ = "insufficient-samples", revision_;
    qint64 bytes_ = 0, elapsedMs_ = 0;
    bool cached_ = false;
    QJsonObject Json() const {
        return {{"version",Version},{"revision",revision_},{"mode",mode_},{"samples",samples_},
            {"longPages",longPages_},{"joinedEdges",joinedEdges_},{"confidence",confidence_},
            {"reason",reason_},{"longFlowShare",longFlowShare_},{"bytes",bytes_},{"elapsedMs",elapsedMs_},{"cached",cached_}};
    }
};
// Metadata and low-resolution pixels only; filenames and genre tags are not inputs.
class ImageReadingClassifier {
    static int edgeEvidence(const QImage& Left, const QImage& Right) {
        if(Left.isNull() || Right.isNull() || Left.width()!=Right.width() || Left.height()<8 || Right.height()<8)return -1;
        double Difference=0,Self=0,OtherSelf=0,MeanA=0,MeanB=0,SquareA=0,SquareB=0;
        const int Width=Left.width();
        for(int X=0;X<Width;++X){
            const auto A=Left.pixelColor(X,Left.height()-1),B=Right.pixelColor(X,0);
            const auto C=Left.pixelColor(X,0),D=Right.pixelColor(X,Right.height()-1);
            const auto Distance=[](QColor P,QColor Q){return (std::abs(P.red()-Q.red())+std::abs(P.green()-Q.green())+std::abs(P.blue()-Q.blue()))/3.;};
            Difference+=Distance(A,B);Self+=Distance(A,C);OtherSelf+=Distance(B,D);
            const double GrayA=qGray(A.rgb()),GrayB=qGray(B.rgb());
            MeanA+=GrayA;MeanB+=GrayB;SquareA+=GrayA*GrayA;SquareB+=GrayB*GrayB;
        }
        MeanA/=Width;MeanB/=Width;
        // Blank margins and repeating top/bottom decorations provide no evidence.
        if(Self/Width<=8 || OtherSelf/Width<=8 || SquareA/Width-MeanA*MeanA<256 || SquareB/Width-MeanB*MeanB<256)return -1;
        return Difference/Width<=10?1:0;
    }
public:
    static ImageReadingAnalysis Classify(const QList<ImageReadingSample>& Samples) {
        ImageReadingAnalysis Result;int Landscape=0,Regular=0,Comparisons=0;
        QList<const ImageReadingSample*> Body;double Flow=0,LongFlow=0;
        for(const auto& Sample:Samples){
            if(Sample.size_.width()<128 || Sample.size_.height()<128)continue;
            const double Ratio=double(Sample.size_.height())/Sample.size_.width();
            if(Ratio<.3 || Ratio>64 || (Sample.page_==0 && Samples.size()>8))continue;
            if(Sample.edgeOnly_){Body.append(&Sample);continue;}
            ++Result.samples_;Landscape+=Ratio<1.;Regular+=Ratio>=1.15 && Ratio<=1.8;
            Result.longPages_+=Ratio>=2.;Flow+=Ratio;if(Ratio>=2.)LongFlow+=Ratio;Body.append(&Sample);
        }
        Result.longFlowShare_=Flow>0?LongFlow/Flow:0;
        if(Result.samples_<8)return Result;
        if(Landscape>=Result.samples_*.35){Result.mode_=1;Result.reason_="landscape-pages";Result.confidence_=.95;return Result;}
        if(Result.longPages_>=3 && Result.longPages_>=Result.samples_*.3 && Landscape<Result.samples_*.2){
            Result.mode_=2;Result.reason_="long-page-distribution";
            Result.confidence_=qMin(.99,.8+.19*Result.longPages_/Result.samples_);return Result;
        }
        for(int I=1;I<Body.size();++I){
            const auto& A=*Body[I-1];const auto& B=*Body[I];
            if(B.page_!=A.page_+1 || std::abs(double(A.size_.width())/B.size_.width()-1.)>.1 || A.preview_.isNull() || B.preview_.isNull())continue;
            const int Evidence=edgeEvidence(A.preview_,B.preview_);if(Evidence>=0){++Comparisons;Result.joinedEdges_+=Evidence;}
        }
        if(Result.joinedEdges_>=3 && Result.joinedEdges_>=Comparisons*.75){
            Result.mode_=2;Result.reason_="continuous-image-edges";Result.confidence_=.9;
        }else if(Result.joinedEdges_>=1 && Result.longPages_>=3 && Result.longFlowShare_>=.35 && Landscape<Result.samples_*.1){
            // Splitting one tall canvas into many short files changes page counts,
            // but not its normalized vertical reading length. Require pixel evidence too.
            Result.mode_=2;Result.reason_="mixed-long-and-cut-pages";Result.confidence_=.82;
        }else if(!Result.longPages_ && Regular>=Result.samples_*.75){
            Result.mode_=1;Result.reason_="regular-page-distribution";Result.confidence_=.85;
        }else Result.reason_="ambiguous-content";
        return Result;
    }
};
} // namespace hc::document
