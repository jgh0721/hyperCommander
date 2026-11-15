#include "stdafx.h"
#include "registryNameOverlay.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
#include <QtitanGrid.h>
#include <fmwidgets/ValueInputs.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QTimer>
#include <QScopedValueRollback>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryNameOverlay, "hc.ui.registryNameOverlay")
RegistryNameOverlay::RegistryNameOverlay(Qtitan::TreeGrid* Grid, Qtitan::GridTreeView* View)
    : QObject(Grid), grid_(Grid), view_(View)
{
    Grid->viewport()->installEventFilter(this); Grid->installEventFilter(this);
}
bool RegistryNameOverlay::Active() const { return !editor_.isNull(); }
bool RegistryNameOverlay::Begin(const QModelIndex& Index, const QString& Name)
{
    if (pending_ || !grid_ || !Index.isValid()) return false;
    Cancel(); index_ = Index; initialFocus_ = true; view_->scrollToIndex(Index, false);
    auto* Editor = new fm::ui::InlineNameEditor(grid_->viewport()); editor_ = Editor;
    Editor->setObjectName("hc.registry.renameEditor"); Editor->setAccessibleName(tr("레지스트리 이름 변경"));
    Editor->setText(Name); Editor->setFont(grid_->font()); Editor->setMaxLength(16383);
    connect(Editor, &fm::ui::InlineNameEditor::cancelRequested, this, &RegistryNameOverlay::Cancel);
    connect(Editor, &fm::ui::InlineNameEditor::commitRequested, this, [this](const QString& Text) {
        if (pending_ || !index_.isValid()) return;
        pending_ = true; editor_->setReadOnly(true); emit CommitRequested(Text);
    });
    Editor->hide(); position();
    QTimer::singleShot(0, this, &RegistryNameOverlay::position);
    return !editor_.isNull();
}
void RegistryNameOverlay::Finish(const QString& Error)
{
    pending_ = false;
    if (!editor_) return;
    if (Error.isEmpty()) { Cancel(); return; }
    editor_->setReadOnly(false); editor_->setToolTip(Error); editor_->setAccessibleDescription(Error);
    position(); if (editor_) { editor_->setFocus(); editor_->selectAll(); }
}
void RegistryNameOverlay::Cancel()
{
    if (pending_) return;
    const bool RestoreFocus = editor_ && editor_->hasFocus();
    if (editor_) { editor_->hide(); editor_->deleteLater(); editor_.clear(); }
    index_ = QModelIndex(); initialFocus_ = false;
    if (RestoreFocus && grid_) grid_->viewport()->setFocus(Qt::OtherFocusReason);
}
void RegistryNameOverlay::position()
{
    if (!editor_ || !grid_ || positioning_) return;
    const QScopedValueRollback Guard(positioning_, true);
    if (!index_.isValid()) { if (!pending_) Cancel(); else editor_->hide(); return; }
    const auto Row = view_->getRow(index_);
    if (!Row.isValid()) { if (!pending_ && !initialFocus_) Cancel(); else editor_->hide(); return; }
    for (const auto& Hit : grid_->hitInfoAll()) {
        if (Hit.info() != Qtitan::GridHitInfo::Cell || Hit.row().rowIndex() != Row.rowIndex() || Hit.modelIndex().column() != index_.column()) continue;
        const auto Rect = Hit.rect().adjusted(1, 1, -1, -1).intersected(grid_->viewport()->rect());
        if (Rect.isValid()) { if (editor_->geometry() != Rect) editor_->setGeometry(Rect); if (editor_->isHidden()) editor_->show();
            if (initialFocus_) { initialFocus_ = false; editor_->raise(); editor_->setFocus(Qt::ShortcutFocusReason); editor_->selectAll(); }
            return; }
    }
    if (!pending_ && !initialFocus_) Cancel(); else editor_->hide();
}
bool RegistryNameOverlay::eventFilter(QObject* Watched, QEvent* Event)
{
    if (editor_ && (Event->type() == QEvent::Paint || Event->type() == QEvent::Resize || Event->type() == QEvent::Wheel))
        QTimer::singleShot(0, this, &RegistryNameOverlay::position);
    return QObject::eventFilter(Watched, Event);
}
