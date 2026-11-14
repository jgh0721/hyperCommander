#include "stdafx.h"
#include "sqliteTableTab.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteConnection.hpp"
#include "sqliteCellDelegate.hpp"
#include "sqliteGlyphs.hpp"
#include "sqliteGrid.hpp"
#include "sqliteValuePanel.hpp"

/// 공통 라이브러리

/// 외부 라이브러리
#include <fmstyle/StyleProps.h>
#include <fmstyle/ThemeColors.h>
#include <fmstyle/ThemeManager.h>

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDateTime>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace hc::sqlite
{
    namespace
    {
        using T = fm::style::Token;

        QFont Mono(const QFont& Base, qreal Delta = 0)
        {
            auto Font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            return SizedLike(Font, Base, Delta, 7.0);
        }
    }

    TableTab::TableTab(Connection& Reader, TableModel::Opener Opener, const ObjectInfo& Object, QObject* Parent)
        : WorkTab(Parent)
    {
        model_ = new TableModel(Reader, std::move(Opener), this);
        model_->SetSource(Object);

        top_ = new QWidget;
        top_->setObjectName(QStringLiteral("sqliteTableBar"));
        auto* TopLayout = new QVBoxLayout(top_);
        TopLayout->setContentsMargins(8, 7, 8, 7);
        TopLayout->setSpacing(6);
        auto* Row = new QHBoxLayout;
        Row->setSpacing(6);
        whereLabel_ = new QLabel(QStringLiteral("WHERE"), top_);
        whereLabel_->setFixedWidth(46);
        where_ = new QLineEdit(top_);
        where_->setObjectName(QStringLiteral("sqliteWhere"));
        where_->setPlaceholderText(tr("조건 — 예: GenreId = 1 AND AlbumId <= 3"));
        where_->setClearButtonEnabled(true);
        orderLabel_ = new QLabel(QStringLiteral("ORDER BY"), top_);
        order_ = new QLineEdit(top_);
        order_->setObjectName(QStringLiteral("sqliteOrderBy"));
        order_->setFixedWidth(170);
        order_->setClearButtonEnabled(true);
        auto* ApplyButton = new QPushButton(tr("적용"), top_);
        ApplyButton->setObjectName(QStringLiteral("sqliteApplyFilter"));
        Row->addWidget(whereLabel_);
        Row->addWidget(where_, 1);
        Row->addWidget(orderLabel_);
        Row->addWidget(order_);
        Row->addWidget(ApplyButton);
        TopLayout->addLayout(Row);
        auto* SqlRow = new QHBoxLayout;
        sql_ = new QLabel(top_);
        sql_->setObjectName(QStringLiteral("sqliteGeneratedSql"));
        sql_->setContentsMargins(52, 0, 0, 0);
        sql_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        SqlRow->addWidget(sql_, 1);
        openQuery_ = new QLabel(top_);
        openQuery_->setObjectName(QStringLiteral("sqliteOpenQuery"));
        openQuery_->setText(QStringLiteral("<a href=\"query\">%1</a>").arg(tr("쿼리 탭으로 열기")));
        openQuery_->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
        SqlRow->addWidget(openQuery_);
        TopLayout->addLayout(SqlRow);
        top_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

        result_ = new QWidget;
        result_->setObjectName(QStringLiteral("sqliteTableResult"));
        auto* ResultLayout = new QVBoxLayout(result_);
        ResultLayout->setContentsMargins(0, 0, 0, 0);
        ResultLayout->setSpacing(0);
        resultTabs_ = new QTabBar(result_);
        resultTabs_->setDocumentMode(true);
        resultTabs_->setExpanding(false);
        resultTabs_->addTab(Object.name_);
        resultTabs_->addTab(tr("메시지"));
        ResultLayout->addWidget(resultTabs_);
        auto* NoteRow = new QWidget(result_);
        NoteRow->setFixedHeight(30);
        auto* NoteLayout = new QHBoxLayout(NoteRow);
        NoteLayout->setContentsMargins(10, 0, 6, 0);
        NoteLayout->setSpacing(2);
        note_ = new QLabel(NoteRow);
        note_->setObjectName(QStringLiteral("sqliteResultNote"));
        NoteLayout->addWidget(note_, 1);
        // 그리드 도구(시안: 행 추가 · 행 삭제 · 값 패널).
        const auto Tool = [NoteRow, NoteLayout](const QString& Name, const QString& Glyph, const QString& Tip, bool Checkable = false) {
            auto* Button = new QToolButton(NoteRow);
            Button->setObjectName(Name);
            Button->setAutoRaise(true);
            Button->setToolTip(Tip);
            Button->setCheckable(Checkable);
            Button->setProperty("sqliteGlyph", Glyph);
            NoteLayout->addWidget(Button);
            return Button;
        };
        addRow_ = Tool(QStringLiteral("sqliteAddRow"), QStringLiteral("addrow"), tr("행 추가 (Alt+Insert)"));
        deleteRows_ = Tool(QStringLiteral("sqliteDeleteRows"), QStringLiteral("delrow"), tr("행 삭제 (Ctrl+Delete)"));
        panelButton_ = Tool(QStringLiteral("sqliteValuePanelToggle"), QStringLiteral("panel"), tr("값 패널"), true);
        exportButton_ = Tool(QStringLiteral("sqliteTableExport"), QStringLiteral("export"), tr("내보내기 (CSV · JSON · SQL)"));
        ResultLayout->addWidget(NoteRow);

        // 보류 편집 띠(시안: ● 커밋하지 않은 변경 n건 … 변경 SQL 보기 · 모두 되돌리기 · 커밋 (Ctrl+S)).
        banner_ = new QFrame(result_);
        banner_->setObjectName(QStringLiteral("sqlitePendingBanner"));
        banner_->setAutoFillBackground(true);
        auto* BannerLayout = new QHBoxLayout(banner_);
        BannerLayout->setContentsMargins(10, 4, 8, 4);
        BannerLayout->setSpacing(8);
        bannerDot_ = new QLabel(QStringLiteral("●"), banner_);
        BannerLayout->addWidget(bannerDot_);
        bannerText_ = new QLabel(banner_);
        bannerText_->setObjectName(QStringLiteral("sqlitePendingText"));
        bannerText_->setTextFormat(Qt::RichText);
        BannerLayout->addWidget(bannerText_, 1);
        auto* ShowSql = new QLabel(QStringLiteral("<a href=\"sql\">%1</a>").arg(tr("변경 SQL 보기")), banner_);
        ShowSql->setObjectName(QStringLiteral("sqlitePendingSql"));
        ShowSql->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
        BannerLayout->addWidget(ShowSql);
        auto* Revert = new QPushButton(tr("모두 되돌리기"), banner_);
        Revert->setObjectName(QStringLiteral("sqlitePendingRevert"));
        BannerLayout->addWidget(Revert);
        auto* Commit = new QPushButton(tr("커밋 (Ctrl+S)"), banner_);
        Commit->setObjectName(QStringLiteral("sqlitePendingCommit"));
        fm::style::setButtonRole(Commit, fm::style::ButtonRole::Primary);
        BannerLayout->addWidget(Commit);
        banner_->hide();
        ResultLayout->addWidget(banner_);

        resultPages_ = new QStackedWidget(result_);
        split_ = new QSplitter(Qt::Horizontal, resultPages_);
        split_->setChildrenCollapsible(false);
        grid_ = new GridView(split_);
        grid_->setModel(model_);
        grid_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
        panel_ = new ValuePanel(split_);
        panel_->setMinimumWidth(220);
        panel_->hide();
        split_->addWidget(grid_);
        split_->addWidget(panel_);
        split_->setStretchFactor(0, 1);
        resultPages_->addWidget(split_);
        messages_ = new QListWidget(resultPages_);
        messages_->setObjectName(QStringLiteral("sqliteMessages"));
        messages_->setFrameShape(QFrame::NoFrame);
        messages_->setWordWrap(true);
        resultPages_->addWidget(messages_);
        ResultLayout->addWidget(resultPages_, 1);

        connect(resultTabs_, &QTabBar::currentChanged, resultPages_, &QStackedWidget::setCurrentIndex);
        connect(ApplyButton, &QPushButton::clicked, this, &TableTab::Apply);
        connect(where_, &QLineEdit::returnPressed, this, &TableTab::Apply);
        connect(order_, &QLineEdit::returnPressed, this, &TableTab::Apply);
        connect(openQuery_, &QLabel::linkActivated, this, [this] { emit OpenQueryRequested(model_->GeneratedSql(1000) + QLatin1Char(';')); });
        connect(model_, &TableModel::OrderChanged, this, [this](const QString& OrderBy) { order_->setText(OrderBy); });
        connect(model_, &TableModel::LoadingChanged, this, [this] { refreshNote(); emit Changed(); });
        connect(model_, &TableModel::Loaded, this, [this](qint64 Rows, qint64 Ms) {
            if (!fitted_) { grid_->FitColumns(); fitted_ = true; }
            addMessage(false, tr("%1 — %2행 · %3 ms").arg(model_->GeneratedSql(0)).arg(QLocale().toString(Rows)).arg(Ms));
            resultTabs_->setCurrentIndex(0);
            refreshNote();
            if (panel_->isVisible()) panel_->SetCell(grid_->currentIndex());
            emit Changed();
        });
        connect(model_, &TableModel::Failed, this, [this](const QString& Error) {
            addMessage(true, Error);
            resultTabs_->setCurrentIndex(1);
            refreshNote();
            emit Changed();
        });
        connect(model_, &TableModel::PendingChanged, this, [this] { refreshPending(); emit Changed(); });
        connect(model_, &QAbstractItemModel::dataChanged, this, [this](const QModelIndex& From, const QModelIndex& To) {
            const auto Current = grid_->currentIndex();
            if (panel_->isVisible() && Current.isValid() && Current.row() >= From.row() && Current.row() <= To.row()) panel_->SetCell(Current);
        });
        connect(grid_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& Current) {
            if (panel_->isVisible()) panel_->SetCell(Current);
            emit Changed();
        });
        connect(grid_->Delegate(), &CellDelegate::PanelEditRequested, this, [this](const QModelIndex& Index) {
            // 셀에서 고치기 어려운 값: 값 패널을 열고 편집기로.
            if (!panel_->isVisible()) ToggleValuePanel();
            panel_->SetCell(Index);
            panel_->FocusEditor();
        });
        connect(panel_, &ValuePanel::ApplyRequested, this, [this](const QPersistentModelIndex& Cell, const QVariant& Value) {
            if (Cell.isValid()) model_->setData(Cell, Value, Qt::EditRole);
        });
        connect(panel_, &ValuePanel::CloseRequested, this, [this] { if (panel_->isVisible()) ToggleValuePanel(); });
        connect(addRow_, &QToolButton::clicked, this, [this] { AddRow(false); });
        connect(deleteRows_, &QToolButton::clicked, this, &TableTab::DeleteRows);
        connect(panelButton_, &QToolButton::clicked, this, &TableTab::ToggleValuePanel);
        connect(exportButton_, &QToolButton::clicked, this, &TableTab::ExportRequested);
        connect(ShowSql, &QLabel::linkActivated, this, [this] { emit ShowSqlRequested(model_->PendingStatements()); });
        connect(Revert, &QPushButton::clicked, this, &TableTab::RevertAll);
        connect(Commit, &QPushButton::clicked, this, &TableTab::CommitRequested);
        connect(&fm::style::ThemeManager::instance(), &fm::style::ThemeManager::changed, this, &TableTab::applyTheme);
        applyTheme();
        refreshNote();
        refreshPending();
    }

    TableTab::~TableTab()
    {
        delete top_.data();
        delete result_.data();
    }

    void TableTab::applyTheme()
    {
        const auto& Colors = fm::style::themeColorsFor(top_);
        const auto Base = top_->font();
        for (auto* Label : {whereLabel_, orderLabel_}) {
            auto Font = Mono(Base, -0.5);
            Font.setWeight(QFont::DemiBold);
            Label->setFont(Font);
            auto Palette = Label->palette();
            Palette.setColor(QPalette::WindowText, Colors[T::AccentFg]);
            Label->setPalette(Palette);
        }
        for (auto* Edit : {where_, order_}) Edit->setFont(Mono(Base));
        sql_->setFont(Mono(Base, -0.5));
        auto Dim = sql_->palette();
        Dim.setColor(QPalette::WindowText, Colors[T::Fg3]);
        sql_->setPalette(Dim);
        auto NotePalette = note_->palette();
        NotePalette.setColor(QPalette::WindowText, Colors[T::Fg2]);
        note_->setPalette(NotePalette);
        for (auto* Button : {addRow_, deleteRows_, panelButton_, exportButton_}) Button->setIcon(Glyph(Button->property("sqliteGlyph").toString(), Colors[T::Fg2], 16));
        auto Banner = banner_->palette();
        Banner.setColor(QPalette::Window, Colors[T::WarnBg]);
        Banner.setColor(QPalette::WindowText, Colors[T::Fg]);
        banner_->setPalette(Banner);
        auto Dot = bannerDot_->palette();
        Dot.setColor(QPalette::WindowText, Colors[T::Warn]);
        bannerDot_->setPalette(Dot);
    }

    void TableTab::Rebind(const ObjectInfo& Object)
    {
        const auto Where = model_->Where(), Order = model_->OrderBy();
        model_->SetSource(Object);
        model_->SetFilter(Where, Order);
        fitted_ = false;
        Reload();
    }

    void TableTab::Reload()
    {
        sql_->setText(model_->GeneratedSql(0));
        model_->Reload();
    }

    void TableTab::Apply()
    {
        if (where_->text().trimmed() != model_->Where() || order_->text().trimmed() != model_->OrderBy()) grid_->ClearSort();
        model_->SetFilter(where_->text(), order_->text());
        Reload();
    }

    void TableTab::addMessage(bool Error, const QString& Text)
    {
        const auto& Colors = fm::style::themeColorsFor(messages_);
        auto* Item = new QListWidgetItem(Glyph(Error ? QStringLiteral("err") : QStringLiteral("ok"), Colors[Error ? T::Danger : T::Ok], 14),
            QStringLiteral("%1  %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), Text));
        messages_->addItem(Item);
        while (messages_->count() > 200) delete messages_->takeItem(0);
        messages_->scrollToBottom();
        resultTabs_->setTabText(1, Error ? tr("메시지 · 오류") : tr("메시지"));
    }

    void TableTab::refreshNote()
    {
        sql_->setText(model_->GeneratedSql(0));
        const auto& Source = model_->Source();
        QString Text;
        if (model_->Loading()) Text = tr("읽는 중…");
        else if (!model_->LastError().isEmpty()) Text = tr("읽지 못했습니다 — 메시지 탭을 보세요.");
        else {
            Text = tr("%1행 · %2 ms").arg(QLocale().toString(model_->rowCount())).arg(model_->ElapsedMs());
            if (Source.type_ == ObjectType::View) Text += tr(" · 뷰 — 읽기 전용");
            else if (!model_->Editable()) Text += tr(" · 읽기 전용");
            else if (model_->KeysetMode()) Text += tr(" · rowid 로 행을 찾아 고칩니다");
            else Text += tr(" · 기본 키로 행을 찾아 고칩니다");
        }
        note_->setText(Text);
        resultTabs_->setTabText(0, model_->Loading() || !model_->LastError().isEmpty() ? Source.name_
            : tr("%1 · %2행").arg(Source.name_, QLocale().toString(model_->rowCount())));
        addRow_->setEnabled(model_->Editable());
        deleteRows_->setEnabled(model_->Editable());
    }

    void TableTab::refreshPending()
    {
        const auto Pending = model_->Pending();
        banner_->setVisible(Pending.Total() > 0 || model_->ConflictCount() > 0);
        QStringList Parts;
        if (Pending.modified_) Parts.append(tr("수정 %1").arg(Pending.modified_));
        if (Pending.inserted_) Parts.append(tr("추가 %1").arg(Pending.inserted_));
        if (Pending.deleted_) Parts.append(tr("삭제 %1").arg(Pending.deleted_));
        auto Text = tr("<b>커밋하지 않은 변경 %1건</b>&nbsp;&nbsp;%2 — 아직 파일에 쓰지 않았습니다. 정렬 · 필터를 바꿔도 유지됩니다.")
                        .arg(Pending.Total()).arg(Parts.join(QStringLiteral(" · ")));
        if (model_->ConflictCount() > 0) Text += tr(" <b>충돌 %1칸</b> — 다른 곳에서 바뀌었습니다.").arg(model_->ConflictCount());
        bannerText_->setText(Text);
    }

    QString TableTab::RowsText() const
    {
        if (model_->Loading()) return tr("읽는 중…");
        return tr("%1행").arg(QLocale().toString(model_->rowCount()));
    }

    QString TableTab::PositionText() const
    {
        const auto Current = grid_->currentIndex();
        if (!Current.isValid()) return {};
        return tr("%1행, %2").arg(Current.row() >= model_->rowCount() - model_->Pending().inserted_ ? QStringLiteral("+") : QLocale().toString(Current.row() + 1),
                                   model_->headerData(Current.column(), Qt::Horizontal).toString());
    }

    QString TableTab::SelectionText() const { return grid_->SelectionText(); }
    void TableTab::FocusGrid() { grid_->setFocus(Qt::OtherFocusReason); }
    QWidget* TableTab::FocusTarget() const { return grid_; }

    void TableTab::ApplyFonts(const QFont& Grid, const QFont& Mono, bool MonoValues)
    {
        grid_->setFont(Grid);
        grid_->Delegate()->SetFonts(Grid, Mono, MonoValues);
        grid_->viewport()->update();
        panel_->SetMonoFont(Mono);
    }

    void TableTab::SetReadOnly(bool ReadOnly)
    {
        model_->SetReadOnly(ReadOnly);
        refreshNote();
    }

    bool TableTab::Commit(QString* Error)
    {
        grid_->closePersistentEditor(grid_->currentIndex());
        QString Problem;
        if (model_->Commit(&Problem)) {
            addMessage(false, tr("커밋했습니다 — %1").arg(model_->Source().name_));
            return true;
        }
        addMessage(true, Problem);
        if (Error) *Error = Problem;
        return false;
    }

    void TableTab::RevertAll() { model_->RevertAll(); }

    QList<int> TableTab::selectedRows() const
    {
        QList<int> Rows;
        for (const auto& Index : grid_->selectionModel()->selectedIndexes()) if (!Rows.contains(Index.row())) Rows.append(Index.row());
        if (Rows.isEmpty() && grid_->currentIndex().isValid()) Rows.append(grid_->currentIndex().row());
        return Rows;
    }

    bool TableTab::EditCurrent()
    {
        const auto Current = grid_->currentIndex();
        if (!Current.isValid()) return false;
        if (grid_->Delegate()->NeedsPanel(Current) || !(Current.flags() & Qt::ItemIsEditable)) {
            if (!panel_->isVisible()) ToggleValuePanel();
            panel_->SetCell(Current);
            panel_->FocusEditor();
            return true;
        }
        grid_->edit(Current);
        return true;
    }

    bool TableTab::AddRow(bool Duplicate)
    {
        const auto Current = grid_->currentIndex();
        const int Row = model_->InsertRow(Duplicate && Current.isValid() ? Current.row() : -1);
        if (Row < 0) return false;
        resultTabs_->setCurrentIndex(0);
        // 첫 칸이 rowid 별칭(자동)이면 다음 칸으로.
        int Column = 0;
        while (Column + 1 < model_->columnCount() && model_->index(Row, Column).data(UnsetRole).toBool()) ++Column;
        grid_->setCurrentIndex(model_->index(Row, Column));
        grid_->scrollTo(model_->index(Row, Column));
        grid_->setFocus(Qt::OtherFocusReason);
        return true;
    }

    bool TableTab::DeleteRows()
    {
        const auto Rows = selectedRows();
        if (Rows.isEmpty() || !model_->Editable()) return false;
        bool Marking = false;
        for (const int Row : Rows)
            Marking |= model_->index(Row, 0).data(PendingRole).toInt() != int(TableModel::PendingState::Deleted);
        if (Marking && confirmDelete_ && QMessageBox::question(grid_, tr("행 삭제"),
                tr("고른 행 %1개를 지우기로 표시할까요? 커밋할 때 파일에서 지웁니다.").arg(Rows.size()), QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
            return false;
        model_->ToggleDelete(Rows);
        return true;
    }

    bool TableTab::SetNull()
    {
        bool Any = false;
        auto Cells = grid_->selectionModel()->selectedIndexes();
        if (Cells.isEmpty() && grid_->currentIndex().isValid()) Cells.append(grid_->currentIndex());
        for (const auto& Cell : Cells) if (Cell.flags() & Qt::ItemIsEditable) Any |= model_->setData(Cell, QVariant(), Qt::EditRole);
        return Any;
    }

    void TableTab::ToggleValuePanel()
    {
        panel_->setVisible(!panel_->isVisible());
        panelButton_->setChecked(panel_->isVisible());
        if (panel_->isVisible()) {
            panel_->SetCell(grid_->currentIndex());
            if (split_->sizes().value(1) < 220) split_->setSizes({qMax(200, split_->width() - 300), 300});
        }
    }

    bool TableTab::ValuePanelVisible() const { return panel_->isVisible(); }
}
