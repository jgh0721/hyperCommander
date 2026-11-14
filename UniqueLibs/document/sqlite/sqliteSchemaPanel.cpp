#include "stdafx.h"
#include "sqliteSchemaPanel.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteGlyphs.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/ThemeColors.h>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/FindBox.h>
#include <fmwidgets/Tag.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QToolButton>
#include <QSet>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace hc::sqlite
{
    namespace
    {
        enum ItemRole { DimRole = Qt::UserRole + 1, BadgeRole, KindRole, DatabaseRole, NameRole, MonoDimRole };
        using T = fm::style::Token;

        // 이름 · NN 배지 · 오른쪽 흐린 글(행 수 · 형식)을 한 줄에 그린다(시안 스키마 트리).
        class SchemaDelegate final : public QStyledItemDelegate
        {
        public:
            using QStyledItemDelegate::QStyledItemDelegate;
            QSize sizeHint(const QStyleOptionViewItem& Option, const QModelIndex& Index) const override
            {
                auto Size = QStyledItemDelegate::sizeHint(Option, Index);
                Size.setHeight(qMax(Size.height(), 22));
                return Size;
            }
            void paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override
            {
                QStyleOptionViewItem Opt(Option);
                initStyleOption(&Opt, Index);
                const auto Text = Opt.text;
                Opt.text.clear();
                auto* Style = Opt.widget ? Opt.widget->style() : QApplication::style();
                Style->drawControl(QStyle::CE_ItemViewItem, &Opt, Painter, Opt.widget);
                const auto& Colors = fm::style::themeColorsFor(Opt.widget);
                const bool On = Opt.state.testFlag(QStyle::State_Selected) && Opt.widget && Opt.widget->hasFocus();
                const QColor Fg = On ? Opt.palette.color(QPalette::HighlightedText) : Colors[T::Fg];
                const QColor Dim = On ? Fg : Colors[T::Fg3];
                const auto Kind = Index.data(KindRole).toString();
                QRect Rect = Style->subElementRect(QStyle::SE_ItemViewItemText, &Opt, Opt.widget).adjusted(2, 0, -6, 0);
                Painter->save();
                // 오른쪽 흐린 글(행 수 · 형식 · 파일 이름).
                const auto DimText = Index.data(DimRole).toString();
                QFont DimFont = Opt.font;
                if (Index.data(MonoDimRole).toBool()) {
                    DimFont = SizedLike(QFontDatabase::systemFont(QFontDatabase::FixedFont), Opt.font, -1.5, 7.0);
                } else DimFont = SizedLike(DimFont, Opt.font, -1.0, 7.0);
                int Right = Rect.right();
                if (!DimText.isEmpty()) {
                    const int Width = QFontMetrics(DimFont).horizontalAdvance(DimText);
                    if (Width < Rect.width() / 2) {
                        Painter->setFont(DimFont);
                        Painter->setPen(Dim);
                        Painter->drawText(QRect(Right - Width, Rect.top(), Width, Rect.height()), Qt::AlignVCenter | Qt::AlignRight, DimText);
                        Right -= Width + 8;
                    }
                }
                QFont Font = Opt.font;
                if (Kind == QLatin1String("db") || Kind == QLatin1String("grp") || Kind == QLatin1String("pk")) Font.setWeight(QFont::DemiBold);
                Painter->setFont(Font);
                Painter->setPen(Kind == QLatin1String("grp") && !On ? Colors[T::Fg2] : Fg);
                const QFontMetrics Metrics(Font);
                const auto Badge = Index.data(BadgeRole).toString();
                QFont BadgeFont = Opt.font;
                BadgeFont = SizedLike(BadgeFont, Opt.font, -2.5, 6.5);
                BadgeFont.setWeight(QFont::DemiBold);
                const int BadgeWidth = Badge.isEmpty() ? 0 : QFontMetrics(BadgeFont).horizontalAdvance(Badge) + 8;
                const int Available = qMax(0, Right - Rect.left() - (BadgeWidth ? BadgeWidth + 5 : 0));
                const auto Shown = Metrics.elidedText(Text, Qt::ElideRight, Available);
                Painter->drawText(QRect(Rect.left(), Rect.top(), Available, Rect.height()), Qt::AlignVCenter | Qt::AlignLeft, Shown);
                if (BadgeWidth) {
                    const int X = Rect.left() + Metrics.horizontalAdvance(Shown) + 5;
                    const QRectF Box(X, Rect.center().y() - 6.5, BadgeWidth, 14);
                    Painter->setRenderHint(QPainter::Antialiasing);
                    Painter->setPen(QPen(On ? Fg : Colors[T::Line], 1.0));
                    Painter->setBrush(Qt::NoBrush);
                    Painter->drawRoundedRect(Box.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
                    Painter->setFont(BadgeFont);
                    Painter->setPen(Dim);
                    Painter->drawText(Box, Qt::AlignCenter, Badge);
                }
                Painter->restore();
            }
        };

        QToolButton* ToolButton(QWidget* Parent, const QString& Glyph, const QString& Tip)
        {
            auto* Button = new QToolButton(Parent);
            Button->setAutoRaise(true);
            Button->setToolTip(Tip);
            Button->setAccessibleName(Tip);
            Button->setProperty("sqliteGlyph", Glyph);
            Button->setIconSize(QSize(14, 14));
            Button->setFixedSize(22, 22);
            return Button;
        }

        QString CountText(qint64 Count) { return Count < 0 ? QStringLiteral("…") : QLocale().toString(Count); }
        QString Key(const QString& Database, const QString& Name) { return Database.toLower() + QLatin1Char('\n') + Name.toLower(); }
    }

    SchemaPanel::SchemaPanel(QWidget* Parent) : QWidget(Parent)
    {
        setObjectName(QStringLiteral("sqliteSchemaPanel"));
        auto* Layout = new QVBoxLayout(this);
        Layout->setContentsMargins(0, 0, 0, 0);
        Layout->setSpacing(0);
        tabs_ = new QTabBar(this);
        tabs_->setObjectName(QStringLiteral("sqliteSchemaTabs"));
        tabs_->setDocumentMode(true);
        tabs_->setExpanding(false);
        tabs_->addTab(tr("스키마"));
        tabs_->addTab(tr("기록"));
        Layout->addWidget(tabs_);
        pages_ = new QStackedWidget(this);
        Layout->addWidget(pages_, 1);

        auto* Schema = new QWidget(pages_);
        auto* Column = new QVBoxLayout(Schema);
        Column->setContentsMargins(0, 4, 0, 0);
        Column->setSpacing(0);
        auto* Head = new QWidget(Schema);
        Head->setFixedHeight(30);
        auto* HeadLayout = new QHBoxLayout(Head);
        HeadLayout->setContentsMargins(8, 0, 4, 0);
        HeadLayout->setSpacing(6);
        icon_ = new QLabel(Head);
        icon_->setFixedSize(15, 15);
        title_ = new QLabel(Head);
        title_->setObjectName(QStringLiteral("sqliteDatabaseName"));
        auto TitleFont = title_->font();
        TitleFont.setWeight(QFont::DemiBold);
        title_->setFont(TitleFont);
        title_->setTextInteractionFlags(Qt::NoTextInteraction);
        HeadLayout->addWidget(icon_);
        HeadLayout->addWidget(title_, 0);
        chips_ = new QHBoxLayout;
        chips_->setSpacing(4);
        HeadLayout->addLayout(chips_);
        HeadLayout->addStretch(1);
        const auto Add = [&](const QString& Glyph, const QString& Tip) {
            auto* Button = ToolButton(Head, Glyph, Tip);
            HeadLayout->addWidget(Button);
            tools_.append(Button);
            return Button;
        };
        connect(Add(QStringLiteral("refresh"), tr("새로 고침")), &QToolButton::clicked, this, &SchemaPanel::RefreshRequested);
        connect(Add(QStringLiteral("attach"), tr("데이터베이스 첨부 (ATTACH)")), &QToolButton::clicked, this, &SchemaPanel::AttachRequested);
        connect(Add(QStringLiteral("collapse"), tr("모두 접기")), &QToolButton::clicked, this, [this] {
            for (int Db = 0; Db < tree_->topLevelItemCount(); ++Db) {
                auto* Database = tree_->topLevelItem(Db);
                for (int Group = 0; Group < Database->childCount(); ++Group)
                    for (int Item = 0; Item < Database->child(Group)->childCount(); ++Item) Database->child(Group)->child(Item)->setExpanded(false);
            }
        });
        auto* More = Add(QStringLiteral("more"), tr("더 보기"));
        connect(More, &QToolButton::clicked, this, [this, More] {
            QMenu Menu(this);
            auto* System = Menu.addAction(tr("시스템 개체 표시"));
            System->setCheckable(true);
            System->setChecked(showSystem_);
            connect(System, &QAction::toggled, this, [this](bool On) { showSystem_ = On; rebuild(); });
            connect(Menu.addAction(tr("행 수 다시 세기")), &QAction::triggered, this, &SchemaPanel::CountRequested);
            Menu.exec(More->mapToGlobal(QPoint(0, More->height())));
        });
        Column->addWidget(Head);
        sub_ = new QLabel(Schema);
        sub_->setObjectName(QStringLiteral("sqliteDatabasePath"));
        sub_->setContentsMargins(8, 0, 8, 2);
        Column->addWidget(sub_);
        auto* FilterRow = new QWidget(Schema);
        auto* FilterLayout = new QHBoxLayout(FilterRow);
        FilterLayout->setContentsMargins(8, 4, 8, 5);
        filter_ = new fm::ui::FindBox(FilterRow);
        filter_->setObjectName(QStringLiteral("sqliteSchemaFilter"));
        filter_->setPlaceholderText(tr("테이블 · 열 이름 필터"));
        FilterLayout->addWidget(filter_);
        Column->addWidget(FilterRow);
        tree_ = new QTreeWidget(Schema);
        tree_->setObjectName(QStringLiteral("sqliteSchemaTree"));
        tree_->setHeaderHidden(true);
        tree_->setColumnCount(1);
        tree_->setIndentation(14);
        tree_->setUniformRowHeights(true);
        tree_->setFrameShape(QFrame::NoFrame);
        tree_->setItemDelegate(new SchemaDelegate(tree_));
        tree_->setContextMenuPolicy(Qt::CustomContextMenu);
        tree_->setExpandsOnDoubleClick(false);
        tree_->installEventFilter(this);
        Column->addWidget(tree_, 1);
        pages_->addWidget(Schema);

        history_ = new QLabel(tr("실행한 문이 여기에 남습니다."), pages_);
        static_cast<QLabel*>(history_)->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        history_->setContentsMargins(12, 24, 12, 12);
        pages_->addWidget(history_);

        lockedPage_ = new QWidget(pages_);
        auto* LockedLayout = new QVBoxLayout(lockedPage_);
        LockedLayout->setContentsMargins(18, 40, 18, 18);
        auto* LockedTitle = new QLabel(tr("잠겨 있음"), lockedPage_);
        auto LockedFont = LockedTitle->font();
        LockedFont.setWeight(QFont::DemiBold);
        LockedTitle->setFont(LockedFont);
        LockedTitle->setAlignment(Qt::AlignHCenter);
        auto* LockedText = new QLabel(tr("키를 넣으면 스키마를 읽습니다."), lockedPage_);
        LockedText->setAlignment(Qt::AlignHCenter);
        LockedText->setWordWrap(true);
        LockedLayout->addWidget(LockedTitle);
        LockedLayout->addWidget(LockedText);
        LockedLayout->addStretch(1);
        pages_->addWidget(lockedPage_);

        connect(tabs_, &QTabBar::currentChanged, this, [this](int Index) {
            pages_->setCurrentIndex(locked_ && Index == 0 ? 2 : Index);
        });
        connect(filter_, &QLineEdit::textChanged, this, &SchemaPanel::applyFilter);
        connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* Item) {
            const auto Kind = Item->data(0, KindRole).toString();
            if (Kind == QLatin1String("table") || Kind == QLatin1String("view")) openItem(Item);
            else Item->setExpanded(!Item->isExpanded());
        });
        connect(tree_, &QTreeWidget::customContextMenuRequested, this, &SchemaPanel::showContextMenu);
        connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, &SchemaPanel::applyTheme);
        applyTheme();
    }

    QWidget* SchemaPanel::HistoryPage() const { return history_; }

    void SchemaPanel::SetHistoryWidget(QWidget* Widget)
    {
        // 작업 창이 기록 패널(sqliteHistory)을 넣는다. 자리는 그대로(스키마 다음).
        if (!Widget || Widget == history_) return;
        const int Index = pages_->indexOf(history_);
        const bool Current = pages_->currentWidget() == history_;
        pages_->removeWidget(history_);
        delete history_;
        history_ = Widget;
        pages_->insertWidget(Index, Widget);
        if (Current) pages_->setCurrentWidget(Widget);
    }

    void SchemaPanel::ShowHistory() { tabs_->setCurrentIndex(1); }
    void SchemaPanel::FocusFilter() { tabs_->setCurrentIndex(0); filter_->setFocus(Qt::ShortcutFocusReason); filter_->selectAll(); }

    void SchemaPanel::SetHeader(const Header& Value)
    {
        header_ = Value;
        title_->setText(Value.fileName_);
        title_->setToolTip(Value.path_);
        for (auto* Tag : chipTags_) delete Tag;
        chipTags_.clear();
        const auto Chip = [&](const QString& Text, fm::ui::Tag::Tone Tone) {
            auto* Tag = new fm::ui::Tag(this);
            Tag->setText(Text);
            Tag->setTone(Tone);
            Tag->setCompact(true);
            chips_->addWidget(Tag);
            chipTags_.append(Tag);
        };
        if (Value.encrypted_) Chip(QStringLiteral("SEE · AES-256"), fm::ui::Tag::Info);
        if (!Value.journalMode_.isEmpty()) Chip(Value.journalMode_.toUpper(), fm::ui::Tag::Mute);
        if (Value.readOnly_) Chip(tr("읽기 전용"), fm::ui::Tag::Warn);
        QStringList Sub{QDir::toNativeSeparators(Value.path_)};
        if (Value.fileSize_ >= 0) Sub.append(QLocale().formattedDataSize(Value.fileSize_, 1, QLocale::DataSizeTraditionalFormat));
        if (const auto* Main = schema_.Database(QStringLiteral("main")); Main && !locked_)
            Sub.append(tr("테이블 %1").arg(Main->Objects(ObjectType::Table, showSystem_).size()));
        subText_ = Sub.join(QStringLiteral(" · "));
        sub_->setToolTip(subText_);
        elideSub();
    }

    void SchemaPanel::SetLocked(bool Locked)
    {
        locked_ = Locked;
        for (auto* Button : tools_) Button->setEnabled(!Locked);
        filter_->setEnabled(!Locked);
        pages_->setCurrentIndex(Locked && tabs_->currentIndex() == 0 ? 2 : tabs_->currentIndex());
    }

    void SchemaPanel::SetSchema(const SchemaInfo& Schema)
    {
        schema_ = Schema;
        rebuild();
        SetHeader(header_);
    }

    void SchemaPanel::SetRowCount(const QString& Database, const QString& Table, qint64 Count)
    {
        counts_.insert(Key(Database, Table), Count);
        for (QTreeWidgetItemIterator It(tree_); *It; ++It) {
            auto* Item = *It;
            if (Item->data(0, KindRole).toString() == QLatin1String("table") && Item->data(0, DatabaseRole).toString().compare(Database, Qt::CaseInsensitive) == 0
                && Item->data(0, NameRole).toString().compare(Table, Qt::CaseInsensitive) == 0) {
                Item->setData(0, DimRole, CountText(Count));
                break;
            }
        }
    }

    void SchemaPanel::applyTheme()
    {
        const auto& Colors = fm::style::themeColorsFor(this);
        icon_->setPixmap(Glyph(QStringLiteral("db"), Colors[T::AccentFg], 15).pixmap(15, 15));
        for (auto* Button : tools_) Button->setIcon(Glyph(Button->property("sqliteGlyph").toString(), Colors[T::Fg2], 14));
        auto Palette = sub_->palette();
        Palette.setColor(QPalette::WindowText, Colors[T::Fg3]);
        sub_->setPalette(Palette);
        if (qobject_cast<QLabel*>(history_)) history_->setPalette(Palette);
        rebuild();
    }

    void SchemaPanel::elideSub()
    {
        // 경로는 앞을 줄여 파일 이름 · 크기 · 테이블 수가 보이게 한다.
        const int Width = qMax(40, width() - 16);
        sub_->setText(sub_->fontMetrics().elidedText(subText_, Qt::ElideLeft, Width));
    }

    void SchemaPanel::resizeEvent(QResizeEvent* Event)
    {
        QWidget::resizeEvent(Event);
        elideSub();
    }

    void SchemaPanel::changeEvent(QEvent* Event)
    {
        QWidget::changeEvent(Event);
        if (Event->type() == QEvent::FontChange) rebuild();
    }

    void SchemaPanel::rebuild()
    {
        const auto& Colors = fm::style::themeColorsFor(this);
        const auto Icon = [&](const char* Name, T Color) { return Glyph(QLatin1String(Name), Colors[Color], 14); };
        // 펼침 상태를 지킨다(다시 읽기 · 테마 바꿈 뒤).
        QSet<QString> Expanded;
        bool First = tree_->topLevelItemCount() == 0;
        for (QTreeWidgetItemIterator It(tree_); *It; ++It)
            if ((*It)->isExpanded()) Expanded.insert((*It)->data(0, DatabaseRole).toString() + QLatin1Char('/') + (*It)->data(0, KindRole).toString() + QLatin1Char('/') + (*It)->text(0));
        const QString Current = tree_->currentItem() ? tree_->currentItem()->data(0, DatabaseRole).toString() + QLatin1Char('/') + tree_->currentItem()->data(0, NameRole).toString() : QString();
        tree_->clear();
        const auto Make = [&](QTreeWidgetItem* Parent, const QString& Text, const QString& Kind, const QString& Database, const QIcon& Glyph) {
            auto* Item = Parent ? new QTreeWidgetItem(Parent) : new QTreeWidgetItem(tree_);
            Item->setText(0, Text);
            Item->setData(0, KindRole, Kind);
            Item->setData(0, DatabaseRole, Database);
            Item->setIcon(0, Glyph);
            return Item;
        };
        QTreeWidgetItem* Select = nullptr;
        for (const auto& Database : schema_.databases_) {
            const bool Main = Database.name_.compare(QLatin1String("main"), Qt::CaseInsensitive) == 0;
            auto* Db = Make(nullptr, Database.name_, QStringLiteral("db"), Database.name_, Icon("db", T::AccentFg));
            Db->setData(0, DimRole, Database.file_.isEmpty() ? QString() : QFileInfo(Database.file_).fileName());
            Db->setToolTip(0, QDir::toNativeSeparators(Database.file_));
            const auto AddColumns = [&](QTreeWidgetItem* Parent, const ObjectInfo& Object) {
                for (const auto& Column : Object.columns_) {
                    if (Column.hidden_) continue;
                    const bool Key = Column.primaryKey_ > 0, Foreign = !Column.foreignTable_.isEmpty();
                    auto* Item = Make(Parent, Column.name_, Key ? QStringLiteral("pk") : Foreign ? QStringLiteral("fk") : QStringLiteral("col"), Database.name_,
                        Key ? Icon("pk", T::Warn) : Foreign ? Icon("fk", T::AccentFg) : Icon("col", T::Fg3));
                    Item->setData(0, NameRole, Column.name_);
                    Item->setData(0, DimRole, Column.type_);
                    Item->setData(0, MonoDimRole, true);
                    if (Column.notNull_ && !Key) Item->setData(0, BadgeRole, QStringLiteral("NN"));
                    QStringList Tip{Column.name_ + (Column.type_.isEmpty() ? QString() : QStringLiteral(" · ") + Column.type_)};
                    if (Key) Tip.append(tr("기본 키"));
                    if (Foreign) Tip.append(tr("외래 키 → %1(%2)").arg(Column.foreignTable_, Column.foreignColumn_));
                    if (!Column.defaultValue_.isEmpty()) Tip.append(tr("기본값 %1").arg(Column.defaultValue_));
                    Item->setToolTip(0, Tip.join(QStringLiteral(" · ")));
                }
            };
            const auto Group = [&](ObjectType Type, const QString& Title) {
                const auto Objects = Database.Objects(Type, showSystem_);
                auto* Item = Make(Db, Title, QStringLiteral("grp"), Database.name_, {});
                Item->setData(0, DimRole, QString::number(Objects.size()));
                for (const auto* Object : Objects) {
                    const char* GlyphName = Type == ObjectType::Table ? "table" : Type == ObjectType::View ? "view" : Type == ObjectType::Index ? "index" : "trigger";
                    const T Tone = Type == ObjectType::Table || Type == ObjectType::View ? T::Ok : Type == ObjectType::Index ? T::Fg2 : T::Warn;
                    const QString Kind = Type == ObjectType::Table ? QStringLiteral("table") : Type == ObjectType::View ? QStringLiteral("view")
                        : Type == ObjectType::Index ? QStringLiteral("index") : QStringLiteral("trigger");
                    auto* Child = Make(Item, Object->name_, Kind, Database.name_, Icon(GlyphName, Tone));
                    Child->setData(0, NameRole, Object->name_);
                    if (Type == ObjectType::Table) {
                        Child->setData(0, DimRole, Object->virtual_ ? tr("가상") : CountText(counts_.value(Key(Database.name_, Object->name_), -1)));
                        if (Object->withoutRowid_) Child->setData(0, BadgeRole, QStringLiteral("WR"));
                        if (Object->strict_) Child->setData(0, BadgeRole, QStringLiteral("STRICT"));
                    }
                    if (Type == ObjectType::Index || Type == ObjectType::Trigger) Child->setData(0, DimRole, Object->table_);
                    Child->setToolTip(0, Object->sql_.left(400));
                    if (Type == ObjectType::Table || Type == ObjectType::View) AddColumns(Child, *Object);
                    if (Database.name_ + QLatin1Char('/') + Object->name_ == Current) Select = Child;
                }
                return Item;
            };
            auto* Tables = Group(ObjectType::Table, tr("테이블"));
            Group(ObjectType::View, tr("뷰"));
            Group(ObjectType::Index, tr("인덱스"));
            Group(ObjectType::Trigger, tr("트리거"));
            if (First) { Db->setExpanded(Main); Tables->setExpanded(Main); }
        }
        if (!First)
            for (QTreeWidgetItemIterator It(tree_); *It; ++It)
                (*It)->setExpanded(Expanded.contains((*It)->data(0, DatabaseRole).toString() + QLatin1Char('/') + (*It)->data(0, KindRole).toString() + QLatin1Char('/') + (*It)->text(0)));
        if (Select) tree_->setCurrentItem(Select);
        applyFilter();
    }

    void SchemaPanel::applyFilter()
    {
        const auto Text = filter_->text().trimmed();
        const auto Matches = [&](QTreeWidgetItem* Item) { return Item->text(0).contains(Text, Qt::CaseInsensitive); };
        for (int Db = 0; Db < tree_->topLevelItemCount(); ++Db) {
            auto* Database = tree_->topLevelItem(Db);
            for (int G = 0; G < Database->childCount(); ++G) {
                auto* Group = Database->child(G);
                bool Any = false;
                for (int O = 0; O < Group->childCount(); ++O) {
                    auto* Object = Group->child(O);
                    const bool Self = Text.isEmpty() || Matches(Object) || Object->data(0, DimRole).toString().contains(Text, Qt::CaseInsensitive) && Object->data(0, KindRole) != QLatin1String("table");
                    bool ColumnHit = false;
                    for (int C = 0; C < Object->childCount(); ++C) {
                        auto* Column = Object->child(C);
                        const bool Hit = !Text.isEmpty() && Matches(Column);
                        ColumnHit |= Hit;
                        Column->setHidden(!Text.isEmpty() && !Self && !Hit);
                    }
                    Object->setHidden(!(Self || ColumnHit));
                    if (!Text.isEmpty() && ColumnHit) Object->setExpanded(true);
                    Any |= !Object->isHidden();
                }
                Group->setHidden(!Text.isEmpty() && !Any);
                if (!Text.isEmpty() && Any) { Group->setExpanded(true); Database->setExpanded(true); }
            }
        }
    }

    void SchemaPanel::openItem(QTreeWidgetItem* Item)
    {
        if (!Item) return;
        const auto Kind = Item->data(0, KindRole).toString();
        if (Kind == QLatin1String("table") || Kind == QLatin1String("view"))
            emit OpenObject(Item->data(0, DatabaseRole).toString(), Item->data(0, NameRole).toString());
        else if (Kind == QLatin1String("pk") || Kind == QLatin1String("fk") || Kind == QLatin1String("col")) openItem(Item->parent());
    }

    void SchemaPanel::showContextMenu(const QPoint& Position)
    {
        auto* Item = tree_->itemAt(Position);
        QMenu Menu(this);
        if (Item) {
            const auto Kind = Item->data(0, KindRole).toString();
            const auto Database = Item->data(0, DatabaseRole).toString();
            const auto Name = Item->data(0, NameRole).toString();
            const auto Command = [&](const QString& Text, const QString& Id, const QKeySequence& Key = {}) {
                auto* Action = Menu.addAction(Text, this, [this, Item, Id] { tree_->setCurrentItem(Item); emit CommandRequested(Id); });
                if (!Key.isEmpty()) Action->setShortcut(Key);
                Action->setEnabled(!locked_);
            };
            const bool Table = Kind == QLatin1String("table"), Object = Table || Kind == QLatin1String("view");
            // 시안 ‘부품’ 보드의 메뉴 순서: 데이터 보기 · 새 쿼리 탭에 SELECT · 스크립트 만들기 · DDL 보기 | 테이블 수정 · 이름 바꾸기 ·
            // 행 수 세기 | 가져오기 · 내보내기 | 테이블 비우기 · 삭제.
            if (Object) {
                auto* Open = Menu.addAction(tr("데이터 보기"), this, [this, Item] { openItem(Item); });
                Open->setShortcut(Qt::Key_Return);
                Command(tr("새 쿼리 탭에 SELECT"), QStringLiteral("sqlite.schemaSelect"), QKeySequence(QStringLiteral("Ctrl+Return")));
                auto* Script = Menu.addMenu(tr("스크립트 만들기"));
                Script->setObjectName(QStringLiteral("sqliteScriptMenu"));
                Script->setEnabled(!locked_);
                for (const auto& Each : Table ? QStringList{QStringLiteral("SELECT"), QStringLiteral("INSERT"), QStringLiteral("UPDATE"), QStringLiteral("DELETE"), QStringLiteral("CREATE")}
                                              : QStringList{QStringLiteral("SELECT"), QStringLiteral("CREATE")})
                    Script->addAction(Each, this, [this, Database, Name, Each] { emit ScriptRequested(Database, Name, Each); });
            }
            if (Object || Kind == QLatin1String("index") || Kind == QLatin1String("trigger"))
                Menu.addAction(tr("DDL 보기"), this, [this, Database, Name] { emit ShowDdlRequested(Database, Name); });
            if (Object) Menu.addSeparator();
            // 구조 바꾸기(설계 7절): 표는 수정 · 이름 바꾸기 · 삭제, 열은 이름 바꾸기 · 삭제, 뷰 · 색인 · 트리거는 삭제.
            const bool Column = Kind == QLatin1String("col") || Kind == QLatin1String("pk") || Kind == QLatin1String("fk");
            if (Table) Command(tr("테이블 수정…"), QStringLiteral("sqlite.editTable"), QKeySequence(QStringLiteral("Ctrl+E")));
            if (Table || Column) Command(Column ? tr("열 이름 바꾸기…") : tr("이름 바꾸기…"), QStringLiteral("sqlite.renameObject"), QKeySequence(Qt::Key_F2));
            if (Table) Menu.addAction(tr("행 수 세기"), this, &SchemaPanel::CountRequested)->setEnabled(!locked_);
            if (Object) {
                Menu.addSeparator();
                if (Table) Menu.addAction(tr("가져오기 (CSV · JSON)…"), this, [this, Database, Name] { emit ImportRequested(Database, Name); })->setEnabled(!locked_);
                Menu.addAction(tr("내보내기 (CSV · JSON · SQL)…"), this, [this, Database, Name] { emit ExportRequested(Database, Name); })->setEnabled(!locked_);
                Menu.addSeparator();
            }
            if (Table) Command(tr("테이블 비우기…"), QStringLiteral("sqlite.emptyTable"));
            if (Object || Kind == QLatin1String("index") || Kind == QLatin1String("trigger") || Column)
                Command(Column ? tr("열 삭제…") : tr("삭제…"), QStringLiteral("sqlite.dropObject"), QKeySequence(QKeySequence::Delete));
            Menu.addSeparator();
            if (!Name.isEmpty()) Menu.addAction(tr("이름 복사"), this, [Name] { QApplication::clipboard()->setText(Name); });
            if (Kind == QLatin1String("db") && Database.compare(QLatin1String("main"), Qt::CaseInsensitive) != 0
                && Database.compare(QLatin1String("temp"), Qt::CaseInsensitive) != 0)
                Menu.addAction(tr("분리 (DETACH)"), this, [this, Database] { emit DetachRequested(Database); });
            Menu.addSeparator();
        }
        Menu.addAction(tr("새 테이블…"), this, [this] { emit CommandRequested(QStringLiteral("sqlite.newTable")); })->setEnabled(!locked_);
        Menu.addAction(tr("데이터베이스 정보 · 유지 관리…"), this, [this] { emit CommandRequested(QStringLiteral("sqlite.dbInfo")); })->setEnabled(!locked_);
        Menu.addSeparator();
        Menu.addAction(tr("새로 고침"), this, &SchemaPanel::RefreshRequested);
        Menu.exec(tree_->viewport()->mapToGlobal(Position));
    }

    bool SchemaPanel::eventFilter(QObject* Watched, QEvent* Event)
    {
        if (Watched == tree_ && Event->type() == QEvent::KeyPress) {
            auto* Key = static_cast<QKeyEvent*>(Event);
            if ((Key->key() == Qt::Key_Return || Key->key() == Qt::Key_Enter) && Key->modifiers() == Qt::NoModifier) {
                openItem(tree_->currentItem());
                return true;
            }
            if (Key->matches(QKeySequence::Find)) { FocusFilter(); return true; }
        }
        return QWidget::eventFilter(Watched, Event);
    }
}
