#pragma once

/// 고유 라이브러리
#include "sqliteResultModel.hpp"
#include "sqliteWorkTab.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QObject>
#include <QPointer>

class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QToolButton;
class QWidget;

namespace hc::sqlite
{
    class GridView;
    class ValuePanel;

    // 테이블 · 뷰 탭(설계 4.2 · 6.2, 시안 ‘테이블 탭 — 보류 편집과 값 패널’). 위 칸은 WHERE · ORDER BY 막대와 생성한
    // SQL 한 줄, 아래 칸은 결과(데이터 · 메시지 탭, 안내 줄과 그리드 도구, 보류 편집 띠, 그리드 | 값 패널).
    // 두 칸은 작업 창의 위 · 아래 영역에 따로 들어간다. 커밋은 작업 창이 맡는다(쿼리 탭 트랜잭션과 순서를 맞춘다).
    class TableTab final : public WorkTab
    {
        Q_OBJECT
    public:
        TableTab(Connection& Reader, TableModel::Opener Opener, const ObjectInfo& Object, QObject* Parent = nullptr);
        ~TableTab() override;
        QWidget* Top() const override { return top_; }
        QWidget* Result() const override { return result_; }
        const ObjectInfo& Object() const { return model_->Source(); }
        QString Title() const override { return model_->Source().name_; }
        QString GlyphName() const override { return model_->Source().type_ == ObjectType::View ? QStringLiteral("view") : QStringLiteral("table"); }
        GridView* CurrentGrid() const override { return grid_; }
        QWidget* FocusTarget() const override;
        void Cancel() override { model_->Cancel(); }
        void ApplyFonts(const QFont& Grid, const QFont& Mono, bool MonoValues) override;
        bool Modified() const override { return model_->HasPending(); }
        int PendingCount() const override { return model_->Pending().Total(); }
        TableModel* Model() const { return model_; }
        GridView* Grid() const { return grid_; }
        ValuePanel* Panel() const { return panel_; }
        // 스키마가 바뀐 뒤 같은 개체를 새 정의로 다시 읽는다.
        void Rebind(const ObjectInfo& Object);
        void Reload();
        void Apply();
        // 상태 표시줄: "14행 / 3,503행" · 현재 칸 "4행, Composer".
        QString RowsText() const override;
        QString PositionText() const override;
        QString SelectionText() const override;
        void FocusGrid();

        // 표 편집(설계 6.2 · 6.4). 커밋은 작업 창이 Commit 을 부른다.
        void SetReadOnly(bool ReadOnly);
        void SetEditing(bool ConfirmDelete) { confirmDelete_ = ConfirmDelete; }
        bool Commit(QString* Error);
        void RevertAll();
        bool EditCurrent();                 // F2: 셀 편집, 긴 값은 값 패널
        bool AddRow(bool Duplicate);        // Alt+Insert · Ctrl+D
        bool DeleteRows();                  // Ctrl+Delete(설정이 켜져 있으면 묻는다)
        bool SetNull();                     // Ctrl+Shift+N
        void ToggleValuePanel();
        bool ValuePanelVisible() const;
    signals:
        void CommitRequested();
        void ShowSqlRequested(const QStringList& Statements);
        void OpenQueryRequested(const QString& Sql);
        // 결과 도구의 ‘내보내기’.
        void ExportRequested();
    private:
        void addMessage(bool Error, const QString& Text);
        void refreshNote();
        void refreshPending();
        void applyTheme();
        QList<int> selectedRows() const;

        TableModel* model_ = nullptr;
        QPointer<QWidget> top_, result_;
        QLineEdit* where_ = nullptr;
        QLineEdit* order_ = nullptr;
        QLabel* whereLabel_ = nullptr;
        QLabel* orderLabel_ = nullptr;
        QLabel* sql_ = nullptr;
        QLabel* openQuery_ = nullptr;
        QTabBar* resultTabs_ = nullptr;
        QStackedWidget* resultPages_ = nullptr;
        QLabel* note_ = nullptr;
        QToolButton* addRow_ = nullptr;
        QToolButton* deleteRows_ = nullptr;
        QToolButton* panelButton_ = nullptr;
        QToolButton* exportButton_ = nullptr;
        QFrame* banner_ = nullptr;
        QLabel* bannerText_ = nullptr;
        QLabel* bannerDot_ = nullptr;
        QListWidget* messages_ = nullptr;
        QSplitter* split_ = nullptr;
        GridView* grid_ = nullptr;
        ValuePanel* panel_ = nullptr;
        bool fitted_ = false, confirmDelete_ = true;
    };
}
