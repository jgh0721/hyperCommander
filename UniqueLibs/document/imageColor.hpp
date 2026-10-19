#pragma once
#include <QColorSpace>
#include <QImage>
namespace hc::document
{
    inline QImage ImageColorSource(QImage Source)
    {
        if(!Source.colorSpace().isValid()){
            // An RGB profile cannot be assigned to CMYK pixels. Untagged JPEGs
            // use Qt's default CMYK conversion before the RGB color pipeline.
            if(Source.format()==QImage::Format_CMYK8888)Source=Source.convertToFormat(QImage::Format_RGBA8888);
            Source.setColorSpace(QColorSpace::SRgb);
        }
        return Source;
    }
    inline QColorSpace ImageWorkingColorSpace(const QImage& Source)
    {
        const auto Profile=Source.colorSpace();
        if(!Profile.isValid())return QColorSpace(QColorSpace::SRgb);
        // CMYK, gray and LUT profiles require a transform to a linear RGB space.
        if(Profile.colorModel()!=QColorSpace::ColorModel::Rgb||Profile.transformModel()!=QColorSpace::TransformModel::ThreeComponentMatrix)
            return QColorSpace(QColorSpace::ProPhotoRgb);
        return Profile;
    }
    inline QColorSpace ImageEncodedColorSpace(const QImage& Source,const QColorSpace& Working,const QColorSpace& Target)
    {
        const auto Profile=Source.colorSpace();
        if(Profile.isValid()&&(Profile.colorModel()!=QColorSpace::ColorModel::Rgb||Profile.transformModel()!=QColorSpace::TransformModel::ThreeComponentMatrix))
            return Target.isValid()?Target:QColorSpace(QColorSpace::SRgb);
        return Working;
    }
}
