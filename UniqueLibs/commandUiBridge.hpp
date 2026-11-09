#pragma once
#include <QObject>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QVariantMap>
#include <QStringList>
#include <QHash>
#include <QSet>
#include "commonLibs/operations/fileOperation.hpp"
#include "commonLibs/commands/commandBus.hpp"
#include "commonLibs/commands/automation.hpp"
#include "commonLibs/scripting/scriptApi.hpp"

class QWidget;
class CmpPanel;
class CFSModel;
class CGridView;
class FileJobUi;
class QtViewerHost;
class CQuickView;
class QAction;
namespace hc::document { class DocumentOpenController; }
namespace hc::audio { class AudioSession; }
namespace hc::ui { class AudioPopover; }
namespace hc::ui { class UserMenu; }
namespace hc::sys { class SystemCommands; }

std::shared_ptr<hc::commands::IExecutor> MakeQtExecutor(QObject* Owner);

class CommandUiBridge : public QObject
{
public:
    explicit CommandUiBridge(QWidget* Main);
    ~CommandUiBridge() override;
    hc::storage::Task<hc::commands::CommandOutcome> Dispatch(const QString& Id, const QVariantMap& Parameters,
        const QString& Source, const QModelIndex& Index = {});
    hc::storage::Task<hc::commands::CommandOutcome> DispatchInvocation(hc::commands::CommandInvocation Invocation);
    hc::storage::CommandContext Capture(const QModelIndex& Index = {});
    bool DispatchFolder(const QModelIndex& Index);
    void PromptFileOperation(const QString& Id, const QString& Source, const QModelIndex& Index = {});
    void PromptFileTools(bool Rename, const QModelIndex& Index = {});
    void PromptFileAttributes(const QModelIndex& Index = {});
    void AllowInternetBlock(const QModelIndex& Index = {});
    void PromptArchive(bool Extract, const QModelIndex& Index = {});
    hc::storage::Result<QStringList> CompareArguments(const QModelIndex& Index = {});
    bool ExecuteCommand(const QString& Id, const QModelIndex& Index = {});
    void ShowShellMenu(const QModelIndex& Index = {});
    QAction* Action(const QString& Id) const;
    void RefreshShortcutLabels();
    bool ReloadMenu();
    void RefreshFunctionBar();
    bool CancelClipboard();
    void DropFiles(const QStringList& Paths, const QString& Destination, Qt::DropAction Action);
    bool HasFileClipboard() const { return clipboard_.has_value() || clipboardImporting_; }
    FileJobUi* FileJobs() const { return fileJobs_; }
    hc::audio::AudioSession* Audio() const { return audio_; }
    // 패널 화면 순서로 같은 폴더의 오디오를 모아 Index 항목부터 재생한다. 엔진을 쓸 수 없거나
    // 대상이 아니면 false(호출자는 기존처럼 연결 프로그램을 연다).
    bool StartAudioPreview(CmpPanel* Panel, const QModelIndex& Index);
    void LocateAudio();
    // 파일 목록의 Esc 로 미리 듣기를 끝낸다(재생·일시정지·오류 표시 모두). 끝낼 것이 없으면 false.
    bool CancelAudioPreview();
    bool HasAudioPreview() const;
    // 프로세스·서비스 명령. Source는 명령을 부른 시스템 보기나 상세 보기(없으면 활성 패널).
    bool ExecuteSystemCommand(const QString& Id, const QVariant& Argument = {}, QObject* Source = nullptr);
    hc::sys::SystemCommands* System() const { return system_; }
    std::shared_ptr<hc::commands::EventHub> Events() const { return events_; }
    hc::automation::Host AutomationHost();
    std::shared_ptr<hc::script::IScriptHostApi> ScriptApi(std::set<std::string> Permissions);
private:
    hc::storage::Result<hc::commands::CommandOutcome> executeClipboard(const hc::commands::CommandInvocation& Invocation, CFSModel* Model);
    void refreshClipboardVisuals();
    bool ownsFileClipboard() const;
    void updateClipboard();
    bool clipboardExternal_ = false, clipboardImporting_ = false;
    quint64 clipboardGeneration_ = 0;
    std::stop_source clipboardImportStop_;
    std::optional<hc::operations::FileOperationRequest> clipboard_;
    QByteArray clipboardToken_;
    QSet<QString> cutKeys_;
    QString cutRoot_;
    std::vector<hc::storage::Locator> clipboardLocations_;
    std::map<std::string, QByteArray> clipboardMoves_;
    hc::storage::Result<QStringList> compareArguments(const hc::storage::CommandContext& Context, CFSModel* Model);
    void bindModels();
    void createActions();
    void createCommandUi();
    void refreshActionState();
    void finishCreatedFolderNavigation();
    void finishCreatedFileFocus();
    void submitInlineRename(const QModelIndex& Index, const QString& Name, std::function<void(bool)> Completion);
    QModelIndex commandIndex() const;
    void openDataPlugin(bool Metadata, bool Dopus = false);
    void openIso(bool Inspect);
    void openArchive(std::string InitialFormat = {});
    void extractArchive();
    void openSearch();
    CFSModel* findModel(const std::string& TabId) const;
    hc::storage::Result<hc::commands::CommandOutcome> execute(const hc::commands::CommandInvocation& Invocation);
    hc::storage::CommandContext captureFiles(const QModelIndex& Index, bool Single);
    hc::storage::Result<hc::commands::CommandOutcome> executeFile(const hc::commands::CommandInvocation& Invocation, CFSModel* Model);
    hc::storage::Result<hc::commands::CommandOutcome> executeViewer(const hc::commands::CommandInvocation& Invocation, CFSModel* Model);
    hc::storage::Result<std::string> captureDocument(const hc::commands::CommandInvocation& Invocation, CFSModel* Model);
    hc::storage::Result<hc::commands::CommandOutcome> executeDocument(const hc::commands::CommandInvocation& Invocation, CFSModel* Model);
    void refreshPreview();
    void rememberJobSelection(const std::string& Id, const hc::storage::CommandContext& Context);
    void closeQuick();
    void returnQuickFocus();
    QPointer<QWidget> main_;
    QPointer<CmpPanel> activePanel_;
    std::shared_ptr<hc::commands::IExecutor> executor_;
    std::shared_ptr<hc::commands::CommandRegistry> registry_;
    std::shared_ptr<hc::commands::EventHub> events_;
    std::unique_ptr<hc::commands::CommandDispatcher> dispatcher_;
    hc::commands::EventSubscription errorSubscription_;
    FileJobUi* fileJobs_ = nullptr;
    QtViewerHost* viewers_ = nullptr;
    hc::audio::AudioSession* audio_ = nullptr;
    QPointer<hc::ui::AudioPopover> audioPopover_;
    void createAudioUi(QWidget* Window);
    void refreshAudioActions();
    void refreshNowPlayingAnimation();
    hc::document::DocumentOpenController* documents_ = nullptr;
    QPointer<CmpPanel> quickSource_, quickPanel_;
    QPointer<CQuickView> quick_;
    QString quickKey_;
    QHash<QString, QPointer<QAction>> actions_;
    hc::ui::UserMenu* userMenu_ = nullptr;
    hc::sys::SystemCommands* system_ = nullptr;
    int functionBarKind_ = -2;              // 기능 키 막대가 보이는 보기 종류(-1 파일, 0 프로세스, 1 서비스)
    QSet<QString> systemDisabled_;          // 시스템 보기 때문에 끈 명령. 파일 보기로 돌아오면 다시 켠다
    // Modal commands process watcher refreshes; resets must invalidate the
    // saved proxy index before action-state updates map it back to the source.
    QPersistentModelIndex commandIndex_;
    std::map<std::string, std::vector<hc::storage::Locator>> affectedLocations_;
    struct JobSelection { QPointer<CGridView> view_; QPointer<CFSModel> model_; QString location_; QStringList keys_; };
    std::map<std::string, JobSelection> jobSelections_;
    std::map<std::string, std::function<void(bool)>> inlineRenameCompletions_;
    struct CreatedFolderNavigation { std::string tab_; hc::storage::Locator source_, target_; QPointer<CFSModel> model_; bool ready_ = false; };
    std::map<std::string, CreatedFolderNavigation> createdFolderNavigation_;
    struct CreatedFileFocus { std::string tab_; hc::storage::Locator source_; QPointer<CFSModel> model_; QString name_; bool ready_ = false; };
    std::map<std::string, CreatedFileFocus> createdFileFocus_;
};
