#include "stdafx.h"
#include "sqliteDocumentView.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteApi.hpp"
#include "sqliteCellDelegate.hpp"
#include "sqliteDatabaseInfo.hpp"
#include "sqliteTableDesigner.hpp"
#include "sqliteGlyphs.hpp"
#include "sqliteGrid.hpp"
#include "sqliteKeyStore.hpp"
#include "sqliteQueryTab.hpp"
#include "sqliteResultModel.hpp"
#include "sqliteRuntime.hpp"
#include "sqliteSchemaPanel.hpp"
#include "sqliteTableTab.hpp"
#include "sqliteWorker.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/ThemeColors.h>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/SegmentedControl.h>
#include <fmwidgets/Tag.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAction>
#include <QActionGroup>
#include <QSignalBlocker>
#include <QWidgetAction>
#include <QApplication>
#include <QDateTime>
#include <QInputDialog>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QtConcurrent/QtConcurrentRun>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStackedWidget>
#include <QJsonArray>
#include <QJsonObject>
#include <QTabBar>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace hc::sqlite
{
    namespace
    {
        using T = fm::style::Token;

        // 작업 창 안의 영역(스키마 · 위 · 결과). 시안3(Paper Fluent)은 둥근 테두리 카드, 그 밖은 각진 면.
        class Area final : public QWidget
        {
        public:
            explicit Area(QWidget* Parent = nullptr) : QWidget(Parent)
            {
                auto* Layout = new QVBoxLayout(this);
                Layout->setContentsMargins(1, 1, 1, 1);
                Layout->setSpacing(0);
            }
        protected:
            void paintEvent(QPaintEvent*) override
            {
                const auto& Colors = fm::style::themeColorsFor(this);
                const bool Round = fm::style::ThemeManager::instance().design() == fm::style::Design::PaperFluent;
                QPainter Painter(this);
                Painter.setRenderHint(QPainter::Antialiasing, Round);
                Painter.setPen(QPen(Colors[T::Line], 1.0));
                Painter.setBrush(Colors[T::Surface]);
                const QRectF Box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
                if (Round) Painter.drawRoundedRect(Box, 6, 6);
                else Painter.drawRect(Box);
            }
        };

        QLabel* Hint(const QString& Text, QWidget* Parent)
        {
            auto* Label = new QLabel(Text, Parent);
            Label->setAlignment(Qt::AlignCenter);
            Label->setWordWrap(true);
            Label->setContentsMargins(18, 18, 18, 18);
            return Label;
        }

        void Dim(QWidget* Widget, T Token = T::Fg3)
        {
            auto Palette = Widget->palette();
            Palette.setColor(QPalette::WindowText, fm::style::themeColorsFor(Widget)[Token]);
            Widget->setPalette(Palette);
        }

        QString SizeText(qint64 Bytes) { return QLocale().formattedDataSize(qMax<qint64>(0, Bytes), 1, QLocale::DataSizeTraditionalFormat); }

        bool Within(const QWidget* Ancestor, QWidget* Focus)
        {
            return Ancestor && Focus && (Focus == Ancestor || Ancestor->isAncestorOf(Focus));
        }
    }

    // 도구 모음의 ‘커밋 자동 | 수동’(시안). 메뉴에는 넣지 않는다(MenuActions 의 두 선택 항목).
    class CommitModeAction final : public QWidgetAction
    {
    public:
        using QWidgetAction::QWidgetAction;
        std::function<void(bool Manual)> chosen_;
        void SetState(bool Manual, bool Enabled)
        {
            manual_ = Manual;
            setEnabled(Enabled);
            for (auto* Widget : createdWidgets())
                if (auto* Segments = Widget->findChild<fm::ui::SegmentedControl*>()) { const QSignalBlocker Block(Segments); Segments->setCurrentIndex(Manual ? 1 : 0); }
        }
    protected:
        QWidget* createWidget(QWidget* Parent) override
        {
            auto* Box = new QWidget(Parent);
            Box->setObjectName(QStringLiteral("sqliteCommitMode"));
            auto* Layout = new QHBoxLayout(Box);
            Layout->setContentsMargins(8, 0, 4, 0);
            Layout->setSpacing(6);
            Layout->addWidget(new QLabel(DocumentView::tr("커밋"), Box));
            auto* Segments = new fm::ui::SegmentedControl(QStringList{DocumentView::tr("자동"), DocumentView::tr("수동")}, Box);
            Segments->setObjectName(QStringLiteral("sqliteCommitModeSegments"));
            Segments->setSegmentSize(fm::ui::SegmentedControl::Small);
            Segments->setItemToolTips({DocumentView::tr("자동 — 실행하면 바로 파일에 씁니다."), DocumentView::tr("수동 — Ctrl+S 로 커밋할 때 씁니다. 그 전엔 롤백할 수 있습니다.")});
            Segments->setCurrentIndex(manual_ ? 1 : 0);
            Layout->addWidget(Segments);
            QObject::connect(Segments, &fm::ui::SegmentedControl::currentIndexChanged, Box, [this](int Index) { if (chosen_) chosen_(Index == 1); });
            return Box;
        }
    private:
        bool manual_ = false;
    };

    DocumentView::DocumentView(const QString& Path, Mode ViewMode, QWidget* Parent)
        : QWidget(Parent), path_(Path), mode_(ViewMode)
    {
        setObjectName(QStringLiteral("sqliteDocumentView"));
        options_.readOnly_ = mode_ == Mode::QuickPreview;
        auto* Layout = new QVBoxLayout(this);
        Layout->setContentsMargins(0, 0, 0, 0);
        stack_ = new QStackedWidget(this);
        Layout->addWidget(stack_);
        if (mode_ == Mode::QuickPreview) buildPreview(); else buildWorkWindow();

        lockedPage_ = new QWidget(stack_);
        auto* Locked = new QVBoxLayout(lockedPage_);
        Locked->addStretch(1);
        auto* LockIcon = new QLabel(lockedPage_);
        LockIcon->setAlignment(Qt::AlignCenter);
        LockIcon->setObjectName(QStringLiteral("sqliteLockIcon"));
        auto* LockTitle = new QLabel(tr("암호화된 데이터베이스"), lockedPage_);
        auto TitleFont = Resized(LockTitle->font(), 2);
        TitleFont.setWeight(QFont::DemiBold);
        LockTitle->setFont(TitleFont);
        LockTitle->setAlignment(Qt::AlignCenter);
        auto* LockText = new QLabel(mode_ == Mode::QuickPreview
            ? tr("빠른 보기는 키를 묻지 않습니다. 이 파일에 연결해 둔 저장 키로 열지 못했습니다.\nF3 · F4 로 열면 키를 입력할 수 있습니다.")
            : tr("SQLite 헤더를 읽을 수 없습니다. SEE 키를 넣으면 엽니다."), lockedPage_);
        LockText->setObjectName(QStringLiteral("sqliteLockText"));
        LockText->setAlignment(Qt::AlignCenter);
        LockText->setWordWrap(true);
        Locked->addWidget(LockIcon);
        Locked->addWidget(LockTitle);
        Locked->addWidget(LockText);
        // 키 대화를 닫았거나 저절로 띄우지 않은 경우(설계 8.2).
        keyButton_ = new QPushButton(tr("키 입력…"), lockedPage_);
        keyButton_->setObjectName(QStringLiteral("sqliteKeyButton"));
        keyButton_->hide();
        connect(keyButton_, &QPushButton::clicked, this, &DocumentView::RequestKey);
        Locked->addSpacing(8);
        Locked->addWidget(keyButton_, 0, Qt::AlignHCenter);
        Locked->addStretch(2);
        stack_->addWidget(lockedPage_);

        failure_ = Hint({}, stack_);
        failure_->setObjectName(QStringLiteral("sqliteFailure"));
        stack_->addWidget(failure_);
        stack_->setCurrentIndex(0);

        if (mode_ != Mode::QuickPreview) {
            // 도구 모음(시안 ‘SQLite 작업 창’). 단축키는 문서 창의 라우터가 sqlite.* 명령으로 부른다(설정 › 단축키).
            const auto Make = [this](const QString& Id, const QString& Text, const QString& GlyphName, const QString& Key = {}) {
                auto* Action = new QAction(Text, this);
                Action->setObjectName(QStringLiteral("sqliteAction.") + Id);
                Action->setProperty("sqliteCommand", Id);
                Action->setProperty("sqliteGlyph", GlyphName);
                Action->setToolTip(Key.isEmpty() ? Text : QStringLiteral("%1 (%2)").arg(Text, Key));
                connect(Action, &QAction::triggered, this, [this, Id] { RunCommand(Id); });
                actions_.append(Action);
                return Action;
            };
            const auto Separator = [this] { auto* Action = new QAction(this); Action->setSeparator(true); actions_.append(Action); };
            Make(QStringLiteral("sqlite.newQuery"), tr("새 쿼리 탭"), QStringLiteral("newq"), QStringLiteral("Ctrl+T"));
            Make(QStringLiteral("sqlite.openSql"), tr("SQL 파일 열기…"), QStringLiteral("open"));
            Make(QStringLiteral("sqlite.saveQuery"), tr("쿼리 저장"), QStringLiteral("save"));
            Separator();
            Make(QStringLiteral("sqlite.run"), tr("실행"), QStringLiteral("play"), QStringLiteral("F5"));
            Make(QStringLiteral("sqlite.runCurrent"), tr("현재 문 실행"), QStringLiteral("playline"), QStringLiteral("Ctrl+Enter"));
            Make(QStringLiteral("sqlite.stop"), tr("중지"), QStringLiteral("stop"), QStringLiteral("Ctrl+."));
            Make(QStringLiteral("sqlite.explain"), tr("실행 계획"), QStringLiteral("plan"), QStringLiteral("Ctrl+L"));
            Make(QStringLiteral("sqlite.format"), tr("SQL 서식"), QStringLiteral("format"), QStringLiteral("Alt+Shift+F"));
            Separator();
            // 가져오기 · 내보내기(시안 도구 모음).
            Make(QStringLiteral("sqlite.import"), tr("가져오기 (CSV · JSON)…"), QStringLiteral("import"));
            Make(QStringLiteral("sqlite.export"), tr("내보내기 (CSV · JSON · SQL)…"), QStringLiteral("export"));
            Separator();
            Make(QStringLiteral("sqlite.commit"), tr("커밋"), QStringLiteral("commit"), QStringLiteral("Ctrl+S"));
            Make(QStringLiteral("sqlite.rollback"), tr("롤백 · 모두 되돌리기"), QStringLiteral("rollback"));
            Separator();
            Make(QStringLiteral("sqlite.refresh"), tr("스키마 새로 고침"), QStringLiteral("refresh"), QStringLiteral("Ctrl+R"));
            auto* Toggle = Make(QStringLiteral("sqlite.toggleSchema"), tr("스키마 패널"), QStringLiteral("panel"));
            Toggle->setCheckable(true);
            Toggle->setChecked(true);
            Make(QStringLiteral("sqlite.attach"), tr("데이터베이스 첨부 (ATTACH)…"), QStringLiteral("attach"));
            Make(QStringLiteral("sqlite.dbInfo"), tr("데이터베이스 정보 · 유지 관리…"), QStringLiteral("info"));
            menuActions_ = actions_;
            // 데이터베이스 › 암호화…(설계 8.1): 메뉴에만.
            {
                auto* Encrypt = new QAction(tr("암호화…"), this);
                Encrypt->setObjectName(QStringLiteral("sqliteAction.sqlite.encrypt"));
                Encrypt->setProperty("sqliteCommand", QStringLiteral("sqlite.encrypt"));
                connect(Encrypt, &QAction::triggered, this, [this] { RunCommand(QStringLiteral("sqlite.encrypt")); });
                menuActions_.append(Encrypt);
            }
            // 커밋 모드(설계 6.3, 시안 도구 모음 오른쪽 ‘커밋 자동 | 수동’): 지금 쿼리 탭의 모드. 메뉴에는 두 선택 항목으로.
            commitMode_ = new CommitModeAction(this);
            commitMode_->chosen_ = [this](bool Manual) { if (auto* Query = currentQuery()) Query->SetManualCommit(Manual); refreshCommitMode(); };
            actions_.append(commitMode_);
            auto* Modes = new QActionGroup(this);
            for (const bool Manual : {false, true}) {
                auto* Mode = new QAction(Manual ? tr("수동 커밋") : tr("자동 커밋"), Modes);
                Mode->setObjectName(Manual ? QStringLiteral("sqliteAction.commitManual") : QStringLiteral("sqliteAction.commitAuto"));
                Mode->setCheckable(true);
                connect(Mode, &QAction::triggered, this, [this, Manual] { if (auto* Query = currentQuery()) Query->SetManualCommit(Manual); refreshCommitMode(); });
                menuActions_.append(Mode);
            }
            connect(this, &DocumentView::StateChanged, this, &DocumentView::refreshCommitMode);
        }
        const auto Retheme = [this] {
            const auto& Colors = fm::style::themeColorsFor(this);
            for (auto* Action : actions_)
                if (!Action->isSeparator() && Action->property("sqliteGlyph").isValid()) Action->setIcon(Glyph(Action->property("sqliteGlyph").toString(), Colors[T::Fg2], 16));
            if (auto* Icon = findChild<QLabel*>(QStringLiteral("sqliteLockIcon"))) Icon->setPixmap(Glyph(QStringLiteral("lock"), Colors[T::Fg3], 34).pixmap(34, 34));
            if (auto* Text = findChild<QLabel*>(QStringLiteral("sqliteLockText"))) Dim(Text, T::Fg2);
            Dim(failure_, T::Fg2);
            for (int Index = 0; tabs_ && Index < works_.size(); ++Index)
                tabs_->setTabIcon(Index, Glyph(works_[Index]->GlyphName(), works_[Index]->IsQuery() ? Colors[T::AccentFg] : Colors[T::Ok], 13));
        };
        connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, Retheme);
        Retheme();
    }

    DocumentView::~DocumentView()
    {
        // 쿼리 탭 보관(설계 5.5): 탭을 지우기 전에 쓴다.
        SaveTabs();
        stopCounting();
        // 행 수 세기 작업이 연결을 닫을 때까지 기다린다(탭을 닫은 뒤 탐색기가 곧바로 파일을 옮길 수 있게, 설계 4.6).
        for (const auto& Job : retiredJobs_) Job->Wait(3000);
        // 탭(쿼리 실행기 · keyset 작업)이 먼저 연결을 닫고, 그다음 GUI 연결을 닫는다.
        qDeleteAll(works_);
        works_.clear();
        delete previewModel_;
        previewModel_ = nullptr;
    }

    void DocumentView::buildWorkWindow()
    {
        auto* Work = new QWidget(stack_);
        auto* Layout = new QHBoxLayout(Work);
        Layout->setContentsMargins(6, 6, 6, 6);
        split_ = new QSplitter(Qt::Horizontal, Work);
        split_->setChildrenCollapsible(false);
        split_->setHandleWidth(6);
        Layout->addWidget(split_);

        auto* SchemaArea = new Area(split_);
        schemaArea_ = SchemaArea;
        schemaPanel_ = new SchemaPanel(SchemaArea);
        SchemaArea->layout()->addWidget(schemaPanel_);
        SchemaArea->setMinimumWidth(180);

        rightSplit_ = new QSplitter(Qt::Vertical, split_);
        rightSplit_->setChildrenCollapsible(false);
        rightSplit_->setHandleWidth(6);
        auto* TopArea = new Area(rightSplit_);
        auto* TabRow = new QWidget(TopArea);
        auto* TabLayout = new QHBoxLayout(TabRow);
        TabLayout->setContentsMargins(0, 0, 4, 0);
        TabLayout->setSpacing(2);
        tabs_ = new QTabBar(TabRow);
        tabs_->setObjectName(QStringLiteral("sqliteDocumentTabs"));
        tabs_->setDocumentMode(true);
        tabs_->setExpanding(false);
        tabs_->setTabsClosable(true);
        tabs_->setMovable(true);
        tabs_->setUsesScrollButtons(true);
        TabLayout->addWidget(tabs_);
        auto* Plus = new QPushButton(QStringLiteral("+"), TabRow);
        Plus->setObjectName(QStringLiteral("sqliteNewQueryButton"));
        Plus->setToolTip(tr("새 쿼리 탭 (Ctrl+T)"));
        Plus->setFlat(true);
        Plus->setFixedSize(26, 24);
        connect(Plus, &QPushButton::clicked, this, [this] { NewQuery(); });
        TabLayout->addWidget(Plus);
        TabLayout->addStretch(1);
        TopArea->layout()->addWidget(TabRow);
        topStack_ = new QStackedWidget(TopArea);
        emptyTop_ = Hint(tr("왼쪽 스키마에서 테이블이나 뷰를 두 번 누르면(또는 Enter) 데이터 탭이, Ctrl+T 는 빈 쿼리 탭이 열립니다."), topStack_);
        topStack_->addWidget(emptyTop_);
        TopArea->layout()->addWidget(topStack_);
        auto* ResultArea = new Area(rightSplit_);
        resultStack_ = new QStackedWidget(ResultArea);
        emptyResult_ = Hint({}, resultStack_);
        resultStack_->addWidget(emptyResult_);
        ResultArea->layout()->addWidget(resultStack_);
        rightSplit_->setStretchFactor(0, 0);
        rightSplit_->setStretchFactor(1, 1);
        split_->setStretchFactor(0, 0);
        split_->setStretchFactor(1, 1);
        split_->setSizes({268, 900});
        stack_->addWidget(Work);

        // 기록 탭(설계 5.5): 고른 문을 지금 쿼리 탭(없으면 새 탭)에 넣는다.
        history_ = new HistoryPanel(SchemaArea);
        schemaPanel_->SetHistoryWidget(history_);
        connect(history_, &HistoryPanel::InsertRequested, this, [this](const QString& Sql) {
            if (auto* Query = currentQuery()) Query->InsertStatement(Sql); else NewQuery(Sql.trimmed().endsWith(QLatin1Char(';')) ? Sql : Sql.trimmed() + QLatin1Char(';'));
        });
        connect(history_, &HistoryPanel::OpenRequested, this, [this](const QString& Sql) { NewQuery(Sql); });
        connect(history_, &HistoryPanel::ClearRequested, this, [this] {
            if (QMessageBox::question(this, tr("기록 지우기"), tr("이 데이터베이스에서 실행한 문의 기록을 모두 지울까요?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
            ProfileStore(profile_, path_).ClearHistory();
            history_->SetEntries({});
        });
        tabsTimer_ = new QTimer(this);
        tabsTimer_->setSingleShot(true);
        tabsTimer_->setInterval(1500);
        connect(tabsTimer_, &QTimer::timeout, this, &DocumentView::SaveTabs);
        connect(schemaPanel_, &SchemaPanel::OpenObject, this, &DocumentView::OpenObject);
        connect(schemaPanel_, &SchemaPanel::RefreshRequested, this, [this] { RunCommand(QStringLiteral("sqlite.refresh")); });
        connect(schemaPanel_, &SchemaPanel::AttachRequested, this, &DocumentView::attachDatabase);
        connect(schemaPanel_, &SchemaPanel::DetachRequested, this, &DocumentView::detachDatabase);
        connect(schemaPanel_, &SchemaPanel::CountRequested, this, &DocumentView::startCounting);
        connect(schemaPanel_, &SchemaPanel::ShowDdlRequested, this, &DocumentView::showDdl);
        connect(schemaPanel_, &SchemaPanel::CommandRequested, this, [this](const QString& Id) { RunCommand(Id); });
        connect(schemaPanel_, &SchemaPanel::ImportRequested, this, [this](const QString&, const QString& Name) { ShowImport(Name); });
        connect(schemaPanel_, &SchemaPanel::ExportRequested, this, [this](const QString& Database, const QString& Name) { ShowExport(ExportRequestFor(Database, Name)); });
        connect(schemaPanel_, &SchemaPanel::ScriptRequested, this, [this](const QString& Database, const QString& Name, const QString& Kind) {
            if (const auto Text = ScriptFor(Database, Name, Kind); !Text.isEmpty()) NewQuery(Text);
        });
        connect(tabs_, &QTabBar::currentChanged, this, &DocumentView::activateTab);
        connect(tabs_, &QTabBar::tabCloseRequested, this, [this](int Index) { closeTab(Index); });
        connect(tabs_, &QTabBar::tabMoved, this, [this](int From, int To) { works_.move(From, To); });
    }

    void DocumentView::buildPreview()
    {
        auto* Page = new QWidget(stack_);
        auto* Layout = new QVBoxLayout(Page);
        Layout->setContentsMargins(8, 6, 8, 6);
        Layout->setSpacing(6);
        auto* Head = new QHBoxLayout;
        previewHead_ = new QLabel(Page);
        previewHead_->setObjectName(QStringLiteral("sqlitePreviewHeader"));
        auto HeadFont = previewHead_->font();
        HeadFont.setWeight(QFont::DemiBold);
        previewHead_->setFont(HeadFont);
        Head->addWidget(previewHead_);
        auto* ReadOnly = new fm::ui::Tag(Page);
        ReadOnly->setText(tr("열람만"));
        ReadOnly->setTone(fm::ui::Tag::Mute);
        ReadOnly->setCompact(true);
        Head->addWidget(ReadOnly);
        Head->addStretch(1);
        auto* Keys = new QLabel(tr("F3 · F4 로 열면 편집 · SQL 실행"), Page);
        Keys->setObjectName(QStringLiteral("sqlitePreviewKeys"));
        Head->addWidget(Keys);
        Layout->addLayout(Head);

        auto* Split = new QSplitter(Qt::Horizontal, Page);
        Split->setChildrenCollapsible(false);
        auto* Left = new Area(Split);
        auto* LeftTitle = new QLabel(Left);
        LeftTitle->setObjectName(QStringLiteral("sqlitePreviewListTitle"));
        LeftTitle->setContentsMargins(8, 6, 8, 4);
        Left->layout()->addWidget(LeftTitle);
        previewList_ = new QListWidget(Left);
        previewList_->setObjectName(QStringLiteral("sqlitePreviewList"));
        previewList_->setFrameShape(QFrame::NoFrame);
        Left->layout()->addWidget(previewList_);
        Left->setMinimumWidth(150);

        auto* Right = new Area(Split);
        auto* TitleRow = new QWidget(Right);
        auto* TitleLayout = new QHBoxLayout(TitleRow);
        TitleLayout->setContentsMargins(10, 6, 8, 4);
        previewTitle_ = new QLabel(TitleRow);
        previewTitle_->setObjectName(QStringLiteral("sqlitePreviewTitle"));
        TitleLayout->addWidget(previewTitle_, 1);
        auto* Segments = new fm::ui::SegmentedControl(QStringList{tr("데이터"), tr("구조")}, TitleRow);
        Segments->setObjectName(QStringLiteral("sqlitePreviewSegments"));
        Segments->setSegmentSize(fm::ui::SegmentedControl::Small);
        previewSegments_ = Segments;
        TitleLayout->addWidget(Segments);
        Right->layout()->addWidget(TitleRow);
        previewPages_ = new QStackedWidget(Right);
        previewGrid_ = new GridView(previewPages_);
        connect(previewGrid_, &GridView::CopyRequested, this, &DocumentView::CopyRequested);
        previewPages_->addWidget(previewGrid_);
        auto* Structure = new QWidget(previewPages_);
        auto* StructureLayout = new QVBoxLayout(Structure);
        StructureLayout->setContentsMargins(8, 4, 8, 8);
        previewColumns_ = new QListWidget(Structure);
        previewColumns_->setObjectName(QStringLiteral("sqlitePreviewColumns"));
        previewColumns_->setFrameShape(QFrame::NoFrame);
        StructureLayout->addWidget(previewColumns_, 2);
        previewDdl_ = new QLabel(Structure);
        previewDdl_->setObjectName(QStringLiteral("sqlitePreviewDdl"));
        previewDdl_->setWordWrap(true);
        previewDdl_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        previewDdl_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        StructureLayout->addWidget(previewDdl_, 1);
        previewPages_->addWidget(Structure);
        Right->layout()->addWidget(previewPages_);
        Split->setStretchFactor(1, 1);
        Split->setSizes({220, 600});
        Layout->addWidget(Split, 1);
        auto* Foot = new QLabel(tr("스크롤하면 이어서 읽습니다. 편집 · SQL 실행은 F3 · F4 에서."), Page);
        Foot->setObjectName(QStringLiteral("sqlitePreviewFoot"));
        Layout->addWidget(Foot);
        Dim(Keys);
        Dim(Foot);
        Dim(LeftTitle, T::Fg2);
        stack_->addWidget(Page);

        connect(Segments, &fm::ui::SegmentedControl::currentIndexChanged, previewPages_, &QStackedWidget::setCurrentIndex);
        connect(previewList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* Item) {
            if (Item) showPreviewObject(Item->data(Qt::UserRole).toString(), Item->data(Qt::UserRole + 1).toString());
        });
    }

    bool DocumentView::applyKey(Connection& Db, QString* Error) const
    {
        QByteArray Protected;
        KeyFormat Format;
        {
            std::lock_guard Lock(keyMutex_);
            Protected = keyProtected_;
            Format = keyFormat_;
        }
        return Protected.isEmpty() || ApplyProtectedKey(Db, Protected, Format, Error);
    }

    std::unique_ptr<Connection> DocumentView::openWorker(QString* Error) const
    {
        // 작업 스레드 연결(쿼리 탭 · keyset · 행 수 · 유지 관리)도 문서 연결과 같은 설정(설정 › SQLite › 열기)과 같은 키.
        auto Db = Connection::Open(path_, options_, Error);
        if (Db && !applyKey(*Db, Error)) return {};
        if (Db && queryPreferences_.foreignKeys_) Db->Exec(QStringLiteral("PRAGMA foreign_keys = ON"), nullptr);
        if (Db && queryPreferences_.loadExtension_) sqlite3_enable_load_extension(Db->Handle(), 1);
        return Db;
    }

    void DocumentView::setFailure(const QString& Error)
    {
        error_ = Error;
        failure_->setText(Error);
        stack_->setCurrentWidget(failure_);
    }

    void DocumentView::Open()
    {
        if (opened_) return;
        opened_ = true;
        opening_ = true;
        emit StateChanged();
        header_ = InspectFile(path_);
        switch (header_.kind_) {
        case FileKind::Unreadable:
            setFailure(tr("파일을 열 수 없습니다: %1").arg(header_.error_));
            break;
        case FileKind::NotSqlite:
            setFailure(tr("SQLite 데이터베이스가 아닙니다. 머리 16바이트가 ‘SQLite format 3’ 이 아니고 SEE 암호화 형식으로도 보이지 않습니다."));
            break;
        case FileKind::SeeEncrypted:
            // 저장한 키를 먼저 시도하고(빠른 보기는 그것만), 안 되면 잠김 화면과 키 대화(설계 8.2).
            locked_ = true;
            stack_->setCurrentWidget(lockedPage_);
            if (schemaPanel_) schemaPanel_->SetLocked(true);
            if (Runtime::See() != SeeState::Available) {
                if (auto* Text = findChild<QLabel*>(QStringLiteral("sqliteLockText"))) Text->setText(Text->text() + QStringLiteral("\n") + Runtime::SeeText(Runtime::See()));
            } else if (!tryStoredKey() && mode_ != Mode::QuickPreview) {
                keyButton_->show();
            }
            break;
        case FileKind::Plain:
        case FileKind::Empty:
            openDatabase();
            break;
        }
        opening_ = false;
        ready_ = error_.isEmpty();
        emit StateChanged();
        schedulePrompt();
    }

    void DocumentView::showEvent(QShowEvent* Event)
    {
        QWidget::showEvent(Event);
        schedulePrompt();
    }

    void DocumentView::schedulePrompt()
    {
        // 잠긴 문서를 처음 보일 때 키 대화를 한 번 띄운다. 닫으면 잠김 화면의 ‘키 입력…’ 으로 다시 연다.
        if (!opened_ || opening_ || !locked_ || keyPrompted_ || !promptEnabled_ || mode_ == Mode::QuickPreview || Runtime::See() != SeeState::Available) return;
        keyPrompted_ = true;
        QTimer::singleShot(0, this, [this] {
            if (locked_ && isVisible()) RequestKey();
            else keyPrompted_ = false;
        });
    }

    bool DocumentView::tryStoredKey()
    {
        const bool Allowed = mode_ == Mode::QuickPreview ? queryPreferences_.previewSavedKeys_ : queryPreferences_.trySavedKeys_;
        if (!Allowed || profile_.isEmpty()) return false;
        KeyStore Store(profile_);
        const auto Entry = Store.Find(path_);
        if (!Entry) return false;
        auto Key = RevealKey(Entry->protected_);
        if (Key.isEmpty()) return false;
        // 빠른 보기는 언제나 읽기 전용(options_).
        const auto Result = tryKey(Key, Entry->format_, false);
        WipeKey(Key);
        if (!Result.ok_) return false;
        usedSavedKey_ = true;
        Store.Touch(path_);
        return true;
    }

    SeeKeyDialog::Outcome DocumentView::tryKey(const QByteArray& Key, KeyFormat Format, bool ReadOnly)
    {
        auto Options = options_;
        Options.readOnly_ = Options.readOnly_ || ReadOnly;
        QString Error;
        const auto Attempt = [&](const OpenOptions& With, std::unique_ptr<Connection>& Db) {
            Db = Connection::Open(path_, With, &Error);
            if (!Db) return SQLITE_CANTOPEN;
            if (const int Rc = Db->ApplyKey(Key, Format); Rc != SQLITE_OK) { Error = Db->ErrorMessage(); return Rc; }
            return Db->Probe(&Error);
        };
        std::unique_ptr<Connection> Db;
        int Rc = Attempt(Options, Db);
        if (Rc != SQLITE_OK && Rc != SQLITE_NOTADB && header_.wal_ && Options.readOnly_) {
            // 쓸 수 없는 폴더의 WAL 파일(openDatabase 와 같은 처리).
            Options.immutable_ = true;
            Rc = Attempt(Options, Db);
        }
        if (Rc == SQLITE_NOTADB) return {false, true, {}};
        if (Rc != SQLITE_OK) return {false, false, Error.isEmpty() ? QString::fromUtf8(sqlite3_errstr(Rc)) : Error};
        auto Protected = ProtectKey(Key);
        if (Protected.isEmpty()) return {false, false, tr("작업 연결에 쓸 키를 보호하지 못했습니다(DPAPI).")};
        {
            std::lock_guard Lock(keyMutex_);
            keyProtected_ = Protected;
            keyFormat_ = Format;
        }
        options_ = Options;
        db_ = std::move(Db);
        encrypted_ = true;
        locked_ = false;
        if (schemaPanel_) schemaPanel_->SetLocked(false);
        keyButton_->hide();
        finishOpen();
        ready_ = error_.isEmpty();
        emit StateChanged();
        return {true};
    }

    bool DocumentView::Unlock(const QByteArray& Key, KeyFormat Format, bool ReadOnly, bool Save, bool* WrongKey, QString* Error)
    {
        if (!locked_) return false;
        const auto Result = tryKey(Key, Format, ReadOnly);
        if (WrongKey) *WrongKey = Result.wrongKey_;
        if (Error) *Error = Result.error_;
        if (Result.ok_ && Save && !profile_.isEmpty()) KeyStore(profile_).Save(path_, Key, Format);
        return Result.ok_;
    }

    bool DocumentView::RequestKey()
    {
        if (!locked_ || mode_ == Mode::QuickPreview || Runtime::See() != SeeState::Available) return false;
        SeeKeyDialog Dialog(path_, queryPreferences_.seeFormat_, queryPreferences_.saveKeyDefault_ && !profile_.isEmpty(), false,
                            [this](const QByteArray& Key, KeyFormat Format, bool ReadOnly) { return tryKey(Key, Format, ReadOnly); }, this);
        if (Dialog.exec() != QDialog::Accepted) return false;
        auto Key = Dialog.TakeKey();
        if (Dialog.SaveKey() && !profile_.isEmpty() && !KeyStore(profile_).Save(path_, Key, Dialog.Format()))
            QMessageBox::warning(this, tr("키 저장"), tr("키 저장소(sqlite-keys.dat)에 쓰지 못했습니다. 문서는 열었습니다."));
        WipeKey(Key);
        return true;
    }

    void DocumentView::openDatabase()
    {
        QString Error;
        db_ = Connection::Open(path_, options_, &Error);
        if (db_ && db_->Probe(&Error) != SQLITE_OK && header_.wal_ && options_.readOnly_) {
            // 쓸 수 없는 폴더의 WAL 파일은 -shm 을 만들 수 없어 읽기 전용으로도 열리지 않는다. immutable 로 다시 연다.
            db_.reset();
            options_.immutable_ = true;
            db_ = Connection::Open(path_, options_, &Error);
        }
        if (!db_ || db_->Probe(&Error) != SQLITE_OK) {
            db_.reset();
            setFailure(tr("데이터베이스를 열지 못했습니다: %1").arg(Error));
            return;
        }
        finishOpen();
    }

    void DocumentView::finishOpen()
    {
        {
            Statement Journal(*db_, QStringLiteral("PRAGMA main.journal_mode"));
            if (Journal.Valid() && Journal.Step() == SQLITE_ROW) journalMode_ = Journal.Value(0).toString();
        }
        if (queryPreferences_.foreignKeys_ && !options_.readOnly_) db_->Exec(QStringLiteral("PRAGMA foreign_keys = ON"), nullptr);
        {
            Statement Functions(*db_, QStringLiteral("SELECT DISTINCT name FROM pragma_function_list ORDER BY name"));
            while (Functions.Valid() && Functions.Step() == SQLITE_ROW) functions_.append(Functions.Value(0).toString());
        }
        // 자동 완성 자료: PRAGMA 이름 · 정렬 규칙(설계 5.2).
        {
            Statement Pragmas(*db_, QStringLiteral("SELECT name FROM pragma_pragma_list ORDER BY name"));
            while (Pragmas.Valid() && Pragmas.Step() == SQLITE_ROW) completion_.pragmas_.append(Pragmas.Value(0).toString());
            Statement Collations(*db_, QStringLiteral("SELECT name FROM pragma_collation_list ORDER BY name"));
            while (Collations.Valid() && Collations.Step() == SQLITE_ROW) completion_.collations_.append(Collations.Value(0).toString());
            completion_.functions_ = functions_;
        }
        readSchema();
        stack_->setCurrentIndex(0);
        startCounting();
        if (mode_ == Mode::QuickPreview && previewList_->count() > 0) previewList_->setCurrentRow(0);
        loadProfile();
    }

    void DocumentView::loadProfile()
    {
        if (mode_ == Mode::QuickPreview || !history_ || profile_.isEmpty()) return;
        ProfileStore Store(profile_, path_);
        if (queryPreferences_.historyEnabled_) history_->SetEntries(Store.LoadHistory());
        if (queryPreferences_.restoreTabs_) restoreTabs();
    }

    void DocumentView::restoreTabs()
    {
        const auto State = ProfileStore(profile_, path_).LoadTabs();
        const auto Tabs = State["tabs"].toArray();
        if (Tabs.isEmpty()) return;
        restoring_ = true;
        for (const auto& Value : Tabs) {
            const auto Tab = Value.toObject();
            if (Tab["kind"].toString() == QLatin1String("table")) {
                if (schema_.Find(Tab["database"].toString(), Tab["name"].toString())) OpenObject(Tab["database"].toString(), Tab["name"].toString());
                continue;
            }
            const auto File = Tab["file"].toString();
            QueryTab* Query = nullptr;
            if (!File.isEmpty()) { if (QFileInfo::exists(File)) Query = NewQuery({}, File); }
            else Query = NewQuery(Tab["text"].toString());
            if (!Query) continue;
            if (File.isEmpty() && !Tab["title"].toString().isEmpty()) Query->SetTitle(Tab["title"].toString());
            Query->SetRowLimit(Tab["rowLimit"].toInt(queryPreferences_.rowLimit_));
            Query->SetCaret(Tab["caret"].toInt());
        }
        queryCounter_ = qMax(queryCounter_, State["counter"].toInt());
        if (const int Current = State["current"].toInt(-1); Current >= 0 && Current < tabs_->count()) tabs_->setCurrentIndex(Current);
        restoring_ = false;
    }

    void DocumentView::SaveTabs()
    {
        if (tabsTimer_) tabsTimer_->stop();
        if (profile_.isEmpty() || mode_ == Mode::QuickPreview || !ready_ || locked_ || restoring_ || !queryPreferences_.restoreTabs_) return;
        QJsonArray Tabs;
        for (auto* Work : works_) {
            if (auto* Table = qobject_cast<TableTab*>(Work)) {
                Tabs.append(QJsonObject{{"kind", "table"}, {"database", Table->Object().database_}, {"name", Table->Object().name_}});
                continue;
            }
            auto* Query = qobject_cast<QueryTab*>(Work);
            if (!Query) continue;
            QJsonObject Tab{{"kind", "query"}, {"caret", Query->Caret()}, {"rowLimit", Query->RowLimit()}};
            // .sql 파일 탭은 경로만, 이름 없는 탭은 글(키를 넣는 문은 빼고).
            if (!Query->File().isEmpty()) Tab["file"] = Query->File();
            else { Tab["title"] = Query->Title(); Tab["text"] = ProfileStore::WithoutSensitive(Query->Text()); }
            Tabs.append(Tab);
        }
        ProfileStore(profile_, path_).SaveTabs(QJsonObject{{"tabs", Tabs}, {"current", tabs_ ? tabs_->currentIndex() : -1}, {"counter", queryCounter_}});
    }

    void DocumentView::recordHistory(const QList<HistoryEntry>& Entries)
    {
        if (!queryPreferences_.historyEnabled_) return;
        const int Limit = qMax(1, queryPreferences_.historyLimit_);
        if (!profile_.isEmpty()) ProfileStore(profile_, path_).AppendHistory(Entries, Limit);
        if (history_) history_->Prepend(Entries, Limit);
    }

    void DocumentView::readSchema()
    {
        QString Error;
        if (!ReadSchema(*db_, schema_, &Error)) { setFailure(tr("스키마를 읽지 못했습니다: %1").arg(Error)); return; }
        const QFileInfo Info(path_);
        if (schemaPanel_) {
            SchemaPanel::Header Head;
            Head.fileName_ = Info.fileName();
            Head.path_ = Info.absoluteFilePath();
            Head.fileSize_ = Info.size();
            Head.journalMode_ = journalMode_;
            Head.readOnly_ = IsReadOnly();
            Head.encrypted_ = encrypted_;
            schemaPanel_->SetSchema(schema_);
            schemaPanel_->SetHeader(Head);
        }
        if (previewList_) {
            const auto& Colors = fm::style::themeColorsFor(this);
            previewHead_->setText(QStringLiteral("%1  ·  %2  ·  %3").arg(Info.fileName(), journalMode_.toUpper(), SizeText(Info.size()))
                                  + (encrypted_ ? QStringLiteral("  ·  SEE · AES-256") : QString()));
            previewList_->clear();
            int TableCount = 0, ViewCount = 0;
            if (const auto* Main = schema_.Database(QStringLiteral("main"))) {
                for (const auto Type : {ObjectType::Table, ObjectType::View}) {
                    for (const auto* Object : Main->Objects(Type)) {
                        auto* Item = new QListWidgetItem(Glyph(Type == ObjectType::Table ? QStringLiteral("table") : QStringLiteral("view"), Colors[T::Ok], 14),
                            Object->name_, previewList_);
                        Item->setData(Qt::UserRole, Object->database_);
                        Item->setData(Qt::UserRole + 1, Object->name_);
                        ++(Type == ObjectType::Table ? TableCount : ViewCount);
                    }
                }
            }
            if (auto* Title = findChild<QLabel*>(QStringLiteral("sqlitePreviewListTitle"))) Title->setText(tr("테이블 %1 · 뷰 %2").arg(TableCount).arg(ViewCount));
        }
        completion_.schema_ = schema_;
        refreshSchemaWords();
        emit StateChanged();
    }

    void DocumentView::refreshSchemaWords()
    {
        QStringList Tables, Columns;
        for (const auto& Database : schema_.databases_)
            for (const auto& Object : Database.objects_) {
                if (Object.type_ != ObjectType::Table && Object.type_ != ObjectType::View) continue;
                Tables.append(Object.name_);
                for (const auto& Column : Object.columns_) Columns.append(Column.name_);
            }
        for (auto* Work : works_)
            if (auto* Query = qobject_cast<QueryTab*>(Work)) Query->SetSchemaWords(Tables, Columns, functions_);
    }

    void DocumentView::startCounting()
    {
        stopCounting();
        if (!db_) return;
        // 설정 › SQLite › 열기 ‘테이블 행 수 세기’: 큰 파일은 count(*) 가 오래 걸린다.
        if (queryPreferences_.countRows_ == 2 || (queryPreferences_.countRows_ == 0 && QFileInfo(path_).size() > 256ll * 1024 * 1024)) return;
        QList<std::pair<QString, QString>> Targets;
        for (const auto& Database : schema_.databases_)
            for (const auto* Object : Database.Objects(ObjectType::Table, true))
                if (!Object->virtual_) Targets.append({Database.name_, Object->name_});
        if (Targets.isEmpty()) return;
        auto Job = std::make_shared<WorkerJob>();
        counting_ = Job;
        QByteArray Protected;
        KeyFormat Format;
        {
            std::lock_guard Lock(keyMutex_);
            Protected = keyProtected_;
            Format = keyFormat_;
        }
        QThreadPool::globalInstance()->start([Job, Owner = this, Targets, Path = path_, Options = options_, Protected, Format] {
            if (!Job->Begin()) return;
            QString Error;
            auto Db = Connection::Open(Path, Options, &Error);
            if (Db && !Protected.isEmpty() && !ApplyProtectedKey(*Db, Protected, Format, &Error)) Db.reset();
            if (Db && Job->Attach(Db.get())) {
                for (const auto& [Database, Table] : Targets) {
                    const auto Count = CountRows(*Db, Database, Table);
                    const bool Sent = Job->Post([&] {
                        QMetaObject::invokeMethod(Owner, [Owner, Database, Table, Count] {
                            if (Owner->schemaPanel_) Owner->schemaPanel_->SetRowCount(Database, Table, Count);
                            Owner->completion_.rowCounts_.insert((Database + QLatin1Char('.') + Table).toLower(), Count);
                            for (int Row = 0; Owner->previewList_ && Row < Owner->previewList_->count(); ++Row) {
                                auto* Item = Owner->previewList_->item(Row);
                                if (Item->data(Qt::UserRole + 1).toString() == Table && Item->data(Qt::UserRole).toString() == Database)
                                    Item->setText(QStringLiteral("%1   %2").arg(Table, QLocale().toString(Count)));
                            }
                        }, Qt::QueuedConnection);
                    });
                    if (!Sent) break;
                }
                Job->Detach();
            }
            Job->Finish();
        });
    }

    void DocumentView::stopCounting()
    {
        if (auto Job = std::exchange(counting_, {})) { Job->Cancel(); retiredJobs_.append(Job); }
        retiredJobs_.erase(std::remove_if(retiredJobs_.begin(), retiredJobs_.end(), [](const auto& Job) { return Job->Done(); }), retiredJobs_.end());
    }

    bool DocumentView::IsReadOnly() const
    {
        return options_.readOnly_ || options_.immutable_ || (db_ && sqlite3_db_readonly(db_->Handle(), "main") == 1);
    }

    bool DocumentView::IsBusy() const
    {
        if (opening_) return true;
        for (auto* Work : works_)
            if (auto* Query = qobject_cast<QueryTab*>(Work); Query && Query->Running()) return true;
        return false;
    }

    bool DocumentView::RequestClose()
    {
        // 커밋하지 않은 편집 · 열린 트랜잭션: 커밋 · 버리기 · 취소(설계 5.5).
        if (const int Pending = PendingTotal(); Pending > 0) {
            QMessageBox Box(QMessageBox::Question, tr("커밋하지 않은 변경"),
                tr("%1 에 커밋하지 않은 변경이 %2건 있습니다. 닫기 전에 파일에 쓸까요?").arg(QFileInfo(path_).fileName()).arg(Pending), QMessageBox::NoButton, this);
            auto* CommitButton = Box.addButton(tr("커밋"), QMessageBox::AcceptRole);
            auto* DiscardButton = Box.addButton(tr("버리기"), QMessageBox::DestructiveRole);
            Box.addButton(tr("취소"), QMessageBox::RejectRole);
            Box.setDefaultButton(CommitButton);
            Box.exec();
            if (Box.clickedButton() == CommitButton) { if (!commitAll(true)) return false; }
            else if (Box.clickedButton() == DiscardButton) rollbackAll(true);
            else return false;
        }
        QStringList Unsaved;
        for (auto* Work : works_)
            if (auto* Query = qobject_cast<QueryTab*>(Work); Query && !Query->File().isEmpty() && Query->Modified()) Unsaved.append(Query->Title());
        if (Unsaved.isEmpty()) return true;
        const auto Answer = QMessageBox::question(this, tr("쿼리 저장"),
            tr("저장하지 않은 SQL 파일이 있습니다.\n\n%1\n\n저장할까요?").arg(Unsaved.join(QLatin1Char('\n'))),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (Answer == QMessageBox::Cancel) return false;
        if (Answer == QMessageBox::Save)
            for (auto* Work : works_)
                if (auto* Query = qobject_cast<QueryTab*>(Work); Query && !Query->File().isEmpty() && Query->Modified() && !saveQuery(Query, false)) return false;
        return true;
    }

    void DocumentView::Cancel()
    {
        stopCounting();
        for (auto* Work : works_) Work->Cancel();
        if (previewModel_) previewModel_->Cancel();
    }

    WorkTab* DocumentView::CurrentTab() const
    {
        if (!tabs_) return nullptr;
        const int Index = tabs_->currentIndex();
        return Index >= 0 && Index < works_.size() ? works_[Index] : nullptr;
    }

    int DocumentView::TabCount() const { return int(works_.size()); }
    TableTab* DocumentView::currentTable() const { return qobject_cast<TableTab*>(CurrentTab()); }
    QueryTab* DocumentView::currentQuery() const { return qobject_cast<QueryTab*>(CurrentTab()); }

    TableModel* DocumentView::CurrentModel() const
    {
        if (previewModel_) return previewModel_;
        auto* Tab = currentTable();
        return Tab ? Tab->Model() : nullptr;
    }

    QStringList DocumentView::OpenTables() const
    {
        QStringList Names;
        for (const auto* Work : works_) if (!Work->IsQuery()) Names.append(Work->Title());
        return Names;
    }

    void DocumentView::configureGrid(GridView* Grid)
    {
        if (!Grid) return;
        if (hasFonts_) {
            const auto GridFont = fonts_.customGrid_ ? fonts_.grid_ : font();
            Grid->setFont(GridFont);
            Grid->Delegate()->SetFonts(GridFont, fonts_.mono_, fonts_.monoValues_);
        }
        Grid->SetBrokered(brokered_);
        Grid->Delegate()->SetDisplay(queryPreferences_.cells_);
        connect(Grid, &GridView::CopyRequested, this, &DocumentView::CopyRequested, Qt::UniqueConnection);
    }

    void DocumentView::addTab(WorkTab* Tab)
    {
        connect(Tab, &WorkTab::Changed, this, [this, Tab] { refreshTabTitle(Tab); emit StateChanged(); if (tabsTimer_ && !restoring_) tabsTimer_->start(); });
        connect(Tab, &WorkTab::GridAdded, this, &DocumentView::configureGrid);
        if (hasFonts_) Tab->ApplyFonts(fonts_.customGrid_ ? fonts_.grid_ : font(), fonts_.mono_, fonts_.monoValues_);
        configureGrid(Tab->CurrentGrid());
        works_.append(Tab);
        topStack_->addWidget(Tab->Top());
        resultStack_->addWidget(Tab->Result());
        const auto& Colors = fm::style::themeColorsFor(this);
        const int Index = tabs_->addTab(Glyph(Tab->GlyphName(), Tab->IsQuery() ? Colors[T::AccentFg] : Colors[T::Ok], 13), Tab->Title());
        tabs_->setTabToolTip(Index, Tab->ToolTip());
        tabs_->setCurrentIndex(Index);
        if (auto* Target = Tab->FocusTarget()) Target->setFocus(Qt::OtherFocusReason);
    }

    void DocumentView::refreshTabTitle(WorkTab* Tab)
    {
        const int Index = int(works_.indexOf(Tab));
        if (Index < 0 || !tabs_) return;
        // 저장하지 않은 .sql 쿼리는 이름 뒤에 점(시안의 파란 점).
        tabs_->setTabText(Index, Tab->Modified() ? Tab->Title() + QStringLiteral("  ●") : Tab->Title());
        tabs_->setTabToolTip(Index, Tab->ToolTip());
    }

    void DocumentView::OpenObject(const QString& Database, const QString& Name)
    {
        if (mode_ == Mode::QuickPreview) { showPreviewObject(Database, Name); return; }
        for (int Index = 0; Index < works_.size(); ++Index) {
            auto* Table = qobject_cast<TableTab*>(works_[Index]);
            if (!Table) continue;
            const auto& Object = Table->Object();
            if (Object.database_.compare(Database, Qt::CaseInsensitive) == 0 && Object.name_.compare(Name, Qt::CaseInsensitive) == 0) {
                tabs_->setCurrentIndex(Index);
                Table->FocusGrid();
                return;
            }
        }
        const auto* Object = schema_.Find(Database, Name);
        if (!Object || !db_) return;
        auto* Tab = new TableTab(*db_, [this](QString* Error) { return openWorker(Error); }, *Object, this);
        Tab->SetReadOnly(IsReadOnly());
        Tab->SetEditing(queryPreferences_.confirmDelete_);
        connect(Tab, &TableTab::CommitRequested, this, [this] { Commit(); });
        connect(Tab, &TableTab::ShowSqlRequested, this, [this](const QStringList& Statements) { NewQuery(Statements.join(QLatin1Char('\n')) + QLatin1Char('\n')); });
        connect(Tab, &TableTab::OpenQueryRequested, this, [this](const QString& Sql) { NewQuery(Sql + QLatin1Char('\n')); });
        connect(Tab, &TableTab::ExportRequested, this, [this, Tab] { ShowExport(ExportRequestFor(Tab->Object().database_, Tab->Object().name_)); });
        addTab(Tab);
        Tab->Reload();
        Tab->FocusGrid();
    }

    QueryTab* DocumentView::NewQuery(const QString& Text, const QString& File)
    {
        if (mode_ == Mode::QuickPreview || !db_) return nullptr;
        // 같은 .sql 파일이 이미 쿼리 탭에 있으면 그 탭으로 간다(한 파일을 두 편집기가 쓰지 않게, 설계 4.6).
        if (!File.isEmpty())
            for (int Index = 0; Index < works_.size(); ++Index)
                if (auto* Query = qobject_cast<QueryTab*>(works_[Index]); Query && QFileInfo(Query->File()) == QFileInfo(File)) { tabs_->setCurrentIndex(Index); return Query; }
        auto* Tab = new QueryTab([this](QString* Error) { return openWorker(Error); }, tr("쿼리 %1").arg(++queryCounter_), this);
        connect(Tab, &QueryTab::DataWritten, this, &DocumentView::dataWritten);
        connect(Tab, &QueryTab::ExportRequested, this, [this] { ExportRequest Request; if (CurrentResultExport(&Request)) ShowExport(Request); });
        connect(Tab, &QueryTab::CopyRequested, this, &DocumentView::CopyRequested);
        Tab->SetBrokered(brokered_);
        Tab->SetCompletionProvider([this] { return &completion_; });
        connect(Tab, &QueryTab::Executed, this, &DocumentView::recordHistory);
        connect(Tab, &QueryTab::TransactionChanged, this, [this](bool Open) {
            // Ctrl+S: 쿼리 탭의 트랜잭션을 먼저 커밋하고, 쓰기 잠금이 풀리면 표의 보류 편집을 쓴다.
            if (!Open && tablesAfterCommit_) { tablesAfterCommit_ = false; commitTables(); }
            emit StateChanged();
        });
        Tab->SetPreferences(queryPreferences_, true);
        Tab->SetManualCommit(queryPreferences_.manualCommit_);
        Tab->SetWritable(!IsReadOnly());
        if (!File.isEmpty()) Tab->SetFile(File);
        else if (!Text.isEmpty()) Tab->SetText(Text);
        addTab(Tab);
        refreshSchemaWords();
        return Tab;
    }

    void DocumentView::dataWritten()
    {
        // 쓰기 문이 끝났다: 스키마가 바뀌었으면 다시 읽고, 열린 테이블 탭과 행 수를 새로 읽는다.
        if (!db_) return;
        if (SchemaVersion(*db_) != schema_.schemaVersion_) {
            readSchema();
            for (int Index = int(works_.size()) - 1; Index >= 0; --Index)
                if (auto* Table = qobject_cast<TableTab*>(works_[Index])) {
                    // 지웠거나 이름을 바꾼 개체의 탭은 닫는다.
                    if (const auto* Object = schema_.Find(Table->Object().database_, Table->Object().name_)) Table->Rebind(*Object);
                    else closeTab(Index, false);
                }
        } else {
            for (auto* Work : works_) if (auto* Table = qobject_cast<TableTab*>(Work)) Table->Reload();
        }
        startCounting();
    }

    void DocumentView::activateTab(int Index)
    {
        auto* Tab = Index >= 0 && Index < works_.size() ? works_[Index] : nullptr;
        topStack_->setCurrentWidget(Tab ? Tab->Top() : emptyTop_);
        resultStack_->setCurrentWidget(Tab ? Tab->Result() : emptyResult_);
        // 테이블 탭의 위 칸은 막대만큼만, 쿼리 탭은 편집기와 결과를 나눈다.
        const int Total = rightSplit_->height();
        int Top = qMax(120, Total / 3);
        if (Tab && !Tab->IsQuery()) Top = Tab->Top()->sizeHint().height() + tabs_->sizeHint().height() + 2;
        else if (Tab) Top = qMax(200, Total * 9 / 20);
        rightSplit_->setSizes({Top, qMax(100, Total - Top)});
        emit StateChanged();
    }

    bool DocumentView::closeTab(int Index, bool Ask)
    {
        if (Index < 0 || Index >= works_.size()) return false;
        auto* Tab = works_[Index];
        if (auto* Query = qobject_cast<QueryTab*>(Tab); Ask && Query && !Query->File().isEmpty() && Query->Modified()) {
            const auto Answer = QMessageBox::question(this, tr("쿼리 저장"), tr("%1 의 바뀐 내용을 저장할까요?").arg(Query->Title()),
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
            if (Answer == QMessageBox::Cancel || (Answer == QMessageBox::Save && !saveQuery(Query, false))) return false;
        }
        works_.removeAt(Index);
        tabs_->removeTab(Index);
        delete Tab;
        if (tabsTimer_ && !restoring_) tabsTimer_->start();
        activateTab(tabs_->currentIndex());
        return true;
    }

    bool DocumentView::saveQuery(QueryTab* Tab, bool As)
    {
        if (!Tab) return false;
        if (As || Tab->File().isEmpty()) {
            const auto Path = QFileDialog::getSaveFileName(this, tr("쿼리 저장"), QFileInfo(path_).absoluteDir().filePath(Tab->Title() + QStringLiteral(".sql")),
                tr("SQL (*.sql);;모든 파일 (*)"));
            if (Path.isEmpty()) return false;
            const auto Text = Tab->Text();
            Tab->SetFile(Path);
            Tab->SetText(Text);
        }
        QString Error;
        if (!Tab->Save(&Error)) {
            QMessageBox::warning(this, tr("쿼리 저장"), tr("저장하지 못했습니다: %1").arg(Error));
            return false;
        }
        refreshTabTitle(Tab);
        return true;
    }

    void DocumentView::openSqlFile()
    {
        const auto Path = QFileDialog::getOpenFileName(this, tr("SQL 파일 열기"), QFileInfo(path_).absolutePath(), tr("SQL (*.sql);;모든 파일 (*)"));
        if (!Path.isEmpty()) NewQuery({}, Path);
    }

    void DocumentView::selectFromSchema()
    {
        // 스키마 트리의 테이블 · 뷰로 새 쿼리 탭에 SELECT(설계 단축키 표 ‘새 쿼리 탭에 SELECT’).
        auto* Item = schemaPanel_ ? schemaPanel_->Tree()->currentItem() : nullptr;
        while (Item && Item->parent() && Item->parent()->parent() && Item->parent()->parent()->parent()) Item = Item->parent();
        if (!Item) return;
        const auto Name = Item->data(0, Qt::UserRole + 5).toString();
        const auto Database = Item->data(0, Qt::UserRole + 4).toString();
        const auto* Object = schema_.Find(Database, Name);
        if (!Object) return;
        QStringList Columns;
        for (const auto& Column : Object->columns_) if (!Column.hidden_) Columns.append(IdentifierText(Column.name_));
        const auto From = Database.compare(QLatin1String("main"), Qt::CaseInsensitive) == 0 ? IdentifierText(Name)
                                                                                          : IdentifierText(Database) + QLatin1Char('.') + IdentifierText(Name);
        auto* Query = NewQuery(QStringLiteral("SELECT %1\nFROM %2\nLIMIT 1000;\n").arg(Columns.join(QStringLiteral(", ")), From));
        if (Query) Query->RunAll();
    }

    void DocumentView::showPreviewObject(const QString& Database, const QString& Name)
    {
        const auto* Object = schema_.Find(Database, Name);
        if (!Object || !db_) return;
        previewDatabase_ = Database;
        previewName_ = Name;
        if (!previewModel_) {
            previewModel_ = new TableModel(*db_, [this](QString* Error) { return openWorker(Error); });
            previewGrid_->setModel(previewModel_);
            connect(previewModel_, &TableModel::Loaded, this, [this](qint64 Rows) {
                previewGrid_->FitColumns(260);
                const auto* Current = schema_.Find(previewDatabase_, previewName_);
                previewTitle_->setText(tr("%1 · %2행 · 열 %3").arg(previewName_, QLocale().toString(Rows)).arg(Current ? Current->columns_.size() : 0));
                emit StateChanged();
            });
            connect(previewModel_, &TableModel::Failed, this, [this](const QString& Error) { previewTitle_->setText(Error); emit StateChanged(); });
        }
        previewGrid_->ClearSort();
        previewModel_->SetSource(*Object);
        previewTitle_->setText(tr("%1 · 읽는 중…").arg(Name));
        previewModel_->Reload();
        refreshPreviewStructure();
    }

    void DocumentView::refreshPreviewStructure()
    {
        previewColumns_->clear();
        const auto* Object = schema_.Find(previewDatabase_, previewName_);
        if (!Object) return;
        const auto& Colors = fm::style::themeColorsFor(this);
        for (const auto& Column : Object->columns_) {
            QStringList Parts{Column.name_};
            if (!Column.type_.isEmpty()) Parts.append(Column.type_);
            if (Column.primaryKey_ > 0) Parts.append(tr("기본 키"));
            if (Column.notNull_) Parts.append(QStringLiteral("NOT NULL"));
            if (!Column.foreignTable_.isEmpty()) Parts.append(tr("→ %1(%2)").arg(Column.foreignTable_, Column.foreignColumn_));
            if (!Column.defaultValue_.isEmpty()) Parts.append(tr("기본값 %1").arg(Column.defaultValue_));
            const auto GlyphName = Column.primaryKey_ > 0 ? QStringLiteral("pk") : !Column.foreignTable_.isEmpty() ? QStringLiteral("fk") : QStringLiteral("col");
            const auto Tone = Column.primaryKey_ > 0 ? T::Warn : !Column.foreignTable_.isEmpty() ? T::AccentFg : T::Fg3;
            new QListWidgetItem(Glyph(GlyphName, Colors[Tone], 14), Parts.join(QStringLiteral("  ·  ")), previewColumns_);
        }
        previewDdl_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        previewDdl_->setText(Object->sql_);
    }

    void DocumentView::showDdl(const QString& Database, const QString& Name)
    {
        const auto* Object = schema_.Find(Database, Name);
        if (!Object) return;
        QDialog Dialog(this);
        Dialog.setWindowTitle(tr("DDL — %1").arg(Name));
        auto* Layout = new QVBoxLayout(&Dialog);
        auto* Text = new QPlainTextEdit(&Dialog);
        Text->setReadOnly(true);
        Text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        Text->setPlainText(Object->sql_.isEmpty() ? tr("-- SQLite 가 만든 개체라 DDL 이 없습니다.") : Object->sql_ + QLatin1Char(';'));
        Layout->addWidget(Text);
        auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Close, &Dialog);
        auto* Copy = Buttons->addButton(tr("복사"), QDialogButtonBox::ActionRole);
        connect(Copy, &QPushButton::clicked, &Dialog, [Text] { QApplication::clipboard()->setText(Text->toPlainText()); });
        auto* ToQuery = Buttons->addButton(tr("쿼리 탭으로 열기"), QDialogButtonBox::ActionRole);
        connect(ToQuery, &QPushButton::clicked, &Dialog, [this, &Dialog, Text] { NewQuery(Text->toPlainText() + QLatin1Char('\n')); Dialog.accept(); });
        connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
        Layout->addWidget(Buttons);
        Dialog.resize(640, 360);
        Dialog.exec();
    }

    void DocumentView::attachDatabase()
    {
        if (!db_ || mode_ == Mode::QuickPreview) return;
        const auto File = QFileDialog::getOpenFileName(this, tr("데이터베이스 첨부 (ATTACH)"), QFileInfo(path_).absolutePath(),
            tr("SQLite 데이터베이스 (*.db *.sqlite *.sqlite3 *.db3 *.s3db *.sl3);;모든 파일 (*)"));
        if (File.isEmpty()) return;
        // 이름: 파일 이름에서 영숫자만, 겹치면 숫자를 붙인다.
        auto Base = QFileInfo(File).completeBaseName();
        Base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]")), QStringLiteral("_"));
        if (Base.isEmpty() || Base[0].isDigit()) Base.prepend(QLatin1Char('a'));
        auto Name = Base;
        for (int Suffix = 2; schema_.Database(Name); ++Suffix) Name = Base + QString::number(Suffix);
        Statement Attach(*db_, QStringLiteral("ATTACH DATABASE ?1 AS %1").arg(QuoteIdentifier(Name)));
        Attach.Bind(1, QDir::toNativeSeparators(File));
        if (!Attach.Valid() || Attach.Step() != SQLITE_DONE) {
            QMessageBox::warning(this, tr("데이터베이스 첨부"), tr("첨부하지 못했습니다: %1").arg(Attach.Error()));
            return;
        }
        readSchema();
        startCounting();
    }

    void DocumentView::detachDatabase(const QString& Name)
    {
        if (!db_) return;
        QString Error;
        for (int Index = int(works_.size()) - 1; Index >= 0; --Index)
            if (auto* Table = qobject_cast<TableTab*>(works_[Index]); Table && Table->Object().database_.compare(Name, Qt::CaseInsensitive) == 0) closeTab(Index, false);
        if (!db_->Exec(QStringLiteral("DETACH DATABASE %1").arg(QuoteIdentifier(Name)), &Error)) {
            QMessageBox::warning(this, tr("데이터베이스 분리"), tr("분리하지 못했습니다: %1").arg(Error));
            return;
        }
        readSchema();
    }

    bool DocumentView::CanRunCommand(const QString& Id) const
    {
        if (mode_ == Mode::QuickPreview || !ready_ || locked_ || !db_) return false;
        auto* Query = currentQuery();
        if (Id == QLatin1String("sqlite.run") || Id == QLatin1String("sqlite.runCurrent") || Id == QLatin1String("sqlite.comment"))
            return Query && !Query->Running();
        if (Id == QLatin1String("sqlite.stop")) return IsBusy();
        if (Id == QLatin1String("sqlite.complete") || Id == QLatin1String("sqlite.paramHelp") || Id == QLatin1String("sqlite.quickFix")) return Query != nullptr;
        if (Id == QLatin1String("sqlite.format")) return Query && !Query->Running();
        if (Id == QLatin1String("sqlite.explain") || Id == QLatin1String("sqlite.explainBytecode")) return Query && !Query->Running();
        if (Id == QLatin1String("sqlite.saveQuery")) return Query != nullptr;
        if (Id == QLatin1String("sqlite.closeTab") || Id == QLatin1String("sqlite.nextTab") || Id == QLatin1String("sqlite.prevTab")) return !works_.isEmpty();
        if (Id == QLatin1String("sqlite.copyInsert")) {
            auto* Tab = CurrentTab();
            return Tab && Tab->CurrentGrid() && Tab->CurrentGrid()->currentIndex().isValid();
        }
        if (Id == QLatin1String("sqlite.commit") || Id == QLatin1String("sqlite.rollback")) return HasPending();
        if (Id == QLatin1String("sqlite.toggleCommitMode")) return Query != nullptr;
        if (Id == QLatin1String("sqlite.editTable") || Id == QLatin1String("sqlite.newTable") || Id == QLatin1String("sqlite.renameObject")
            || Id == QLatin1String("sqlite.dropObject")) return !IsReadOnly();
        if (Id == QLatin1String("sqlite.encrypt")) return !IsReadOnly() && Runtime::See() == SeeState::Available;
        if (Id == QLatin1String("sqlite.import") || Id == QLatin1String("sqlite.emptyTable")) return !IsReadOnly();
        // 있는지만 본다(요청을 채우면 결과 행을 복사한다).
        if (Id == QLatin1String("sqlite.export")) return currentTable() != nullptr || CurrentResultExport(nullptr);
        // 표 편집(결과 · 표 범위): 테이블 탭에서만. 값 패널은 읽기 전용 표에서도 연다.
        auto* Table = currentTable();
        if (Id == QLatin1String("sqlite.editValue") || Id == QLatin1String("sqlite.valuePanel")) return Table != nullptr;
        if (Id == QLatin1String("sqlite.addRow") || Id == QLatin1String("sqlite.duplicateRow") || Id == QLatin1String("sqlite.deleteRows")
            || Id == QLatin1String("sqlite.setNull")) return Table && Table->Model()->Editable();
        return Id.startsWith(QLatin1String("sqlite."));
    }

    int DocumentView::PendingTotal() const
    {
        int Total = 0;
        for (const auto* Work : works_) Total += Work->PendingCount();
        return Total;
    }

    bool DocumentView::Commit()
    {
        if (!HasPending()) return false;
        // 쿼리 탭의 열린 트랜잭션(수동 커밋)을 먼저 커밋한다. 그 쓰기 잠금이 풀린 뒤(TransactionChanged) 표의 보류 편집을 쓴다.
        bool Waiting = false;
        for (auto* Work : works_)
            if (auto* Query = qobject_cast<QueryTab*>(Work); Query && Query->InTransaction()) Waiting |= Query->EndTransaction(true);
        if (Waiting) { tablesAfterCommit_ = true; return true; }
        return commitTables();
    }

    bool DocumentView::commitTables()
    {
        QList<TableTab*> Tables;
        for (auto* Work : works_) if (auto* Table = qobject_cast<TableTab*>(Work); Table && Table->Model()->HasPending()) Tables.append(Table);
        if (Tables.isEmpty()) { emit StateChanged(); return true; }
        if (queryPreferences_.showSqlBeforeCommit_) {
            // 설정 ‘Ctrl+S 로 저장하기 전에 변경 SQL 보기’.
            QStringList All;
            for (auto* Table : Tables) All += Table->Model()->PendingStatements();
            QDialog Dialog(this);
            Dialog.setWindowTitle(tr("변경 SQL — 커밋"));
            auto* Layout = new QVBoxLayout(&Dialog);
            auto* Text = new QPlainTextEdit(All.join(QLatin1Char('\n')), &Dialog);
            Text->setObjectName(QStringLiteral("sqliteCommitPreview"));
            Text->setReadOnly(true);
            Text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            Layout->addWidget(Text);
            auto* Buttons = new QDialogButtonBox(&Dialog);
            Buttons->addButton(tr("커밋"), QDialogButtonBox::AcceptRole);
            Buttons->addButton(tr("취소"), QDialogButtonBox::RejectRole);
            connect(Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept);
            connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
            Layout->addWidget(Buttons);
            Dialog.resize(720, 360);
            if (Dialog.exec() != QDialog::Accepted) return false;
        }
        QStringList Errors;
        bool Wrote = false;
        for (auto* Table : Tables) {
            QString Error;
            if (Table->Commit(&Error)) Wrote = true;
            else Errors.append(QStringLiteral("%1 — %2").arg(Table->Title(), Error));
        }
        if (Wrote) dataWritten();
        if (!Errors.isEmpty()) QMessageBox::warning(this, tr("커밋"), tr("쓰지 못한 변경이 있습니다. 보류 편집은 그대로 남아 있습니다.\n\n%1").arg(Errors.join(QLatin1Char('\n'))));
        emit StateChanged();
        return Errors.isEmpty();
    }

    bool DocumentView::commitAll(bool Blocking)
    {
        // 문서를 닫을 때: 끝날 때까지 기다린다.
        for (auto* Work : works_)
            if (auto* Query = qobject_cast<QueryTab*>(Work); Query && Query->InTransaction()) {
                QString Error;
                if (!Query->EndTransaction(true, Blocking, &Error)) { QMessageBox::warning(this, tr("커밋"), Error); return false; }
            }
        tablesAfterCommit_ = false;
        return commitTables();
    }

    void DocumentView::rollbackAll(bool Blocking)
    {
        for (auto* Work : works_) {
            if (auto* Query = qobject_cast<QueryTab*>(Work); Query && Query->InTransaction()) Query->EndTransaction(false, Blocking);
            if (auto* Table = qobject_cast<TableTab*>(Work)) Table->RevertAll();
        }
        tablesAfterCommit_ = false;
        emit StateChanged();
    }

    void DocumentView::refreshCommitMode()
    {
        auto* Query = currentQuery();
        const bool Manual = Query ? Query->ManualCommit() : queryPreferences_.manualCommit_;
        if (commitMode_) commitMode_->SetState(Manual, Query != nullptr);
        const bool Pending = HasPending();
        for (auto* Action : menuActions_) {
            const auto Name = Action->objectName();
            if (Name == QLatin1String("sqliteAction.commitManual") || Name == QLatin1String("sqliteAction.commitAuto")) {
                Action->setEnabled(Query != nullptr);
                Action->setChecked(Manual == (Name == QLatin1String("sqliteAction.commitManual")));
            }
            if (Name == QLatin1String("sqliteAction.sqlite.commit") || Name == QLatin1String("sqliteAction.sqlite.rollback")) Action->setEnabled(Pending);
        }
    }

    DocumentView::SchemaTarget DocumentView::schemaTarget() const
    {
        SchemaTarget Target;
        auto* Item = schemaPanel_ ? schemaPanel_->Tree()->currentItem() : nullptr;
        if (!Item) {
            // 스키마에 고른 것이 없으면 지금 테이블 탭.
            if (auto* Table = currentTable()) { Target.kind_ = Table->Object().type_ == ObjectType::View ? QStringLiteral("view") : QStringLiteral("table"); Target.database_ = Table->Object().database_; Target.name_ = Table->Object().name_; }
            return Target;
        }
        Target.kind_ = Item->data(0, Qt::UserRole + 3).toString();
        Target.database_ = Item->data(0, Qt::UserRole + 4).toString();
        Target.name_ = Item->data(0, Qt::UserRole + 5).toString();
        if ((Target.kind_ == QLatin1String("col") || Target.kind_ == QLatin1String("pk") || Target.kind_ == QLatin1String("fk")) && Item->parent())
            Target.table_ = Item->parent()->data(0, Qt::UserRole + 5).toString();
        return Target;
    }

    bool DocumentView::structureBlocked()
    {
        // 구조를 바꾸기 전에 커밋하지 않은 편집 · 열린 트랜잭션을 정리한다(재작성이 쓰기 잠금을 얻어야 한다).
        if (IsReadOnly()) { QMessageBox::information(this, tr("구조 바꾸기"), tr("읽기 전용으로 연 데이터베이스입니다.")); return true; }
        if (!HasPending()) return false;
        QMessageBox::information(this, tr("구조 바꾸기"), tr("커밋하지 않은 변경이 %1건 있습니다. 먼저 커밋(Ctrl+S)하거나 되돌린 뒤 구조를 바꾸세요.").arg(PendingTotal()));
        return true;
    }

    bool DocumentView::editTable(bool Create)
    {
        const auto Target = schemaTarget();
        const auto Database = Target.database_.isEmpty() ? QStringLiteral("main") : Target.database_;
        const auto* Object = Create ? nullptr : schema_.Find(Database, Target.table_.isEmpty() ? Target.name_ : Target.table_);
        if (!Create && (!Object || Object->type_ != ObjectType::Table)) return false;
        if (structureBlocked()) return false;
        stopCounting();
        // 재작성 전 사본(설정 › 결과 · 편집): <파일 이름>.before-<테이블>-<시각>.db
        QString Backup;
        if (Object && queryPreferences_.backupBeforeRebuild_) {
            const QFileInfo Info(path_);
            Backup = Info.absoluteDir().filePath(QStringLiteral("%1.before-%2-%3.db").arg(Info.completeBaseName(), Object->name_, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
        }
        TableDesigner Dialog(*db_, schema_, Object, Database, Backup, this);
        connect(&Dialog, &TableDesigner::CopyToQueryRequested, this, [this](const QString& Sql) { NewQuery(Sql + QLatin1Char('\n')); });
        const bool Applied = Dialog.exec() == QDialog::Accepted;
        if (Applied) {
            const auto Name = Dialog.Design().name_;
            dataWritten();
            if (Create) OpenObject(Database, Name);
        } else startCounting();
        return Applied;
    }

    bool DocumentView::renameObject()
    {
        const auto Target = schemaTarget();
        const bool Column = !Target.table_.isEmpty();
        if (!Column && Target.kind_ != QLatin1String("table")) {
            if (!Target.kind_.isEmpty() && Target.kind_ != QLatin1String("db") && Target.kind_ != QLatin1String("grp"))
                QMessageBox::information(this, tr("이름 바꾸기"), tr("SQLite 는 뷰 · 색인 · 트리거의 이름을 바꾸지 못합니다. DDL 을 고쳐 다시 만드세요."));
            return false;
        }
        if (structureBlocked()) return false;
        const auto Table = Column ? Target.table_ : Target.name_;
        bool Ok = false;
        const auto NewName = QInputDialog::getText(this, Column ? tr("열 이름 바꾸기") : tr("이름 바꾸기"),
            Column ? tr("%1.%2 의 새 이름 — ALTER TABLE … RENAME COLUMN").arg(Table, Target.name_) : tr("%1 의 새 이름 — ALTER TABLE … RENAME TO").arg(Table),
            QLineEdit::Normal, Target.name_, &Ok).trimmed();
        if (!Ok || NewName.isEmpty() || NewName == Target.name_) return false;
        const auto Qualified = Target.database_.compare(QLatin1String("main"), Qt::CaseInsensitive) == 0 ? QuoteIdentifier(Table) : hc::sqlite::Qualified(Target.database_, Table);
        const auto Sql = Column ? QStringLiteral("ALTER TABLE %1 RENAME COLUMN %2 TO %3").arg(Qualified, QuoteIdentifier(Target.name_), QuoteIdentifier(NewName))
                                : QStringLiteral("ALTER TABLE %1 RENAME TO %2").arg(Qualified, QuoteIdentifier(NewName));
        stopCounting();
        QString Error;
        if (!db_->Exec(Sql, &Error)) { QMessageBox::warning(this, tr("이름 바꾸기"), Error); startCounting(); return false; }
        dataWritten();
        return true;
    }

    QString DocumentView::ScriptFor(const QString& Database, const QString& Name, const QString& Kind) const
    {
        const auto* Object = schema_.Find(Database, Name);
        if (!Object) return {};
        if (Kind == QLatin1String("CREATE")) return Object->sql_.trimmed() + QStringLiteral(";\n");
        const auto Target = Database.compare(QLatin1String("main"), Qt::CaseInsensitive) == 0 ? IdentifierText(Name) : IdentifierText(Database) + QLatin1Char('.') + IdentifierText(Name);
        QStringList Columns, Keys;
        for (const auto& Column : Object->columns_) {
            if (Column.hidden_ || (Kind != QLatin1String("SELECT") && Column.generated_)) continue;
            Columns.append(IdentifierText(Column.name_));
            if (Column.primaryKey_ > 0) Keys.append(IdentifierText(Column.name_));
        }
        // 기본 키가 없으면 rowid 로 고른다(WITHOUT ROWID 는 기본 키가 늘 있다).
        if (Keys.isEmpty()) Keys.append(QStringLiteral("rowid"));
        QStringList Where;
        for (const auto& Key : Keys) Where.append(QStringLiteral("%1 = NULL").arg(Key));
        if (Kind == QLatin1String("SELECT")) return QStringLiteral("SELECT %1\nFROM %2;\n").arg(Columns.join(QStringLiteral(", ")), Target);
        if (Kind == QLatin1String("INSERT")) {
            QStringList Values;
            for (int Index = 0; Index < Columns.size(); ++Index) Values.append(QStringLiteral("NULL"));
            return QStringLiteral("INSERT INTO %1 (%2)\nVALUES (%3);\n").arg(Target, Columns.join(QStringLiteral(", ")), Values.join(QStringLiteral(", ")));
        }
        if (Kind == QLatin1String("UPDATE")) {
            QStringList Sets;
            for (const auto& Column : Columns) if (!Keys.contains(Column)) Sets.append(QStringLiteral("%1 = NULL").arg(Column));
            return QStringLiteral("UPDATE %1\nSET %2\nWHERE %3;\n").arg(Target, Sets.join(QStringLiteral(",\n    ")), Where.join(QStringLiteral(" AND ")));
        }
        if (Kind == QLatin1String("DELETE")) return QStringLiteral("DELETE FROM %1\nWHERE %2;\n").arg(Target, Where.join(QStringLiteral(" AND ")));
        return {};
    }

    bool DocumentView::emptyTable()
    {
        const auto Target = schemaTarget();
        if (Target.kind_ != QLatin1String("table") || structureBlocked()) return false;
        const auto Database = Target.database_.isEmpty() ? QStringLiteral("main") : Target.database_;
        const auto Sql = QStringLiteral("DELETE FROM %1").arg(Qualified(Database, Target.name_));
        const auto Count = CountRows(*db_, Database, Target.name_);
        if (QMessageBox::warning(this, tr("테이블 비우기"), tr("%1 의 행 %2개를 모두 지웁니다. 되돌릴 수 없습니다.\n\n%3;").arg(Target.name_, QLocale().toString(Count), Sql),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return false;
        stopCounting();
        QString Error;
        if (!db_->Exec(Sql, &Error)) { QMessageBox::warning(this, tr("테이블 비우기"), Error); startCounting(); return false; }
        dataWritten();
        return true;
    }

    bool DocumentView::dropObject()
    {
        const auto Target = schemaTarget();
        const bool Column = !Target.table_.isEmpty();
        static const QHash<QString, QString> Kinds{{QStringLiteral("table"), QStringLiteral("TABLE")}, {QStringLiteral("view"), QStringLiteral("VIEW")},
                                                   {QStringLiteral("index"), QStringLiteral("INDEX")}, {QStringLiteral("trigger"), QStringLiteral("TRIGGER")}};
        if (!Column && !Kinds.contains(Target.kind_)) return false;
        if (structureBlocked()) return false;
        const auto Name = [&](const QString& Each) {
            return Target.database_.compare(QLatin1String("main"), Qt::CaseInsensitive) == 0 ? QuoteIdentifier(Each) : hc::sqlite::Qualified(Target.database_, Each);
        };
        const auto Sql = Column ? QStringLiteral("ALTER TABLE %1 DROP COLUMN %2").arg(Name(Target.table_), QuoteIdentifier(Target.name_))
                                : QStringLiteral("DROP %1 %2").arg(Kinds.value(Target.kind_), Name(Target.name_));
        if (QMessageBox::warning(this, tr("삭제"), tr("%1 을(를) 지웁니다. 되돌릴 수 없습니다.\n\n%2;").arg(Column ? Target.table_ + QLatin1Char('.') + Target.name_ : Target.name_, Sql),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return false;
        stopCounting();
        QString Error;
        if (!db_->Exec(Sql, &Error)) { QMessageBox::warning(this, tr("삭제"), Error); startCounting(); return false; }
        dataWritten();
        return true;
    }

    ExportRequest DocumentView::ExportRequestFor(const QString& Database, const QString& Name) const
    {
        ExportRequest Request;
        Request.sql_ = QStringLiteral("SELECT * FROM %1").arg(Qualified(Database, Name));
        Request.source_ = Name;
        Request.table_ = Name;
        if (const auto* Object = schema_.Find(Database, Name); Object && Object->type_ == ObjectType::Table) Request.createSql_ = Object->sql_;
        Request.path_ = QDir(QFileInfo(path_).absolutePath()).filePath(Name + QStringLiteral(".csv"));
        return Request;
    }

    bool DocumentView::CurrentResultExport(ExportRequest* Request) const
    {
        auto* Query = currentQuery();
        auto* Model = Query ? Query->CurrentResultModel() : nullptr;
        if (!Model || !db_) return false;
        if (!Request) return true;
        *Request = {};
        Request->source_ = tr("쿼리 결과");
        // 읽기 문이면 작업 연결에서 다시 읽어 모든 행을 쓴다. 쓰기 문(RETURNING)의 결과는 이미 읽은 행만.
        const auto Sql = Query->CurrentResultSql();
        Statement Probe(*db_, Sql);
        if (!Sql.isEmpty() && Probe.Valid() && Probe.ReadOnly()) Request->sql_ = Sql;
        else {
            for (const auto& Column : Model->Columns()) Request->columns_.append(Column.name_);
            Request->rows_ = Model->Rows();
        }
        QString Table;
        for (const auto& Column : Model->Columns()) if (!Column.table_.isEmpty()) { Table = Column.table_; break; }
        Request->table_ = Table.isEmpty() ? QStringLiteral("result") : Table;
        Request->path_ = QDir(QFileInfo(path_).absolutePath()).filePath(Request->table_ + QStringLiteral(".csv"));
        return true;
    }

    bool DocumentView::exportCurrent()
    {
        if (auto* Table = currentTable()) return ShowExport(ExportRequestFor(Table->Object().database_, Table->Object().name_));
        ExportRequest Request;
        return CurrentResultExport(&Request) && ShowExport(Request);
    }

    TransferResult DocumentView::runTransfer(const QString& Title, qint64 Total, TransferWork Work)
    {
        QString Error;
        std::shared_ptr<Connection> Db(openWorker(&Error).release());
        if (!Db) return {false, false, 0, Error.isEmpty() ? tr("데이터베이스를 열지 못했습니다.") : Error};
        auto Cancel = std::make_shared<std::atomic_bool>(false);
        auto Progress = std::make_shared<std::atomic<qint64>>(0);
        QFutureWatcher<TransferResult> Watcher;
        QEventLoop Loop;
        connect(&Watcher, &QFutureWatcher<TransferResult>::finished, &Loop, &QEventLoop::quit);
        Watcher.setFuture(QtConcurrent::run([Db, Cancel, Progress, Work = std::move(Work)] { return Work(*Db, *Cancel, *Progress); }));
        // 0.4초 넘게 걸리면 진행 창(취소 가능).
        QProgressDialog Dialog(Title, tr("취소"), 0, Total > 0 ? int(qMin<qint64>(Total, INT_MAX)) : 0, this);
        Dialog.setObjectName(QStringLiteral("sqliteTransferProgress"));
        Dialog.setWindowModality(Qt::WindowModal);
        Dialog.setMinimumDuration(400);
        Dialog.setAutoClose(false);
        Dialog.setAutoReset(false);
        connect(&Dialog, &QProgressDialog::canceled, this, [Cancel] { Cancel->store(true); });
        QTimer Tick;
        connect(&Tick, &QTimer::timeout, this, [&] {
            const auto Done = Progress->load();
            if (Total > 0) Dialog.setValue(int(qMin<qint64>(Done, Total)));
            Dialog.setLabelText(QStringLiteral("%1\n%2").arg(Title, tr("%1행").arg(QLocale().toString(Done))));
        });
        Tick.start(100);
        if (!Watcher.isFinished()) Loop.exec();
        Tick.stop();
        return Watcher.result();
    }

    TransferResult DocumentView::Export(ExportRequest Request)
    {
        if (!db_ || locked_) return {false, false, 0, tr("열린 데이터베이스가 없습니다.")};
        // SQL INSERT 는 생성 열 · 숨은 열을 뺀다(넣을 수 없다).
        if (Request.format_ == TransferFormat::Sql && Request.sql_.startsWith(QLatin1String("SELECT * FROM "))) {
            if (const auto* Object = schema_.Find(QStringLiteral("main"), Request.table_); Object && Object->type_ == ObjectType::Table) {
                QStringList Columns;
                for (const auto& Column : Object->columns_) if (!Column.hidden_ && !Column.generated_) Columns.append(QuoteIdentifier(Column.name_));
                if (!Columns.isEmpty()) Request.sql_ = QStringLiteral("SELECT %1 FROM %2").arg(Columns.join(QStringLiteral(", ")), Request.sql_.mid(14));
            }
        }
        if (Request.sql_.isEmpty()) {
            std::atomic_bool Never = false;
            std::atomic<qint64> Count = 0;
            return ExportData(nullptr, Request, Never, Count);
        }
        return runTransfer(tr("내보내는 중 — %1").arg(QFileInfo(Request.path_).fileName()), 0,
                           [Request](Connection& Db, const std::atomic_bool& Cancel, std::atomic<qint64>& Progress) { return ExportData(&Db, Request, Cancel, Progress); });
    }

    TransferResult DocumentView::Import(const ImportSource& Source, const ImportRequest& Request)
    {
        if (!db_ || locked_ || IsReadOnly()) return {false, false, 0, tr("읽기 전용으로 연 문서에는 가져올 수 없습니다.")};
        for (auto* Work : works_)
            if (auto* Query = qobject_cast<QueryTab*>(Work); Query && (Query->InTransaction() || Query->Running()))
                return {false, false, 0, tr("쿼리 탭의 트랜잭션 · 실행이 끝난 뒤에 가져올 수 있습니다. 먼저 커밋 · 롤백하거나 멈추세요.")};
        // 롤백 저널에서는 다른 탭의 열린 결과(남은 행 읽기)가 쓰기를 막는다. WAL 은 읽기가 쓰기를 막지 않는다.
        if (journalMode_.compare(QLatin1String("wal"), Qt::CaseInsensitive) != 0)
            for (auto* Work : works_) if (auto* Query = qobject_cast<QueryTab*>(Work)) Query->ReleaseConnection();
        stopCounting();
        const auto Result = runTransfer(tr("가져오는 중 — %1").arg(Request.table_), Source.rows_.size(),
                                        [Source, Request](Connection& Db, const std::atomic_bool& Cancel, std::atomic<qint64>& Progress) { return ImportData(Db, Source, Request, Cancel, Progress); });
        dataWritten();
        return Result;
    }

    bool DocumentView::ShowExport(const ExportRequest& Request)
    {
        if (!db_ || locked_) return false;
        ExportDialog Dialog(Request, !Request.createSql_.isEmpty(), this);
        if (Dialog.exec() != QDialog::Accepted) return false;
        const auto Chosen = Dialog.Request();
        const auto Result = Export(Chosen);
        if (Result.canceled_) return false;
        if (!Result.ok_) { QMessageBox::warning(this, tr("내보내기"), Result.error_); return false; }
        QMessageBox::information(this, tr("내보내기"), tr("%1행을 내보냈습니다.\n%2").arg(QLocale().toString(Result.rows_), QDir::toNativeSeparators(Chosen.path_)));
        return true;
    }

    bool DocumentView::ShowImport(const QString& Table, const QString& Path)
    {
        if (!db_ || locked_ || IsReadOnly()) return false;
        auto File = Path;
        if (File.isEmpty())
            File = QFileDialog::getOpenFileName(this, tr("가져올 파일"), QFileInfo(path_).absolutePath(),
                                                tr("CSV · JSON · SQL (*.csv *.tsv *.txt *.json *.jsonl *.ndjson *.sql);;모든 파일 (*)"));
        if (File.isEmpty()) return false;
        // .sql 은 쿼리 탭으로 연다(실행은 사용자가).
        if (QFileInfo(File).suffix().compare(QLatin1String("sql"), Qt::CaseInsensitive) == 0) return NewQuery({}, File) != nullptr;
        QList<std::pair<QString, QStringList>> Tables;
        if (const auto* Main = schema_.Database(QStringLiteral("main")))
            for (const auto* Object : Main->Objects(ObjectType::Table))
                if (!Object->virtual_) {
                    QStringList Columns;
                    for (const auto& Column : Object->columns_) if (!Column.hidden_ && !Column.generated_) Columns.append(Column.name_);
                    Tables.append({Object->name_, Columns});
                }
        ImportDialog Dialog(File, Tables, Table, this);
        if (Dialog.exec() != QDialog::Accepted) return false;
        const auto Request = Dialog.Request();
        const auto Result = Import(Dialog.Source(), Request);
        if (Result.canceled_) return false;
        if (!Result.ok_) { QMessageBox::warning(this, tr("가져오기"), Result.error_); return false; }
        QMessageBox::information(this, tr("가져오기"), tr("%1 에 %2행을 넣었습니다.").arg(Request.table_, QLocale().toString(Result.rows_)));
        OpenObject(QStringLiteral("main"), Request.table_);
        return true;
    }

    void DocumentView::releaseConnections()
    {
        stopCounting();
        for (const auto& Job : retiredJobs_) Job->Wait(3000);
        for (auto* Work : works_) if (auto* Query = qobject_cast<QueryTab*>(Work)) Query->ReleaseConnection();
    }

    bool DocumentView::ChangeEncryption(SeeEncryptDialog::Operation Operation, const QByteArray& Key, KeyFormat Format, bool Backup, QString* Error)
    {
        const auto Fail = [Error](const QString& Text) { if (Error) *Error = Text; return false; };
        if (!db_ || locked_ || IsReadOnly()) return Fail(tr("읽기 전용으로 연 문서는 암호화를 바꿀 수 없습니다."));
        if (Runtime::See() != SeeState::Available) return Fail(Runtime::SeeText(Runtime::See()));
        if (HasPending()) return Fail(tr("커밋하지 않은 변경이 있습니다. 먼저 커밋하거나 롤백하세요."));
        if (IsBusy()) return Fail(tr("실행 중인 쿼리 · 읽기를 먼저 멈추세요."));
        const bool Decrypt = Operation == SeeEncryptDialog::Operation::Decrypt;
        if (!Decrypt && Key.isEmpty()) return Fail(tr("새 키가 비어 있습니다."));
        // 다른 연결이 남아 있으면 rekey 가 잠금을 얻지 못하고, 그 연결은 옛 키로 읽는다. 쿼리 탭은 다음 실행에서 새로 연다.
        releaseConnections();
        QString Problem;
        if (Backup && !CopyDatabaseFile(*db_, BackupPathFor(path_), &Problem)) { startCounting(); return Fail(Problem); }
        if (!hc::sqlite::ChangeEncryption(*db_, Decrypt ? QByteArray() : Key, Format, &Problem)) { startCounting(); return Fail(Problem); }
        {
            std::lock_guard Lock(keyMutex_);
            keyProtected_ = Decrypt ? QByteArray() : ProtectKey(Key);
            keyFormat_ = Format;
        }
        encrypted_ = !Decrypt;
        header_ = InspectFile(path_);
        // 저장한 키(설계 8.3): 키를 바꾸면 새 키로, 해제하면 지운다. 평문을 암호화할 때는 ‘저장을 미리 켜 두기’ 설정을 따른다.
        if (!profile_.isEmpty()) {
            KeyStore Store(profile_);
            const bool Stored = Store.Find(path_).has_value();
            if (Decrypt) Store.Remove(path_);
            else if (Stored || queryPreferences_.saveKeyDefault_) Store.Save(path_, Key, Format);
        }
        readSchema();
        startCounting();
        emit StateChanged();
        return true;
    }

    bool DocumentView::showEncryptDialog(QWidget* Parent)
    {
        if (Runtime::See() != SeeState::Available) { QMessageBox::warning(Parent, tr("암호화"), Runtime::SeeText(Runtime::See())); return false; }
        if (IsReadOnly()) { QMessageBox::information(Parent, tr("암호화"), tr("읽기 전용으로 열었습니다. 암호화를 바꾸려면 쓰기로 다시 여세요.")); return false; }
        if (HasPending()) {
            if (QMessageBox::question(Parent, tr("암호화"), tr("커밋하지 않은 변경이 %1건 있습니다. 먼저 커밋할까요?").arg(PendingTotal()),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes) != QMessageBox::Yes || !commitAll(true)) return false;
        }
        SeeEncryptDialog Dialog(path_, encrypted_, keyFormat_, queryPreferences_.seeFormat_, queryPreferences_.backupBeforeRekey_, Parent);
        if (Dialog.exec() != QDialog::Accepted) return false;
        auto Key = Dialog.TakeKey();
        QString Error;
        const bool Ok = ChangeEncryption(Dialog.Chosen(), Key, Dialog.Format(), Dialog.Backup(), &Error);
        WipeKey(Key);
        if (!Ok) QMessageBox::warning(Parent, tr("암호화"), Error);
        return Ok;
    }

    void DocumentView::showDatabaseInfo()
    {
        DatabaseInfoDialog Dialog(*db_, [this](QString* Error) { return openWorker(Error); }, path_, [this] {
            if (!HasPending()) return true;
            if (QMessageBox::question(this, QStringLiteral("VACUUM"), tr("커밋하지 않은 변경이 %1건 있습니다. 먼저 커밋할까요?").arg(PendingTotal()),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes) != QMessageBox::Yes) return false;
            return commitAll(true);
        }, IsReadOnly(), this);
        // 암호화 칸(설계 8): 방식 · 저장한 키와 ‘암호화…’ · ‘관리…’.
        Dialog.SetEncryption([this, &Dialog] {
            DatabaseInfoDialog::Encryption Crypto;
            Crypto.method_ = encrypted_ ? tr("SEE · AES-256 · %1").arg(KeyFormatLabel(keyFormat_)) : QString();
            Crypto.savedKey_ = profile_.isEmpty() ? tr("쓰지 않음") : KeyStore(profile_).Find(path_) ? tr("이 PC 에 저장됨 (DPAPI)") : tr("없음");
            if (!IsReadOnly() && Runtime::See() == SeeState::Available) Crypto.change_ = [this, &Dialog] { return showEncryptDialog(&Dialog); };
            if (!profile_.isEmpty()) Crypto.manageKeys_ = [this, &Dialog] {
                Dialog.reject();
                QTimer::singleShot(0, this, [this] { emit SettingsRequested(QStringLiteral("sqlite-keys")); });
            };
            return Crypto;
        });
        connect(&Dialog, &DatabaseInfoDialog::DatabaseChanged, this, [this] {
            Statement Journal(*db_, QStringLiteral("PRAGMA main.journal_mode"));
            if (Journal.Valid() && Journal.Step() == SQLITE_ROW) journalMode_ = Journal.Value(0).toString();
            dataWritten();
        });
        Dialog.exec();
    }

    bool DocumentView::RunCommand(const QString& Id)
    {
        if (!CanRunCommand(Id) && Id != QLatin1String("sqlite.toggleSchema")) return false;
        auto* Query = currentQuery();
        if (Id == QLatin1String("sqlite.newQuery")) { NewQuery(); return true; }
        if (Id == QLatin1String("sqlite.openSql")) { openSqlFile(); return true; }
        if (Id == QLatin1String("sqlite.saveQuery")) return saveQuery(Query, false);
        if (Id == QLatin1String("sqlite.run")) { Query->RunAll(); return true; }
        if (Id == QLatin1String("sqlite.runCurrent")) { Query->RunCurrent(); return true; }
        if (Id == QLatin1String("sqlite.comment")) { Query->ToggleComment(); return true; }
        if (Id == QLatin1String("sqlite.stop")) { Cancel(); return true; }
        if (Id == QLatin1String("sqlite.complete")) { Query->FocusTarget()->setFocus(Qt::ShortcutFocusReason); return Query->Complete(true) || true; }
        if (Id == QLatin1String("sqlite.paramHelp")) return Query->ShowParameterHelp() || true;
        if (Id == QLatin1String("sqlite.quickFix")) return Query->QuickFix() || true;
        if (Id == QLatin1String("sqlite.format")) { Query->FormatSql(); return true; }
        if (Id == QLatin1String("sqlite.explain") || Id == QLatin1String("sqlite.explainBytecode")) { Query->Explain(Id == QLatin1String("sqlite.explainBytecode")); return true; }
        if (Id == QLatin1String("sqlite.closeTab")) return closeTab(tabs_->currentIndex());
        if (Id == QLatin1String("sqlite.nextTab") || Id == QLatin1String("sqlite.prevTab")) {
            const int Count = tabs_->count();
            tabs_->setCurrentIndex((tabs_->currentIndex() + (Id == QLatin1String("sqlite.nextTab") ? 1 : Count - 1)) % Count);
            if (auto* Target = CurrentTab()->FocusTarget()) Target->setFocus(Qt::OtherFocusReason);
            return true;
        }
        if (Id == QLatin1String("sqlite.refresh")) {
            readSchema();
            for (auto* Work : works_)
                if (auto* Table = qobject_cast<TableTab*>(Work))
                    if (const auto* Object = schema_.Find(Table->Object().database_, Table->Object().name_)) Table->Rebind(*Object);
            startCounting();
            return true;
        }
        if (Id == QLatin1String("sqlite.toggleSchema")) {
            if (!schemaArea_) return false;
            schemaArea_->setVisible(!schemaArea_->isVisible());
            for (auto* Action : actions_) if (Action->property("sqliteCommand") == Id) Action->setChecked(schemaArea_->isVisible());
            return true;
        }
        if (Id == QLatin1String("sqlite.attach")) { attachDatabase(); return true; }
        if (Id == QLatin1String("sqlite.commit")) return Commit();
        if (Id == QLatin1String("sqlite.rollback")) {
            if (QMessageBox::question(this, tr("롤백"), tr("커밋하지 않은 변경 %1건을 버릴까요? 열린 트랜잭션은 롤백하고 표의 보류 편집은 되돌립니다.").arg(PendingTotal()),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return false;
            rollbackAll(false);
            return true;
        }
        if (Id == QLatin1String("sqlite.toggleCommitMode")) { Query->SetManualCommit(!Query->ManualCommit()); refreshCommitMode(); return true; }
        if (Id == QLatin1String("sqlite.editTable")) return editTable(false);
        if (Id == QLatin1String("sqlite.newTable")) return editTable(true);
        if (Id == QLatin1String("sqlite.renameObject")) return renameObject();
        if (Id == QLatin1String("sqlite.dropObject")) return dropObject();
        if (Id == QLatin1String("sqlite.emptyTable")) return emptyTable();
        if (Id == QLatin1String("sqlite.dbInfo")) { showDatabaseInfo(); return true; }
        if (Id == QLatin1String("sqlite.encrypt")) return showEncryptDialog(this);
        if (Id == QLatin1String("sqlite.import")) return ShowImport(currentTable() ? currentTable()->Object().name_ : QString());
        if (Id == QLatin1String("sqlite.export")) return exportCurrent();
        if (auto* Table = currentTable()) {
            if (Id == QLatin1String("sqlite.editValue")) return Table->EditCurrent();
            if (Id == QLatin1String("sqlite.valuePanel")) { Table->ToggleValuePanel(); return true; }
            if (Id == QLatin1String("sqlite.addRow")) return Table->AddRow(false);
            if (Id == QLatin1String("sqlite.duplicateRow")) return Table->AddRow(true);
            if (Id == QLatin1String("sqlite.deleteRows")) return Table->DeleteRows();
            if (Id == QLatin1String("sqlite.setNull")) return Table->SetNull();
        }
        if (Id == QLatin1String("sqlite.schemaSelect")) { selectFromSchema(); return true; }
        if (Id == QLatin1String("sqlite.ddl")) {
            auto* Item = schemaPanel_->Tree()->currentItem();
            if (Item) showDdl(Item->data(0, Qt::UserRole + 4).toString(), Item->data(0, Qt::UserRole + 5).toString());
            return Item != nullptr;
        }
        if (Id == QLatin1String("sqlite.schemaFilter")) { schemaPanel_->FocusFilter(); return true; }
        if (Id == QLatin1String("sqlite.copyInsert")) {
            // 고른 행을 INSERT 문으로(테이블 탭은 그 테이블, 쿼리 결과는 열이 한 테이블에서 왔을 때 그 테이블).
            auto* Grid = CurrentTab()->CurrentGrid();
            auto* Model = Grid->model();
            QString Table = currentTable() ? IdentifierText(currentTable()->Object().name_) : QString();
            if (Table.isEmpty())
                if (auto* Result = qobject_cast<QueryModel*>(Model)) {
                    QSet<QString> Tables;
                    for (const auto& Column : Result->Columns()) Tables.insert(Column.table_);
                    Table = Tables.size() == 1 && !Tables.begin()->isEmpty() ? IdentifierText(*Tables.begin()) : QStringLiteral("\"table\"");
                }
            QStringList Names;
            for (int Column = 0; Column < Model->columnCount(); ++Column) Names.append(IdentifierText(Model->headerData(Column, Qt::Horizontal).toString()));
            QSet<int> Rows;
            for (const auto& Index : Grid->selectionModel()->selectedIndexes()) Rows.insert(Index.row());
            if (Rows.isEmpty()) Rows.insert(Grid->currentIndex().row());
            auto Sorted = Rows.values();
            std::sort(Sorted.begin(), Sorted.end());
            QStringList Lines;
            for (const int Row : Sorted) {
                QStringList Values;
                for (int Column = 0; Column < Model->columnCount(); ++Column) Values.append(SqlLiteral(Model->index(Row, Column).data(ValueRole)));
                Lines.append(QStringLiteral("INSERT INTO %1 (%2) VALUES (%3);").arg(Table, Names.join(QStringLiteral(", ")), Values.join(QStringLiteral(", "))));
            }
            if (brokered_) {
                // 중개는 곧바로(직접 연결) SelectionText 를 읽는다. 그동안만 INSERT 문을 돌려준다.
                pendingCopy_ = Lines.join(QStringLiteral("\r\n"));
                emit CopyRequested();
                pendingCopy_.clear();
            } else QApplication::clipboard()->setText(Lines.join(QStringLiteral("\r\n")));
            return true;
        }
        return false;
    }

    bool DocumentView::OwnsShortcutScope(const QString& Scope, QWidget* Focus) const
    {
        if (mode_ == Mode::QuickPreview || !Within(this, Focus)) return false;
        if (Scope == QLatin1String("sqlite")) return true;
        if (Scope == QLatin1String("sqlite.query")) {
            auto* Query = currentQuery();
            return Query && Within(Query->FocusTarget(), Focus);
        }
        if (Scope == QLatin1String("sqlite.grid")) {
            for (auto* Widget = Focus; Widget && Widget != this; Widget = Widget->parentWidget())
                if (qobject_cast<GridView*>(Widget)) return true;
            return false;
        }
        if (Scope == QLatin1String("sqlite.schema")) return schemaPanel_ && Within(schemaPanel_->Tree(), Focus);
        return false;
    }

    QWidget* DocumentView::InputWidget() const
    {
        if (mode_ == Mode::QuickPreview) return previewGrid_;
        if (auto* Tab = CurrentTab()) return Tab->FocusTarget();
        return schemaPanel_ ? static_cast<QWidget*>(schemaPanel_->Tree()) : nullptr;
    }

    void DocumentView::FocusInput()
    {
        if (auto* Input = InputWidget(); Input && Input->isVisible()) Input->setFocus(Qt::OtherFocusReason);
    }

    QString DocumentView::SelectionText() const
    {
        if (!pendingCopy_.isEmpty()) return pendingCopy_;
        if (mode_ == Mode::QuickPreview) return previewGrid_ ? previewGrid_->SelectionText() : QString();
        auto* Tab = CurrentTab();
        return Tab ? Tab->SelectionText() : QString();
    }

    bool DocumentView::HasSelection() const
    {
        // 쿼리 편집기에 포커스가 있으면 편집기 선택, 아니면 그리드의 현재 칸.
        if (auto* Query = currentQuery(); Query && Within(Query->FocusTarget(), QApplication::focusWidget())) return Query->HasEditorSelection();
        const auto* Grid = mode_ == Mode::QuickPreview ? previewGrid_ : CurrentTab() ? CurrentTab()->CurrentGrid() : nullptr;
        return Grid && Grid->model() && Grid->currentIndex().isValid();
    }

    QList<StatusItem> DocumentView::StatusItems() const
    {
        QList<StatusItem> Items;
        const auto Add = [&](const QString& Id, const QString& Text, bool Essential = true, bool Trailing = false, const QString& Detail = {}, bool Warning = false) {
            if (!Text.isEmpty()) Items.append(StatusItem{Id, Text, {}, Detail, Essential, Trailing, Warning});
        };
        const QFileInfo Info(path_);
        if (!error_.isEmpty()) { Add(QStringLiteral("format"), QStringLiteral("SQLite")); return Items; }
        if (locked_) {
            Add(QStringLiteral("sqliteEncryption"), tr("헤더 없음 — 암호화 추정"), true, false, Runtime::SeeText(Runtime::See()));
            Add(QStringLiteral("format"), QStringLiteral("SQLite · ") + SizeText(Info.size()), false);
            return Items;
        }
        Add(QStringLiteral("sqliteDatabase"), QStringLiteral("main · %1").arg(Info.fileName()), true, false, QDir::toNativeSeparators(Info.absoluteFilePath()));
        if (encrypted_) Add(QStringLiteral("sqliteEncryption"), QStringLiteral("SEE · AES-256"), false, false, tr("키 형식: %1").arg(KeyFormatLabel(keyFormat_)));
        if (!journalMode_.isEmpty()) Add(QStringLiteral("sqliteJournal"), journalMode_.toUpper(), false);
        if (IsReadOnly()) Add(QStringLiteral("sqliteReadOnly"), options_.immutable_ ? tr("읽기 전용 · 변경 감시 안 함") : tr("읽기 전용"), true);
        else {
            // 커밋 대기(시안: 상태 표시줄의 주황 ‘커밋 대기 3건’). 수동 커밋 쿼리 탭의 열린 트랜잭션도 주황.
            bool Transaction = false;
            int Edits = 0;
            for (const auto* Work : works_) {
                if (const auto* Query = qobject_cast<const QueryTab*>(Work)) Transaction |= Query->InTransaction();
                else Edits += Work->PendingCount();
            }
            if (Transaction) Add(QStringLiteral("sqliteTransaction"), tr("트랜잭션 열림"), true, false, tr("수동 커밋 — Ctrl+S 로 커밋하고 ‘롤백’ 으로 버립니다. 그동안 다른 프로그램은 쓰기를 기다립니다."), true);
            if (Edits > 0) Add(QStringLiteral("sqlitePending"), tr("커밋 대기 %1건").arg(Edits), true, false, tr("표의 보류 편집 — Ctrl+S 로 파일에 씁니다."), true);
            if (!Transaction && Edits == 0) Add(QStringLiteral("sqliteTransaction"), tr("트랜잭션 없음"), false);
        }
        Add(QStringLiteral("format"), QStringLiteral("SQLite %1 · %2").arg(Runtime::Version(), SizeText(Info.size())), false);
        if (auto* Tab = CurrentTab()) {
            Add(QStringLiteral("sqliteRows"), Tab->RowsText(), true, true);
            Add(QStringLiteral("position"), Tab->PositionText(), true, true);
        } else if (previewModel_) Add(QStringLiteral("sqliteRows"), tr("%1행").arg(QLocale().toString(previewModel_->rowCount())), true, true);
        return Items;
    }

    void DocumentView::SetClipboardBrokered(bool Enabled)
    {
        brokered_ = Enabled;
        for (auto* Work : works_) {
            if (auto* Grid = Work->CurrentGrid()) Grid->SetBrokered(Enabled);
            if (auto* Query = qobject_cast<QueryTab*>(Work)) Query->SetBrokered(Enabled);
        }
        if (previewGrid_) previewGrid_->SetBrokered(Enabled);
    }

    void DocumentView::ApplyQueryPreferences(const QueryPreferences& Preferences)
    {
        queryPreferences_ = Preferences;
        options_.busyTimeoutMs_ = Preferences.busyTimeoutMs_;
        completion_.upperKeywords_ = Preferences.upperKeywords_;
        completion_.joinFirst_ = Preferences.joinFirst_;
        for (auto* Work : works_) {
            if (auto* Query = qobject_cast<QueryTab*>(Work)) Query->SetPreferences(Preferences, false);
            if (auto* Table = qobject_cast<TableTab*>(Work)) Table->SetEditing(Preferences.confirmDelete_);
            if (auto* Grid = Work->CurrentGrid()) { Grid->Delegate()->SetDisplay(Preferences.cells_); Grid->viewport()->update(); }
        }
        if (previewGrid_) { previewGrid_->Delegate()->SetDisplay(Preferences.cells_); previewGrid_->viewport()->update(); }
        refreshCommitMode();
    }

    void DocumentView::ApplyFonts(const ViewFonts& Fonts)
    {
        fonts_ = Fonts;
        hasFonts_ = true;
        const auto GridFont = Fonts.customGrid_ ? Fonts.grid_ : font();
        if (schemaPanel_) schemaPanel_->Tree()->setFont(Fonts.customSchema_ ? Fonts.schema_ : font());
        if (history_) history_->SetMonoFont(SizedLike(Fonts.mono_, font(), 0.0));
        for (auto* Work : works_) Work->ApplyFonts(GridFont, Fonts.mono_, Fonts.monoValues_);
        if (previewGrid_) {
            previewGrid_->setFont(GridFont);
            previewGrid_->Delegate()->SetFonts(GridFont, Fonts.mono_, Fonts.monoValues_);
        }
    }
}
