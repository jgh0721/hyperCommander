#include "stdafx.h"
#include "explorerPanel.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일
#include "explorerFileOps.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "UniqueLibs/document/explorer/explorerFilterWalker.hpp"
#include "UniqueLibs/document/explorer/explorerModel.hpp"
#include "UniqueLibs/document/explorer/explorerNames.hpp"
#include "UniqueLibs/document/explorer/remoteDrive.hpp"
#include "UniqueLibs/document/workspace/workspaceDiscovery.hpp"
#include "UniqueLibs/document/workspace/workspacePaths.hpp"

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/StylePaint.h>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/Banner.h>
#include <fmwidgets/Button.h>
#include <fmwidgets/Card.h>
#include <fmwidgets/DialogCards.h>
#include <fmwidgets/DialogWidgets.h>
#include <fmwidgets/FindBox.h>
#include <fmwidgets/Menu.h>
#include <fmwidgets/Tag.h>
#include <fmwidgets/ToolTip.h>

/// 선언 헤더파일들
#include "core/ToolbarIcons.h"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace hc::document
{
    namespace
    {
        using workspace::PathKey;
        using workspace::NormalizePath;

        QString Native(const QString& Path) { return QDir::toNativeSeparators(Path); }
        QString NameOf(const QString& Path)
        {
            const auto Name = QFileInfo(Path).fileName();
            return Name.isEmpty() ? Native(Path) : Name;
        }
        // 로컬 고정 디스크만 자식 폴더의 표시(.multiroot)를 확인한다 — 네트워크에서 폴더마다 묻지 않는다(설계 5.4).
        bool LocalFixed(const QString& Path)
        {
            if (explorer::IsUncPath(Path)) return false;
            const auto Root = Native(explorer::DriveRootOf(Path));
            return !Root.isEmpty() && GetDriveTypeW(reinterpret_cast<LPCWSTR>(Root.utf16())) == DRIVE_FIXED;
        }
        QString WithKey(const QString& Label, const QString& Key) { return Key.isEmpty() ? Label : Label + QLatin1Char('\t') + Key; }
    }

    // 트리 한 줄: 아이콘 · 이름(필터 일치 강조) · 바깥 파일의 폴더(흐리게) · 오른쪽 끝 표시 칩 · 저장하지 않은 문서 점.
    class ExplorerDelegate final : public QStyledItemDelegate
    {
    public:
        explicit ExplorerDelegate(ExplorerPanel* Panel) : QStyledItemDelegate(Panel), panel_(Panel) {}
        QSize sizeHint(const QStyleOptionViewItem& Option, const QModelIndex& Index) const override
        {
            auto Size = QStyledItemDelegate::sizeHint(Option, Index);
            Size.setHeight(qMax(Size.height(), 22));
            return Size;
        }
        void paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override
        {
            QStyleOptionViewItem Opt(Option); initStyleOption(&Opt, Index);
            const auto Info = panel_->rowInfo(Index);
            const QString Text = Opt.text; Opt.text.clear();
            auto* Style = Opt.widget ? Opt.widget->style() : QApplication::style();
            Style->drawControl(QStyle::CE_ItemViewItem, &Opt, Painter, Opt.widget);
            const QRect TextRect = Style->subElementRect(QStyle::SE_ItemViewItemText, &Opt, Opt.widget);
            const auto& Tc = fm::style::themeColorsFor(Opt.widget);
            using T = fm::style::Token;
            const bool Selected = Opt.state.testFlag(QStyle::State_Selected);
            const bool Active = Opt.state.testFlag(QStyle::State_Active) && Opt.widget && Opt.widget->hasFocus();
            const QColor TextColor = Selected && Active ? Opt.palette.color(QPalette::HighlightedText) : Opt.palette.color(QPalette::Text);
            Painter->save();
            Painter->setRenderHint(QPainter::Antialiasing);
            int Right = TextRect.right() - 2;
            if (Info.modified_) {
                const QRectF Dot(Right - 7, TextRect.center().y() - 3.5, 7, 7);
                Painter->setPen(Qt::NoPen); Painter->setBrush(Selected && Active ? TextColor : Tc[T::Accent]);
                Painter->drawEllipse(Dot); Right -= 12;
            }
            if (!Info.tag_.isEmpty()) {
                QFont Chip = Opt.font; Chip.setPixelSize(qMax(9, QFontInfo(Opt.font).pixelSize() - 2)); Chip.setWeight(QFont::DemiBold);
                const int Width = QFontMetrics(Chip).horizontalAdvance(Info.tag_) + 10;
                if (Right - Width > TextRect.left() + 40) {
                    const QRectF Box(Right - Width, TextRect.center().y() - 7.5, Width, 15);
                    const bool On = Selected && Active;
                    Painter->setPen(QPen(On ? TextColor : Info.tagWorkspace_ ? Tc[T::Accent] : Tc[T::Line], 1.0));
                    Painter->setBrush(On ? Qt::transparent : Info.tagWorkspace_ ? Tc[T::AccentSoft] : Tc[T::Alt]);
                    Painter->drawRoundedRect(Box.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
                    Painter->setFont(Chip);
                    Painter->setPen(On ? TextColor : Info.tagWorkspace_ ? Tc[T::AccentFg] : Tc[T::KCode]);
                    Painter->drawText(Box, Qt::AlignCenter, Info.tag_);
                    Right -= Width + 6;
                }
            }
            QFont Font = Opt.font; if (Info.group_) Font.setWeight(QFont::DemiBold);
            Painter->setFont(Font);
            const QFontMetrics Metrics(Font);
            const int Available = qMax(0, Right - TextRect.left());
            const QString Shown = Metrics.elidedText(Text, Qt::ElideRight, Available);
            int X = TextRect.left();
            const int Baseline = TextRect.top() + (TextRect.height() + Metrics.ascent() - Metrics.descent()) / 2;
            // 부분 일치면 맞은 글자를 강조한다(와일드카드는 이름 전체가 맞은 것이라 따로 칠하지 않는다).
            const QString Filter = panel_->filter_ ? panel_->filter_->FilterText() : QString();
            const qsizetype At = Info.match_ && !panel_->filter_->Matcher().IsWildcard() ? Shown.indexOf(Filter, 0, Qt::CaseInsensitive) : -1;
            if (At >= 0) {
                const QString Before = Shown.left(At), Hit = Shown.mid(At, Filter.size()), After = Shown.mid(At + Filter.size());
                Painter->setPen(TextColor); Painter->drawText(X, Baseline, Before); X += Metrics.horizontalAdvance(Before);
                const QRect Mark(X - 1, TextRect.center().y() - Metrics.height() / 2, Metrics.horizontalAdvance(Hit) + 2, Metrics.height());
                if (!(Selected && Active)) { Painter->setPen(Qt::NoPen); Painter->setBrush(Tc[T::AccentSoft]); Painter->drawRoundedRect(Mark, 2, 2); }
                QFont Bold = Font; Bold.setWeight(QFont::DemiBold); Painter->setFont(Bold);
                Painter->setPen(Selected && Active ? TextColor : Tc[T::AccentFg]); Painter->drawText(X, Baseline, Hit); X += QFontMetrics(Bold).horizontalAdvance(Hit);
                Painter->setFont(Font); Painter->setPen(TextColor); Painter->drawText(X, Baseline, After); X += Metrics.horizontalAdvance(After);
            } else {
                Painter->setPen(TextColor); Painter->drawText(X, Baseline, Shown); X += Metrics.horizontalAdvance(Shown);
            }
            if (!Info.dim_.isEmpty() && X + 24 < Right) {
                QFont Small = Font; Small.setPixelSize(qMax(9, QFontInfo(Font).pixelSize() - 1)); Small.setWeight(QFont::Normal);
                Painter->setFont(Small); Painter->setPen(Selected && Active ? TextColor : Tc[T::Fg3]);
                Painter->drawText(QRect(X + 6, TextRect.top(), Right - X - 6, TextRect.height()), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(Small).elidedText(Info.dim_, Qt::ElideLeft, Right - X - 6));
            }
            Painter->restore();
        }
    private:
        ExplorerPanel* panel_;
    };

    ExplorerPanel::ExplorerPanel(bool ReadOnly, QWidget* Parent)
        : QWidget(Parent), readOnly_(ReadOnly)
    {
        setObjectName("documentExplorer");
        auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0); Layout->setSpacing(0);
        // 머리줄(28 px): 폴더 아이콘 · 이름 · Sphinx 수 · 단추 다섯.
        auto* Head = new QWidget(this); Head->setObjectName("explorerHeader"); Head->setFixedHeight(28);
        auto* HeadLayout = new QHBoxLayout(Head); HeadLayout->setContentsMargins(8, 0, 4, 0); HeadLayout->setSpacing(6);
        headIcon_ = new QLabel(Head); headIcon_->setFixedSize(16, 16);
        title_ = new QLabel(tr("탐색기"), Head); title_->setObjectName("explorerTitle");
        QFont Bold = title_->font(); Bold.setWeight(QFont::DemiBold); title_->setFont(Bold);
        title_->setTextFormat(Qt::PlainText); title_->setMinimumWidth(20);
        chip_ = new fm::ui::Tag(Head); chip_->setObjectName("explorerSphinxCount"); chip_->setCompact(true); chip_->setTone(fm::ui::Tag::Info); chip_->hide();
        HeadLayout->addWidget(headIcon_); HeadLayout->addWidget(title_, 1); HeadLayout->addWidget(chip_);
        const auto Tool = [this, Head, HeadLayout](const QString& Icon, const QString& Tip, const char* Name) {
            auto* Button = new QToolButton(Head); Button->setObjectName(QString::fromLatin1(Name));
            Button->setAutoRaise(true); Button->setIconSize(QSize(16, 16)); Button->setFixedSize(22, 22);
            Button->setIcon(ToolbarIcons::iconByName(Icon)); Button->setToolTip(Tip); Button->setAccessibleName(Tip);
            // 단추는 Tab 순서에 들지 않는다 — 키보드는 트리 · 필터에 둔다(MRE MainWindow.cpp:4773-4776).
            Button->setFocusPolicy(Qt::NoFocus);
            HeadLayout->addWidget(Button); return Button;
        };
        newFile_ = Tool("new-file", tr("새 파일"), "explorerNewFile");
        newFolder_ = Tool("new-folder", tr("새 폴더"), "explorerNewFolder");
        locate_ = Tool("locate", tr("현재 문서 위치"), "explorerLocate");
        collapse_ = Tool("collapse-all", tr("모두 접기"), "explorerCollapseAll");
        more_ = Tool("more", tr("더 보기"), "explorerMore");
        Layout->addWidget(Head);
        // 경로 줄(20 px).
        auto* PathRow = new QWidget(this); PathRow->setObjectName("explorerPathRow"); PathRow->setFixedHeight(20);
        auto* PathLayout = new QHBoxLayout(PathRow); PathLayout->setContentsMargins(8, 0, 6, 0); PathLayout->setSpacing(6);
        path_ = new QLabel(PathRow); path_->setObjectName("explorerPath"); path_->setTextFormat(Qt::PlainText);
        path_->setMinimumWidth(10); path_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        pathLink_ = new fm::ui::Button(tr("워크스페이스로 열기"), PathRow); pathLink_->setObjectName("explorerOpenAsWorkspace");
        pathLink_->setRole(fm::ui::Button::Link); pathLink_->setFocusPolicy(Qt::TabFocus); pathLink_->hide();
        PathLayout->addWidget(path_, 1); PathLayout->addWidget(pathLink_);
        Layout->addWidget(PathRow);
        // 필터.
        filterRow_ = new QWidget(this); auto* FilterLayout = new QHBoxLayout(filterRow_); FilterLayout->setContentsMargins(8, 2, 8, 6);
        filterBox_ = new fm::ui::FindBox(filterRow_); filterBox_->setObjectName("explorerFilter");
        filterBox_->setPlaceholderText(tr("필터 (부분 일치, * ? 가능)"));
        filterBox_->setToolTip(tr("이름의 일부를 치면 걸러집니다. `*` 나 `?` 를 넣으면 와일드카드로 봅니다."));
        filterBox_->setClearButtonEnabled(true);
        FilterLayout->addWidget(filterBox_);
        Layout->addWidget(filterRow_);
        // 알림(MRE 세션 가져옴 · 상위 워크스페이스 열었음 · 프로필 대체 저장).
        notice_ = new fm::ui::Banner(this); notice_->setObjectName("explorerNotice"); notice_->setClosable(true); notice_->hide();
        notice_->setIconAlignment(fm::ui::Banner::IconTop);
        auto* NoticeHolder = new QWidget(this); auto* NoticeLayout = new QVBoxLayout(NoticeHolder); NoticeLayout->setContentsMargins(8, 0, 8, 6);
        NoticeLayout->addWidget(notice_); NoticeHolder->hide();
        Layout->addWidget(NoticeHolder);
        connect(notice_, &fm::ui::Banner::closeRequested, this, [this, NoticeHolder] { const auto Id = noticeId_; HideNotice(); emit NoticeClosed(Id); NoticeHolder->hide(); });
        // 상위 워크스페이스 선택 카드(모달 아님).
        auto* PickHolder = new QWidget(this); PickHolder->setObjectName("explorerPickHolder");
        auto* PickHolderLayout = new QVBoxLayout(PickHolder); PickHolderLayout->setContentsMargins(8, 0, 8, 8);
        pick_ = new fm::ui::Card(tr("워크스페이스를 고르십시오"), PickHolder); pick_->setObjectName("explorerPick");
        auto* PickLayout = new QVBoxLayout(pick_); PickLayout->setContentsMargins(10, fm::ui::Card::kHeaderHeight + 2, 10, 10); PickLayout->setSpacing(6);
        pickText_ = new QLabel(pick_); pickText_->setWordWrap(true); pickText_->setObjectName("explorerPickText");
        PickLayout->addWidget(pickText_);
        auto* Rows = new QWidget(pick_); pickRows_ = new QVBoxLayout(Rows); pickRows_->setContentsMargins(0, 0, 0, 0); pickRows_->setSpacing(4);
        PickLayout->addWidget(Rows);
        pickGroup_ = new QButtonGroup(this); pickGroup_->setExclusive(true);
        pickRemember_ = new QCheckBox(tr("이 위치에서는 다시 묻지 않기"), pick_); pickRemember_->setObjectName("explorerPickRemember");
        PickLayout->addWidget(pickRemember_);
        auto* PickButtons = new QHBoxLayout; PickButtons->addStretch(1);
        auto* PickOpen = new fm::ui::Button(tr("열기"), pick_); PickOpen->setObjectName("explorerPickOpen"); PickOpen->setRole(fm::ui::Button::Primary); PickOpen->setCompact(true);
        auto* PickLater = new fm::ui::Button(tr("나중에"), pick_); PickLater->setObjectName("explorerPickLater"); PickLater->setCompact(true);
        PickButtons->addWidget(PickOpen); PickButtons->addWidget(PickLater);
        PickLayout->addLayout(PickButtons);
        PickHolderLayout->addWidget(pick_); PickHolder->hide();
        Layout->addWidget(PickHolder);
        connect(PickOpen, &QPushButton::clicked, this, [this] {
            const int Chosen = pickGroup_->checkedId(); if (Chosen < 0) return;
            const bool Remember = pickRemember_->isChecked();
            HidePick(); emit PickChosen(Chosen, Remember);
        });
        connect(PickLater, &QPushButton::clicked, this, [this] { HidePick(); emit PickDeferred(); });
        // 트리.
        tree_ = new QTreeView(this); tree_->setObjectName("explorerTree");
        tree_->setHeaderHidden(true); tree_->setUniformRowHeights(true); tree_->setAnimated(false); tree_->setIndentation(14);
        tree_->setEditTriggers(QAbstractItemView::NoEditTriggers); tree_->setFrameShape(QFrame::NoFrame);
        tree_->setSelectionMode(QAbstractItemView::SingleSelection); tree_->setContextMenuPolicy(Qt::CustomContextMenu);
        tree_->setDragEnabled(true); tree_->setDragDropMode(QAbstractItemView::DragOnly);
        model_ = new explorer::FileSystemModel(this);
        filter_ = new explorer::FilterProxy(this); filter_->setSourceModel(model_);
        outside_ = new explorer::OutsideProxy(this); outside_->setSourceModel(filter_);
        tree_->setModel(outside_);
        filter_->sort(0);
        for (int Column = 1; Column < 4; ++Column) tree_->setColumnHidden(Column, true);
        delegate_ = new ExplorerDelegate(this); tree_->setItemDelegate(delegate_);
        tree_->installEventFilter(this); tree_->viewport()->installEventFilter(this);
        Layout->addWidget(tree_, 1);
        walker_ = new explorer::FilterWalker(tree_, model_,
            [this](const QModelIndex& Source) { return outside_->mapFromSource(filter_->mapFromSource(Source)); },
            [this](const QModelIndex& View) { return filter_->mapToSource(outside_->mapToSource(View)); }, this);
        // 빈 상태: 문서도 워크스페이스도 없는 창(설계 5.2 · 시안 ‘빈 상태’).
        empty_ = new QWidget(this); empty_->setObjectName("explorerEmpty");
        auto* EmptyLayout = new QVBoxLayout(empty_); EmptyLayout->setContentsMargins(12, 10, 12, 10); EmptyLayout->setSpacing(10);
        auto* EmptyTitle = new QLabel(tr("열린 폴더가 없습니다"), empty_); EmptyTitle->setFont(Bold);
        auto* EmptyText = new QLabel(tr("문서를 열면 그 문서가 있는 폴더가 여기에 나옵니다. 폴더를 워크스페이스로 열면 탭 · 커서 · 창 배치가 폴더마다 남습니다."), empty_);
        EmptyText->setWordWrap(true);
        auto* OpenFolder = new fm::ui::Button(tr("폴더 열기…"), empty_); OpenFolder->setObjectName("explorerOpenFolder"); OpenFolder->setRole(fm::ui::Button::Primary);
        OpenFolder->setKeyHint(QStringLiteral("Ctrl+Shift+O"));
        recentTitle_ = new QLabel(tr("최근 워크스페이스"), empty_); recentTitle_->setFont(Bold);
        auto* RecentHolder = new QWidget(empty_); recentLayout_ = new QVBoxLayout(RecentHolder); recentLayout_->setContentsMargins(0, 0, 0, 0); recentLayout_->setSpacing(2);
        EmptyLayout->addWidget(EmptyTitle); EmptyLayout->addWidget(EmptyText); EmptyLayout->addWidget(OpenFolder);
        EmptyLayout->addSpacing(6); EmptyLayout->addWidget(recentTitle_); EmptyLayout->addWidget(RecentHolder); EmptyLayout->addStretch(1);
        Layout->addWidget(empty_, 1);
        connect(OpenFolder, &QPushButton::clicked, this, &ExplorerPanel::OpenFolderRequested);
        // 바닥줄: 필터 상태 · 보기 전용 안내.
        footer_ = new QLabel(this); footer_->setObjectName("explorerFooter"); footer_->setContentsMargins(8, 3, 8, 3); footer_->hide();
        Layout->addWidget(footer_);
        viewerFooter_ = new QWidget(this); viewerFooter_->setObjectName("explorerViewerFooter");
        auto* ViewerLayout = new QVBoxLayout(viewerFooter_); ViewerLayout->setContentsMargins(8, 6, 8, 6); ViewerLayout->setSpacing(4);
        auto* ViewerNote = new QLabel(tr("보기 전용 · 파일을 만들거나 바꾸지 않습니다"), viewerFooter_); ViewerNote->setWordWrap(true);
        auto* ViewerOpen = new fm::ui::Button(tr("HyperEditor에서 폴더 열기"), viewerFooter_); ViewerOpen->setObjectName("explorerOpenFolderInEditor"); ViewerOpen->setCompact(true);
        ViewerLayout->addWidget(ViewerNote); ViewerLayout->addWidget(ViewerOpen);
        Layout->addWidget(viewerFooter_);
        viewerFooter_->setVisible(readOnly_);
        connect(ViewerOpen, &QPushButton::clicked, this, [this] { const auto Folder = documentFolder_.isEmpty() ? workspaceRoot_ : documentFolder_; if (!Folder.isEmpty()) emit OpenFolderInEditorRequested(Folder); });

        filterDebounce_ = new QTimer(this); filterDebounce_->setSingleShot(true); filterDebounce_->setInterval(180);
        connect(filterBox_, &QLineEdit::textChanged, filterDebounce_, qOverload<>(&QTimer::start));
        connect(filterDebounce_, &QTimer::timeout, this, &ExplorerPanel::startFilter);
        connect(walker_, &explorer::FilterWalker::Progress, this, [this](int Checked) {
            if (!filter_->IsFiltering()) return;
            footer_->setText(tr("폴더 %1개 확인").arg(Checked));
        });
        connect(newFile_, &QToolButton::clicked, this, &ExplorerPanel::NewFile);
        connect(newFolder_, &QToolButton::clicked, this, &ExplorerPanel::NewFolder);
        connect(locate_, &QToolButton::clicked, this, [this] { RevealActive(false); });
        connect(collapse_, &QToolButton::clicked, this, &ExplorerPanel::CollapseAll);
        connect(more_, &QToolButton::clicked, this, &ExplorerPanel::showMoreMenu);
        connect(pathLink_, &QPushButton::clicked, this, [this] { if (!documentFolder_.isEmpty()) emit OpenWorkspaceRequested(documentFolder_); });
        connect(tree_, &QTreeView::customContextMenuRequested, this, &ExplorerPanel::showContextMenu);
        connect(tree_, &QTreeView::doubleClicked, this, [this](const QModelIndex& Index) {
            const auto Info = rowInfo(Index);
            if (Info.valid_ && !Info.folder_ && !Info.group_ && !Info.path_.isEmpty()) emit OpenRequested(Info.path_, false);
        });
        connect(tree_, &QTreeView::expanded, this, [this](const QModelIndex& Index) {
            const auto Path = pathOf(Index); if (Path.isEmpty()) return;
            if (expanding_) autoExpanded_.insert(PathKey(Path)); else autoExpanded_.remove(PathKey(Path));
            if (mode_ == Mode::Document) scanMarkers(Path);
            emit StateChanged();
        });
        connect(tree_, &QTreeView::collapsed, this, [this](const QModelIndex& Index) { autoExpanded_.remove(PathKey(pathOf(Index))); emit StateChanged(); });
        connect(tree_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { if (!expanding_) lastUserScroll_ = QDateTime::currentMSecsSinceEpoch(); });
        connect(model_, &QFileSystemModel::directoryLoaded, this, [this](const QString& Path) {
            if (mode_ == Mode::Document) scanMarkers(Path);
            if (!pendingReveal_.isEmpty()) retryReveal();
        });
        connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, [this] { refreshHeader(); tree_->viewport()->update(); });
        newFile_->setVisible(!readOnly_); newFolder_->setVisible(!readOnly_);
        settings_.excluded_ = explorer::FilterProxy::DefaultExcludedNames();
        applyMode();
    }

    ExplorerPanel::~ExplorerPanel() = default;

    // ---------------------------------------------------------------------------------------- 상태

    void ExplorerPanel::SetWorkspace(const QString& Root, const QString& Name)
    {
        const auto Normalized = Root.isEmpty() ? QString() : workspace::NormalizeRoot(Root);
        if (Normalized == workspaceRoot_ && Name == workspaceName_) return;
        workspaceRoot_ = Normalized; workspaceName_ = Name;
        cancelEdit(); autoExpanded_.clear(); markerScanned_.clear();
        mode_ = !workspaceRoot_.isEmpty() ? Mode::Workspace : !documentFolder_.isEmpty() ? Mode::Document : Mode::Empty;
        applyMode();
    }

    void ExplorerPanel::SetDocumentFolder(const QString& Folder)
    {
        const auto Normalized = Folder.isEmpty() ? QString() : NormalizePath(Folder);
        if (Normalized == documentFolder_) return;
        const bool DriveChanged = explorer::DriveRootOf(Normalized).compare(explorer::DriveRootOf(documentFolder_), Qt::CaseInsensitive) != 0;
        documentFolder_ = Normalized;
        if (!workspaceRoot_.isEmpty()) { refreshHeader(); return; }
        const auto Next = documentFolder_.isEmpty() ? Mode::Empty : Mode::Document;
        if (Next != mode_ || DriveChanged) { mode_ = Next; applyMode(); return; }
        filter_->SetScope(documentFolder_);
        refreshHeader(); refreshOutside();
    }

    void ExplorerPanel::ApplySettings(const Settings& Value)
    {
        const bool DrivesChanged = Value.showAllDrives_ != settings_.showAllDrives_;
        settings_ = Value;
        model_->SetShowHidden(settings_.showHidden_);
        filter_->SetShowHidden(settings_.showHidden_);
        filter_->SetExcludedNames(settings_.excluded_);
        QStringList Filters;
        if (!settings_.showAllFiles_) {
            for (const auto& Suffix : settings_.openableSuffixes_) if (!Suffix.isEmpty()) Filters.append(QStringLiteral("*.") + Suffix.toLower());
            for (const auto& Each : explorer::FileSystemModel::TextFamilyNameFilters()) if (!Filters.contains(Each)) Filters.append(Each);
        }
        model_->SetExtensionFilter(Filters);
        if (settings_.font_ != QFont()) tree_->setFont(settings_.font_);
        if (DrivesChanged && mode_ == Mode::Document) applyRoot();
    }

    void ExplorerPanel::SetSphinxRoots(const QStringList& Roots)
    {
        QSet<QString> Next; for (const auto& Each : Roots) if (!Each.isEmpty()) Next.insert(PathKey(Each));
        if (Next == sphinxRoots_) return;
        sphinxRoots_ = Next; refreshHeader(); tree_->viewport()->update();
    }

    void ExplorerPanel::SetOpenDocuments(const QList<OpenDocument>& Documents)
    {
        QHash<QString, bool> Next;
        for (const auto& Each : Documents) if (!Each.path_.isEmpty()) Next.insert(PathKey(Each.path_), Each.modified_);
        // 경로 목록이 같으면 묶음은 그대로 두고 점만 다시 그린다.
        const bool SameSet = Next.keys().size() == openDocuments_.keys().size() && std::all_of(Next.keyBegin(), Next.keyEnd(), [this](const QString& Key) { return openDocuments_.contains(Key); });
        QStringList Paths; for (const auto& Each : Documents) if (!Each.path_.isEmpty()) Paths.append(Each.path_);
        openDocuments_ = Next;
        if (!SameSet) { tree_->setProperty("hc.openPaths", Paths); refreshOutside(); }
        tree_->viewport()->update();
    }

    void ExplorerPanel::SetActiveDocument(const QString& Path)
    {
        const auto Normalized = Path.isEmpty() ? QString() : NormalizePath(Path);
        if (Normalized == activePath_) return;
        activePath_ = Normalized;
        if (activePath_.isEmpty() || !settings_.autoReveal_ || mode_ == Mode::Empty) return;
        // 문서 모드: 자동으로 펼친 폴더 가운데 새 경로에 없는 것은 다시 접는다(설계 5.6).
        if (mode_ == Mode::Document) collapseAutoExpanded(activePath_);
        reveal(activePath_, true, true);
    }

    void ExplorerPanel::RevealActive(bool FocusTree)
    {
        if (FocusTree) this->FocusTree();
        if (!activePath_.isEmpty()) { lastUserScroll_ = 0; reveal(activePath_, true, true); }
    }

    void ExplorerPanel::SetRecentWorkspaces(const QList<RecentWorkspace>& Items)
    {
        recent_ = Items; rebuildRecent();
    }

    void ExplorerPanel::ShowNotice(const QString& Id, int Tone, const QString& Title, const QString& Detail, const QStringList& Actions, bool Closable)
    {
        noticeId_ = Id;
        notice_->setTone(fm::ui::Banner::Tone(Tone));
        notice_->setGlyph(Tone == fm::ui::Banner::Warn || Tone == fm::ui::Banner::Danger ? fm::ui::glyph::Warning : fm::ui::glyph::Info);
        notice_->setText(QStringLiteral("<b>%1</b>").arg(Title.toHtmlEscaped()));
        notice_->setDetail(Detail);
        notice_->clearActions();
        for (int At = 0; At < Actions.size(); ++At) {
            auto* Button = notice_->addAction(Actions.at(At));
            Button->setObjectName(QStringLiteral("explorerNoticeAction%1").arg(At));
            connect(Button, &QPushButton::clicked, this, [this, Id, At] { emit NoticeAction(Id, At); });
        }
        notice_->setClosable(Closable);
        notice_->show(); notice_->parentWidget()->show();
    }

    void ExplorerPanel::HideNotice(const QString& Id)
    {
        if (!Id.isEmpty() && Id != noticeId_) return;
        noticeId_.clear(); notice_->hide(); notice_->parentWidget()->hide();
    }

    void ExplorerPanel::ShowPick(const QString& FileName, const QList<PickChoice>& Choices)
    {
        for (auto* Button : pickGroup_->buttons()) { pickGroup_->removeButton(Button); Button->deleteLater(); }
        int Workspaces = 0; for (const auto& Each : Choices) if (!Each.root_.isEmpty() && !Each.newWorkspace_) ++Workspaces;
        pickText_->setText(tr("%1 위의 폴더에서 워크스페이스 %2개를 찾았습니다. 고르는 동안 문서는 열려 있습니다.").arg(FileName).arg(Workspaces));
        for (int At = 0; At < Choices.size(); ++At) {
            const auto& Each = Choices.at(At);
            auto* Card = new fm::ui::ChoiceCard(Each.name_, pick_);
            Card->setObjectName(QStringLiteral("explorerPickChoice%1").arg(At));
            Card->setFileKind(fm::ui::ChoiceCard::NoKind);
            Card->setDescription(Each.description_);
            Card->setTag(Each.tag_);
            Card->setChecked(Each.checked_);
            pickGroup_->addButton(Card, At);
            pickRows_->addWidget(Card);
        }
        if (pickGroup_->checkedId() < 0 && !pickGroup_->buttons().isEmpty()) pickGroup_->buttons().first()->setChecked(true);
        pickRemember_->setChecked(false);
        pick_->parentWidget()->show();
    }

    void ExplorerPanel::HidePick() { pick_->parentWidget()->hide(); }
    bool ExplorerPanel::PickVisible() const { return pick_->parentWidget()->isVisible(); }

    QStringList ExplorerPanel::ExpandedPaths() const
    {
        QStringList Result;
        const std::function<void(const QModelIndex&)> Walk = [&](const QModelIndex& Parent) {
            const int Rows = outside_->rowCount(Parent);
            for (int Row = 0; Row < Rows; ++Row) {
                const auto Child = outside_->index(Row, 0, Parent);
                if (!tree_->isExpanded(Child)) continue;
                if (const auto Path = pathOf(Child); !Path.isEmpty()) Result.append(Path);
                Walk(Child);
            }
        };
        Walk(tree_->rootIndex());
        return Result;
    }

    QString ExplorerPanel::SelectedPath() const { return currentPath(); }
    QString ExplorerPanel::TopPath() const { return pathOf(tree_->indexAt(QPoint(4, 4))); }

    void ExplorerPanel::RestoreExpansion(const QStringList& Expanded, const QString& Selected, const QString& Top)
    {
        const QScopedValueRollback Guard(expanding_, false);
        for (const auto& Path : Expanded) {
            expandChain(Path);
            if (const auto Index = viewIndex(Path); Index.isValid()) tree_->expand(Index);
        }
        if (!Selected.isEmpty()) reveal(Selected, true, Top.isEmpty());
        if (!Top.isEmpty()) if (const auto Index = viewIndex(Top); Index.isValid()) tree_->scrollTo(Index, QAbstractItemView::PositionAtTop);
    }

    bool ExplorerPanel::OwnsShortcutFocus(QWidget* Focus) const
    {
        // 트리 자체 · 그 표시 영역만. 필터 칸과 이름 입력 칸은 Del · F2 를 자기 것으로 쓴다(MRE ExplorerShortcuts.hpp).
        return Focus && (Focus == tree_ || Focus == tree_->viewport());
    }

    // ---------------------------------------------------------------------------------------- 명령

    void ExplorerPanel::OpenCurrent(bool Beside)
    {
        const auto Info = rowInfo(tree_->currentIndex());
        if (!Info.valid_ || Info.group_) return;
        if (Info.folder_) { tree_->setExpanded(tree_->currentIndex(), !tree_->isExpanded(tree_->currentIndex())); return; }
        emit OpenRequested(Info.path_, Beside);
    }

    void ExplorerPanel::RenameCurrent()
    {
        const auto Info = rowInfo(tree_->currentIndex());
        if (readOnly_ || !Info.valid_ || Info.group_ || Info.outside_ || Info.path_.isEmpty()) return;
        // 드라이브 · 워크스페이스 루트 자체는 바꾸지 않는다.
        if (QFileInfo(Info.path_).isRoot() || workspace::SamePath(Info.path_, workspaceRoot_)) return;
        startEdit(Info.path_, false, Info.folder_);
    }

    void ExplorerPanel::DeleteCurrent(bool Permanent)
    {
        const auto Info = rowInfo(tree_->currentIndex());
        if (readOnly_ || !Info.valid_ || Info.group_ || Info.outside_ || Info.path_.isEmpty()) return;
        if (QFileInfo(Info.path_).isRoot() || workspace::SamePath(Info.path_, workspaceRoot_)) return;
        const auto Name = NameOf(Info.path_);
        // 열린 문서 수: 그 항목이거나 그 아래.
        int Open = 0; QString OpenName;
        for (auto At = openDocuments_.cbegin(); At != openDocuments_.cend(); ++At)
            if (workspace::IsInside(At.key(), Info.path_, false)) { ++Open; if (OpenName.isEmpty()) OpenName = NameOf(At.key()); }
        if (!Permanent && !explorer::CanRecycle(Info.path_)) {
            ShowNotice(QStringLiteral("deleteNoRecycle"), fm::ui::Banner::Warn, tr("휴지통으로 옮길 수 없습니다"),
                tr("이 위치에는 휴지통이 없습니다. 영구 삭제하려면 Shift+Del 을 누르십시오."));
            return;
        }
        QString Text;
        if (Permanent) Text = tr("‘%1’ 을(를) 지웁니다. 휴지통을 거치지 않으며 되돌릴 수 없습니다.").arg(Name);
        else if (Info.folder_) Text = tr("‘%1’ 폴더와 그 안의 항목을 휴지통으로 옮깁니다.").arg(Name);
        else Text = tr("‘%1’ 을(를) 휴지통으로 옮깁니다.").arg(Name);
        Text += QStringLiteral("\n\n") + Native(Info.path_);
        if (Open > 0) Text += QStringLiteral("\n\n") + tr("열린 문서 %1개(%2)는 닫히지 않고 ‘사라진 파일’로 표시됩니다.").arg(Open).arg(OpenName);
        if (Permanent || settings_.confirmRecycle_) {
            hc::ui::MessageDialog Box(Permanent ? QMessageBox::Warning : QMessageBox::Question, Permanent ? tr("영구 삭제") : tr("휴지통으로 옮기기"), Text,
                QMessageBox::Yes | QMessageBox::Cancel, this);
            Box.button(QMessageBox::Yes)->setText(Permanent ? tr("영구 삭제") : tr("휴지통으로 옮기기"));
            Box.button(QMessageBox::Cancel)->setText(tr("취소"));
            // 영구 삭제의 기본 단추는 취소다.
            Box.setDefaultButton(Permanent ? QMessageBox::Cancel : QMessageBox::Yes);
            if (Box.exec() != QMessageBox::Yes) return;
        }
        const auto Result = Permanent ? explorer::PermanentlyRemove(Info.path_) : explorer::Recycle(Info.path_);
        if (!Result) ShowNotice(QStringLiteral("deleteFailed"), fm::ui::Banner::Warn, Permanent ? tr("지우지 못했습니다") : tr("휴지통으로 옮기지 못했습니다"), Result.error_);
        else if (Info.folder_) workspace::MarkerCache::Instance().Invalidate(Info.path_);
    }

    void ExplorerPanel::CopyCurrentPath(bool Relative)
    {
        const auto Path = rowInfo(tree_->currentIndex()).path_; if (Path.isEmpty()) return;
        QString Text = Native(Path);
        if (Relative) {
            const auto Base = !workspaceRoot_.isEmpty() ? workspaceRoot_ : documentFolder_;
            const auto Rel = workspace::RelativeTo(Base, Path);
            if (!Rel.isEmpty()) Text = Native(Rel);
        }
        QGuiApplication::clipboard()->setText(Text);
    }

    void ExplorerPanel::NewFile() { createEntry(false); }
    void ExplorerPanel::NewFolder() { createEntry(true); }

    void ExplorerPanel::CollapseAll()
    {
        tree_->collapseAll(); autoExpanded_.clear();
        // 문서 모드는 드라이브 행만 남긴다(설계 5.6).
        emit StateChanged();
    }

    void ExplorerPanel::Refresh()
    {
        // 감시가 놓친 바뀜(네트워크 공유)을 다시 읽는다: 원본 모델의 루트를 다시 세운다.
        workspace::MarkerCache::Instance().Clear(); markerScanned_.clear();
        const auto Expanded = ExpandedPaths(); const auto Selected = SelectedPath();
        const auto Root = model_->rootPath();
        model_->setRootPath(QDir::tempPath()); model_->setRootPath(Root);
        applyRoot();
        RestoreExpansion(Expanded, Selected, {});
    }

    void ExplorerPanel::FocusTree()
    {
        if (!tree_->isVisible()) return;
        tree_->setFocus(Qt::ShortcutFocusReason);
        if (!tree_->currentIndex().isValid()) if (const auto First = outside_->index(0, 0, tree_->rootIndex()); First.isValid()) tree_->setCurrentIndex(First);
    }

    void ExplorerPanel::FinishRename(const QString& OldPath, const QString& NewPath, const QString& Error)
    {
        if (!Error.isEmpty()) {
            ShowNotice(QStringLiteral("renameFailed"), fm::ui::Banner::Warn, tr("이름을 바꿀 수 없습니다 — %1").arg(NameOf(OldPath)), Error);
            return;
        }
        // 펼침 상태를 새 경로로 옮긴다.
        const auto OldKey = PathKey(OldPath);
        QSet<QString> Moved;
        for (const auto& Key : std::as_const(autoExpanded_)) Moved.insert(Key.startsWith(OldKey) ? PathKey(NewPath) + Key.mid(OldKey.size()) : Key);
        autoExpanded_ = Moved;
        reveal(NewPath, true, true);
    }

    // ---------------------------------------------------------------------------------------- 안쪽

    QString ExplorerPanel::pathOf(const QModelIndex& Index) const
    {
        if (!Index.isValid()) return {};
        if (outside_->IsOutsideFile(Index)) return outside_->FilePath(Index);
        if (outside_->IsGroup(Index)) return {};
        const auto Source = filter_->mapToSource(outside_->mapToSource(Index));
        return Source.isValid() ? NormalizePath(filter_->SourcePath(Source)) : QString();
    }

    QModelIndex ExplorerPanel::viewIndex(const QString& Path) const
    {
        if (Path.isEmpty()) return {};
        if (const auto Outside = outside_->IndexForFile(Path); Outside.isValid()) return Outside;
        const auto Source = model_->index(QDir::fromNativeSeparators(Path));
        return Source.isValid() ? outside_->mapFromSource(filter_->mapFromSource(Source)) : QModelIndex();
    }

    ExplorerPanel::RowInfo ExplorerPanel::rowInfo(const QModelIndex& Index) const
    {
        RowInfo Info;
        if (!Index.isValid()) return Info;
        Info.valid_ = true;
        if (outside_->IsGroup(Index)) { Info.group_ = true; return Info; }
        if (outside_->IsOutsideFile(Index)) {
            Info.outside_ = true; Info.path_ = outside_->FilePath(Index);
            Info.dim_ = Native(QFileInfo(Info.path_).absolutePath());
            Info.modified_ = openDocuments_.value(PathKey(Info.path_));
            Info.match_ = filter_->IsFiltering() && filter_->Matcher().Matches(QFileInfo(Info.path_).fileName());
            return Info;
        }
        const auto Source = filter_->mapToSource(outside_->mapToSource(Index));
        if (!Source.isValid()) { Info.valid_ = false; return Info; }
        Info.path_ = NormalizePath(filter_->SourcePath(Source));
        Info.folder_ = model_->isDir(Source);
        const auto Key = PathKey(Info.path_);
        Info.modified_ = !Info.folder_ && openDocuments_.value(Key);
        Info.match_ = filter_->IsMatch(Source);
        if (Info.folder_) {
            workspace::MarkerKind Marker{};
            // 워크스페이스 표시는 문서 모드에서만(워크스페이스 안의 다른 워크스페이스는 이번 범위 밖).
            if (mode_ == Mode::Document && workspace::MarkerCache::Instance().Peek(Info.path_, &Marker) && Marker != workspace::MarkerKind::None) {
                Info.tag_ = Marker == workspace::MarkerKind::HyperEditor ? tr("워크스페이스") : tr("MRE 세션"); Info.tagWorkspace_ = true;
            } else if (sphinxRoots_.contains(Key)) Info.tag_ = QStringLiteral("Sphinx");
        }
        return Info;
    }

    QString ExplorerPanel::currentPath() const { return rowInfo(tree_->currentIndex()).path_; }

    QString ExplorerPanel::targetFolder() const
    {
        // 고른 폴더, 아니면 고른 파일의 폴더, 아니면 루트(문서 모드는 문서 폴더) — MRE MainWindow.cpp:4966-4979.
        const auto Info = rowInfo(tree_->currentIndex());
        if (Info.valid_ && !Info.group_ && !Info.outside_ && !Info.path_.isEmpty())
            return Info.folder_ ? Info.path_ : NormalizePath(QFileInfo(Info.path_).absolutePath());
        return !workspaceRoot_.isEmpty() ? workspaceRoot_ : documentFolder_;
    }

    bool ExplorerPanel::insideSphinx(const QString& Path) const
    {
        for (const auto& Ancestor : workspace::AncestorFolders(Path)) if (sphinxRoots_.contains(PathKey(Ancestor))) return true;
        return false;
    }

    void ExplorerPanel::applyMode()
    {
        const bool Empty = mode_ == Mode::Empty;
        tree_->setVisible(!Empty); filterRow_->setVisible(!Empty); empty_->setVisible(Empty && !readOnly_);
        if (Empty) { filterBox_->clear(); walker_->Stop(); footer_->hide(); }
        applyRoot();
        refreshHeader();
    }

    void ExplorerPanel::applyRoot()
    {
        const QScopedValueRollback Guard(expanding_, true);
        if (mode_ == Mode::Workspace) {
            const auto Source = model_->setRootPath(workspaceRoot_);
            filter_->SetAllowedTopLevelRoots({});
            filter_->SetRootSourceIndex(Source);
            filter_->SetScope(workspaceRoot_);
            outside_->SetGroupLabel(tr("워크스페이스 밖")); outside_->SetGroupToolTip(tr("워크스페이스 밖에서 연 문서"));
            outside_->SetRoot(workspaceRoot_, filter_->mapFromSource(Source));
            tree_->setRootIndex(outside_->mapFromSource(filter_->mapFromSource(Source)));
        } else {
            // 문서 모드: 원본 루트는 내 PC, 맨 위 단은 문서 폴더의 드라이브(모든 드라이브 표시면 전부) — 설계 5.3.
            model_->setRootPath(QString());
            filter_->SetRootSourceIndex({});
            filter_->SetScope(documentFolder_);
            const auto Drive = explorer::DriveRootOf(documentFolder_);
            filter_->SetAllowedTopLevelRoots(settings_.showAllDrives_ || Drive.isEmpty() ? QStringList{} : QStringList{Drive});
            outside_->SetGroupLabel(tr("다른 드라이브")); outside_->SetGroupToolTip(tr("다른 드라이브에서 연 문서"));
            outside_->SetRoot({}, {});
            tree_->setRootIndex({});
            if (mode_ == Mode::Document) {
                // UNC 문서는 공유 루트를 맨 위 행으로 둔다(내 PC 아래에 없으므로 그 자리를 모델에 만든다).
                if (explorer::IsUncPath(documentFolder_)) model_->index(Drive);
                expandChain(documentFolder_);
            }
        }
        refreshOutside();
        if (filter_->IsFiltering()) startFilter();
    }

    void ExplorerPanel::refreshHeader()
    {
        const auto& Tc = fm::style::themeColorsFor(this);
        headIcon_->setPixmap(fm::style::glyphIcon(fm::style::Glyph::Folder, Tc[fm::style::Token::Folder], 16).pixmap(16, 16));
        QPalette Dim = path_->palette(); Dim.setColor(QPalette::WindowText, Tc[fm::style::Token::Fg3]); path_->setPalette(Dim); footer_->setPalette(Dim);
        const bool Empty = mode_ == Mode::Empty;
        newFile_->setVisible(!readOnly_ && !Empty); newFolder_->setVisible(!readOnly_ && !Empty);
        locate_->setVisible(!Empty); collapse_->setVisible(!Empty);
        if (mode_ == Mode::Workspace) {
            title_->setText(workspaceName_.isEmpty() ? NameOf(workspaceRoot_) : workspaceName_);
            int Projects = 0; for (const auto& Key : std::as_const(sphinxRoots_)) if (workspace::IsInside(Key, workspaceRoot_, false)) ++Projects;
            chip_->setText(QStringLiteral("Sphinx %1").arg(Projects)); chip_->setVisible(Projects > 0);
            path_->setText(Native(workspaceRoot_)); path_->setToolTip(Native(workspaceRoot_));
            pathLink_->hide();
        } else if (mode_ == Mode::Document) {
            title_->setText(NameOf(documentFolder_)); chip_->hide();
            path_->setText(tr("문서 모드 · %1").arg(Native(documentFolder_))); path_->setToolTip(Native(documentFolder_));
            pathLink_->setVisible(!readOnly_);
        } else {
            title_->setText(tr("탐색기")); chip_->hide();
            path_->setText(tr("워크스페이스 없음")); path_->setToolTip({});
            pathLink_->hide();
        }
    }

    void ExplorerPanel::refreshOutside()
    {
        const auto Paths = tree_->property("hc.openPaths").toStringList();
        if (mode_ == Mode::Workspace) { outside_->SetFiles(Paths); return; }
        if (mode_ != Mode::Document || settings_.showAllDrives_) { outside_->SetFiles({}); return; }
        // 문서 모드: 트리에 없는 드라이브의 열린 문서만 묶는다.
        const auto Drive = explorer::DriveRootOf(documentFolder_);
        QStringList Away;
        for (const auto& Each : Paths) if (explorer::DriveRootOf(Each).compare(Drive, Qt::CaseInsensitive) != 0) Away.append(Each);
        outside_->SetFiles(Away);
    }

    void ExplorerPanel::expandChain(const QString& Path)
    {
        if (Path.isEmpty()) return;
        const QScopedValueRollback Guard(expanding_, true);
        auto Chain = workspace::AncestorFolders(Path);
        std::reverse(Chain.begin(), Chain.end());
        for (const auto& Folder : Chain) {
            if (mode_ == Mode::Workspace && !workspace::IsInside(Folder, workspaceRoot_, false)) continue;
            if (mode_ == Mode::Workspace && workspace::SamePath(Folder, workspaceRoot_)) continue;
            const auto Index = viewIndex(Folder);
            if (!Index.isValid()) break;
            if (!tree_->isExpanded(Index)) { tree_->expand(Index); autoExpanded_.insert(PathKey(Folder)); }
        }
    }

    void ExplorerPanel::collapseAutoExpanded(const QString& Keep)
    {
        const QScopedValueRollback Guard(expanding_, true);
        QStringList Collapse;
        for (const auto& Key : std::as_const(autoExpanded_)) if (!workspace::IsInside(Keep, Key, false)) Collapse.append(Key);
        // 깊은 것부터 접는다.
        std::sort(Collapse.begin(), Collapse.end(), [](const QString& A, const QString& B) { return A.size() > B.size(); });
        for (const auto& Key : Collapse) {
            autoExpanded_.remove(Key);
            if (const auto Index = viewIndex(Key); Index.isValid()) tree_->collapse(Index);
        }
    }

    void ExplorerPanel::reveal(const QString& Path, bool Select, bool Scroll)
    {
        if (Path.isEmpty() || mode_ == Mode::Empty) return;
        if (mode_ == Mode::Workspace && !workspace::IsInside(Path, workspaceRoot_, false)) {
            // 바깥 문서: 묶음을 펼치고 고른다.
            const auto Index = outside_->IndexForFile(Path);
            if (!Index.isValid()) return;
            tree_->expand(outside_->GroupIndex());
            if (Select) tree_->selectionModel()->setCurrentIndex(Index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            if (Scroll && QDateTime::currentMSecsSinceEpoch() - lastUserScroll_ > 2000) { const QScopedValueRollback Guard(expanding_, true); tree_->scrollTo(Index); }
            return;
        }
        expandChain(QFileInfo(Path).isDir() ? Path : NormalizePath(QFileInfo(Path).absolutePath()));
        const auto Index = viewIndex(Path);
        if (!Index.isValid()) {
            // 아직 읽히지 않았다: 폴더가 읽히면 다시 해 본다(2초까지).
            pendingReveal_ = Path; pendingSelect_ = Select; revealDeadline_ = QDateTime::currentMSecsSinceEpoch() + 2000;
            return;
        }
        pendingReveal_.clear();
        if (Select) tree_->selectionModel()->setCurrentIndex(Index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        // 사용자가 방금 스크롤했으면 화면을 옮기지 않는다(설계 5.6).
        if (Scroll && QDateTime::currentMSecsSinceEpoch() - lastUserScroll_ > 2000) { const QScopedValueRollback Guard(expanding_, true); tree_->scrollTo(Index); }
    }

    void ExplorerPanel::retryReveal()
    {
        if (QDateTime::currentMSecsSinceEpoch() > revealDeadline_) { pendingReveal_.clear(); return; }
        const auto Path = pendingReveal_;
        reveal(Path, pendingSelect_, true);
    }

    void ExplorerPanel::scanMarkers(const QString& Folder)
    {
        // 펼친 폴더의 자식 폴더에 .multiroot 표시가 있는지 일꾼 스레드에서 본다(로컬 고정 디스크만).
        if (Folder.isEmpty() || !LocalFixed(Folder) || markerScanned_.contains(PathKey(Folder))) return;
        const auto Source = model_->index(Folder);
        if (!Source.isValid() || model_->canFetchMore(Source)) return;
        QStringList Children;
        for (int Row = 0; Row < model_->rowCount(Source); ++Row) {
            const auto Child = model_->index(Row, 0, Source);
            if (model_->isDir(Child)) Children.append(model_->filePath(Child));
        }
        markerScanned_.insert(PathKey(Folder));
        if (Children.isEmpty()) return;
        QPointer<ExplorerPanel> Guard(this);
        (void)QtConcurrent::run([Children, Guard] {
            bool Any = false;
            for (const auto& Each : Children) Any = workspace::MarkerCache::Instance().Get(Each) != workspace::MarkerKind::None || Any;
            if (Any) QMetaObject::invokeMethod(qApp, [Guard] { if (Guard) Guard->tree_->viewport()->update(); }, Qt::QueuedConnection);
        });
    }

    void ExplorerPanel::rebuildRecent()
    {
        while (auto* Item = recentLayout_->takeAt(0)) { if (auto* Widget = Item->widget()) Widget->deleteLater(); delete Item; }
        int Shown = 0;
        for (const auto& Each : std::as_const(recent_)) {
            if (Shown >= 3) break;
            auto* Card = new fm::ui::ActionCard(empty_);
            Card->setObjectName(QStringLiteral("explorerRecent%1").arg(Shown));
            Card->setText(Each.name_.isEmpty() ? NameOf(Each.root_) : Each.name_);
            Card->setDetail(Native(Each.root_));
            Card->setIcon(fm::ui::IconSpec::folder());
            connect(Card, &QAbstractButton::clicked, this, [this, Root = Each.root_] { emit OpenWorkspaceRequested(Root); });
            recentLayout_->addWidget(Card); ++Shown;
        }
        recentTitle_->setVisible(Shown > 0);
    }

    void ExplorerPanel::startFilter()
    {
        const auto Text = filterBox_->text();
        const bool Was = filter_->IsFiltering();
        // 필터를 걸기 직전의 펼침을 기억했다가 지우면 되돌린다(MRE MainWindow.cpp:4989-5022).
        if (!Was && !Text.trimmed().isEmpty()) savedExpansion_ = ExpandedPaths();
        walker_->Stop();
        filter_->SetFilterText(Text);
        if (filter_->IsFiltering()) {
            const auto Scope = mode_ == Mode::Workspace ? workspaceRoot_ : documentFolder_;
            walker_->Start(model_->index(Scope));
            footer_->setText(tr("거르는 중…")); footer_->show();
            return;
        }
        footer_->hide();
        if (Was) {
            tree_->setUpdatesEnabled(false);
            tree_->collapseAll();
            RestoreExpansion(savedExpansion_, activePath_, {});
            tree_->setUpdatesEnabled(true);
            savedExpansion_.clear();
        }
    }

    // ---------------------------------------------------------------------------------------- 만들기 · 이름 바꾸기

    void ExplorerPanel::createEntry(bool Folder)
    {
        if (readOnly_ || mode_ == Mode::Empty) return;
        const auto Parent = targetFolder();
        if (Parent.isEmpty()) return;
        // 먼저 기본 이름으로 만들고 그 자리에서 이름을 받는다. Esc 면 방금 만든 빈 항목을 지운다(시안 ‘새 파일’).
        const auto Name = Folder ? explorer::DefaultNewFolderName(Parent) : explorer::DefaultNewFileName(Parent, insideSphinx(Parent));
        QString NewPath;
        const auto Result = Folder ? explorer::CreateFolder(Parent, Name, &NewPath) : explorer::CreateEmptyFile(Parent, Name, &NewPath);
        if (!Result) { ShowNotice(QStringLiteral("createFailed"), fm::ui::Banner::Warn, Folder ? tr("폴더를 만들지 못했습니다") : tr("파일을 만들지 못했습니다"), Result.error_); return; }
        expandChain(Parent);
        if (const auto ParentIndex = viewIndex(Parent); ParentIndex.isValid()) tree_->expand(ParentIndex);
        // 모델이 새 항목을 읽을 때까지 기다렸다가 입력 칸을 연다.
        auto* Wait = new QTimer(this); Wait->setInterval(40);
        const qint64 Deadline = QDateTime::currentMSecsSinceEpoch() + 3000;
        connect(Wait, &QTimer::timeout, this, [this, Wait, NewPath, Folder, Deadline] {
            const auto Index = viewIndex(NewPath);
            if (!Index.isValid() && QDateTime::currentMSecsSinceEpoch() < Deadline) return;
            Wait->stop(); Wait->deleteLater();
            if (!Index.isValid()) { emit Created(NewPath, Folder); return; }
            tree_->setCurrentIndex(Index); tree_->scrollTo(Index);
            startEdit(NewPath, true, Folder);
        });
        Wait->start();
    }

    void ExplorerPanel::startEdit(const QString& Path, bool IsNew, bool Folder)
    {
        cancelEdit(false);
        const auto Index = viewIndex(Path); if (!Index.isValid()) return;
        editPath_ = Path; editCreated_ = IsNew; editFolder_ = Folder;
        editor_ = new QLineEdit(tree_->viewport()); editor_->setObjectName("explorerNameEditor");
        QStyleOptionViewItem Option; Option.initFrom(tree_); Option.rect = tree_->visualRect(Index); Option.features |= QStyleOptionViewItem::HasDecoration;
        Option.decorationSize = tree_->iconSize().isValid() ? tree_->iconSize() : QSize(16, 16);
        const auto TextRect = tree_->style()->subElementRect(QStyle::SE_ItemViewItemText, &Option, tree_);
        editor_->setGeometry(QRect(TextRect.left() - 2, Option.rect.top(), qMax(80, Option.rect.right() - TextRect.left()), Option.rect.height()));
        const auto Name = QFileInfo(Path).fileName();
        editor_->setText(Name);
        // 확장자를 뺀 부분을 고른 채로 시작한다(점으로 시작하는 이름 · 폴더는 전체).
        const auto Dot = Folder ? -1 : Name.lastIndexOf(QLatin1Char('.'));
        editor_->setSelection(0, Dot > 0 ? Dot : Name.size());
        editor_->installEventFilter(this);
        connect(editor_, &QLineEdit::textChanged, this, [this](const QString& Text) {
            const auto Problem = explorer::EntryNameProblem(QFileInfo(editPath_).absolutePath(), Text.trimmed(), editPath_);
            showEditError(Problem);
        });
        editor_->show(); editor_->setFocus(Qt::OtherFocusReason);
        if (IsNew) { editTip_ = new fm::ui::ToolTip(editor_); editTip_->setPlacement(fm::ui::ToolTip::Below); editTip_->setDuration(4000);
            editTip_->setText(tr("Enter 만들기 · Esc 취소")); editTip_->showAt(editor_->mapToGlobal(QPoint(0, editor_->height()))); }
    }

    void ExplorerPanel::showEditError(const QString& Error)
    {
        if (!editor_) return;
        editor_->setProperty("hc.invalid", !Error.isEmpty());
        const auto& Tc = fm::style::themeColorsFor(this);
        editor_->setStyleSheet(Error.isEmpty() ? QString() : QStringLiteral("QLineEdit { border: 1px solid %1; }").arg(Tc[fm::style::Token::Danger].name()));
        if (!editTip_) { editTip_ = new fm::ui::ToolTip(editor_); editTip_->setPlacement(fm::ui::ToolTip::Below); }
        if (Error.isEmpty()) { editTip_->hideTip(); return; }
        editTip_->setGlyph(fm::ui::glyph::Warning); editTip_->setDuration(-1); editTip_->setText(Error);
        editTip_->showAt(editor_->mapToGlobal(QPoint(0, editor_->height())));
    }

    void ExplorerPanel::commitEdit()
    {
        if (!editor_) return;
        const auto Name = editor_->text().trimmed();
        const auto Path = editPath_; const bool IsNew = editCreated_, Folder = editFolder_;
        if (Name == QFileInfo(Path).fileName() || Name.isEmpty()) {
            cancelEdit(false);
            if (IsNew) emit Created(Path, Folder);
            return;
        }
        if (const auto Problem = explorer::EntryNameProblem(QFileInfo(Path).absolutePath(), Name, Path); !Problem.isEmpty()) { showEditError(Problem); return; }
        cancelEdit(false);
        if (IsNew) {
            // 방금 만든 빈 항목은 열린 탭이 없으니 여기서 바꾼다.
            QString Renamed;
            const auto Result = explorer::RenameEntry(Path, Name, &Renamed);
            if (!Result) { ShowNotice(QStringLiteral("renameFailed"), fm::ui::Banner::Warn, tr("이름을 바꾸지 못했습니다"), Result.error_); return; }
            reveal(Renamed, true, true);
            emit Created(Renamed, Folder);
            return;
        }
        emit RenameRequested(Path, Name);
    }

    void ExplorerPanel::cancelEdit() { cancelEdit(true); }

    void ExplorerPanel::cancelEdit(bool Discard)
    {
        if (editTip_) { editTip_->hideTip(); editTip_->deleteLater(); editTip_ = nullptr; }
        if (!editor_) return;
        auto* Editor = editor_; editor_ = nullptr;
        Editor->removeEventFilter(this); Editor->hide(); Editor->deleteLater();
        // Esc: 방금 만든 항목이 아직 비어 있으면 지운다(사용자가 그새 무엇을 써 넣었으면 남긴다).
        if (Discard && editCreated_) {
            const QFileInfo Info(editPath_);
            if (editFolder_ ? QDir(editPath_).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System) : Info.size() == 0)
                explorer::PermanentlyRemove(editPath_);
        }
        editPath_.clear(); editCreated_ = editFolder_ = false;
        tree_->setFocus(Qt::OtherFocusReason);
    }

    bool ExplorerPanel::eventFilter(QObject* Watched, QEvent* Event)
    {
        if (editor_ && Watched == editor_) {
            if (Event->type() == QEvent::KeyPress) {
                const auto* Key = static_cast<QKeyEvent*>(Event);
                if (Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter) { commitEdit(); return true; }
                if (Key->key() == Qt::Key_Escape) { cancelEdit(); return true; }
            }
            if (Event->type() == QEvent::FocusOut && !QApplication::activePopupWidget()) {
                // 다른 곳을 누르면 맞는 이름은 받아들이고 틀린 이름은 취소한다.
                QTimer::singleShot(0, this, [this] { if (!editor_) return; if (editor_->property("hc.invalid").toBool()) cancelEdit(); else commitEdit(); });
            }
            return false;
        }
        if (Watched == tree_ && Event->type() == QEvent::KeyPress) {
            const auto* Key = static_cast<QKeyEvent*>(Event);
            // Enter 는 열기, Ctrl+Enter 는 옆에 열기(MRE 는 두 번 클릭만 열었다). Enter 는 포커스 기본키라 단축키로 등록하지 않는다.
            const auto Mods = Key->modifiers() & ~Qt::KeypadModifier;
            if ((Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter) && (Mods == Qt::NoModifier || Mods == Qt::ControlModifier)) { OpenCurrent(Mods == Qt::ControlModifier); return true; }
        }
        return QWidget::eventFilter(Watched, Event);
    }

    void ExplorerPanel::changeEvent(QEvent* Event)
    {
        QWidget::changeEvent(Event);
        if (Event->type() == QEvent::PaletteChange || Event->type() == QEvent::StyleChange) refreshHeader();
    }

    // ---------------------------------------------------------------------------------------- 메뉴

    void ExplorerPanel::showContextMenu(const QPoint& Position)
    {
        const auto Index = tree_->indexAt(Position);
        if (Index.isValid()) tree_->setCurrentIndex(Index);
        const auto Info = rowInfo(Index);
        if (Info.group_) return;
        fm::ui::Menu Menu(this); Menu.setObjectName("explorerContextMenu");
        const auto Add = [&Menu](const QString& Label, const QString& Key, const char* Name, auto Slot, bool Enabled = true) {
            auto* Action = Menu.addAction(WithKey(Label, Key)); Action->setObjectName(QString::fromLatin1(Name)); Action->setEnabled(Enabled);
            QObject::connect(Action, &QAction::triggered, Slot); return Action;
        };
        const auto Path = Info.path_;
        const bool Real = Info.valid_ && !Path.isEmpty();
        // 옆에 열기는 트리가 Ctrl+Enter 로 직접 받는다(사용자가 다른 키를 주면 그 키).
        const auto BesideKey = key("explorer.openBeside").isEmpty() ? QStringLiteral("Ctrl+Enter") : key("explorer.openBeside");
        if (readOnly_) {
            Add(tr("열기"), QStringLiteral("Enter"), "explorerMenuOpen", [this] { OpenCurrent(false); }, Real && !Info.folder_);
            Add(tr("HyperEditor에서 편집"), {}, "explorerMenuEdit", [this, Path] { emit EditInEditorRequested(Path); }, Real && !Info.folder_);
            Add(tr("HyperEditor에서 폴더 열기"), {}, "explorerMenuEditorFolder", [this, Path, Folder = Info.folder_] {
                emit OpenFolderInEditorRequested(Folder ? Path : NormalizePath(QFileInfo(Path).absolutePath())); }, Real);
            Menu.addSeparator();
            Add(tr("경로 복사"), key("explorer.copyPath"), "explorerMenuCopyPath", [this] { CopyCurrentPath(false); }, Real);
            Add(tr("Windows 탐색기에서 보기"), {}, "explorerMenuReveal", [Path] { explorer::RevealInShell(Path); }, Real);
            Menu.exec(tree_->viewport()->mapToGlobal(Position));
            return;
        }
        if (Info.outside_) {
            Add(tr("열기"), QStringLiteral("Enter"), "explorerMenuOpen", [this] { OpenCurrent(false); });
            Add(tr("옆에 열기"), BesideKey, "explorerMenuOpenBeside", [this] { OpenCurrent(true); });
            Menu.addSeparator();
            Add(tr("경로 복사"), key("explorer.copyPath"), "explorerMenuCopyPath", [this] { CopyCurrentPath(false); });
            Add(tr("Windows 탐색기에서 보기"), {}, "explorerMenuReveal", [Path] { explorer::RevealInShell(Path); });
            Menu.exec(tree_->viewport()->mapToGlobal(Position));
            return;
        }
        const bool Root = Real && (QFileInfo(Path).isRoot() || workspace::SamePath(Path, workspaceRoot_));
        if (Real && Info.folder_) {
            if (sphinxRoots_.contains(PathKey(Path))) { Add(tr("빌드…"), {}, "explorerMenuBuild", [this, Path] { emit BuildRequested(Path); }); Menu.addSeparator(); }
            Add(tr("새 파일…"), key("explorer.newFile"), "explorerMenuNewFile", [this] { NewFile(); });
            Add(tr("새 폴더…"), key("explorer.newFolder"), "explorerMenuNewFolder", [this] { NewFolder(); });
            Menu.addSeparator();
            Add(tr("이 폴더에서 찾기…"), {}, "explorerMenuFindHere", [this, Path] { emit FindInFolderRequested(Path); });
            Add(tr("워크스페이스로 열기"), {}, "explorerMenuOpenWorkspace", [this, Path] { emit OpenWorkspaceRequested(Path); }, !workspace::SamePath(Path, workspaceRoot_));
            Menu.addSeparator();
        } else {
            Add(tr("열기"), QStringLiteral("Enter"), "explorerMenuOpen", [this] { OpenCurrent(false); }, Real);
            Add(tr("옆에 열기"), BesideKey, "explorerMenuOpenBeside", [this] { OpenCurrent(true); }, Real);
            Menu.addSeparator();
            Add(tr("새 파일…"), key("explorer.newFile"), "explorerMenuNewFile", [this] { NewFile(); });
            Add(tr("새 폴더…"), key("explorer.newFolder"), "explorerMenuNewFolder", [this] { NewFolder(); });
            Menu.addSeparator();
        }
        Add(tr("이름 바꾸기"), key("explorer.rename"), "explorerMenuRename", [this] { RenameCurrent(); }, Real && !Root);
        Add(tr("휴지통으로 삭제"), key("explorer.recycle"), "explorerMenuDelete", [this] { DeleteCurrent(false); }, Real && !Root);
        Menu.addSeparator();
        Add(tr("경로 복사"), key("explorer.copyPath"), "explorerMenuCopyPath", [this] { CopyCurrentPath(false); }, Real);
        Add(tr("상대 경로 복사"), key("explorer.copyRelativePath"), "explorerMenuCopyRelative", [this] { CopyCurrentPath(true); }, Real);
        Add(tr("Windows 탐색기에서 보기"), {}, "explorerMenuReveal", [Path] { explorer::RevealInShell(Path); }, Real);
        if (!Info.folder_) {
            Menu.addSeparator();
            auto* All = Menu.addAction(tr("모든 파일 표시")); All->setObjectName("explorerMenuShowAll"); All->setCheckable(true); All->setChecked(settings_.showAllFiles_);
            connect(All, &QAction::toggled, this, [this](bool On) { auto Next = settings_; Next.showAllFiles_ = On; ApplySettings(Next); emit SettingChanged(QStringLiteral("showAllFiles"), On); });
        }
        Menu.exec(tree_->viewport()->mapToGlobal(Position));
    }

    void ExplorerPanel::showMoreMenu()
    {
        fm::ui::Menu Menu(this); Menu.setObjectName("explorerMoreMenu");
        const auto Toggle = [this, &Menu](const QString& Label, const char* Name, bool Value, const QString& Key, auto Apply) {
            auto* Action = Menu.addAction(Label); Action->setObjectName(QString::fromLatin1(Name)); Action->setCheckable(true); Action->setChecked(Value);
            connect(Action, &QAction::toggled, this, [this, Key, Apply](bool On) { auto Next = settings_; Apply(Next, On); ApplySettings(Next); emit SettingChanged(Key, On); });
        };
        Toggle(tr("모든 파일 표시"), "explorerMoreShowAll", settings_.showAllFiles_, QStringLiteral("showAllFiles"), [](Settings& S, bool On) { S.showAllFiles_ = On; });
        Toggle(tr("숨김 항목 표시"), "explorerMoreShowHidden", settings_.showHidden_, QStringLiteral("showHidden"), [](Settings& S, bool On) { S.showHidden_ = On; });
        if (mode_ == Mode::Document)
            Toggle(tr("모든 드라이브 표시"), "explorerMoreShowDrives", settings_.showAllDrives_, QStringLiteral("showAllDrives"), [](Settings& S, bool On) { S.showAllDrives_ = On; });
        Menu.addSeparator();
        auto* Reload = Menu.addAction(WithKey(tr("새로 고침"), key("explorer.refresh"))); Reload->setObjectName("explorerMoreRefresh");
        connect(Reload, &QAction::triggered, this, &ExplorerPanel::Refresh);
        auto* Options = Menu.addAction(tr("탐색기 설정…")); Options->setObjectName("explorerMoreSettings");
        connect(Options, &QAction::triggered, this, &ExplorerPanel::ShowSettingsRequested);
        Menu.exec(more_->mapToGlobal(QPoint(0, more_->height())));
    }
}
