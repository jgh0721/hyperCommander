#include "stdafx.h"
#include "archiveCompareView.hpp"
#include "runtime/controllerMemory.hpp"
#include "runtime/saveRecovery.hpp"
#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QtConcurrentRun>

namespace hc::compare
{
    namespace
    {
        class ArchiveFilter final : public QSortFilterProxyModel
        {
        public:
            using QSortFilterProxyModel::QSortFilterProxyModel;
            void SetQuery(QString Query, bool Different) { beginFilterChange(); query_ = std::move(Query); different_ = Different; endFilterChange(Direction::Rows); }
        protected:
            bool filterAcceptsRow(int Row, const QModelIndex& Parent) const override
            {
                const auto item = sourceModel()->data(sourceModel()->index(Row, 0, Parent), Qt::UserRole).toJsonObject();
                return item["path"].toString().contains(query_, Qt::CaseInsensitive) && (!different_ || item["state"] != "Equal");
            }
        private:
            QString query_; bool different_ = false;
        };
    }
    QVariant ArchiveModel::data(const QModelIndex& Index, int Role) const
    {
        if (!Index.isValid() || Index.row() < 0 || Index.row() >= entries_.size() || Index.column() < 0 || Index.column() >= 4) return {};
        const auto item = entries_[Index.row()].toObject();
        if (Role == Qt::UserRole) return item;
        if (Role == Qt::UserRole + 1)
        {
            if (Index.column() == 0) return item["path"].toString();
            if (Index.column() == 1) return item["state"].toString();
            const auto entry = item[Index.column() == 2 ? "left" : "right"].toObject();
            return entry.isEmpty() ? qint64(-2) : entry["directory"].toBool() ? qint64(-1) : entry["size"].toString().toLongLong();
        }
        if (Role == Qt::ToolTipRole) return item["path"].toString();
        if (Role != Qt::DisplayRole && Role != Qt::AccessibleTextRole) return {};
        if (Index.column() == 0) return item["path"].toString();
        if (Index.column() == 1)
        {
            const auto state = item["state"].toString();
            return state == "Equal" ? tr("동일") : state == "Changed" ? tr("내용 다름") : state == "TypeChanged" ? tr("종류 다름") : state == "LeftOnly" ? tr("왼쪽만 존재") : tr("오른쪽만 존재");
        }
        const auto entry = item[Index.column() == 2 ? "left" : "right"].toObject();
        return entry.isEmpty() ? tr("없음") : entry["directory"].toBool() ? tr("폴더") : tr("%1 B").arg(QLocale().toString(entry["size"].toString().toULongLong()));
    }
    QVariant ArchiveModel::headerData(int Section, Qt::Orientation Orientation, int Role) const
    {
        if (Orientation == Qt::Horizontal && Role == Qt::DisplayRole && Section >= 0 && Section < 4) return QStringList{tr("내부 경로"), tr("상태"), "LEFT", "RIGHT"}[Section];
        return {};
    }
    ArchiveCompareView::ArchiveCompareView(const QJsonObject& Comparison, const QStringList& Paths, QWidget* Parent) : ExtensionView(Parent)
    {
        setObjectName("archiveCompareView"); auto* layout = new QVBoxLayout(this);
        auto* roots = new QLabel(Paths.join("\n"), this); roots->setTextFormat(Qt::PlainText); roots->setWordWrap(true); layout->addWidget(roots);
        auto* summary = new QLabel((Comparison["contentsEqual"].toBool() ? (Comparison["rawEqual"].toBool() ? tr("파일·내부 항목 동일") : tr("내부 항목 동일 · 압축 파일 바이트 다름")) : tr("내부 항목 내용 또는 구성 다름")) + tr(" · 읽기 전용"), this);
        summary->setObjectName("archiveSummary"); layout->addWidget(summary);
        auto* row = new QHBoxLayout; auto* search = new QLineEdit(this); search->setObjectName("archiveQuery"); search->setPlaceholderText(tr("내부 경로 검색")); search->setAccessibleName(tr("압축 내부 경로 검색"));
        auto* state = new QComboBox(this); state->setObjectName("archiveStatus"); state->addItems({tr("모든 항목"), tr("차이만")}); row->addWidget(search, 1); row->addWidget(state); layout->addLayout(row);
        auto* model = new ArchiveModel(Comparison["entries"].toArray(), this); model_ = model; auto* filter = new ArchiveFilter(this); filter->setSourceModel(model); filter->setSortRole(Qt::UserRole + 1);
        table_ = new QTableView(this); table_->setObjectName("archiveTable"); table_->setModel(filter); table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->verticalHeader()->hide(); table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table_->setColumnWidth(1, 150); table_->setColumnWidth(2, 140); table_->setColumnWidth(3, 140); table_->setSortingEnabled(true); layout->addWidget(table_, 1);
        connect(search, &QLineEdit::textChanged, this, [filter, search, state] { filter->SetQuery(search->text(), state->currentIndex() == 1); });
        connect(state, &QComboBox::currentIndexChanged, this, [filter, search, state] { filter->SetQuery(search->text(), state->currentIndex() == 1); });
    }
    ArchiveCompareView::ArchiveCompareView(Request Input, std::shared_ptr<ArchiveSession> Session, QWidget* Parent)
        : ArchiveCompareView(Session->comparison_, {Input.file1_, Input.file2_}, Parent)
    {
        input_ = std::move(Input); session_ = std::move(Session); for (int side = 0; side < 2; ++side) saved_[side] = session_->originals_[side].raw_; outputNeverSaved_ = input_.mode_ == "merge";
        table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        auto* actions = new QHBoxLayout(); static_cast<QVBoxLayout*>(layout())->insertLayout(1, actions);
        const auto button = [&](const QString& Text, const QString& Name, auto Function) { auto* button = new QPushButton(Text, this); button->setObjectName(Name); actions->addWidget(button); connect(button, &QPushButton::clicked, this, Function); };
        button(tr("선택 →"), "archiveCopyRight", [this] { CopySelected(0); }); button(tr("← 선택"), "archiveCopyLeft", [this] { CopySelected(1); });
        button(tr("왼쪽 항목 제거"), "archiveRemoveLeft", [this] { RemoveSelected(0); }); button(tr("오른쪽 항목 제거"), "archiveRemoveRight", [this] { RemoveSelected(1); });
        button(tr("왼쪽 저장"), "archiveSaveLeft", [this] { Save(0); }); button(tr("오른쪽 저장"), "archiveSaveRight", [this] { SaveActive(); });
        button(tr("다른 이름으로…"), "archiveSaveAs", [this] { const auto path = QFileDialog::getSaveFileName(this, tr("오른쪽 압축 작업본 저장"), input_.file2_); if (!path.isEmpty()) Save(1, path); });
        button(tr("실행 취소"), "archiveUndo", [this] { Undo(false); }); button(tr("다시 실행"), "archiveRedo", [this] { Undo(true); });
        button(tr("항목 비교"), "archiveOpen", [this] { OpenCurrent(); });
        button(tr("상위 압축"), "archiveParent", [this] { if (Busy()) return; run([input = input_, current = session_](const Budget& Limits) { auto next = std::make_shared<ArchiveSession>(*current); bool changed = false; for (auto& chain : next->members_) if (!chain.isEmpty()) { chain.removeLast(); changed = true; } if (changed) next->comparison_ = ArchiveOperation(input, *next, {}, Limits); return Response{next}; }); });
        button(tr("취소"), "archiveCancel", [this] { Cancel(); });
        connect(table_, &QTableView::doubleClicked, this, [this] { OpenCurrent(); }); refresh();
    }
    ArchiveCompareView::~ArchiveCompareView() { canceled_->store(true); watcher_.waitForFinished(); }
    bool ArchiveCompareView::Dirty() const { return session_ && (outputNeverSaved_ || session_->working_ != saved_); }
    void ArchiveCompareView::refresh()
    {
        if (!session_) return; model_->SetEntries(session_->comparison_["entries"].toArray());
        findChild<QLabel*>("archiveSummary")->setText(tr("ZIP·7z 작업본 · 항목은 순번으로 식별 · 중첩 깊이 %1/%2 · %3 · 원본 쓰기는 저장할 때만 수행")
            .arg(session_->members_[0].size()).arg(session_->members_[1].size()).arg(Dirty() ? tr("수정됨") : tr("저장됨")));
        if (input_.readOnlyInputs_) { findChild<QLabel*>("archiveSummary")->setText(tr("읽기 전용 압축 항목 비교")); for (auto* button : findChildren<QPushButton*>()) if (!QStringList{"archiveOpen", "archiveParent", "archiveCancel"}.contains(button->objectName())) button->setEnabled(false); }
        else if (input_.mode_ == "merge") for (const auto& name : {"archiveCopyLeft", "archiveRemoveLeft", "archiveSaveLeft"}) findChild<QPushButton*>(name)->setEnabled(false);
    }
    void ArchiveCompareView::run(std::function<Response(const Budget&)> Work, bool Mutation)
    {
        if (Busy()) return; busy_ = true; canceled_ = std::make_shared<std::atomic_bool>(false);
        for (auto* button : findChildren<QPushButton*>()) button->setEnabled(button->objectName() == "archiveCancel"); table_->setEnabled(false); if (stateChanged_) stateChanged_();
        disconnect(&watcher_, nullptr, this, nullptr);
        connect(&watcher_, &QFutureWatcher<Response>::finished, this, [this, Mutation] {
            const auto result = watcher_.result(); busy_ = false;
            for (auto* button : findChildren<QPushButton*>()) button->setEnabled(true); table_->setEnabled(true);
            if (!result.error_.isEmpty()) findChild<QLabel*>("archiveSummary")->setText(result.error_);
            else if (result.session_) {
                if (Mutation) { undo_.push_back(session_); if (undo_.size() > 8) undo_.erase(undo_.begin()); redo_.clear(); }
                session_ = result.session_; if (result.saved_ >= 0) { saved_[result.saved_] = session_->working_[result.saved_]; if (result.saved_) { input_.file2_ = session_->originals_[1].path_; outputNeverSaved_ = false; } else input_.file1_ = session_->originals_[0].path_; }
                refresh();
            }
            if (stateChanged_) stateChanged_(); if (result.open_ && open_) open_(*result.open_, result.title_);
        });
        watcher_.setFuture(QtConcurrent::run([work = std::move(Work), canceled = canceled_, memory = input_.memoryMiB_] {
            try { Budget limits; limits.canceled_ = canceled.get(); limits.deadline_ = std::chrono::steady_clock::time_point::max(); ConfigureMemoryBudget(limits, memory); return work(limits); }
            catch (const SaveRecoveryFailure& error) { Response result; result.error_ = QString::fromUtf8(error.what()) + tr(" · 복구 기록: ") + QString::fromUtf8(QJsonDocument(error.recovery_).toJson(QJsonDocument::Compact)); return result; }
            catch (const std::exception& error) { Response result; result.error_ = QString::fromUtf8(error.what()); return result; }
        }));
    }
    QJsonArray ArchiveCompareView::selectedChanges(int Side, bool Remove) const
    {
        QSet<int> ordinals; QStringList prefixes;
        for (const auto& index : table_->selectionModel()->selectedRows()) { const auto entry = index.data(Qt::UserRole).toJsonObject()[Side ? "right" : "left"].toObject(); if (entry.isEmpty()) continue; ordinals.insert(entry["ordinal"].toInt()); if (entry["directory"].toBool()) prefixes.append(entry["path"].toString() + '/'); }
        for (const auto& value : session_->comparison_["entries"].toArray()) { const auto entry = value.toObject()[Side ? "right" : "left"].toObject(); for (const auto& prefix : prefixes) if (entry["path"].toString().startsWith(prefix)) ordinals.insert(entry["ordinal"].toInt()); }
        QJsonArray changes; for (const auto ordinal : ordinals) changes.append(QJsonObject{{"side", Side}, {"ordinal", ordinal}, {"remove", Remove}}); return changes;
    }
    void ArchiveCompareView::CopySelected(int Source)
    {
        if (!session_ || Busy() || input_.readOnlyInputs_ || (input_.mode_ == "merge" && Source == 1)) return; const auto changes = selectedChanges(Source, false); if (changes.isEmpty()) return; activeSide_ = 1 - Source;
        run([input = input_, current = session_, changes, target = 1 - Source](const Budget& Limits) {
            auto next = std::make_shared<ArchiveSession>(*current); const auto rewritten = ArchiveOperation(input, *next, {{"operation", "rewrite"}, {"target", target}, {"changes", changes}}, Limits);
            auto decoded = QByteArray::fromBase64Encoding(rewritten["bytes"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors); if (!decoded || decoded.decoded.size() > 16 * 1024 * 1024 || QCryptographicHash::hash(decoded.decoded, QCryptographicHash::Sha256).toHex() != rewritten["sha256"].toString().toLatin1()) throw Failure("ArchiveWorkerProtocol");
            next->working_[target] = std::move(decoded.decoded); next->members_[target] = rewritten["members"].toArray(); next->comparison_ = ArchiveOperation(input, *next, {}, Limits); return Response{next};
        }, true);
    }
    void ArchiveCompareView::RemoveSelected(int Side)
    {
        if (!session_ || Busy() || input_.readOnlyInputs_ || (input_.mode_ == "merge" && Side == 0)) return; const auto changes = selectedChanges(Side, true); if (changes.isEmpty()) return; activeSide_ = Side;
        run([input = input_, current = session_, changes, Side](const Budget& Limits) {
            auto next = std::make_shared<ArchiveSession>(*current); const auto rewritten = ArchiveOperation(input, *next, {{"operation", "rewrite"}, {"target", Side}, {"changes", changes}}, Limits);
            const auto decoded = QByteArray::fromBase64Encoding(rewritten["bytes"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors); if (!decoded || decoded.decoded.size() > 16 * 1024 * 1024 || QCryptographicHash::hash(decoded.decoded, QCryptographicHash::Sha256).toHex() != rewritten["sha256"].toString().toLatin1()) throw Failure("ArchiveWorkerProtocol");
            next->working_[Side] = decoded.decoded; next->members_[Side] = rewritten["members"].toArray(); next->comparison_ = ArchiveOperation(input, *next, {}, Limits); return Response{next};
        }, true);
    }
    void ArchiveCompareView::Undo(bool Redo)
    {
        if (!CanUndo(Redo)) return; auto& from = Redo ? redo_ : undo_; auto& to = Redo ? undo_ : redo_; const auto current = session_; const auto desired = from.back();
        // Preparation is infallible here: the revision already owns a verified comparison.
        auto next = std::make_shared<ArchiveSession>(*desired); next->originals_ = current->originals_; from.pop_back(); to.push_back(current); session_ = std::move(next); refresh(); if (stateChanged_) stateChanged_();
    }
    void ArchiveCompareView::Save(int Side, const QString& Path)
    {
        if (!session_ || Busy() || input_.readOnlyInputs_ || Side < 0 || Side > 1) return;
        run([input = input_, current = session_, Side, Path](const Budget& Limits) {
            const bool newDestination = !Path.isEmpty() && QFileInfo(Path).absoluteFilePath().compare(QFileInfo(current->originals_[Side].path_).absoluteFilePath(), Qt::CaseInsensitive) != 0;
            if (!newDestination) for (const auto& original : current->originals_) { const auto now = ReadFileSnapshot(original.path_, 16 * 1024 * 1024, Limits); if (now.entry_.id_.revision_ != original.entry_.id_.revision_ || now.raw_ != original.raw_) throw Failure("ArchiveInputChanged"); }
            auto next = std::make_shared<ArchiveSession>(*current); auto expected = next->originals_[Side];
            if (!Path.isEmpty() && QFileInfo(Path).absoluteFilePath() != QFileInfo(expected.path_).absoluteFilePath()) { expected.path_ = QFileInfo(Path).absoluteFilePath(); if (QFileInfo::exists(Path)) expected = ReadFileSnapshot(Path, 16 * 1024 * 1024, Limits); else { expected.exists_ = false; expected.entry_ = {}; } }
            auto protectedInput = input; for (int side = 0; side < 2; ++side) if (side != Side || expected.path_ != current->originals_[side].path_) protectedInput.protectedInputs_.append(current->originals_[side].path_);
            next->originals_[Side] = SaveArchiveSnapshot(protectedInput, expected, next->working_[Side], Limits); Response result{next}; result.saved_ = Side; return result;
        });
    }
    void ArchiveCompareView::OpenCurrent()
    {
        if (!session_ || Busy() || !table_->currentIndex().isValid()) return;
        const auto row = table_->currentIndex().data(Qt::UserRole).toJsonObject();
        run([input = input_, current = session_, row](const Budget& Limits) {
            auto next = std::make_shared<ArchiveSession>(*current); std::array<QByteArray, 2> bytes; bool nested = true;
            if (QStringList{"docx", "xlsx", "pptx"}.contains(QFileInfo(row["path"].toString()).suffix().toLower())) nested = false;
            for (int side = 0; side < 2; ++side) { const auto entry = row[side ? "right" : "left"].toObject(); if (entry.isEmpty()) { nested = false; continue; } if (entry["directory"].toBool()) throw Failure("ArchiveDirectorySelectChildren"); const auto value = ArchiveOperation(input, *next, {{"operation", "read"}, {"side", side}, {"ordinal", entry["ordinal"]}}, Limits); auto decoded = QByteArray::fromBase64Encoding(value["bytes"].toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors); if (!decoded) throw Failure("ArchiveWorkerProtocol"); bytes[side] = std::move(decoded.decoded); nested &= bytes[side].startsWith("PK") || bytes[side].startsWith("Rar!") || bytes[side].startsWith(QByteArray::fromHex("377abcaf271c")); }
            if (nested) { for (int side = 0; side < 2; ++side) next->members_[side].append(row[side ? "right" : "left"].toObject()["ordinal"]); next->comparison_ = ArchiveOperation(input, *next, {}, Limits); return Response{next}; }
            const auto isArchive = [](const QByteArray& Bytes) { return Bytes.startsWith("PK") || Bytes.startsWith("Rar!") || Bytes.startsWith(QByteArray::fromHex("377abcaf271c")); };
            for (int side = 0; side < 2; ++side) if (row[side ? "right" : "left"].toObject().isEmpty() && isArchive(bytes[1 - side])) bytes[side] = QByteArray::fromHex("504b0506") + QByteArray(18, '\0');
            auto files = std::make_shared<QTemporaryDir>(); if (!files->isValid()) throw Failure("ArchiveTemporaryWriteFailed"); Request child; child.mode_ = "compare"; child.theme_ = input.theme_; child.memoryMiB_ = input.memoryMiB_; child.readOnlyInputs_ = true; child.resourceLifetime_ = files;
            const auto suffix = QFileInfo(row["path"].toString()).suffix();
            for (int side = 0; side < 2; ++side) { const auto path = files->filePath(QString(side ? "RIGHT." : "LEFT.") + suffix); QFile file(path); if (!file.open(QIODevice::WriteOnly) || file.write(bytes[side]) != bytes[side].size()) throw Failure("ArchiveTemporaryWriteFailed"); if (side) child.file2_ = path; else child.file1_ = path; child.protectedInputs_.append(path); }
            Response result{next}; result.open_ = std::move(child); result.title_ = tr("HyperCompare — 압축 항목(읽기 전용): %1").arg(row["path"].toString()); return result;
        });
    }
    void ArchiveCompareView::ExportReport(const QString& Path, bool ReplaceExisting)
    {
        if (!session_ || Busy()) return;
        run([input = input_, current = session_, Path, ReplaceExisting](const Budget& Limits) {
            QJsonArray files; auto protectedInput = input;
            for (int side = 0; side < current->originals_.size(); ++side) { const auto& original = current->originals_[side]; protectedInput.protectedInputs_.append(original.path_); files.append(QJsonObject{{"path", original.path_}, {"inputSha256", QString::fromLatin1(QCryptographicHash::hash(original.raw_, QCryptographicHash::Sha256).toHex())}, {"workingSha256", QString::fromLatin1(QCryptographicHash::hash(current->working_[side], QCryptographicHash::Sha256).toHex())}}); }
            const auto bytes = QJsonDocument(QJsonObject{{"kind", "HyperCompareArchiveReport"}, {"schemaVersion", 1}, {"workingCopy", true}, {"files", files}, {"comparison", current->comparison_}}).toJson(); if (bytes.size() > 16 * 1024 * 1024) throw Failure("ArchiveReportLimit");
            Snapshot expected; expected.path_ = QFileInfo(Path).absoluteFilePath(); expected.exists_ = QFileInfo::exists(expected.path_);
            if (expected.exists_) { if (!ReplaceExisting) throw Failure("ArchiveReportOverwriteRequired"); expected = ReadFileSnapshot(expected.path_, 16 * 1024 * 1024, Limits); }
            SaveArchiveSnapshot(protectedInput, expected, bytes, Limits); return Response{current};
        });
    }
    void ArchiveCompareView::SetListFont(const QFont& Font)
    { table_->setFont(Font); table_->verticalHeader()->setDefaultSectionSize(QFontMetrics(Font).height() + 10); }
}
