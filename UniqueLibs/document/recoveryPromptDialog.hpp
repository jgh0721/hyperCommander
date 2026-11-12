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
#include <QStringList>
#include <vector>

class QCheckBox;
class QLabel;
class QPushButton;
class QWidget;

namespace hc::document
{
    /// Asked once when HyperEditor starts after an abnormal exit: every window that ended that way is a group,
    /// pending archive copies are one more. Restore opens the checked documents (unchecked ones are kept);
    /// Keep sends everything to the recovery list without asking again; Delete removes the checked copies.
    class RecoveryPromptDialog final : public QDialog
    {
        Q_OBJECT
    public:
        enum Choice { Restore, Keep, Delete };
        RecoveryPromptDialog( std::vector< recovery::PromptGroup > Groups, int RetentionDays, QWidget* Parent = nullptr );
        Choice Chosen() const { return choice_; }
        /// Keys (document ids, archive copy folders) of the checked rows.
        QStringList Selected() const;
        const std::vector< recovery::PromptGroup >& Groups() const { return groups_; }
    private:
        void refresh();
        void finish( Choice Value );
        std::vector< recovery::PromptGroup > groups_;
        QList< QPair< QString, QCheckBox* > > rows_;
        Choice choice_ = Keep;
        QLabel* count_ = nullptr;
        QLabel* confirmText_ = nullptr;
        QPushButton* restore_ = nullptr;
        QPushButton* delete_ = nullptr;
        QWidget* buttons_ = nullptr;
        QWidget* confirm_ = nullptr;
    };
}
