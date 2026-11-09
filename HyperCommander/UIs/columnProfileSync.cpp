#include "stdafx.h"
#include "columnProfileSync.hpp"

/// 모듈
/// UIs
#include "Components/cmpView.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/qtitanColumnAdapter.hpp"
/// 공통 라이브러리
#include "commonLibs/columns/columnProfileStorage.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::columns
{
    void CaptureView(CGridView* View, QVector<ViewSnapshot>& Previous)
    {
        for (const auto& Snapshot : Previous) if (Snapshot.view_ == View) return;
        Previous.append({View, View->CaptureWorkspaceView(), View->CaptureScrollState(), View->CaptureSelection(), View->CaptureFocus()});
    }
    void RestoreViews(const QVector<ViewSnapshot>& Previous)
    {
        for (const auto& Snapshot : Previous) if (Snapshot.view_) {
            Snapshot.view_->RestoreWorkspaceView(Snapshot.workspace_);
            Snapshot.view_->RestoreSelection(Snapshot.selection_, Snapshot.focus_);
            Snapshot.view_->RestoreScrollState(Snapshot.scroll_);
            emit Snapshot.view_->sigViewSettingsChanged();
        }
    }
    bool SyncOpenViews(QWidget* Window, const QHash<QUuid, ColumnProfile>& Changed,
        const QSet<CGridView*>& Skip, QVector<ViewSnapshot>& Previous)
    {
        if (!Window) return true;
        for (auto* View : Window->findChildren<CGridView*>()) {
            const auto Id = View->ColumnProfile().id_;
            if (Skip.contains(View) || !Changed.contains(Id)) continue;
            CaptureView(View, Previous);
            auto Layout = View->ColumnWidths()->WorkspaceLayout();
            const auto Profile = Changed.value(Id);
            if (!View->ApplyColumnProfile(Profile, CGridView::ApplyReason::Sync)) return false;
            // Width overlays are per-column UUID. Drop removed columns, retain
            // existing widths, and let new columns use the updated set widths.
            QSet<QString> Ids; for (const auto& Column : Profile.columns_) Ids.insert(IdText(Column.id_));
            auto Modes = Layout["profiles"].toObject();
            for (auto Mode = Modes.begin(); Mode != Modes.end(); ++Mode) {
                auto Widths = Mode.value().toObject();
                for (auto Width = Widths.begin(); Width != Widths.end();) {
                    if (!Ids.contains(Width.key())) { Width = Widths.erase(Width); continue; }
                    auto Policy = hc::view::DecodeWidth(Width.value().toObject());
                    if (Policy) for (const auto& Column : Profile.columns_) if (IdText(Column.id_) == Width.key()) {
                        Policy->hidden_ = Column.widths_[int(ParseMode(Mode.key()).value_or(LayoutMode::Details))].hidden_;
                        Width.value() = hc::view::EncodeWidth(*Policy); break;
                    }
                    ++Width;
                }
                Mode.value() = Widths;
            }
            Layout["profiles"] = Modes; Layout["definition"] = EncodeProfile(Profile); Layout["mode"] = ModeName(Profile.mode_);
            View->ColumnWidths()->RestoreWorkspaceLayout({{WorkspaceKey, Layout}});
            View->ColumnWidths()->Apply(); emit View->sigViewSettingsChanged();
        }
        return true;
    }
}
