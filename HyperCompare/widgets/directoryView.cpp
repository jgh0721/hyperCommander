#include "stdafx.h"
#include "directoryView.hpp"
#include <QTimer>
#include "runtime/directoryApply.hpp"
#include <QToolButton>
#include <QMenu>
#include <QAction>
#include <QInputDialog>
#include <QFileInfo>
#include <QShortcut>
#include <QtConcurrentRun>
#include "directoryTreeModel.hpp"
#include "directoryMergeDialog.hpp"
#include <QDir>
#include <QJsonDocument>
#include <QLocale>
#include <QFontMetrics>
#include <QTableView>
#include <QTreeView>
#include <QSplitter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QScopedValueRollback>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace hc::compare
{
    namespace
    {
        class TreeDelegate final : public QStyledItemDelegate
        {
        public:
            explicit TreeDelegate(QObject* Parent) : QStyledItemDelegate(Parent) {}
            QSize sizeHint(const QStyleOptionViewItem& Option, const QModelIndex& Index) const override
            {
                auto size = QStyledItemDelegate::sizeHint(Option, Index);
                size.setHeight(std::max(28, Option.fontMetrics.height() + 10)); return size;
            }
        };
        QString statusName(const QString& Status)
        {
            static const QMap<QString, QString> labels{{"Equivalent", QObject::tr("내용 동일 · 인코딩/BOM 차이")}, {"Pending", QObject::tr("비교 대기")}, {"Error", QObject::tr("비교 불가")}, {"Equal", QObject::tr("동일")}, {"Different", QObject::tr("다름")},
                {"LeftOnly", QObject::tr("왼쪽만 존재")}, {"RightOnly", QObject::tr("오른쪽만 존재")},
                {"TypeConflict", QObject::tr("파일/폴더 충돌")}, {"NameDifferent", QObject::tr("이름 다름")},
                {"LeftChanged", QObject::tr("왼쪽 수정")}, {"RightChanged", QObject::tr("오른쪽 수정")},
                {"LeftAdded", QObject::tr("왼쪽 추가")}, {"RightAdded", QObject::tr("오른쪽 추가")},
                {"LeftDeleted", QObject::tr("왼쪽 삭제")}, {"RightDeleted", QObject::tr("오른쪽 삭제")},
                {"BothDeleted", QObject::tr("양쪽 삭제")}, {"BothSame", QObject::tr("양쪽 동일 변경")}, {"Conflict", QObject::tr("충돌")}, {"MirrorExtra", QObject::tr("출력에만 존재")}};
            return labels.value(Status, Status);
        }
        bool file(const QJsonObject& Entry) { return Entry["present"].toBool() && !Entry["directory"].toBool(); }
        bool openable(const QJsonObject& Item)
        {
            const auto left = Item["left"].toObject(), right = Item["right"].toObject();
            return Item["status"] != "Error" && (file(left) || file(right)) &&
                !left["directory"].toBool() && !right["directory"].toBool();
        }
    }
    bool DirectoryEntryMatches(const QJsonObject& Entry, const QString& Query, int Status)
    {
        const auto status = Entry["status"].toString();
        return Entry["path"].toString().contains(Query, Qt::CaseInsensitive) &&
            (Status == 0 || (Status == 1 && !DirectoryContentEqual(status)) || (Status == 2 && (status == "Conflict" || status == "TypeConflict")) || (Status == 3 && status == "Error"));
    }
    bool DirectoryEntryMatches(const DirectoryRow& Entry, const QString& Query, int Status)
    {
        const auto status = Entry.Status();
        return Entry.path_.contains(Query, Qt::CaseInsensitive) &&
            (Status == 0 || (Status == 1 && !DirectoryContentEqual(status)) || (Status == 2 && (status == "Conflict" || status == "TypeConflict")) || (Status == 3 && status == "Error"));
    }
    QString DirectoryStatusName(const QString& Status) { return statusName(Status); }
    void DirectoryModel::SetEntries(DirectoryEntries Entries) { beginResetModel(); entries_ = std::move(Entries); endResetModel(); }
    int DirectoryModel::rowCount(const QModelIndex& Parent) const { return Parent.isValid() ? 0 : int(entries_.size()); }
    int DirectoryModel::columnCount(const QModelIndex& Parent) const { return Parent.isValid() ? 0 : 5; }
    QJsonObject DirectoryModel::Entry(int Row) const { return Row >= 0 && Row < entries_.size() ? entries_[Row].toObject() : QJsonObject{}; }
    QVariant DirectoryModel::data(const QModelIndex& Index, int Role) const
    {
        if (!Index.isValid() || Index.row() < 0 || Index.row() >= rowCount() || Index.column() < 0 || Index.column() >= columnCount()) return {};
        const auto& row = entries_.at(Index.row());
        if (Role == Qt::UserRole) return row.toObject();
        if (Role == Qt::ToolTipRole)
            return row.path_ + '\n' + statusName(row.Status()) + '\n' +
                tr("LEFT↔RIGHT: %1 · LEFT↔BASE: %2 · RIGHT↔BASE: %3").arg(row.Status(1), row.Status(2), row.Status(3)) + '\n' + QString::fromUtf8(QJsonDocument(row.errors_).toJson(QJsonDocument::Compact));
        if (Role != Qt::DisplayRole && Role != Qt::AccessibleTextRole) return {};
        if (Index.column() == 0) return row.path_;
        if (Index.column() == 1) return statusName(row.Status());
        const auto& side = row.sides_[Index.column() - 2];
        if (!side.present_) return row.Status() == "Error" ? tr("확인 불가") : tr("없음");
        return side.directory_ ? tr("폴더") : tr("%1 B").arg(QLocale().toString(qulonglong(side.size_)));
    }
    QVariant DirectoryModel::headerData(int Section, Qt::Orientation Orientation, int Role) const
    {
        if (Role != Qt::DisplayRole) return {};
        if (Orientation == Qt::Vertical) return Section + 1;
        return Section >= 0 && Section < 5 ? QStringList{tr("경로"), tr("상태"), "LEFT", "RIGHT", "BASE"}[Section] : QVariant{};
    }
    void DirectoryFilter::SetQuery(QString Query, int Status)
    { beginFilterChange(); query_ = std::move(Query); status_ = Status; endFilterChange(Direction::Rows); }
    bool DirectoryFilter::filterAcceptsRow(int Row, const QModelIndex& Parent) const
    {
        Q_UNUSED(Parent);
        return static_cast<const DirectoryModel*>(sourceModel())->Matches(Row, query_, status_);
    }
    DirectoryView::DirectoryView(QWidget* Parent) : QWidget(Parent)
    {
        setObjectName("directoryView"); auto* box = new QVBoxLayout(this);
        roots_ = new QLabel(this); roots_->setTextFormat(Qt::PlainText); roots_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        roots_->setWordWrap(true); box->addWidget(roots_);
        auto* searchRow = new QHBoxLayout(); auto* query = new QLineEdit(this); query->setObjectName("directoryQuery");
        query->setPlaceholderText(tr("경로에서 찾기")); query->setAccessibleName(tr("디렉터리 경로 검색")); query->setClearButtonEnabled(true);
        auto* status = new QComboBox(this); status->setObjectName("directoryStatusFilter"); status->setAccessibleName(tr("디렉터리 상태 필터"));
        status->addItems({tr("모든 항목"), tr("차이만"), tr("충돌만"), tr("비교 불가만")});
        openButton_ = new QPushButton(tr("파일 비교 열기"), this); openButton_->setObjectName("directoryOpenFile");
        openButton_->setToolTip(tr("한쪽에만 있는 파일은 반대편을 빈 문서로 열어 비교·병합합니다. 저장할 때 해당 경로에 파일을 만듭니다."));
        auto* merge = new QPushButton(tr("병합 계획…"), this); merge->setObjectName("directoryMergePlan");
        connect(merge, &QPushButton::clicked, this, [this] {
            auto* dialog = new DirectoryMergeDialog(request_, comparison_, this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->show();
        });
        searchRow->addWidget(merge);
        for (int source = 0; source < 2; ++source)
        {
            auto* transfer = new QPushButton(source == 0 ? tr("선택 →") : tr("← 선택"), this);
            transfer->setObjectName(source == 0 ? "directoryCopyRight" : "directoryCopyLeft");
            transfer->setToolTip(source == 0 ? tr("선택한 왼쪽 항목을 오른쪽 폴더로 복사하는 계획 검토") : tr("선택한 오른쪽 항목을 왼쪽 폴더로 복사하는 계획 검토"));
            transfer->setEnabled(false); searchRow->addWidget(transfer);
            connect(transfer, &QPushButton::clicked, this, [this, source] {
                if (streaming_ || !comparison_.directoryComplete_) return;
                const auto path = currentEntry()["path"].toString(); if (path.isEmpty()) return;
                auto* dialog = new DirectoryMergeDialog(request_, comparison_, this); dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->statusChanged_ = [this](int Code) { if (Code == 0 && levelChanged_) QTimer::singleShot(0, this, [this] { if (levelChanged_) levelChanged_(request_.directory_.level_); }); };
                dialog->show(); dialog->PrepareTransfer(source, path);
            });
        }
        auto* rename = new QToolButton(this); rename->setText(tr("이름 변경")); rename->setPopupMode(QToolButton::InstantPopup);
        rename->setObjectName("directoryRenameMenu"); rename->setAccessibleName(tr("선택 항목 이름 변경")); auto* menu = new QMenu(rename); rename->setMenu(menu);
        for (int side = 0; side < 2; ++side)
        { auto* action = menu->addAction(side == 0 ? tr("왼쪽 이름 변경…") : tr("오른쪽 이름 변경…"), this, [this, side] { promptRename(side); }); action->setObjectName(side == 0 ? "directoryRenameLeft" : "directoryRenameRight"); }
        searchRow->addWidget(rename);
        connect(&renameWatcher_, &QFutureWatcher<Result>::finished, this, [this] {
            renaming_ = false; mutation_ = renameWatcher_.result(); setEnabled(true);
            if (mutation_.error_.isEmpty()) comparison_.directoryComplete_ = false;
            updateSelection(); summary_->setText(mutation_.error_.isEmpty() ? tr("이름 변경 완료 — 비교 갱신 중") : tr("이름 변경 실패: %1").arg(mutation_.error_));
            if (mutation_.error_.isEmpty() && !renameCanceled_->load() && levelChanged_) levelChanged_(request_.directory_.level_);
            if (activityChanged_) activityChanged_();
        });
        viewMode_ = new QComboBox(this); viewMode_->setObjectName("directoryViewMode");
        viewMode_->setAccessibleName(tr("디렉터리 표시 방식")); viewMode_->addItems({tr("좌우 폴더 트리"), tr("경로 목록")});
        searchRow->addWidget(viewMode_);
        auto* level = new QComboBox(this); level->setObjectName("directoryComparisonLevel"); level->setAccessibleName(tr("디렉터리 비교 기준"));
        level->addItem(tr("텍스트 내용"), "text"); level->addItem(tr("바이트"), "bytes"); level->addItem(tr("크기·시각"), "metadata");
        level->setToolTip(tr("기준을 변경하면 다시 비교합니다. 텍스트 기준은 인코딩/BOM만 다른 파일을 내용 동일로 판정합니다."));
        searchRow->addWidget(level);
        connect(level, &QComboBox::currentIndexChanged, this, [this, level] { if (!streaming_ && levelChanged_) levelChanged_(level->currentData().toString()); });
        searchRow->addWidget(query, 1); searchRow->addWidget(status); searchRow->addWidget(openButton_); box->addLayout(searchRow);
        model_ = new DirectoryModel(this); model_->setObjectName("directoryModel"); filter_ = new DirectoryFilter(this); filter_->setSourceModel(model_);
        filter_->setSortCaseSensitivity(Qt::CaseInsensitive);
        table_ = new QTableView(this); table_->setObjectName("directoryTable"); table_->setAccessibleName(tr("디렉터리 비교 결과"));
        table_->setModel(filter_); table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setAlternatingRowColors(true); table_->setSortingEnabled(true); table_->sortByColumn(0, Qt::AscendingOrder);
        table_->verticalHeader()->setVisible(false); table_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int column = 1; column < 5; ++column) table_->setColumnWidth(column, 140);
        box->addWidget(table_, 1); table_->hide();
        treeModel_ = new DirectoryTreeModel(this); treeModel_->setObjectName("directoryTreeModel");
        treeFilter_ = new DirectoryTreeFilter(this); treeFilter_->setSourceModel(treeModel_);
        treeBox_ = new QSplitter(Qt::Horizontal, this); treeBox_->setObjectName("directoryTreeSplitter"); treeBox_->setChildrenCollapsible(false);
        for (int side = 0; side < 2; ++side)
        {
            auto* pane = new QWidget(treeBox_); auto* column = new QVBoxLayout(pane); column->setContentsMargins(0, 0, 0, 0);
            auto* label = new QLabel(pane); treeRoots_[side] = label; label->setTextFormat(Qt::PlainText);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
            label->setMinimumWidth(0); label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); column->addWidget(label);
            auto* tree = new QTreeView(pane); trees_[side] = tree;
            tree->setObjectName(side ? "directoryRightTree" : "directoryLeftTree");
            tree->setAccessibleName(side ? tr("오른쪽 폴더 트리") : tr("왼쪽 폴더 트리")); tree->setModel(treeFilter_);
            auto* renameKey = new QShortcut(QKeySequence(Qt::Key_F2), tree); renameKey->setContext(Qt::WidgetWithChildrenShortcut);
            connect(renameKey, &QShortcut::activated, this, [this, side] { promptRename(side); });
            tree->setUniformRowHeights(true); tree->setItemDelegate(new TreeDelegate(tree));
            tree->setSelectionMode(QAbstractItemView::SingleSelection); tree->setSelectionBehavior(QAbstractItemView::SelectRows);
            tree->setEditTriggers(QAbstractItemView::NoEditTriggers); tree->setAlternatingRowColors(true);
            tree->setExpandsOnDoubleClick(false);
            tree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
            tree->setTreePosition(side * 3); tree->header()->setStretchLastSection(false);
            for (int i = 0; i < 6; ++i) tree->setColumnHidden(i, i / 3 != side);
            tree->header()->setSectionResizeMode(side * 3, QHeaderView::Stretch);
            tree->setColumnWidth(side * 3 + 1, 140); tree->setColumnWidth(side * 3 + 2, 100);
            column->addWidget(tree, 1);
            connect(tree, &QTreeView::activated, this, [this] { OpenCurrent(); });
            connect(tree, &QTreeView::doubleClicked, this, [this, tree](const QModelIndex& Index) {
                const auto branch = Index.siblingAtColumn(0);
                if (branch.isValid() && treeFilter_->hasChildren(branch)) tree->setExpanded(branch, !tree->isExpanded(branch));
            });
        }
        for (int side = 0; side < 2; ++side)
        {
            auto* tree = trees_[side]; auto* other = trees_[1 - side];
            const auto expansion = [this, other](const QModelIndex& Index, bool Expanded) {
                if (syncing_) return;
                const auto path = Index.data(DirectoryTreeModel::PathRole).toString();
                if (Expanded)
                {
                    expandedPaths_.insert(path);
                    if (streaming_ && request_.directoryProgress_) request_.directoryProgress_->Prioritize(path);
                }
                else expandedPaths_.remove(path);
                QScopedValueRollback guard(syncing_, true); other->setExpanded(Index, Expanded);
            };
            connect(tree, &QTreeView::expanded, this, [expansion](const QModelIndex& Index) { expansion(Index, true); });
            connect(tree, &QTreeView::collapsed, this, [expansion](const QModelIndex& Index) { expansion(Index, false); });
            connect(tree->verticalScrollBar(), &QScrollBar::valueChanged, other->verticalScrollBar(), &QScrollBar::setValue);
            connect(tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this, other, side](const QModelIndex& Index) {
                if (syncing_) return;
                { QScopedValueRollback guard(syncing_, true); other->setCurrentIndex(Index.isValid() ? Index.siblingAtColumn((1 - side) * 3) : QModelIndex{}); }
                updateSelection();
            });
        }
        connect(viewMode_, &QComboBox::currentIndexChanged, this, [this](int Index) { switchView(Index == 0); });
        box->addWidget(treeBox_, 1); summary_ = new QLabel(this); summary_->setTextFormat(Qt::PlainText); box->addWidget(summary_);
        const auto change = [this, query, status] {
            filter_->SetQuery(query->text(), status->currentIndex()); treeFilter_->SetQuery(query->text(), status->currentIndex());
            if ((!query->text().isEmpty() || status->currentIndex()) && treeModel_->VisibleEntries() <= 2000)
                for (auto* tree : trees_) tree->expandAll();
            updateSelection();
        };
        connect(query, &QLineEdit::textChanged, this, change); connect(status, &QComboBox::currentIndexChanged, this, change);
        connect(table_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { updateSelection(); });
        connect(table_, &QTableView::doubleClicked, this, [this] { OpenCurrent(); });
        connect(openButton_, &QPushButton::clicked, this, [this] { OpenCurrent(); });
        updateSelection();
    }
    DirectoryView::~DirectoryView() { Cancel(); renameWatcher_.waitForFinished(); }
    void DirectoryView::promptRename(int Side)
    {
        if (Busy() || streaming_ || !comparison_.directoryComplete_) return;
        const auto item = currentEntry(); const auto entry = item[Side == 0 ? "left" : "right"].toObject(); if (!entry["present"].toBool()) return;
        const auto path = entry["path"].toString(item["path"].toString()); bool ok = false;
        const auto name = QInputDialog::getText(this, Side == 0 ? tr("왼쪽 이름 변경") : tr("오른쪽 이름 변경"), tr("새 이름:"), QLineEdit::Normal, QFileInfo(path).fileName(), &ok);
        if (ok) RenameCurrent(Side, name);
    }
    void DirectoryView::RenameCurrent(int Side, const QString& Name)
    {
        if (Busy() || streaming_ || !comparison_.directoryComplete_ || Side < 0 || Side > 1) return;
        const auto path = currentEntry()["path"].toString(); if (path.isEmpty()) return;
        renameCanceled_->store(false); renaming_ = true; setEnabled(false); summary_->setText(tr("이름 변경 중…"));
        renameWatcher_.setFuture(QtConcurrent::run([input = request_, comparison = comparison_, cancel = renameCanceled_, Side, path, Name] {
            try { Budget limits; limits.canceled_ = cancel.get(); limits.deadline_ = input.timeoutMs_ == 0 ? std::chrono::steady_clock::time_point::max() : std::chrono::steady_clock::now() + std::chrono::milliseconds(input.timeoutMs_);
                return RenameDirectoryEntry(input, comparison, Side, path, Name, limits); }
            catch (const Failure& error) { Result result; result.error_ = QString::fromStdString(error.code_); return result; }
            catch (const std::exception& error) { Result result; result.error_ = QString::fromUtf8(error.what()); return result; }
        }));
        if (activityChanged_) activityChanged_();
    }
    void DirectoryView::BeginComparison(const Request& Input)
    {
        Result pending; pending.directoryComplete_ = false; pending.directoryLevel_ = Input.directory_.level_;
        SetComparison(Input, pending); streaming_ = true; findChild<QComboBox*>("directoryComparisonLevel")->setEnabled(false); viewMode_->setCurrentIndex(0); viewMode_->setEnabled(false);
        findChild<QLineEdit*>("directoryQuery")->clear(); findChild<QLineEdit*>("directoryQuery")->setEnabled(false);
        findChild<QComboBox*>("directoryStatusFilter")->setCurrentIndex(0); findChild<QComboBox*>("directoryStatusFilter")->setEnabled(false);
        roots_->setText(tr("폴더를 찾는 대로 표시합니다. 비교 대기 항목은 존재 여부와 내용을 확인 중입니다."));
    }
    void DirectoryView::ApplyProgress(const DirectoryProgress& Progress)
    {
        if (!streaming_) return;
        treeModel_->ApplyProgress(Progress);
        const auto phase = Progress.phase_ == "scan" ? tr("폴더 열거 중") : Progress.phase_ == "compare" ? tr("파일 내용 비교 중") : tr("변경 여부 재확인 중");
        summary_->setText(Progress.total_ ? tr("%1 — %2 / %3 · 표시 %4개").arg(phase).arg(Progress.processed_).arg(Progress.total_).arg(treeModel_->VisibleEntries()) :
            tr("%1 — 발견 %2개 · 표시 %3개").arg(phase).arg(Progress.processed_).arg(treeModel_->VisibleEntries()));
        openButton_->setEnabled(openable(currentEntry()));
    }
    void DirectoryView::EndProgress(bool PreserveRows)
    {
        if (streaming_ && PreserveRows)
        {
            treeModel_->FinishProgress();
            listDeferred_ = treeMode_ && treeModel_->Entries().size() > 10000;
            if (!listDeferred_) model_->SetEntries(treeModel_->Entries());
        }
        streaming_ = false; viewMode_->setEnabled(true); findChild<QComboBox*>("directoryComparisonLevel")->setEnabled(true);
        findChild<QLineEdit*>("directoryQuery")->setEnabled(true); findChild<QComboBox*>("directoryStatusFilter")->setEnabled(true);
    }
    void DirectoryView::SetComparison(const Request& Input, const Result& Value)
    {
        const bool wasStreaming = streaming_;
        const auto selectedPath = wasStreaming ? currentEntry()["path"].toString() : QString{};
        const bool reuse = wasStreaming && Value.directoryComplete_ && Value.completion_ == "Complete" && treeModel_->Entries().size() == Value.entries_.size();
        const auto expanded = expandedPaths_; if (!reuse) expandedPaths_.clear();
        EndProgress(false); request_ = Input; comparison_ = Value;
        auto* level = findChild<QComboBox*>("directoryComparisonLevel");
        { const QSignalBlocker blocker(level); level->setCurrentIndex(level->findData(Input.directory_.level_)); }
        findChild<QPushButton*>("directoryMergePlan")->setEnabled(Value.directoryComplete_); table_->setColumnHidden(4, Input.base_.isEmpty());
        if (reuse) treeModel_->FinishProgress(); else treeModel_->SetEntries(Value.entries_);
        // A hidden flat proxy sorts every row when populated. Build it only when
        // requested for large trees, using the same entry order for selection mapping.
        listDeferred_ = treeMode_ && Value.entries_.size() > 10000;
        model_->SetEntries(listDeferred_ ? DirectoryEntries{} : treeModel_->Entries());
        treeFilter_->SetQuery(findChild<QLineEdit*>("directoryQuery")->text(), findChild<QComboBox*>("directoryStatusFilter")->currentIndex());
        for (int side = 0; side < 2; ++side)
        {
            const auto path = side ? Input.file2_ : Input.file1_;
            treeRoots_[side]->setText((side ? tr("RIGHT  ") : tr("LEFT  ")) + path); treeRoots_[side]->setToolTip(path);
            if (!wasStreaming) trees_[side]->expandToDepth(0);
        }
        if (wasStreaming && !reuse)
        {
            for (const auto& path : expanded)
            {
                const auto index = treeFilter_->mapFromSource(treeModel_->ForPath(path));
                if (index.isValid()) for (auto* tree : trees_) tree->expand(index);
            }
            if (!selectedPath.isEmpty()) selectTree(treeFilter_->mapFromSource(treeModel_->ForPath(selectedPath)));
        }
        updateRoots(); updateSelection();
    }
    void DirectoryView::updateRoots()
    {
        QStringList lines;
        if (!treeMode_) lines << tr("LEFT  %1").arg(request_.file1_) << tr("RIGHT  %1").arg(request_.file2_);
        if (!request_.base_.isEmpty()) lines << tr("BASE  %1").arg(request_.base_);
        lines << ((comparison_.directoryLevel_ == "metadata" ? tr("메타데이터 비교 — 크기·수정 시각만 검사, 내용 동일을 보장하지 않음") : comparison_.directoryLevel_ == "text" ? tr("텍스트 내용 비교 · 인코딩/BOM 차이 구분 · 바이너리/16 MiB 초과는 바이트 비교") : tr("바이트 비교")) +
            (comparison_.directoryFiltered_ ? tr(" · 포함/제외 필터 적용") : QString{}));
        if (!comparison_.directoryComplete_) lines << tr("일부 항목을 비교하지 못했습니다. ‘비교 불가만’ 필터에서 경로와 오류를 확인하십시오. 병합 계획은 제한됩니다.");
        QStringList issues;
        for (const auto& warning : comparison_.warnings_)
        {
            const auto item = warning.toObject();
            if (issues.size() < 32 && item.contains("root")) issues << QDir(item["root"].toString()).filePath(item["path"].toString()) + " — " + item["error"].toString();
        }
        roots_->setToolTip(issues.join('\n'));
        roots_->setText(lines.join('\n'));
    }
    void DirectoryView::SetListFont(const QFont& Font)
    {
        table_->setFont(Font); table_->verticalHeader()->setDefaultSectionSize(std::max(28, QFontMetrics(Font).height() + 10));
        for (auto* tree : trees_) { tree->setFont(Font); tree->doItemsLayout(); }
    }
    QJsonObject DirectoryView::currentEntry() const
    {
        return treeMode_ ? treeModel_->Entry(treeFilter_->mapToSource(trees_[0]->currentIndex())) :
            model_->Entry(filter_->mapToSource(table_->currentIndex()).row());
    }
    void DirectoryView::selectTree(const QModelIndex& Index)
    {
        if (!Index.isValid()) return;
        for (auto parent = Index.parent(); parent.isValid(); parent = parent.parent())
            for (auto* tree : trees_) tree->setExpanded(parent, true);
        trees_[0]->setCurrentIndex(Index);
        for (int side = 0; side < 2; ++side) trees_[side]->scrollTo(Index.siblingAtColumn(side * 3), QAbstractItemView::EnsureVisible);
    }
    void DirectoryView::switchView(bool Tree)
    {
        if (Tree == treeMode_) return;
        const int row = treeMode_ ? trees_[0]->currentIndex().data(DirectoryTreeModel::EntryRole).toInt() : filter_->mapToSource(table_->currentIndex()).row();
        const bool selected = treeMode_ ? trees_[0]->currentIndex().isValid() : table_->currentIndex().isValid();
        if (!Tree && listDeferred_) { model_->SetEntries(treeModel_->Entries()); listDeferred_ = false; }
        treeMode_ = Tree; treeBox_->setVisible(Tree); table_->setVisible(!Tree); updateRoots();
        if (selected)
        {
            if (Tree) selectTree(treeFilter_->mapFromSource(treeModel_->ForEntry(row)));
            else { const auto index = filter_->mapFromSource(model_->index(row, 0)); table_->setCurrentIndex(index); table_->scrollTo(index); }
        }
        updateSelection();
    }
    void DirectoryView::updateSelection()
    {
        const auto item = currentEntry();
        for (int side = 0; side < 2; ++side)
            findChild<QPushButton*>(side == 0 ? "directoryCopyRight" : "directoryCopyLeft")->setEnabled(!streaming_ && comparison_.directoryComplete_ && request_.base_.isEmpty() && item[side == 0 ? "left" : "right"].toObject()["present"].toBool());
        for (int side = 0; side < 2; ++side)
            findChild<QAction*>(side == 0 ? "directoryRenameLeft" : "directoryRenameRight")->setEnabled(!Busy() && !streaming_ && comparison_.directoryComplete_ && item[side == 0 ? "left" : "right"].toObject()["present"].toBool());
        if (streaming_) { openButton_->setEnabled(openable(item)); return; }
        openButton_->setEnabled(openable(item));
        summary_->setText((treeMode_ ? tr("일치 %1 / 전체 %2개 · F7 다음 차이 · Shift+F7 이전 차이") : tr("표시 %1 / 전체 %2개 · F7 다음 차이 · Shift+F7 이전 차이")).arg(treeMode_ ? treeModel_->VisibleEntries() : filter_->rowCount()).arg(treeModel_->Entries().size()));
    }
    void DirectoryView::OpenCurrent()
    {
        const auto item = currentEntry();
        if (!openable(item) || !open_) return;
        if (streaming_ && request_.directoryProgress_) request_.directoryProgress_->Prioritize(item["path"].toString());
        Request next; next.mode_ = "compare"; next.encoding_ = request_.encoding_; next.theme_ = request_.theme_;
        next.allowMissingInputs_ = {!file(item["left"].toObject()), !file(item["right"].toObject())};
        next.file1_ = QDir(request_.file1_).filePath(item["left"].toObject()["path"].toString(item["path"].toString()));
        next.file2_ = QDir(request_.file2_).filePath(item["right"].toObject()["path"].toString(item["path"].toString()));
        next.inputEncodings_ = request_.inputEncodings_;
        if (file(item["base"].toObject()) || (item["status"] == "Pending" && !request_.base_.isEmpty()))
            next.base_ = QDir(request_.base_).filePath(item["base"].toObject()["path"].toString(item["path"].toString()));
        open_(std::move(next));
    }
    void DirectoryView::NavigateDifference(int Direction)
    {
        if (treeMode_)
        {
            selectTree(treeFilter_->mapFromSource(treeModel_->NextDifference(treeFilter_->mapToSource(trees_[0]->currentIndex()), Direction))); return;
        }
        const int count = filter_->rowCount(); if (!count || !Direction) return;
        const auto current = table_->currentIndex().row();
        for (int offset = 1; offset <= count; ++offset)
        {
            const int row = current < 0 ? (Direction > 0 ? offset - 1 : count - offset) : (current + (Direction > 0 ? offset : -offset) + count) % count;
            const auto index = filter_->index(row, 0); const auto entry = model_->Entry(filter_->mapToSource(index).row());
            if (!DirectoryContentEqual(entry["status"].toString())) { table_->setCurrentIndex(index); table_->scrollTo(index); return; }
        }
    }
}
