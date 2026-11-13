#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <functional>
#include <QFont>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QWidget>

class QAbstractButton;
class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QMenu;
class QModelIndex;
class QStackedWidget;
class QTimer;
class QToolButton;
class QTreeView;
class QVBoxLayout;
namespace fm::ui { class Banner; class Button; class Card; class FindBox; class Tag; class ToolTip; }
namespace hc::document::explorer { class FileSystemModel; class FilterProxy; class OutsideProxy; class FilterWalker; }

namespace hc::document
{
    class ExplorerDelegate;

    // 탐색기 도구 창의 내용(설계 5.2, 시안 ‘탐색기 패널 — 상태별’).
    // 위에서 아래로: 머리줄(이름 · Sphinx 수 · 단추) · 경로 줄 · 필터 · 알림 · 선택 카드 · 트리 · 빈 상태 · 바닥줄.
    // 화면과 파일 만들기 · 지우기는 여기서 하고, 열린 문서와 얽히는 일(열기 · 이름 바꾸기 · 워크스페이스)은
    // 신호로 문서 작업 공간에 넘긴다. 보기 전용(HyperViewer)이면 바꾸는 동작을 모두 숨긴다.
    class ExplorerPanel final : public QWidget
    {
        Q_OBJECT
    public:
        enum class Mode { Empty, Document, Workspace };
        struct RecentWorkspace { QString root_, name_; };
        struct OpenDocument { QString path_; bool modified_ = false; };
        // 상위 워크스페이스 선택 카드의 한 줄. root_ 가 비면 ‘워크스페이스 없이 열기’.
        struct PickChoice { QString root_, name_, tag_, description_; bool newWorkspace_ = false; bool checked_ = false; };
        struct Settings
        {
            bool autoReveal_ = true, showAllFiles_ = false, showHidden_ = false, showAllDrives_ = false, confirmRecycle_ = true;
            QStringList excluded_, openableSuffixes_;
            QFont font_;
        };

        explicit ExplorerPanel(bool ReadOnly, QWidget* Parent = nullptr);
        ~ExplorerPanel() override;

        // 워크스페이스 모드: 루트 고정. 빈 문자열이면 문서 모드(또는 빈 상태)로 돌아간다.
        void SetWorkspace(const QString& Root, const QString& Name = {});
        // 문서 모드의 문서 폴더(포커스 문서의 폴더). 워크스페이스 모드에서는 기억만 한다.
        void SetDocumentFolder(const QString& Folder);
        Mode CurrentMode() const { return mode_; }
        QString WorkspaceRoot() const { return workspaceRoot_; }
        QString DocumentFolder() const { return documentFolder_; }
        void ApplySettings(const Settings& Value);
        const Settings& CurrentSettings() const { return settings_; }
        void SetSphinxRoots(const QStringList& Roots);
        void SetOpenDocuments(const QList<OpenDocument>& Documents);
        // 포커스 문서. 자동 찾기가 켜져 있으면 펼쳐서 고른다.
        void SetActiveDocument(const QString& Path);
        // 현재 문서 위치 단추 · Ctrl+Shift+E: 자동 찾기와 상관없이 찾는다.
        void RevealActive(bool FocusTree);
        void SetRecentWorkspaces(const QList<RecentWorkspace>& Items);
        // 알림(Banner). Actions 의 i 번째 단추를 누르면 NoticeAction(Id, i).
        void ShowNotice(const QString& Id, int Tone, const QString& Title, const QString& Detail, const QStringList& Actions = {}, bool Closable = true);
        void HideNotice(const QString& Id = {});
        QString NoticeId() const { return noticeId_; }
        void ShowPick(const QString& FileName, const QList<PickChoice>& Choices);
        void HidePick();
        bool PickVisible() const;
        // 워크스페이스 상태(펼친 폴더 · 고른 항목 · 맨 위 항목). 경로는 절대 경로.
        QStringList ExpandedPaths() const;
        QString SelectedPath() const;
        QString TopPath() const;
        void RestoreExpansion(const QStringList& Expanded, const QString& Selected, const QString& Top);
        // 단축키 범위: 트리 자체에 포커스가 있을 때만(필터 칸 · 이름 입력 칸은 뺀다).
        bool OwnsShortcutFocus(QWidget* Focus) const;
        QTreeView* Tree() const { return tree_; }
        // 명령(단축키 · 메뉴).
        void OpenCurrent(bool Beside);
        void RenameCurrent();
        void DeleteCurrent(bool Permanent);
        void CopyCurrentPath(bool Relative);
        void NewFile();
        void NewFolder();
        void CollapseAll();
        void Refresh();
        void FocusTree();
        // 이름 바꾸기가 끝났다(문서 작업 공간이 열린 탭을 정리한 뒤). 실패면 Error 를 입력 칸 아래에 보인다.
        void FinishRename(const QString& OldPath, const QString& NewPath, const QString& Error);
        // 메뉴에 보일 단축키 글(명령 ID → 지금 키). 작업 공간이 단축키 설정을 반영해 준다.
        void SetShortcutText(std::function<QString(const QString&)> Provider) { keyText_ = std::move(Provider); }
    signals:
        void OpenRequested(const QString& Path, bool Beside);
        void OpenFolderRequested();
        void OpenWorkspaceRequested(const QString& Folder);
        void FindInFolderRequested(const QString& Folder);
        void BuildRequested(const QString& Folder);
        void EditInEditorRequested(const QString& Path);
        void OpenFolderInEditorRequested(const QString& Folder);
        // 열린 탭이 걸린 이름 바꾸기는 작업 공간이 한다. 끝나면 FinishRename 을 부른다.
        void RenameRequested(const QString& Path, const QString& NewName);
        void Created(const QString& Path, bool Folder);
        void NoticeAction(const QString& Id, int Index);
        void NoticeClosed(const QString& Id);
        void PickChosen(int Index, bool Remember);
        void PickDeferred();
        void StateChanged();
        // 더 보기 메뉴에서 바꾼 표시 설정(작업 공간이 설정 파일에 남긴다).
        void SettingChanged(const QString& Key, bool Value);
        void ShowSettingsRequested();
    protected:
        bool eventFilter(QObject* Watched, QEvent* Event) override;
        void changeEvent(QEvent* Event) override;
    private:
        friend class ExplorerDelegate;
        struct RowInfo
        {
            QString path_, dim_, tag_;
            bool valid_ = false, folder_ = false, group_ = false, outside_ = false, modified_ = false, tagWorkspace_ = false, match_ = false;
        };
        RowInfo rowInfo(const QModelIndex& Index) const;
        QModelIndex viewIndex(const QString& Path) const;
        QString pathOf(const QModelIndex& Index) const;
        QString currentPath() const;
        QString targetFolder() const;
        void applyMode();
        void applyRoot();
        void refreshHeader();
        void refreshOutside();
        void reveal(const QString& Path, bool Select, bool Scroll);
        void expandChain(const QString& Path);
        void collapseAutoExpanded(const QString& Keep);
        void showContextMenu(const QPoint& Position);
        void showMoreMenu();
        void startFilter();
        void startEdit(const QString& Path, bool Created, bool Folder);
        void commitEdit();
        void cancelEdit();
        // Discard: 방금 만든 빈 항목이면 지운다(Esc).
        void cancelEdit(bool Discard);
        void showEditError(const QString& Error);
        void createEntry(bool Folder);
        void scanMarkers(const QString& Folder);
        void rebuildRecent();
        bool insideSphinx(const QString& Path) const;

        bool readOnly_;
        Mode mode_ = Mode::Empty;
        QString workspaceRoot_, workspaceName_, documentFolder_, activePath_, noticeId_;
        Settings settings_;
        explorer::FileSystemModel* model_ = nullptr;
        explorer::FilterProxy* filter_ = nullptr;
        explorer::OutsideProxy* outside_ = nullptr;
        explorer::FilterWalker* walker_ = nullptr;
        QTreeView* tree_ = nullptr;
        ExplorerDelegate* delegate_ = nullptr;
        QLabel* headIcon_ = nullptr;
        QLabel* title_ = nullptr;
        fm::ui::Tag* chip_ = nullptr;
        QToolButton* newFile_ = nullptr;
        QToolButton* newFolder_ = nullptr;
        QToolButton* locate_ = nullptr;
        QToolButton* collapse_ = nullptr;
        QToolButton* more_ = nullptr;
        QLabel* path_ = nullptr;
        fm::ui::Button* pathLink_ = nullptr;
        QWidget* filterRow_ = nullptr;
        fm::ui::FindBox* filterBox_ = nullptr;
        QTimer* filterDebounce_ = nullptr;
        QLabel* footer_ = nullptr;
        fm::ui::Banner* notice_ = nullptr;
        fm::ui::Card* pick_ = nullptr;
        QLabel* pickText_ = nullptr;
        QVBoxLayout* pickRows_ = nullptr;
        QButtonGroup* pickGroup_ = nullptr;
        QCheckBox* pickRemember_ = nullptr;
        QWidget* empty_ = nullptr;
        QVBoxLayout* recentLayout_ = nullptr;
        QLabel* recentTitle_ = nullptr;
        QWidget* viewerFooter_ = nullptr;
        QLineEdit* editor_ = nullptr;
        fm::ui::ToolTip* editTip_ = nullptr;
        QString editPath_;
        bool editCreated_ = false, editFolder_ = false, expanding_ = false;
        QList<RecentWorkspace> recent_;
        QHash<QString, bool> openDocuments_;
        QSet<QString> sphinxRoots_, autoExpanded_, markerScanned_;
        QHash<QString, int> markers_;
        QStringList savedExpansion_;
        QString pendingReveal_;
        bool pendingSelect_ = false;
        qint64 lastUserScroll_ = 0, revealDeadline_ = 0;
        std::function<QString(const QString&)> keyText_;
        void retryReveal();
        QString key(const QString& Id) const { return keyText_ ? keyText_(Id) : QString(); }
    };
}
