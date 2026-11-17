#pragma once

/// 고유 라이브러리
#include "sqliteConnection.hpp"
#include "sqliteFormat.hpp"
#include "sqliteHistory.hpp"
#include "sqliteCompletion.hpp"
#include "sqliteSchema.hpp"
#include "sqliteSeeDialog.hpp"
#include "sqliteKeyStore.hpp"
#include "sqliteTransfer.hpp"
#include "sqliteWorkTab.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <memory>
#include <mutex>
#include <QFont>
#include <QList>
#include <QWidget>

class QAction;
class QLabel;
class QListWidget;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTimer;
class QToolBar;

namespace hc::sqlite
{
    class SchemaPanel;
    class TableTab;
    class TableModel;
    class QueryTab;
    class GridView;
    class WorkTab;
    class WorkerJob;

    // 상태 표시줄 칸 하나(문서 창의 DocumentStatusField 로 옮긴다).
    struct StatusItem
    {
        QString id_, text_, compact_, detail_;
        bool essential_ = true, trailing_ = false;
        bool warning_ = false;              // 주황(커밋 대기 · 열린 트랜잭션)
    };

    // 설정 › SQLite › 글꼴(설계 4.5). 문서 창이 설정에서 읽어 넘긴다.
    struct ViewFonts
    {
        QFont grid_, schema_, mono_;
        bool customGrid_ = false, customSchema_ = false, monoValues_ = false;
    };

    // SQLite 데이터베이스 문서 하나의 작업 창(설계 3 · 4 · 5절, 시안 ‘SQLite 작업 창’ · ‘빠른 보기’).
    // Editor(F4 HyperEditor) · Viewer(F3 HyperViewer)는 같은 작업 창이고, QuickPreview(Ctrl+Q)는 열람만 하는 좁은 화면이다.
    // 명령은 문서 창의 단축키 라우터가 범위(sqlite · sqlite.query · sqlite.grid · sqlite.schema)로 부른다.
    class DocumentView final : public QWidget
    {
        Q_OBJECT
    public:
        enum class Mode { Editor, Viewer, QuickPreview };
        DocumentView(const QString& Path, Mode ViewMode, QWidget* Parent = nullptr);
        ~DocumentView() override;

        // 파일 머리를 보고 연결 · 스키마를 연다. 한 번만 부른다(문서 창이 이벤트 루프에서 부른다).
        void Open(const ProtectedKey& InitialKey = {});
        Mode ViewMode() const { return mode_; }
        const QString& Path() const { return path_; }
        bool IsBusy() const;
        bool IsReady() const { return ready_; }
        bool IsLocked() const { return locked_; }
        bool IsReadOnly() const;
        const QString& Error() const { return error_; }
        FileKind Kind() const { return header_.kind_; }
        // 닫아도 되는가: 저장하지 않은 .sql 쿼리 탭을 묻는다(커밋하지 않은 편집은 보류 편집 단계에서 더한다).
        bool RequestClose();
        // 실행 중인 쿼리 · 읽기를 멈춘다(문서 창의 ‘작업 취소’, Ctrl+.).
        void Cancel();
        QWidget* InputWidget() const;
        void FocusInput();
        QList<StatusItem> StatusItems() const;
        // 그리드에서 고른 칸 또는 편집기에서 고른 글(복사 · 클립보드 중개).
        QString SelectionText() const;
        bool HasSelection() const;
        // 문서 창 도구 모음 · 메뉴에 들어갈 동작.
        QList<QAction*> Actions() const { return actions_; }
        void ApplyFonts(const ViewFonts& Fonts);
        // 설정 › 편집기 · 실행. 열린 쿼리 탭에도 적용하되 행 제한은 새 탭부터 쓴다.
        void ApplyQueryPreferences(const QueryPreferences& Preferences);
        void SetClipboardBrokered(bool Enabled);
        // 실행 기록 · 쿼리 탭 보관을 둘 프로필 폴더(문서 창 설정 폴더). 비우면 남기지 않는다. Open 전에 부른다.
        void SetProfileDirectory(const QString& Directory) { profile_ = Directory; }
        // 쿼리 탭 보관을 지금 쓴다(닫을 때 · 바뀐 뒤 잠시 뒤에도 쓴다).
        void SaveTabs();

        // 명령(단축키 · 도구 모음). Id 는 DocumentShortcutRegistry 의 sqlite.* 명령.
        bool CanRunCommand(const QString& Id) const;
        bool RunCommand(const QString& Id);
        // 단축키 범위가 지금 포커스에서 켜져 있는가.
        bool OwnsShortcutScope(const QString& Scope, QWidget* Focus) const;
        // 커밋(설계 6.2 · 6.3, Ctrl+S): 쿼리 탭의 열린 트랜잭션, 그다음 표의 보류 편집. 커밋할 것이 있는가.
        bool Commit();
        int PendingTotal() const;
        bool HasPending() const { return PendingTotal() > 0; }
        // 메뉴 ‘데이터베이스’ 에 넣을 동작(도구 모음의 ‘자동 | 수동’ 대신 두 선택 항목).
        QList<QAction*> MenuActions() const { return menuActions_.isEmpty() ? actions_ : menuActions_; }

        // 탭.
        void OpenObject(const QString& Database, const QString& Name);
        QueryTab* NewQuery(const QString& Text = {}, const QString& File = {});
        WorkTab* CurrentTab() const;
        int TabCount() const;
        // 시험 · 자동화: 열린 테이블 탭 이름, 현재 표 모델.
        QStringList OpenTables() const;
        TableModel* CurrentModel() const;
        const SchemaInfo& Schema() const { return schema_; }

        // SEE(설계 8.2): 이 키로 연다(잠긴 문서). 열었으면 true. Save 면 키 저장소에 넣는다. 틀린 키면 WrongKey.
        bool Unlock(const QByteArray& Key, KeyFormat Format, bool ReadOnly, bool Save, bool* WrongKey = nullptr, QString* Error = nullptr);
        // 키 대화를 띄운다(잠긴 문서, 빠른 보기는 묻지 않는다). 열었으면 true.
        bool RequestKey();
        bool IsEncrypted() const { return encrypted_; }
        KeyFormat EncryptionFormat() const { return keyFormat_; }
        // 저장한 키로 열었는가(빠른 보기 · 저절로 연 경우).
        bool UsedSavedKey() const { return usedSavedKey_; }
        // 시험 · 자동화: 잠긴 문서를 보일 때 키 대화를 저절로 띄우지 않는다. Open 전에 부른다.
        void SetKeyPromptEnabled(bool Enabled) { promptEnabled_ = Enabled; }
        // 데이터베이스 › 암호화…(설계 8.1): 키 바꾸기 · 해제 · 암호화. 다른 연결을 닫고 rekey 한다. 저장한 키도 맞춘다.
        bool ChangeEncryption(SeeEncryptDialog::Operation Operation, const QByteArray& Key, KeyFormat Format, bool Backup, QString* Error = nullptr);

        // 가져오기 · 내보내기(설계 10절 P4). Export · Import 는 대화 없이 작업 스레드에서 돌리고 끝날 때까지 기다린다(진행 창).
        // 내보낼 대상: 테이블 · 뷰(ExportRequestFor), 지금 쿼리 결과(CurrentResultExport).
        ExportRequest ExportRequestFor(const QString& Database, const QString& Name) const;
        bool CurrentResultExport(ExportRequest* Request) const;
        TransferResult Export(ExportRequest Request);
        TransferResult Import(const ImportSource& Source, const ImportRequest& Request);
        // 대화로 묻는다. Path 가 .sql 이면 쿼리 탭으로 연다.
        bool ShowImport(const QString& Table = {}, const QString& Path = {});
        bool ShowExport(const ExportRequest& Request);
        // 스크립트 만들기(스키마 메뉴): SELECT · INSERT · UPDATE · DELETE · CREATE 문의 틀.
        QString ScriptFor(const QString& Database, const QString& Name, const QString& Kind) const;
    signals:
        void StateChanged();
        // 설정 창의 한 페이지를 열어 달라("sqlite-keys": 비밀번호 관리).
        void SettingsRequested(const QString& Page);
        // 클립보드가 막힌 호스트에서 그리드의 Ctrl+C.
        void CopyRequested();
    protected:
        void showEvent(QShowEvent* Event) override;
    private:
        void buildWorkWindow();
        void buildPreview();
        void openDatabase();
        void finishOpen();
        // SEE: 키를 시도하고, 맞으면 그 연결로 문서를 연다. 저장한 키를 먼저 시도한다(설계 8.2 의 2).
        SeeKeyDialog::Outcome tryKey(const QByteArray& Key, KeyFormat Format, bool ReadOnly);
        bool tryStoredKey();
        void schedulePrompt();
        bool showEncryptDialog(QWidget* Parent);
        // 작업 스레드 연결에 문서의 키를 넣는다(키가 없으면 그대로).
        bool applyKey(Connection& Db, QString* Error) const;
        // 쿼리 탭 실행기 · 행 수 세기 연결을 닫는다(rekey 전).
        void releaseConnections();
        using TransferWork = std::function<TransferResult(Connection&, const std::atomic_bool&, std::atomic<qint64>&)>;
        TransferResult runTransfer(const QString& Title, qint64 Total, TransferWork Work);
        bool exportCurrent();
        void readSchema();
        void startCounting();
        void stopCounting();
        void showDdl(const QString& Database, const QString& Name);
        void attachDatabase();
        void detachDatabase(const QString& Name);
        void addTab(WorkTab* Tab);
        bool closeTab(int Index, bool Ask = true);
        void activateTab(int Index);
        void refreshTabTitle(WorkTab* Tab);
        void refreshSchemaWords();
        void dataWritten();
        bool saveQuery(QueryTab* Tab, bool As);
        void openSqlFile();
        void selectFromSchema();
        void configureGrid(GridView* Grid);
        void showPreviewObject(const QString& Database, const QString& Name);
        void refreshPreviewStructure();
        void setFailure(const QString& Error);
        TableTab* currentTable() const;
        QueryTab* currentQuery() const;
        std::unique_ptr<Connection> openWorker(QString* Error) const;
        void loadProfile();
        void restoreTabs();
        void recordHistory(const QList<HistoryEntry>& Entries);
        // 구조 바꾸기(설계 7절)와 데이터베이스 정보.
        struct SchemaTarget { QString kind_, database_, name_, table_; };
        SchemaTarget schemaTarget() const;
        bool structureBlocked();
        bool editTable(bool Create);
        bool renameObject();
        bool dropObject();
        bool emptyTable();
        void showDatabaseInfo();
        bool commitTables();
        bool commitAll(bool Blocking);
        void rollbackAll(bool Blocking);
        void refreshCommitMode();

        QString path_;
        Mode mode_;
        HeaderInfo header_;
        OpenOptions options_;
        std::unique_ptr<Connection> db_;
        SchemaInfo schema_;
        QStringList functions_;
        QString journalMode_, error_, pendingCopy_;
        bool opening_ = false, ready_ = false, locked_ = false, opened_ = false, brokered_ = false;
        // SEE: 작업 스레드 연결용 키는 DPAPI 로 감싼 값만 둔다(설계 8.3). 작업 스레드가 읽으므로 잠금으로 지킨다.
        mutable std::mutex keyMutex_;
        QByteArray keyProtected_;
        KeyFormat keyFormat_ = KeyFormat::Text;
        bool encrypted_ = false, usedSavedKey_ = false, promptEnabled_ = true, keyPrompted_ = false;
        QPushButton* keyButton_ = nullptr;
        int queryCounter_ = 0;
        std::shared_ptr<WorkerJob> counting_;
        QList<std::shared_ptr<WorkerJob>> retiredJobs_;
        ViewFonts fonts_;
        bool hasFonts_ = false;
        QueryPreferences queryPreferences_;
        CompletionSource completion_;   // 쿼리 탭 자동 완성 자료(스키마 · 행 수 · 함수 · PRAGMA)
        QString profile_;
        HistoryPanel* history_ = nullptr;
        QTimer* tabsTimer_ = nullptr;
        bool restoring_ = false;
        QList<QAction*> actions_, menuActions_;
        class CommitModeAction* commitMode_ = nullptr;
        bool tablesAfterCommit_ = false;
        QStackedWidget* stack_ = nullptr;       // 작업 창 · 잠김 · 오류
        QLabel* failure_ = nullptr;
        QWidget* lockedPage_ = nullptr;
        // 작업 창
        QSplitter* split_ = nullptr;
        QSplitter* rightSplit_ = nullptr;
        SchemaPanel* schemaPanel_ = nullptr;
        QWidget* schemaArea_ = nullptr;
        QTabBar* tabs_ = nullptr;
        QStackedWidget* topStack_ = nullptr;
        QStackedWidget* resultStack_ = nullptr;
        QWidget* emptyTop_ = nullptr;
        QWidget* emptyResult_ = nullptr;
        QList<WorkTab*> works_;
        // 빠른 보기
        QListWidget* previewList_ = nullptr;
        QLabel* previewTitle_ = nullptr;
        QLabel* previewHead_ = nullptr;
        QStackedWidget* previewPages_ = nullptr;
        QWidget* previewSegments_ = nullptr;
        TableModel* previewModel_ = nullptr;
        GridView* previewGrid_ = nullptr;
        QListWidget* previewColumns_ = nullptr;
        QLabel* previewDdl_ = nullptr;
        QString previewDatabase_, previewName_;
    };
}
