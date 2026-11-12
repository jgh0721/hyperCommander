#pragma once

/// 모듈
#include "documentRecoveryMaintenance.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDialog>
#include <functional>
#include <vector>

class QLabel;
class QPushButton;
class QTableWidget;
class QWidget;
namespace fm::ui { class FindBox; class SegmentedControl; }

namespace hc::document
{
    /// 복구 사본 관리: edit snapshots, EPUB drafts and archive copies in one list. Checked rows open as
    /// separate documents or are removed; a single archive copy can be written back into its ZIP.
    class RecoveryManagerDialog final : public QDialog
    {
        Q_OBJECT
    public:
        struct Hooks
        {
            std::function< QString( const recovery::Item& ) > open_;    // empty when opened, else a message
            std::function< bool( const recovery::Item& ) > inUse_;      // copies of documents open in this window
        };
        RecoveryManagerDialog( QString Root, recovery::Policy Settings, Hooks Calls, QWidget* Parent = nullptr );
        /// all · documents · epub · archive
        void SetFilter( const QString& Key );
        void Reload();
        /// Human text for an archive write-back error or blocker code.
        static QString ArchiveMessage( const QString& Code );
    signals:
        void SettingsRequested();
    private:
        void populate();
        void applyFilter();
        void refreshButtons();
        void showDetail( int Index );
        void report( const QString& Text );
        std::vector< int > checked() const;
        QString root_;
        recovery::Policy policy_;
        Hooks hooks_;
        std::vector< recovery::Item > items_;
        int current_ = -1;
        bool busy_ = false;
        QTableWidget* table_ = nullptr;
        fm::ui::SegmentedControl* filter_ = nullptr;
        fm::ui::FindBox* find_ = nullptr;
        QLabel* summary_ = nullptr;
        QLabel* detail_ = nullptr;
        QLabel* empty_ = nullptr;
        QLabel* count_ = nullptr;
        QLabel* confirmText_ = nullptr;
        QPushButton* open_ = nullptr;
        QPushButton* apply_ = nullptr;
        QPushButton* delete_ = nullptr;
        QWidget* buttons_ = nullptr;
        QWidget* confirm_ = nullptr;
    };
}
