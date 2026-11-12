#pragma once

/// 고유 라이브러리
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"

/// 외부 라이브러리
#include <fmdock/DockManager.h>

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QByteArray>
#include <QPointer>
#include <QWidget>

#include <algorithm>

namespace hc::document::test
{
    // 테스트용: 도킹 작업 공간의 문서 탭(중앙 영역의 문서 도크)을 예전 QTabWidget처럼 다룬다. 순서는 연 순서,
    // 현재 탭은 작업 공간의 포커스 문서다. 도구 창(tool:*)과 옆 미리보기(preview:*)는 세지 않는다.
    class DocumentTabs
    {
    public:
        explicit DocumentTabs(DocumentWorkspaceWidget* Workspace) : workspace_(Workspace) {}
        explicit operator bool() const { return manager() != nullptr; }
        fm::dock::DockManager* manager() const { return workspace_ ? workspace_->findChild<fm::dock::DockManager*>(QStringLiteral("documentDocks")) : nullptr; }
        QList<fm::dock::DockWidget*> docks() const
        {
            QList<fm::dock::DockWidget*> Result;
            if (auto* Manager = manager())
                for (auto* Dock : Manager->dockWidgets()) if (Dock->id().startsWith(QStringLiteral("doc:")) && !Dock->isClosed()) Result.append(Dock);
            return Result;
        }
        int count() const { return int(docks().size()); }
        fm::dock::DockWidget* dock(int Index) const { const auto List = docks(); return Index >= 0 && Index < List.size() ? List[Index] : nullptr; }
        QWidget* widget(int Index) const { auto* Dock = dock(Index); return Dock ? Dock->widget() : nullptr; }
        QString tabText(int Index) const { auto* Dock = dock(Index); return Dock ? Dock->title() : QString(); }
        int currentIndex() const
        {
            const auto Id = QStringLiteral("doc:") + workspace_->Snapshot()[QStringLiteral("activeDocumentId")].toString();
            const auto List = docks();
            for (int Index = 0; Index < List.size(); ++Index) if (List[Index]->id() == Id) return Index;
            return -1;
        }
        QWidget* currentWidget() const { return widget(currentIndex()); }
        // 탭을 누른 것처럼 앞으로 꺼낸다(내용에 포커스를 주지는 않는다 — 작업 공간이 정한다).
        void setCurrentIndex(int Index) { if (auto* Dock = dock(Index)) Dock->setAsCurrentTab(); }
        // 탭의 닫기 단추와 같다(저장 확인을 거친다).
        void requestClose(int Index) { if (auto* Dock = dock(Index)) Dock->requestClose(); }
        // 문서 Index의 탭 단추(영역 제목 줄의 탭) — 실제로 누를 때 쓴다. 탭이 숨었으면 nullptr.
        QWidget* tabButton(int Index) const
        {
            auto* Dock = dock(Index); auto* Area = Dock ? Dock->dockArea() : nullptr;
            if (!Area) return nullptr;
            QList<QWidget*> Tabs;
            for (auto* Widget : Area->findChildren<QWidget*>())
                if (QByteArray(Widget->metaObject()->className()) == QByteArrayLiteral("fm::dock::detail::DockTab") && Widget->isVisible()) Tabs.append(Widget);
            std::sort(Tabs.begin(), Tabs.end(), [](QWidget* Left, QWidget* Right) { return Left->mapToGlobal(QPoint()).x() < Right->mapToGlobal(QPoint()).x(); });
            const int At = int(Area->openDockWidgets().indexOf(Dock));
            return At >= 0 && At < Tabs.size() ? Tabs[At] : nullptr;
        }
        // 도구 창(파일에서 찾기 = "search", 개요 = "outline" …).
        fm::dock::DockWidget* tool(const QString& Kind) const { auto* Manager = manager(); return Manager ? Manager->findDockWidget(QStringLiteral("tool:") + Kind) : nullptr; }
    private:
        QPointer<DocumentWorkspaceWidget> workspace_;
    };
}
