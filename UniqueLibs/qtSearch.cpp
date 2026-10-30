#include <QCoreApplication>
#include "UniqueLibs/errorPresentation.hpp"
#include <QDir>
#include <QFileInfo>
#include "qtSearch.hpp"
#include "storageProviders.hpp"
#include "qtDialogFrame.hpp"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <set>

namespace hc::search {
using namespace storage;
Result<SearchMatcher> Compile(const QString& Query, MetadataSnapshot Metadata)
{
    if (Query.size() > 4096 || Query.contains(QChar::Null)) return Error{ErrorCategory::InvalidRequest, "search-query-size"};
    QVector<QPair<QString, QString>> Terms;
    for (const auto& Word : Query.split(' ', Qt::SkipEmptyParts)) {
        const auto Colon = Word.indexOf(':');
        auto Field = Colon < 0 ? QStringLiteral("name") : Word.left(Colon).toCaseFolded();
        auto Value = (Colon < 0 ? Word : Word.mid(Colon + 1)).normalized(QString::NormalizationForm_C).toCaseFolded();
        if (Field.isEmpty() || Value.isEmpty()) return Error{ErrorCategory::InvalidRequest, "search-empty-term"};
        Terms.append({Field, Value});
    }
    return SearchMatcher([Terms, Metadata = std::move(Metadata)](const EntrySnapshot& Entry) {
        const auto Name = QString::fromStdString(Entry.name_).normalized(QString::NormalizationForm_C).toCaseFolded();
        for (const auto& [Field, Value] : Terms) {
            QString Actual;
            if (Field == "name") Actual = Name;
            else if (Field == "ext") { if (Entry.directory_) return false; Actual = QFileInfo(Name).suffix(); }
            else if (Field == "type") Actual = Entry.directory_ ? "directory" : "file";
            else if (Field == "path") { Actual = QString::fromStdString(Entry.locator_.root_);
                for (const auto& Hop : Entry.locator_.nesting_) Actual += '/' + QString::fromStdString(Hop.entry_); }
            else {
                const auto Values = Metadata.value(QString::fromStdString(Serialize(Entry.id_)));
                for (auto It = Values.constBegin(); It != Values.constEnd(); ++It)
                    if (It.key().toCaseFolded() == Field) Actual += It.value() + ' ';
                for (const auto& [Key, Text] : Entry.hints_)
                    if (QString::fromStdString(Key).toCaseFolded() == Field) Actual += QString::fromStdString(Text) + ' ';
            }
            Actual = Actual.normalized(QString::NormalizationForm_C).toCaseFolded();
            if (Field == "ext" || Field == "type") { if (Actual != Value) return false; }
            else if (!Actual.contains(Value)) return false;
        }
        return true;
    });
}
namespace {
class IndexCursor final : public ISearchCursor, public std::enable_shared_from_this<IndexCursor> {
public:
    IndexCursor(QString Executable, SearchScope Scope, SearchMatcher Matcher, std::shared_ptr<IStorageProvider> Reader)
        : executable_(std::move(Executable)), scope_(std::move(Scope)), matcher_(std::move(Matcher)), reader_(Reader ? std::move(Reader) : MakeNativeReadProvider(LocatorKind::Local)) {}
    Task<SearchPage> Next(std::size_t MaxItems, OperationContext Context) override {
        auto Self = shared_from_this();
        return std::async(std::launch::async, [Self, MaxItems, Context] { return Self->next(MaxItems, Context); });
    }
private:
    Result<SearchPage> next(std::size_t MaxItems, OperationContext Context) {
        std::unique_lock Lock(mutex_, std::try_to_lock);
        if (!Lock.owns_lock()) return Error{ErrorCategory::InvalidRequest, "search-next-pending"};
        if (!MaxItems || !Context.maxPageItems_) return Error{ErrorCategory::InvalidRequest, "search-page-size"};
        if (const auto Failure = Context.Check({"everything"})) return *Failure;
        SearchPage Page; Page.partial_ = partial_; Page.freshness_ = SearchFreshness::IndexUnverified; Page.observedAt_ = std::chrono::system_clock::now();
        if (ended_) { Page.end_ = true; Page.visited_ = offset_; return Page; }
        const auto Limit = std::min({MaxItems, Context.maxPageItems_, std::size_t(256), scope_.maxVisited_ - offset_});
        if (!Limit) { Page.end_ = Page.partial_ = ended_ = true; Page.visited_ = offset_;
            Page.warnings_.push_back({ErrorCategory::BudgetExceeded, "search-visit-limit"}); return Page; }
        QProcess Process; Process.setProgram(executable_);
        Process.setArguments({QString::fromStdString(scope_.root_.root_), QString::number(offset_), QString::number(Limit)});
        Process.start();
        if (!Process.waitForStarted(2000)) return Error{ErrorCategory::Unavailable, "search-index-helper-unavailable"};
        const auto Deadline = std::min(Context.deadline_, std::chrono::steady_clock::now() + std::chrono::seconds(5));
        while (!Process.waitForFinished(20)) {
            if (Context.Check({"everything"}) || std::chrono::steady_clock::now() >= Deadline) {
                Process.kill(); Process.waitForFinished(2000);
                if (const auto Failure = Context.Check({"everything"})) return *Failure;
                return Error{ErrorCategory::Unavailable, "search-index-timeout"};
            }
        }
        if (Process.exitStatus() != QProcess::NormalExit || Process.exitCode() != 0)
            return Error{ErrorCategory::Unavailable, "search-index-unavailable", {"everything"}, {}, true, "start-everything-or-use-provider-search"};
        const auto Bytes = Process.readAllStandardOutput();
        if (Bytes.size() > 16 * 1024 * 1024) return Error{ErrorCategory::BudgetExceeded, "search-index-response"};
        QJsonParseError Parse;
        const auto Doc = QJsonDocument::fromJson(Bytes, &Parse);
        if (Parse.error != QJsonParseError::NoError || !Doc.isObject() || !Doc["paths"].isArray() || !Doc["total"].isDouble())
            return Error{ErrorCategory::Internal, "search-index-protocol"};
        const auto Paths = Doc["paths"].toArray();
        if (Paths.size() > Limit) return Error{ErrorCategory::Internal, "search-index-page-size"};
        const auto Root = QDir::cleanPath(QDir::fromNativeSeparators(QString::fromStdString(scope_.root_.root_)));
        QString Prefix = Root; if (!Prefix.endsWith('/')) Prefix += '/';
        for (const auto& Value : Paths) {
            if (const auto Failure = Context.Check({"everything"})) return *Failure;
            const auto Path = QDir::cleanPath(QDir::fromNativeSeparators(Value.toString()));
            if (!Path.startsWith(Prefix, Qt::CaseInsensitive)) { Page.partial_ = true; continue; }
            const auto Relative = Path.mid(Prefix.size());
            if (!scope_.recursive_ && Relative.contains('/')) continue;
            if (Relative.count('/') > scope_.maxDepth_) { Page.partial_ = true; continue; }
            if (!seen_.insert(Path.toCaseFolded()).second) continue;
            auto Entry = reader_->Resolve({LocatorKind::Local, QDir::toNativeSeparators(Path).toStdString()}, Context).get();
            if (!Entry.HasValue()) { Page.warnings_.push_back(Entry.Failure()); Page.partial_ = true; continue; }
            if (matcher_(Entry.Value())) Page.matches_.push_back(std::move(Entry.Value()));
        }
        offset_ += Paths.size(); Page.visited_ = offset_;
        ended_ = Paths.isEmpty() || offset_ >= Doc["total"].toDouble(); Page.end_ = ended_;
        partial_ = Page.partial_;
        if (const auto Failure = Context.Check({"everything"})) return *Failure;
        return Page;
    }
    QString executable_;
    SearchScope scope_;
    SearchMatcher matcher_;
    std::shared_ptr<IStorageProvider> reader_;
    std::size_t offset_ = 0;
    bool ended_ = false, partial_ = false;
    std::set<QString> seen_;
    std::mutex mutex_;
};
class IndexSearch final : public ISearchProvider {
public:
    IndexSearch(QString Executable, std::shared_ptr<IStorageProvider> Reader) : executable_(std::move(Executable)), reader_(std::move(Reader)) {}
    Result<std::shared_ptr<ISearchCursor>> Search(SearchScope Scope, SearchMatcher Matcher) override {
        if (Scope.root_.kind_ != LocatorKind::Local || !Scope.root_.nesting_.empty() || !QDir::isAbsolutePath(QString::fromStdString(Scope.root_.root_)))
            return Error{ErrorCategory::Unsupported, "search-index-local-only"};
        if (!Matcher || !Scope.maxVisited_ || Scope.maxVisited_ > 1000000 || Scope.maxDepth_ > 128)
            return Error{ErrorCategory::InvalidRequest, "search-scope"};
        return std::shared_ptr<ISearchCursor>(new IndexCursor(executable_, std::move(Scope), std::move(Matcher), reader_));
    }
private:
    QString executable_;
    std::shared_ptr<IStorageProvider> reader_;
};
class SearchDialog final : public QDialog {
public:
    SearchDialog(QWidget* Parent, std::shared_ptr<IStorageProvider> Provider, Locator Root, MetadataSnapshot Metadata,
        std::function<void(const EntrySnapshot&)> Activate) : QDialog(Parent), provider_(std::move(Provider)), root_(std::move(Root)), metadata_(std::move(Metadata)), activate_(std::move(Activate)) {
        setAttribute(Qt::WA_DeleteOnClose); setObjectName("hc.searchDialog"); setWindowTitle(tr("범위 검색")); resize(820, 500);
        auto* Layout = new QVBoxLayout(this);
        // Query, engine and the sub-folder switch share one row; the search location is the header subtitle.
        auto* QueryRow = new QHBoxLayout; QueryRow->setSpacing(8); Layout->addLayout(QueryRow);
        query_ = new QLineEdit(this); query_->setObjectName("hc.search.query"); query_->setClearButtonEnabled(true); query_->setPlaceholderText(tr("name:이름 ext:확장자 type:file · 공백으로 조건 조합")); QueryRow->addWidget(query_, 1);
        engine_ = new QComboBox(this); engine_->addItem(tr("폴더 직접 검색"));
        if (root_.kind_ == LocatorKind::Local) engine_->addItem(tr("Everything 인덱스 검색"));
        engine_->setObjectName("hc.search.engine"); QueryRow->addWidget(engine_);
        recursive_ = new QCheckBox(tr("하위 폴더 포함"), this); recursive_->setChecked(true); QueryRow->addWidget(recursive_);
        results_ = new QTreeWidget(this); results_->setObjectName("hc.search.results"); results_->setHeaderLabels({tr("이름"), tr("위치")}); results_->header()->setSectionResizeMode(QHeaderView::ResizeToContents); Layout->addWidget(results_);
        status_ = new QLabel(this); status_->setObjectName("hc.search.status"); status_->setWordWrap(true);
        auto* Buttons = new QDialogButtonBox(this);
        auto* Start = start_ = Buttons->addButton(tr("검색"), QDialogButtonBox::ActionRole); Start->setObjectName("hc.search.start");
        next_ = Buttons->addButton(tr("다음 결과"), QDialogButtonBox::ActionRole); next_->setObjectName("hc.search.next"); next_->setEnabled(false);
        auto* Stop = Buttons->addButton(tr("중지"), QDialogButtonBox::ActionRole); Stop->setObjectName("hc.search.stop");
        Buttons->addButton(QDialogButtonBox::Close);
        auto* Footer = hc::ui::ApplyDialogFrame(this, {{}, tr("검색 위치: %1").arg(QString::fromStdString(root_.root_)), fm::ui::glyph::Search});
        status_->setParent(Footer->parentWidget()); Footer->insertWidget(0, status_, 1); hc::ui::MoveToFooter(Footer, Buttons);
        fm::style::setButtonRole(Start, fm::style::ButtonRole::Primary);
        connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
        connect(Start, &QPushButton::clicked, this, [this] { start(); }); connect(query_, &QLineEdit::returnPressed, this, [this] { start(); });
        connect(next_, &QPushButton::clicked, this, [this] { next(); });
        connect(Stop, &QPushButton::clicked, this, [this] { cancel(); status_->setText(tr("검색 중지됨 · 표시된 결과는 유지됩니다.")); });
        connect(results_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* Item, int) { const int Index = results_->indexOfTopLevelItem(Item); if (Index >= 0 && Index < entries_.size()) activate_(entries_[Index]); });
    }
    ~SearchDialog() override { stop_.request_stop(); }
private:
    void cancel() { stop_.request_stop(); ++generation_; cursor_.reset(); next_->setEnabled(false); }
    void start() {
        if (busy_) return;
        cancel(); auto Matcher = Compile(query_->text(), metadata_);
        if (!Matcher.HasValue()) { status_->setText(hc::ui::ErrorText(Matcher.Failure())); return; }
        auto Provider = engine_->currentIndex() == 0 ? MakeEnumeratingSearch(provider_) : MakeEverythingSearch(QCoreApplication::applicationDirPath() + "/hc-search-index.exe", provider_);
        auto Open = Provider->Search({root_, recursive_->isChecked()}, Matcher.Value());
        if (!Open.HasValue()) { status_->setText(hc::ui::ErrorText(Open.Failure())); return; }
        cursor_ = std::move(Open.Value()); stop_ = std::stop_source(); entries_.clear(); results_->clear();
        deadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(5); next();
    }
    void next() {
        if (!cursor_ || busy_) return; busy_ = true; start_->setEnabled(false); next_->setEnabled(false); status_->setText(tr("검색 중…"));
        auto* Watcher = new QFutureWatcher<Result<SearchPage>>(this); const auto Generation = generation_;
        connect(Watcher, &QFutureWatcher<Result<SearchPage>>::finished, this, [this, Watcher, Generation] {
            auto Result = Watcher->result(); Watcher->deleteLater(); busy_ = false; start_->setEnabled(true); if (Generation != generation_) return;
            if (!Result.HasValue()) { status_->setText(tr("검색 실패: %1").arg(hc::ui::ErrorText(Result.Failure()))); cursor_.reset(); return; }
            const auto& Page = Result.Value();
            for (const auto& Entry : Page.matches_) {
                auto Path = QString::fromStdString(Entry.locator_.root_); for (const auto& Hop : Entry.locator_.nesting_) Path += " / " + QString::fromStdString(Hop.entry_);
                new QTreeWidgetItem(results_, {QString::fromStdString(Entry.name_), Path}); entries_.append(Entry);
            }
            status_->setText(tr("%1개 결과 / %2개 확인 · %3%4%5").arg(entries_.size()).arg(Page.visited_)
                .arg(Page.end_ ? tr("완료") : tr("다음 결과를 요청할 수 있습니다"))
                .arg(Page.partial_ ? tr(" · 일부 경로 또는 범위 생략") : QString())
                .arg(Page.freshness_ == SearchFreshness::IndexUnverified ? tr(" · 인덱스 누락 가능, 반환 항목은 파일 시스템에서 확인") : tr(" · 열거 시점 결과")));
            if (!Page.warnings_.empty()) status_->setToolTip(hc::ui::ErrorText(Page.warnings_.front()));
            next_->setEnabled(!Page.end_); if (Page.end_) cursor_.reset();
        });
        OperationContext Context; Context.stopToken_ = stop_.get_token(); Context.deadline_ = deadline_; Context.freshEnumerationMetadata_ = true;
        Watcher->setFuture(QtConcurrent::run([Cursor = cursor_, Context] { return Cursor->Next(128, Context).get(); }));
    }
    std::shared_ptr<IStorageProvider> provider_;
    Locator root_;
    MetadataSnapshot metadata_;
    std::function<void(const EntrySnapshot&)> activate_;
    std::shared_ptr<ISearchCursor> cursor_;
    std::stop_source stop_;
    std::chrono::steady_clock::time_point deadline_;
    quint64 generation_ = 0;
    bool busy_ = false;
    QVector<EntrySnapshot> entries_;
    QLineEdit* query_;
    QCheckBox* recursive_;
    QComboBox* engine_;
    QTreeWidget* results_;
    QLabel* status_;
    QPushButton* next_;
    QPushButton* start_;
};
}
std::shared_ptr<ISearchProvider> MakeEverythingSearch(const QString& Executable, std::shared_ptr<IStorageProvider> Reader) { return std::make_shared<IndexSearch>(Executable, std::move(Reader)); }
void Show(QWidget* Parent, std::shared_ptr<IStorageProvider> Provider, Locator Root, MetadataSnapshot Metadata, std::function<void(const EntrySnapshot&)> Activate)
{ (new SearchDialog(Parent, std::move(Provider), std::move(Root), std::move(Metadata), std::move(Activate)))->show(); }
}
