#include "stdafx.h"
#include "documentSearchPanel.hpp"
#include "UniqueLibs/document/documentStorageAdapter.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAbstractTableModel>
#include <QCache>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>
#include <QPainter>
#include <QApplication>
#include <QGridLayout>
#include <QDesktopServices>

namespace hc::document::search {
class ResultModel final : public QAbstractTableModel {
public:
    explicit ResultModel(QObject* Parent) : QAbstractTableModel(Parent), pages_(32) {}
    int rowCount(const QModelIndex& Parent = {}) const override { return Parent.isValid() ? 0 : count_; }
    int columnCount(const QModelIndex& Parent = {}) const override { return Parent.isValid() ? 0 : 3; }
    QVariant headerData(int Section, Qt::Orientation Orientation, int Role) const override {
        if (Orientation == Qt::Horizontal && Role == Qt::DisplayRole) return QStringList{tr("파일"), tr("줄:열"), tr("미리보기")}.value(Section);
        return {};
    }
    QVariant data(const QModelIndex& Index, int Role) const override {
        if (!Index.isValid()) return {};
        const int Page = Index.row() / 64;
        const auto* Rows = pages_.object(Page);
        if (!Rows) {
            if (pending_.size() < 2 && !pending_.contains(Page)) { pending_.insert(Page); if (request_) request_(Page * 64); }
            return Role == Qt::DisplayRole && Index.column() == 2 ? tr("미리보기 읽는 중…") : QVariant{};
        }
        if (Index.row() % 64 >= Rows->size()) {
            pages_.remove(Page); pending_.remove(Page); return data(Index, Role);
        }
        const auto Row = Rows->at(Index.row() % 64).toObject();
        if (Role == Qt::UserRole) return Row;
        if (Role == Qt::ToolTipRole) return !Row["error"].toString().isEmpty() ? Row["error"].toString() : !Row["replaceError"].toString().isEmpty() ? Row["replaceError"].toString() : Row["path"].toString() + (Row["body"].toBool() ? tr(" · 치환문은 시작 텍스트 노드의 서식을 따릅니다.") : QString{});
        if (Role != Qt::DisplayRole) return {};
        if (Index.column() == 0) return QDir::toNativeSeparators(Row["displayPath"].toString(Row["path"].toString()));
        if (Index.column() == 1) return QString::number(Row["line"].toInteger()) + ':' + QString::number(Row["column"].toInteger());
        return Row["error"].toString().isEmpty() ? Row["before"].toString() + Row["match"].toString() + Row["after"].toString() : Row["error"].toString();
    }
    void Reset() { beginResetModel(); count_ = 0; pages_.clear(); pending_.clear(); endResetModel(); }
    void Count(qint64 Count) {
        const auto New = int(qMin<qint64>(Count, std::numeric_limits<int>::max()));
        if (New <= count_) return;
        // A formerly partial page needs the newly appended rows as well.
        pages_.remove(count_ / 64);
        beginInsertRows({}, count_, New - 1); count_ = New; endInsertRows();
    }
    void Invalidate() { pages_.clear(); pending_.clear(); if (count_) emit dataChanged(index(0, 0), index(count_ - 1, 2)); }
    void Supply(int First, QJsonArray Rows) {
        pending_.remove(First / 64);
        if (Rows.isEmpty()) return;
        const auto Last = qMin(count_ - 1, First + int(Rows.size()) - 1);
        pages_.insert(First / 64, new QJsonArray(std::move(Rows)));
        // Wake visible rows deferred by the two-page request budget. The view
        // asks only for its viewport; completed results stay in the disk spool.
        if (Last >= First && count_) emit dataChanged(index(0, 0), index(count_ - 1, 2));
    }
    std::function<void(int)> request_;
private:
    int count_ = 0;
    mutable QCache<int, QJsonArray> pages_;
    mutable QSet<int> pending_;
};
class PreviewDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* Painter, const QStyleOptionViewItem& Option, const QModelIndex& Index) const override {
        const auto Row = Index.data(Qt::UserRole).toJsonObject();
        if (Index.column() != 2 || Row.isEmpty() || !Row["error"].toString().isEmpty()) { QStyledItemDelegate::paint(Painter, Option, Index); return; }
        QStyleOptionViewItem Background(Option); initStyleOption(&Background, Index); Background.text.clear();
        const auto* Style = Option.widget ? Option.widget->style() : QApplication::style();
        Style->drawControl(QStyle::CE_ItemViewItem, &Background, Painter, Option.widget);
        const auto SingleLine = [](QString Text) { return Text.replace("\r\n", "↵").replace('\n', QChar(0x21b5)).replace('\r', QChar(0x21b5)).replace('\t', ' '); };
        const auto Before = SingleLine(Row["before"].toString()), Match = SingleLine(Row["match"].toString()), Added = SingleLine(Row["replacement"].toString()), After = SingleLine(Row["after"].toString());
        QTextLayout Layout(Before + Match + Added + After, Option.font);
        QList<QTextLayout::FormatRange> Formats;
        const bool Replacing = Row.contains("replacement");
        const bool Dark = Option.palette.color(QPalette::Base).lightness() < 128;
        QTextCharFormat Old; Old.setBackground(Replacing ? QColor(Dark ? "#673c40" : "#ffd9dc") : QColor(Dark ? "#665c26" : "#fff0a0")); Old.setForeground(Dark ? Qt::white : Qt::black); Old.setFontStrikeOut(Replacing);
        Formats.append({int(Before.size()), int(Match.size()), Old});
        if (Replacing) { QTextCharFormat New; New.setBackground(QColor(Dark ? "#285b3f" : "#cef0d5")); New.setForeground(Dark ? Qt::white : Qt::black); Formats.append({int(Before.size() + Match.size()), int(Added.size()), New}); }
        Layout.setFormats(Formats); Layout.beginLayout(); auto Line = Layout.createLine(); Line.setLineWidth(100000); Layout.endLayout();
        Painter->save(); Painter->setClipRect(Option.rect); Painter->setPen(Option.palette.color(Option.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text));
        Layout.draw(Painter, QPointF(Option.rect.left() + 4, Option.rect.top() + (Option.rect.height() - Line.height()) / 2)); Painter->restore();
    }
};
Panel::Panel(QWidget* Parent) : QWidget(Parent) {
    setObjectName("documentSearchPanel");
    auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(6, 6, 6, 6); Layout->setSpacing(4);
    auto* Top = new QHBoxLayout; Layout->addLayout(Top);
    replaceMode_ = new QCheckBox(tr("바꾸기(&R)"), this); Top->addWidget(replaceMode_);
    auto* Title = new QLabel(tr("파일에서 찾기 / 바꾸기"), this); Top->addWidget(Title); Top->addStretch();
    auto* Dock = new QToolButton(this); Dock->setText(tr("위치(&J)")); Dock->setPopupMode(QToolButton::InstantPopup); auto* Menu = new QMenu(Dock); Dock->setMenu(Menu);
    for (const auto& Item : QList<QPair<QString, QString>>{{tr("하단 패널(&M)"), "bottom"}, {tr("콘텐츠 탭(&P)"), "tab"}, {tr("분리 창(&J)"), "floating"}})
        connect(Menu->addAction(Item.first), &QAction::triggered, this, [this, Location = Item.second] { emit PlacementRequested(Location); });
    Top->addWidget(Dock); auto* Close = new QPushButton(tr("닫기(&Q)"), this); Top->addWidget(Close); connect(Close, &QPushButton::clicked, this, &Panel::CloseRequested);
    auto* Grid = new QGridLayout; Layout->addLayout(Grid);
    const auto Field = [this, Grid](int Row, const QString& Caption, const QString& Name) {
        auto* Input = new QLineEdit(this); Input->setObjectName(Name); auto* Label = new QLabel(Caption, this); Label->setBuddy(Input); Grid->addWidget(Label, Row, 0); Grid->addWidget(Input, Row, 1, 1, 4); return Input;
    };
    query_ = Field(0, tr("찾을 내용(&N)"), "documentSearchQuery");
    replacement_ = Field(1, tr("바꿀 내용(&W)"), "documentSearchReplacement"); replacementLabel_ = qobject_cast<QLabel*>(Grid->itemAtPosition(1, 0)->widget());
    root_ = Field(2, tr("루트(&D)"), "documentSearchRoot");
    auto* Folder = new QPushButton(tr("폴더(&H)…"), this); Grid->addWidget(Folder, 2, 5);
    connect(Folder, &QPushButton::clicked, this, [this] { if (book_) return; const auto Path = QFileDialog::getExistingDirectory(this, tr("검색 루트"), root_->text()); if (!Path.isEmpty()) root_->setText(Path); });
    include_ = Field(3, tr("포함(&I)"), "documentSearchInclude"); include_->setPlaceholderText(tr("예: *.cpp;*.h;*.md (비어 있으면 모든 파일)"));
    exclude_ = Field(4, tr("제외(&B)"), "documentSearchExclude"); exclude_->setText(".git;.svn;*.exe;*.dll;*.lib;*.obj;*.pdb;*.png;*.jpg;*.zip;*.7z");
    auto* Options = new QHBoxLayout; Layout->addLayout(Options);
    case_ = new QCheckBox(tr("대소문자 구분(&C)"), this); regex_ = new QCheckBox(tr("정규식(&X)"), this); recursive_ = new QCheckBox(tr("하위 폴더(&U)"), this); recursive_->setChecked(true);
    Options->addWidget(case_); Options->addWidget(regex_); Options->addWidget(recursive_); Options->addStretch();
    auto* TargetLabel = new QLabel(tr("대상(&G)"), this); target_ = new QComboBox(this); target_->setObjectName("documentSearchTarget"); target_->addItem(tr("소스"), "source"); target_->addItem(tr("본문"), "body"); TargetLabel->setBuddy(target_); Options->addWidget(TargetLabel); Options->addWidget(target_); target_->setEnabled(false);
    connect(target_, &QComboBox::currentIndexChanged, this, &Panel::schedule);
    auto* Find = new QPushButton(tr("찾기(&T)"), this); Find->setObjectName("documentSearchFind"); auto* Stop = new QPushButton(tr("중지(&S)"), this); Options->addWidget(Find); Options->addWidget(Stop);
    connect(Find, &QPushButton::clicked, this, &Panel::start);
    auto* Actions = new QHBoxLayout; Layout->addLayout(Actions);
    const auto Button = [this, Actions](const QString& Text, const QString& Scope) { auto* Result = new QPushButton(Text, this); Result->setObjectName("documentSearchReplace_" + Scope); Actions->addWidget(Result); connect(Result, &QPushButton::clicked, this, [this, Scope] { replace(Scope); }); return Result; };
    selected_ = Button(tr("선택 항목 바꾸기(&O)"), "selected"); file_ = Button(tr("선택 파일 바꾸기(&L)"), "files"); all_ = Button(tr("모두 바꾸기(&A)"), "all"); undo_ = Button(tr("바꾸기 취소(&Z)"), "undo"); Actions->addStretch();
    model_ = new ResultModel(this); table_ = new QTableView(this); table_->setObjectName("documentSearchResults"); table_->setModel(model_); table_->setItemDelegate(new PreviewDelegate(table_)); table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::ExtendedSelection); table_->setWordWrap(false); table_->setSortingEnabled(false);
    table_->verticalHeader()->hide(); table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed); table_->verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 8); table_->horizontalHeader()->setStretchLastSection(true); table_->setColumnWidth(0, 240); table_->setColumnWidth(1, 70); Layout->addWidget(table_, 1);
    status_ = new QLabel(this); status_->setObjectName("documentSearchStatus"); status_->setWordWrap(false); Layout->addWidget(status_);
    status_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    status_->setToolTip(tr("오류 상세 기록: 마우스 오른쪽 버튼 → 검색 오류 기록 열기"));status_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(status_,&QWidget::customContextMenuRequested,this,[this](const QPoint& Point){
        QMenu Menu(this);auto* Open=Menu.addAction(tr("검색 오류 기록 열기(&E)"));const auto Path=property("diagnosticLog").toString();Open->setEnabled(QFileInfo(Path).size()>0);
        if(Menu.exec(status_->mapToGlobal(Point))==Open)QDesktopServices::openUrl(QUrl::fromLocalFile(Path));
    });
    thread_ = new QThread; engine_ = new Engine;
    if (qApp->property("documentViewsConfigDir").isValid()) engine_->SetJournalDirectory(qApp->property("documentViewsConfigDir").toString() + "/document-search-undo");
    engine_->SetWriter([](const QString& Path, const QByteArray& ExpectedHash, const QByteArray& Bytes) -> QString {
        const auto Current = ReadDocumentFile(Path, {}, true);
        if (!Current.HasValue()) return QString::fromStdString(Current.Failure().code_);
        if (Current.Value().hash_ != ExpectedHash) return QObject::tr("파일이 검색/치환 후 변경되었습니다: %1").arg(Path);
        const auto Saved = SaveTextFile(Path, Bytes, {true, Current.Value().source_.id_.revision_}, ExpectedHash, {}, false, true);
        return Saved.HasValue() ? QString{} : QString::fromStdString(Saved.Failure().code_);
    });
    engine_->moveToThread(thread_);
    connect(engine_,&Engine::DiagnosticLog,this,[this](quint64 Generation,const QString& Path){if(Generation==generation_)setProperty("diagnosticLog",Path);});
    ++liveThreads_; connect(engine_, &QObject::destroyed, [] { --liveThreads_; });
    connect(thread_, &QThread::finished, engine_, &QObject::deleteLater); connect(thread_, &QThread::finished, thread_, &QObject::deleteLater); thread_->start();
    model_->request_ = [this](int First) { const auto Generation = generation_, Version = previewVersion_; const auto Replacement = replacement_->text(); const bool Replace = replaceMode_->isChecked(); QMetaObject::invokeMethod(engine_, [Engine = engine_, Generation, First, Replacement, Replace, Version] { Engine->Page(Generation, First, 64, Replacement, Replace, Version); }); };
    connect(engine_, &Engine::PageReady, this, [this](quint64 Generation, int First, const QJsonArray& Rows, quint64 Version) { if (Generation == generation_ && Version == previewVersion_) model_->Supply(First, Rows); });
    connect(engine_, &Engine::EditBuffer, this, &Panel::ApplyBuffer);
    connect(engine_, &Engine::ApplyProgress, this, [this](bool Busy, qint64 Changed, const QString& Message) {
        const bool WasApplying = applying_; applying_ = Busy;
        status_->setText(tr("%1 · %2개 파일/버퍼").arg(Message).arg(Changed));
        for (auto* Input : {query_, replacement_, root_, include_, exclude_}) Input->setEnabled(!Busy);
        for (auto* Button : {selected_, file_, all_, undo_}) Button->setEnabled(!Busy && !readOnly_);
        if (WasApplying && !Busy && Changed > 0) schedule();
    });
    connect(engine_, &Engine::Progress, this, [this](quint64 Generation, qint64 Rows, qint64 Files, qint64 Errors, bool Complete, const QString& Message) {
        if (Generation != generation_ || applying_) return; complete_ = Complete && Message.isEmpty(); model_->Count(Rows);
        status_->setText(tr("%1 · %2개 파일 · %3건 · 제외/오류 %4%5").arg(Complete ? tr("검색 완료/중지") : tr("검색 중")).arg(Files).arg(Rows).arg(Errors).arg(Message.isEmpty() ? QString{} : " · " + Message));
        all_->setEnabled(complete_ && Rows > 0 && replaceMode_->isChecked());
    });
    connect(Stop, &QPushButton::clicked, this, [this] { debounce_.stop(); engine_->RequestStop(); QMetaObject::invokeMethod(engine_, &Engine::Stop); status_->setText(tr("검색 중지 요청")); });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex& Index) { const auto Row = Index.data(Qt::UserRole).toJsonObject(); if (!Row.isEmpty()) emit Navigate(Row); });
    debounce_.setSingleShot(true); debounce_.setInterval(250); connect(&debounce_, &QTimer::timeout, this, &Panel::start);
    for (auto* Input : {query_, root_, include_, exclude_}) connect(Input, &QLineEdit::textChanged, this, &Panel::schedule);
    for (auto* Option : {regex_, case_, recursive_}) connect(Option, &QCheckBox::toggled, this, &Panel::schedule);
    connect(replacement_, &QLineEdit::textChanged, this, &Panel::invalidatePreview);
    connect(replaceMode_, &QCheckBox::toggled, this, [this](bool Checked) { replacement_->setVisible(Checked); replacementLabel_->setVisible(Checked); for (auto* Button : {selected_, file_, all_, undo_}) Button->setVisible(Checked); invalidatePreview(); });
    setTabOrder(query_, replacement_); setTabOrder(replacement_, selected_); setTabOrder(selected_, file_); setTabOrder(file_, all_); setTabOrder(all_, undo_);
    SetReadOnly(false); replacement_->hide(); replacementLabel_->hide(); for (auto* Button : {selected_, file_, all_, undo_}) Button->hide();
}
Panel::~Panel() { engine_->RequestStop(); QMetaObject::invokeMethod(engine_, &Engine::Shutdown); }
void Panel::Open(bool Replace, QString DefaultRoot, bool Book, QJsonArray Buffers) {
    const bool Refresh=generation_==0 || book_!=Book || defaultRoot_!=QDir::toNativeSeparators(DefaultRoot);
    starting_ = true;
    if (root_->text().isEmpty() || root_->text()==defaultRoot_ || Book || book_ != Book) root_->setText(QDir::toNativeSeparators(DefaultRoot));
    defaultRoot_=QDir::toNativeSeparators(DefaultRoot);
    book_ = Book; buffers_ = std::move(Buffers); root_->setReadOnly(Book); recursive_->setEnabled(!Book); target_->setEnabled(Book); replaceMode_->setChecked(Replace); starting_ = false;
    show(); query_->setFocus(); query_->selectAll(); if(Refresh)schedule();
}
void Panel::SetReadOnly(bool Value) { readOnly_ = Value; for (auto* Button : {selected_, file_, all_}) Button->setEnabled(true); undo_->setEnabled(!Value); }
void Panel::SetQuery(const QString& Text) { query_->setText(Text); query_->setFocus(); query_->selectAll(); }
QJsonObject Panel::Context() const {
    return {{"query",query_->text()},{"replacement",replacement_->text()},{"root",root_->text()},
        {"include",include_->text()},{"exclude",exclude_->text()},{"regex",regex_->isChecked()},
        {"case",case_->isChecked()},{"recursive",recursive_->isChecked()},{"body",target_->currentData()=="body"}};
}
void Panel::RestoreContext(const QJsonObject& Context) {
    starting_=true; query_->setText(Context["query"].toString()); replacement_->setText(Context["replacement"].toString());
    if(!book_ && !Context["root"].toString().isEmpty())root_->setText(Context["root"].toString());
    include_->setText(Context["include"].toString()); exclude_->setText(Context["exclude"].toString());
    regex_->setChecked(Context["regex"].toBool()); case_->setChecked(Context["case"].toBool()); recursive_->setChecked(Context["recursive"].toBool(true));
    target_->setCurrentIndex(Context["body"].toBool()?1:0); starting_=false; schedule(); query_->setFocus();
}
void Panel::schedule() {
    if (starting_ || applying_) return;
    ++generation_; ++previewVersion_; complete_ = false; model_->Reset(); all_->setEnabled(false);
    engine_->RequestStop(); debounce_.start();
}
void Panel::start() {
    if (applying_) return; debounce_.stop(); ++generation_; ++previewVersion_; complete_ = false; model_->Reset(); all_->setEnabled(false);
    if (sources_ && !book_) buffers_ = sources_();
    const auto BookSources = book_ && bookSources_ ? bookSources_() : std::function<QJsonArray()>{};
    const QJsonObject Request{{"query", query_->text()}, {"root", QDir::fromNativeSeparators(root_->text())}, {"book", book_}, {"body", target_->currentData().toString() == "body"}, {"regex", regex_->isChecked()}, {"case", case_->isChecked()}, {"recursive", recursive_->isChecked()}, {"include", include_->text()}, {"exclude", exclude_->text()}};
    status_->setText(tr("검색 시작…")); const auto Generation = generation_; const auto Buffers = buffers_;
    QMetaObject::invokeMethod(engine_, [Engine = engine_, Generation, Request, Buffers, BookSources] { Engine->Begin(Generation, Request, Buffers, BookSources); });
}
void Panel::invalidatePreview() { ++previewVersion_; model_->Invalidate(); }
void Panel::replace(const QString& Scope) {
    if (applying_) return;
    if (readOnly_) { if(Scope!="undo")emit EditorRequested(Context()); return; }
    if (Scope == "all" && !complete_) { status_->setText(tr("전체 검색 완료 후 모두 바꿀 수 있습니다.")); return; }
    QJsonArray Ids;
    // Persisted match ids follow append-only row order. Send selection intervals,
    // never materialize millions of QModelIndex/JSON objects for Ctrl+A.
    if(Scope=="selected" || Scope=="files")for(const auto& Range:table_->selectionModel()->selection())
        Ids.append(QJsonObject{{"first",Range.top()+1},{"last",Range.bottom()+1}});
    if ((Scope == "selected" || Scope == "files") && Ids.isEmpty()) return;
    debounce_.stop(); applying_ = true;
    const auto Generation = generation_; const auto Replacement = replacement_->text();
    QMetaObject::invokeMethod(engine_, [Engine = engine_, Generation, Scope, Ids, Replacement] {
        if (Scope == "undo") { Engine->Stop(); Engine->Undo(); } else Engine->Apply(Generation, Scope, Ids, Replacement);
    });
}
void Panel::FinishBuffer(quint64 Ticket, const QString& Error) { QMetaObject::invokeMethod(engine_, [Engine = engine_, Ticket, Error] { Engine->BufferApplied(Ticket, Error); }); }
}
