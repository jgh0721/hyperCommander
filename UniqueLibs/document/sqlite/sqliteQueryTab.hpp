#pragma once

/// 고유 라이브러리
#include "sqliteCompletion.hpp"
#include "sqliteHistory.hpp"
#include "sqliteQueryRunner.hpp"
#include "sqliteSql.hpp"
#include "sqliteWorkTab.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <functional>
#include <memory>
#include <QPointer>
#include <QStringList>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTabBar;
class QTimer;
class QToolButton;
class QTreeWidget;
class ScintillaQtDirectBackend;

namespace hc::document { class CompletionPopupWidget; }

namespace hc::sqlite
{
    class QueryModel;

    // 쿼리 탭(설계 5절, 시안 ‘기본 배치’). 위 칸은 실행 막대와 SQL 편집기(Scintilla + LexSQL), 아래 칸은 문마다
    // 결과 탭과 ‘메시지’. 탭마다 실행기(작업 스레드 · 연결)를 둔다. 실행 중지는 문서 창의 ‘작업 취소’(Ctrl+.).
    class QueryTab final : public WorkTab
    {
        Q_OBJECT
    public:
        QueryTab(QueryRunner::Opener Opener, const QString& Title, QObject* Parent = nullptr);
        ~QueryTab() override;

        QWidget* Top() const override { return top_; }
        QWidget* Result() const override { return result_; }
        QString Title() const override;
        QString ToolTip() const override;
        QString GlyphName() const override { return QStringLiteral("sql"); }
        bool IsQuery() const override { return true; }
        bool Modified() const override;
        QString RowsText() const override;
        QString PositionText() const override;
        QString SelectionText() const override;
        GridView* CurrentGrid() const override;
        QWidget* FocusTarget() const override;
        void Cancel() override;
        void ApplyFonts(const QFont& Grid, const QFont& Mono, bool MonoValues) override;

        ScintillaQtDirectBackend* Editor() const { return editor_; }
        QString Text() const;
        void SetText(const QString& Text);
        // 기록에서 고른 문을 커서 자리에 넣는다(줄 중간이면 새 줄로, 끝에 ; 를 붙인다).
        void InsertStatement(const QString& Sql);
        // 쿼리 탭 보관에서 되살릴 때.
        void SetTitle(const QString& Title) { title_ = Title; emit Changed(); }
        void SetRowLimit(int Limit);
        int Caret() const;
        void SetCaret(int Position);
        // .sql 파일과 연결(파일 열기 · 끌어 놓기). 비우면 이름 없는 쿼리.
        void SetFile(const QString& Path);
        const QString& File() const { return file_; }
        bool Save(QString* Error);
        // 문서의 스키마 이름(테이블 · 뷰, 열, 함수). LexSQL 사용자 키워드 1 · 2 · 3 으로 따로 칠한다(설계 5.1).
        void SetSchemaWords(const QStringList& Tables, const QStringList& Columns, const QStringList& Functions);
        // 설정 › 편집기 · 실행. Initial(새 탭)일 때만 행 제한을 바꾼다.
        void SetPreferences(const QueryPreferences& Preferences, bool Initial);
        int RowLimit() const;
        void SetWritable(bool Writable) { writable_ = Writable; }
        // 클립보드를 쓸 수 없는 호스트: 편집기의 복사 키를 문서 창 중개로 보낸다(텍스트 문서와 같은 방식).
        void SetBrokered(bool Enabled);
        bool HasEditorSelection() const;
        bool Running() const { return running_; }
        // 실행기의 연결을 닫는다(rekey 전). 다음 실행에서 새로 연다.
        void ReleaseConnection();
        // 커밋 모드(설계 6.3): 자동(문마다 바로 파일에) · 수동(첫 쓰기 문 앞에서 BEGIN, Ctrl+S 가 커밋).
        void SetManualCommit(bool Manual);
        bool ManualCommit() const { return manual_; }
        bool InTransaction() const { return inTransaction_; }
        int PendingCount() const override { return inTransaction_ ? 1 : 0; }
        // 열린 트랜잭션을 커밋 · 롤백한다. Blocking 이면 끝날 때까지 기다린다(문서를 닫을 때).
        bool EndTransaction(bool Commit, bool Blocking = false, QString* Error = nullptr);
        // 전체(또는 고른 글) / 커서가 있는 문 하나.
        void RunAll();
        void RunCurrent();
        void ToggleComment();
        // 마지막 오류 위치로 간다(메시지의 ‘위치로 이동’).
        void GoToError();

        // 자동 완성(설계 5.2). 후보 자료는 작업 창이 갖고(스키마 · 행 수 · 함수), 탭은 부를 때마다 읽는다.
        using CompletionProvider = std::function<const CompletionSource*()>;
        void SetCompletionProvider(CompletionProvider Provider) { completion_ = std::move(Provider); }
        // Ctrl+Space(Explicit) · 입력 중 자동. 목록을 띄웠으면 true.
        bool Complete(bool Explicit);
        bool CompletionVisible() const;
        hc::document::CompletionPopupWidget* CompletionPopup() const { return popup_; }
        // 함수 매개변수 도움말(Ctrl+Shift+Space, ( 입력). 보였으면 true.
        bool ShowParameterHelp();
        // 오류 빠른 수정(Alt+Enter, 설계 5.4): ‘no such column / table’ 의 이름을 가까운 이름으로. 목록을 띄웠으면 true.
        bool QuickFix();
        // SQL 서식(Alt+Shift+F): 고른 글, 없으면 커서가 든 문.
        void FormatSql();
        // 실행 계획(Ctrl+L): 현재 문 앞에 EXPLAIN QUERY PLAN 을 붙여 트리로. Bytecode 면 EXPLAIN 을 표로.
        void Explain(bool Bytecode = false);
        QTreeWidget* PlanTree() const { return planTree_; }
        // 내보내기: 지금 결과 탭의 모델과 그 결과를 낸 문(없으면 nullptr · 빈 글).
        QueryModel* CurrentResultModel() const;
        QString CurrentResultSql() const;
    signals:
        // 결과 도구 · 도구 모음의 ‘내보내기’ · ‘가져오기’.
        void ExportRequested();
        // 쓰기 문이 끝났다(스키마 · 열린 테이블 탭을 다시 읽는다).
        void DataWritten();
        // 실행한 문(기록, 설계 5.5). 실행 계획 · 바이트코드 보기는 넣지 않는다.
        void Executed(const QList<hc::sqlite::HistoryEntry>& Entries);
        // 중개 모드에서 편집기의 Ctrl+C · Ctrl+Insert.
        void CopyRequested();
        // 연결의 트랜잭션이 열리거나 닫혔다(상태 표시줄 · 커밋).
        void TransactionChanged(bool Open);
    protected:
        bool eventFilter(QObject* Watched, QEvent* Event) override;
    private:
        void run(bool Current);
        void clearResults();
        void addMessage(bool Error, const QString& Where, const QString& Text, int Offset);
        void markError(int Offset);
        void updateStatementBar();
        void applyEditorTheme();
        void applySchemaWords();
        int limit() const;
        QString lineSpan(int Index) const;
        // 자동 완성 · 콜팁.
        struct StatementText { QString text_; int caret_ = 0, start_ = 0; };
        StatementText statementAtCaret() const;
        void charAdded(int Character);
        void refreshCompletion();
        void hideCompletion();
        void insertCompletion(const QString& Text);
        void updateCallTip();
        void showPlan(const QVector<QVector<QVariant>>& Rows);

        QPointer<QWidget> top_, result_;
        ScintillaQtDirectBackend* editor_ = nullptr;
        QPushButton* runButton_ = nullptr;
        QPushButton* currentButton_ = nullptr;
        QComboBox* limit_ = nullptr;
        QLabel* editorNote_ = nullptr;
        QTabBar* resultTabs_ = nullptr;
        QStackedWidget* resultPages_ = nullptr;
        QLabel* note_ = nullptr;
        QToolButton* exportButton_ = nullptr;
        QListWidget* messages_ = nullptr;
        QTimer* barTimer_ = nullptr;
        struct ResultView { QueryModel* model_ = nullptr; GridView* grid_ = nullptr; int index_ = -1; };
        QList<ResultView> results_;
        std::unique_ptr<QueryRunner> runner_;
        QVector<StatementRange> runRanges_;
        QVector<RunStatement> running_Statements_;
        QString title_, file_, savedText_, lastNote_;
        QStringList tables_, columns_, functions_;
        QList<HistoryEntry> executed_;
        QueryPreferences preferences_;
        QFont gridFont_, monoFont_;
        bool monoValues_ = false, hasFonts_ = false, writable_ = true, running_ = false, failed_ = false, brokered_ = false;
        quint64 batch_ = 0;
        int errorPosition_ = -1;
        // 자동 완성
        CompletionProvider completion_;
        hc::document::CompletionPopupWidget* popup_ = nullptr;
        QTimer* completeTimer_ = nullptr;
        CompletionContext shownContext_ = CompletionContext::None;
        bool explicit_ = false, appFilter_ = false, inserting_ = false;
        // 빠른 수정: 오류 메시지와 바꿀 자리(UTF-8 위치).
        QString lastError_;
        int errorEnd_ = -1, fixStart_ = -1, fixEnd_ = -1;
        bool quickFixMode_ = false;
        bool manual_ = false, inTransaction_ = false;
        // 실행 계획
        enum class PlanMode { None, Plan, Bytecode, Transaction };
        PlanMode planMode_ = PlanMode::None;
        QTreeWidget* planTree_ = nullptr;
    };
}
