#pragma once

/// 공통 라이브러리
#include "commonLibs/Qt/elidedLabel.hpp"

/// 외부 라이브러리
#include <fmwidgets/Label.h>

/// 플랫폼/프레임워크 헤더파일
#include <QDir>
#include <QHBoxLayout>
#include <QResizeEvent>

namespace hc::ui {
namespace detail {
class PathDirectoryLabel final : public fm::ui::Label {
public:
    using fm::ui::Label::Label;
    QSize sizeHint() const override { return {fontMetrics().horizontalAdvance(text()), fontMetrics().height()}; }
    QSize minimumSizeHint() const override { return {0, fontMetrics().height()}; }
protected:
    void paintEvent(QPaintEvent*) override {
        // fm::Label replaces tooltips with its own text while painting; keep
        // the endpoint's complete path, including the emphasized item name.
        hc::qt::PaintElidedLabel(*this, Qt::ElideMiddle);
    }
};
}

// Keep the parent path secondary and the item emphasized on one line. Each
// label retains its full text; elision is only applied while painting.
class PathRow final : public QWidget {
public:
    explicit PathRow(const QString& ObjectName, QWidget* Parent = nullptr) : QWidget(Parent) {
        setObjectName(ObjectName);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* Row = new QHBoxLayout(this);
        Row->setContentsMargins(0, 0, 0, 0); Row->setSpacing(0);
        path_ = new detail::PathDirectoryLabel(this);
        path_->setObjectName(ObjectName + ".path"); path_->setTextRole(fm::ui::Label::Meta);
        path_->setTextFormat(Qt::PlainText); path_->setElideMode(Qt::ElideMiddle);
        path_->setMargin(0); path_->setIndent(0);
        path_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        name_ = new hc::qt::ElidedLabel(this);
        name_->setObjectName(ObjectName + ".name"); name_->SetElideMode(Qt::ElideMiddle);
        name_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto Font = name_->font(); Font.setWeight(QFont::DemiBold); name_->setFont(Font);
        Row->addWidget(path_, 0, Qt::AlignVCenter); Row->addWidget(name_, 1, Qt::AlignVCenter);
    }
    QLabel* PathLabel() const { return path_; }
    QLabel* NameLabel() const { return name_; }
    void SetParts(QString Directory, const QString& Name, const QString& Role = {}) {
        if (!Directory.isEmpty() && !Name.isEmpty() && !Directory.endsWith('\\') && !Directory.endsWith('/')
            && !Directory.endsWith(QStringLiteral(" > ")))
            Directory += '\\';
        const auto FullPath = Directory + Name;
        const auto Prefix = Role + (Role.isEmpty() || Directory.isEmpty() ? QString{} : QStringLiteral("   ")) + Directory;
        path_->setText(Prefix); path_->setToolTip(FullPath); path_->setVisible(!Prefix.isEmpty());
        name_->SetText(Name); name_->setToolTip(FullPath);
        setToolTip(FullPath); setAccessibleName(FullPath);
    }
    void SetPath(const QString& Path) {
        const auto NativePath = QDir::toNativeSeparators(Path);
        const auto Separator = NativePath.lastIndexOf('\\');
        if (Separator >= 0 && Separator + 1 < NativePath.size())
            SetParts(NativePath.left(Separator + 1), NativePath.mid(Separator + 1));
        else SetParts({}, NativePath);
    }
    QSize sizeHint() const override {
        return {0, qMax(path_->sizeHint().height(), name_->sizeHint().height())};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void resizeEvent(QResizeEvent* Event) override {
        // Reserve at least half of a narrow row for the emphasized item name.
        path_->setMaximumWidth(qMax(0, contentsRect().width() / 2));
        QWidget::resizeEvent(Event);
    }
private:
    fm::ui::Label* path_ = nullptr;
    hc::qt::ElidedLabel* name_ = nullptr;
};
}
