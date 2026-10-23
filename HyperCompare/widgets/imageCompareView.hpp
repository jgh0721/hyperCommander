#pragma once
#include <QImage>
#include <QJsonObject>
#include <QWidget>
#include <array>

namespace hc::compare
{
    class ImageCompareView final : public QWidget
    {
    public:
        explicit ImageCompareView(const QJsonObject& Comparison, const QStringList& Paths, QWidget* Parent = nullptr);
        const QImage& Image(int Side) const { return images_.at(std::size_t(Side)); }
    private:
        std::array<QImage, 2> images_;
    };
}
