#pragma once

/// 고유 라이브러리
#include "recentDocuments.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDialog>
#include <QPointer>

class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace hc::document
{
    /// '최근 항목 모두 보기' 비모달 대화상자(계획 6.1). 검색, 두 줄 목록(고정 항목 먼저), 열기 · 고정 · 제거 · 모두 지우기.
    /// Enter · 두 번 누르기로 열고 Del 로 제거한다. 없는 파일은 백그라운드로 확인해 흐리게 보인다.
    class RecentDocumentsDialog final : public QDialog
    {
        Q_OBJECT
    public:
        enum ItemRole { DocumentRole = Qt::UserRole + 1, MissingRole };

        explicit RecentDocumentsDialog( RecentDocumentStore* Store, QWidget* Parent = nullptr );
        ~RecentDocumentsDialog() override;

    signals:
        void OpenRequested( const hc::document::RecentDocument& Document );

    protected:
        bool eventFilter( QObject* Watched, QEvent* Event ) override;
        void showEvent( QShowEvent* Event ) override;
        void hideEvent( QHideEvent* Event ) override;

    private:
        void rebuild();
        void applyFilter();
        void refreshButtons();
        void refreshMissing( const QString& Path );
        void openItem( QListWidgetItem* Item );
        void togglePin();
        void removeCurrent();
        void confirmClear();
        QListWidgetItem* currentVisible() const;
        QListWidgetItem* firstVisible() const;

        QPointer< RecentDocumentStore > store_;
        RecentPathProbe*                probe_  = nullptr;
        QLineEdit*                      search_ = nullptr;
        QListWidget*                    list_   = nullptr;
        QPushButton*                    open_   = nullptr;
        QPushButton*                    pin_    = nullptr;
        QPushButton*                    remove_ = nullptr;
        QPushButton*                    clear_  = nullptr;
        QPushButton*                    close_  = nullptr;
        QPointer< QDialog >             confirm_;
    };
}
