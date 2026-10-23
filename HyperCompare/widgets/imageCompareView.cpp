#include "stdafx.h"
#include "imageCompareView.hpp"
#include "core/compare.hpp"
#include <QColorSpace>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollArea>
#include <QSplitter>
#include <QVBoxLayout>

namespace hc::compare
{
    namespace
    {
        class ImageCanvas final : public QWidget
        {
        public:
            ImageCanvas(QImage Image, QRect Bounds, QWidget* Parent) : QWidget(Parent), image_(std::move(Image)), bounds_(Bounds) {}
            QSize sizeHint() const override { return zoom_ > 0 ? image_.size() * zoom_ : QSize(512, 384); }
            QSize minimumSizeHint() const override { return {1, 1}; }
            void SetZoom(double Zoom) { zoom_ = Zoom; if (zoom_ > 0) resize(sizeHint()); updateGeometry(); update(); }
        protected:
            void paintEvent(QPaintEvent* Event) override
            {
                QPainter painter(this); painter.fillRect(rect(), palette().window());
                const double scale = zoom_ > 0 ? zoom_ : std::min(double(width()) / image_.width(), double(height()) / image_.height());
                const QSizeF size(image_.width() * scale, image_.height() * scale);
                const QRectF target(QPointF(zoom_ > 0 ? 0 : (width() - size.width()) / 2, zoom_ > 0 ? 0 : (height() - size.height()) / 2), size);
                painter.save(); painter.setClipRect(target);
                const auto visible = Event->rect().intersected(target.toAlignedRect());
                const int originX = int(target.left()), originY = int(target.top());
                const int beginX = originX + ((visible.left() - originX) / 16) * 16, beginY = originY + ((visible.top() - originY) / 16) * 16;
                for (int y = beginY; y <= visible.bottom(); y += 16)
                    for (int x = beginX; x <= visible.right(); x += 16)
                        painter.fillRect(x, y, 16, 16, ((x - int(target.left())) / 16 + (y - int(target.top())) / 16) % 2 ? QColor("#b0b0b0") : QColor("#e0e0e0"));
                painter.drawImage(target, image_);
                if (!bounds_.isEmpty())
                {
                    painter.setPen(QPen(QColor("#e14545"), 2)); painter.setBrush(Qt::NoBrush);
                    painter.drawRect(QRectF(target.topLeft() + QPointF(bounds_.x() * scale, bounds_.y() * scale), QSizeF(bounds_.width() * scale, bounds_.height() * scale)));
                }
                painter.restore();
            }
        private:
            QImage image_; QRect bounds_; double zoom_ = 0;
        };
    }
    ImageCompareView::ImageCompareView(const QJsonObject& Comparison, const QStringList& Paths, QWidget* Parent) : QWidget(Parent)
    {
        setObjectName("imageCompareView"); const auto documents = Comparison["documents"].toArray(), pairs = Comparison["pairs"].toArray();
        if (documents.size() != 2 || pairs.size() != 1) throw Failure("ImageViewPayload");
        const auto pair = pairs[0].toObject(), box = pair["bounds"].toObject();
        const QRect bounds(box["x"].toInt(), box["y"].toInt(), box["width"].toInt(), box["height"].toInt());
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(4, 4, 4, 4);
        auto* toolbar = new QHBoxLayout; auto* zoom = new QComboBox(this); zoom->setObjectName("imageZoom"); zoom->setAccessibleName(tr("이미지 확대"));
        for (const auto& item : {std::pair{tr("화면 맞춤"), 0.0}, std::pair{QString("25%"), 0.25}, std::pair{QString("50%"), 0.5}, std::pair{QString("100%"), 1.0}, std::pair{QString("200%"), 2.0}}) zoom->addItem(item.first, item.second);
        const auto summary = pair["pixelEqual"].toBool() ? (pair["rawEqual"].toBool() ? tr("파일·픽셀 동일") : tr("픽셀 동일 · 파일 바이트 다름")) : tr("다른 픽셀 %1개 · 빨간 테두리: 변경 영역").arg(pair["differentPixels"].toInteger());
        auto* status = new QLabel(summary + tr(" · 읽기 전용"), this); status->setObjectName("imageCompareStatus"); status->setWordWrap(true);
        toolbar->addWidget(zoom); toolbar->addWidget(status, 1); layout->addLayout(toolbar);
        auto* split = new QSplitter(Qt::Horizontal, this); layout->addWidget(split, 1); split->setChildrenCollapsible(false);
        std::array<ImageCanvas*, 2> canvases{}; std::array<QScrollArea*, 2> areas{};
        for (int side = 0; side < 2; ++side)
        {
            const auto document = documents[side].toObject(); const int width = document["width"].toInt(), height = document["height"].toInt();
            auto decoded = QByteArray::fromBase64Encoding(document["rgba"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (!decoded || width <= 0 || height <= 0 || width > 8192 || height > 8192 || qint64(width) * height > 4 * 1024 * 1024 || decoded.decoded.size() != qint64(width) * height * 4) throw Failure("ImageViewPayload");
            images_[side] = QImage(reinterpret_cast<const uchar*>(decoded.decoded.constData()), width, height, width * 4, QImage::Format_RGBA8888).copy();
            if (images_[side].isNull()) throw Failure("ImageViewPayload"); images_[side].setColorSpace(QColorSpace::SRgb);
            auto* pane = new QWidget(split); auto* column = new QVBoxLayout(pane); column->setContentsMargins(0, 0, 0, 0);
            auto* title = new QLabel(tr("%1 · %2 × %3").arg(Paths.value(side)).arg(width).arg(height), pane); title->setWordWrap(true); column->addWidget(title);
            areas[side] = new QScrollArea(pane); areas[side]->setObjectName(QString("imageArea%1").arg(side)); areas[side]->setWidgetResizable(true);
            canvases[side] = new ImageCanvas(images_[side], bounds.intersected(QRect(0, 0, width, height)), areas[side]); areas[side]->setWidget(canvases[side]); column->addWidget(areas[side], 1);
        }
        connect(zoom, &QComboBox::currentIndexChanged, this, [zoom, canvases, areas] {
            const auto scale = zoom->currentData().toDouble();
            for (int side = 0; side < 2; ++side) { areas[side]->setWidgetResizable(scale == 0); canvases[side]->SetZoom(scale); }
        });
        auto* policy = new QLabel(tr("원본 RGBA8 값을 비교합니다. 표시에는 sRGB 변환을 적용하며, 색상 정보가 없으면 sRGB로 표시합니다. 파일 바이트 차이는 메타데이터·압축 차이도 포함합니다."), this);
        policy->setWordWrap(true); layout->addWidget(policy);
    }
}
