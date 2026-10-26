#include "qtPluginManagementPage.hpp"
#include "qtPluginHostSettings.hpp"
#include "builtinPluginCatalog.hpp"
#include "pluginHost/wlxDetectionSettings.hpp"
#include "tcPluginPackage.hpp"
#include "pluginHost/pluginApplicationRule.hpp"
#include "commonLibs/document/documentFormatSupport.hpp"
#include "commonLibs/layout/productLayout.hpp"
#include <QtWidgets>
#include <QFutureWatcher>
#include <QtConcurrentRun>

#include <array>
#include <algorithm>

namespace {
using hc::plugins::PluginFormat;
using hc::plugins::PluginApplicationRule;
struct FormatInfo { const char* group_; const char* token_; PluginFormat format_; int role_; };
const std::array<FormatInfo, 6> Formats{{
    {"wcx", "WCX", PluginFormat::Wcx, 1}, {"wlx", "WLX", PluginFormat::Wlx, 2},
    {"dopusViewer", "DOpus", PluginFormat::DopusViewer, 2}, {"wfx", "WFX", PluginFormat::Wfx, 3},
    {"dopusVfs", "DOpus", PluginFormat::DopusVfs, 3}, {"wdx", "WDX", PluginFormat::Wdx, 4}
}};
QStringList strings(const QJsonValue& Value) {
    QStringList Result; for (const auto& Item : Value.toArray()) if (Item.isString()) Result.append(Item.toString()); return Result;
}
QString resolvedPath(QString Path, const QString& Root, const QString& Group) {
    Path.replace("%COMMANDER_PATH%", Root, Qt::CaseInsensitive);
    if (Group == "wcx" || Group == "wlx" || Group == "wfx" || Group == "wdx")
        if (Path.endsWith('.' + Group, Qt::CaseInsensitive)) Path += "64";
    return QDir::isRelativePath(Path) ? QDir(Root).absoluteFilePath(Path) : Path;
}
QJsonObject ruleObject(bool Enabled, const QString& Mode, const QStringList& Extensions, const QString& Expression) {
    QJsonObject Result{{"version", 1}, {"enabled", Enabled}, {"mode", Mode}};
    if (Mode == "extensions") Result["extensions"] = QJsonArray::fromStringList(Extensions);
    if (Mode == "tc" || Mode == "pattern") Result["expression"] = Expression;
    return Result;
}
class PackageProgressDialog final : public QDialog {
public:
    std::stop_source stop_;
    QLabel* label_;
    QDialogButtonBox* buttons_;
    explicit PackageProgressDialog(QWidget* Parent) : QDialog(Parent) {
        setObjectName("pluginPackageProgress"); setWindowTitle(tr("플러그인 설치"));
        auto* Layout = new QVBoxLayout(this); label_ = new QLabel(tr("ZIP에서 플러그인을 설치하고 있습니다."), this); Layout->addWidget(label_);
        auto* Progress = new QProgressBar(this); Progress->setRange(0, 0); Layout->addWidget(Progress);
        buttons_ = new QDialogButtonBox(QDialogButtonBox::Cancel, this); Layout->addWidget(buttons_);
        connect(buttons_, &QDialogButtonBox::rejected, this, [this] { reject(); });
    }
    void reject() override {
        stop_.request_stop(); label_->setText(tr("설치를 취소하고 있습니다…")); buttons_->setEnabled(false);
    }
};
hc::storage::Result<hc::plugins::TcPluginInstallation> installPackage(QWidget* Parent, const QString& Path, const QString& Root) {
    PackageProgressDialog Progress(Parent);
    QFutureWatcher<hc::storage::Result<hc::plugins::TcPluginInstallation>> Watcher;
    QObject::connect(&Watcher, &QFutureWatcherBase::finished, &Progress, [&Progress] { Progress.done(QDialog::Accepted); });
    const auto Token = Progress.stop_.get_token();
    Watcher.setFuture(QtConcurrent::run([Path, Root, Token] {
        hc::storage::OperationContext Context; Context.stopToken_ = Token;
        Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        return hc::plugins::InstallTcPluginPackage(Path, Root, Context);
    }));
    Progress.exec();
    if (!Watcher.isFinished()) { Progress.stop_.request_stop(); Watcher.waitForFinished(); }
    return Watcher.result();
}
}

struct QPluginManagementPage::State {
    struct Entry {
        QString id_, name_, group_, path_, support_, actions_;
        QStringList extensions_;
        int index_ = -1, format_ = -1, role_ = 0;
        bool builtin_ = false;
    };
    QPluginManagementPage* owner_;
    QString root_, selectedId_, settingsDirectory_;
    QJsonObject plugins_;
    QVector<Entry> entries_;
    int row_ = -1, role_ = 0;
    bool loading_ = false;
    QListWidget* roles_ = nullptr;
    QTreeWidget* list_ = nullptr;
    QLineEdit* search_ = nullptr;
    QLabel *heading_ = nullptr, *support_ = nullptr, *actions_ = nullptr, *status_ = nullptr, *ruleStatus_ = nullptr, *info_ = nullptr;
    QCheckBox* enabled_ = nullptr;
    QComboBox *mode_ = nullptr, *dialect_ = nullptr, *selection_ = nullptr, *processMode_ = nullptr, *testAction_ = nullptr;
    QListWidget* extensions_ = nullptr;
    QListWidget* builtinExtensions_ = nullptr;
    QLineEdit* builtinExcluded_ = nullptr;
    QWidget* builtinPanel_ = nullptr;
    QPlainTextEdit* expression_ = nullptr;
    QLineEdit* registered_ = nullptr;
    QPlainTextEdit* native_ = nullptr;
    QLabel* nativeLabel_ = nullptr;
    QPushButton* readDetection_ = nullptr;
    bool detectionBusy_ = false;
    QSet<QString> detectionAttempts_;
    QCheckBox* hosted_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QStackedWidget *conditionPages_ = nullptr, *hostPages_ = nullptr;
    QWidget *basic_ = nullptr, *advanced_ = nullptr, *editable_ = nullptr;
    QPushButton *remove_ = nullptr, *up_ = nullptr, *down_ = nullptr, *test_ = nullptr;
    QTreeWidget* results_ = nullptr;
    std::array<QPluginHostSettings*, 6> hosts_{};
    QFutureWatcher<QVector<QStringList>> testWatcher_;
    quint64 testGeneration_ = 0;
    explicit State(QPluginManagementPage* Owner, QString Root) : owner_(Owner), root_(std::move(Root)) {}
    QString key(int Format, int Index) const { return QString::number(Format) + ':' + QString::number(Index); }
    QJsonObject record(const Entry& Value) const { if (Value.builtin_) return plugins_["builtin"].toObject()[Value.id_].toObject(); const auto Items = plugins_.value(Value.group_).toArray(); return Value.index_ >= 0 && Value.index_ < Items.size() ? Items[Value.index_].toObject() : QJsonObject{}; }
    void setRecord(const Entry& Value, const QJsonObject& Record) {
        if (Value.builtin_) { auto Items = plugins_["builtin"].toObject(); Items[Value.id_] = Record; plugins_["builtin"] = Items; return; }
        auto Items = plugins_.value(Value.group_).toArray();
        if (Value.index_ >= 0 && Value.index_ < Items.size()) { Items[Value.index_] = Record; plugins_[Value.group_] = Items; }
    }
    void requestDetection(bool Reset = false) {
        if (loading_ || detectionBusy_ || row_ < 0 || entries_[row_].builtin_ || entries_[row_].group_ != "wlx") return;
        const auto Value = entries_[row_]; const auto Record = record(Value);
        if (!Reset && !Record["hostEnabled"].toBool()) return;
        const auto Reviewed = hc::plugins::ParseManifest(hc::settings::JsonBytes(Record["hostReviewed"]));
        if (!Reviewed.HasValue() || Record["hostManifest"].toString().isEmpty()) {
            if (Reset) hosts_[Value.format_]->ReviewPlugin(Value.index_);
            return;
        }
        const auto Hash = QString::fromStdString(Reviewed.Value().sha256_);
        const auto Key = Value.id_ + ':' + Value.path_ + ':' + Hash;
        if (!Reset && ((Record.contains("nativeDetect") && Record["nativeDetectSha256"] == Hash) || detectionAttempts_.contains(Key))) return;
        detectionAttempts_.insert(Key); detectionBusy_ = true; readDetection_->setEnabled(false);
        hc::plugins::PluginViewerOptions Options; Options.binaryPath_ = Value.path_;
        Options.executable_ = QDir(root_).filePath("hc-plugin-host-x64.exe"); Options.manifestPath_ = Record["hostManifest"].toString();
        Options.defaultIniPath_ = QDir(settingsDirectory_.isEmpty() ? root_ : settingsDirectory_).filePath("lsplugin.ini");
        Options.approval_ = {Reviewed.Value(), true, false, Reviewed.Value().permissions_};
        auto* Watcher = new QFutureWatcher<hc::storage::Result<QString>>(owner_);
        QObject::connect(Watcher, &QFutureWatcherBase::finished, owner_, [this, Watcher, Value, Record, Hash, Reset] {
            const auto Result = Watcher->result(); Watcher->deleteLater(); detectionBusy_ = false; readDetection_->setEnabled(true);
            auto Found = std::find_if(entries_.cbegin(), entries_.cend(), [&](const Entry& Entry) { return Entry.id_ == Value.id_ && Entry.path_ == Value.path_; });
            if (Found == entries_.cend() || record(*Found)["hostReviewed"] != Record["hostReviewed"]) return;
            const bool Selected = row_ >= 0 && entries_[row_].id_ == Value.id_;
            if (!Result.HasValue()) {
                if (Selected) ruleStatus_->setText(QStringLiteral("탐지식을 읽지 못했습니다: ") + QString::fromStdString(Result.Failure().code_));
            } else {
                auto Updated = record(*Found);
                const bool LegacyOverride = !Updated.contains("detectOverride") && !Updated["detect"].toString().isEmpty() && Updated["detect"].toString() != Result.Value();
                const bool ChangedDuringRead = Updated["detect"] != Record["detect"] || Updated["detectOverride"] != Record["detectOverride"];
                const bool KeepOverride = (!Reset || ChangedDuringRead) && (Updated["detectOverride"].toBool() || LegacyOverride);
                Updated["nativeDetect"] = Result.Value(); Updated["nativeDetectSha256"] = Hash; Updated["detectOverride"] = KeepOverride;
                if (!KeepOverride) Updated["detect"] = Result.Value();
                setRecord(*Found, Updated);
                if (Selected) {
                    const QSignalBlocker Block(native_); native_->setPlainText(Updated["detect"].toString());
                    native_->document()->setModified(false);
                    ruleStatus_->setText(KeepOverride ? QStringLiteral("플러그인 원본을 확인했습니다. 사용자 탐지식은 유지합니다.") : QStringLiteral("플러그인 탐지식을 읽었습니다. 수정하면 사용자 탐지식으로 적용합니다."));
                }
            }
            if (!Selected) QTimer::singleShot(0, owner_, [this] { requestDetection(); });
        });
        Watcher->setFuture(QtConcurrent::run([Options] {
            hc::storage::OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            return hc::plugins::ReadWlxDetection(Options, Context);
        }));
    }
    void storeHosts() { for (auto* Host : hosts_) Host->SavePlugins(plugins_); }
    void loadHosts() { for (auto* Host : hosts_) Host->LoadPlugins(plugins_); }
    void addBuiltin(const QString& Id, const QString& Name, int Role, QStringList Extensions, const QString& Support, const QString& Actions, const QString& Path) {
        entries_.append({Id, Name, {}, QDir(root_).filePath(Path), Support, Actions, std::move(Extensions), -1, -1, Role, true});
    }
    void catalog() {
        entries_.clear();
        // Product catalog identity does not depend on an external plugin list or
        // file existence. Missing package components remain diagnosable rows.
        addBuiltin("builtin.sevenzip", "7-Zip", 1, hc::plugins::BuiltinExtensions("builtin.sevenzip"), "7z · zip · zipx · rar · 분할 압축", "열기 · 해제 · 생성(7z/ZIP)", QString(hc::layout::SevenZipPackage) + "/7z.dll");
        addBuiltin("builtin.iso", "ISO 이미지", 1, {"iso"}, "iso", "디스크 이미지 열기 · 생성", QString(hc::layout::IsoImagePackage) + "/hc_isoimage_wcx.wcx64");
        addBuiltin("builtin.alzegg", "ALZ · EGG", 1, {"alz", "egg"}, "alz · egg", "압축 열기 · 압축 해제", QString(hc::layout::AlzEggPackage) + "/UnEGG64.dll");
        const auto Documents = hc::plugins::BuiltinExtensions("builtin.hyperviewer");

        addBuiltin("builtin.hyperviewer", "HyperViewer", 2, Documents, hc::plugins::BuiltinDocumentSupport(), "보기", QString(hc::layout::HyperViewerPackage) + "/HyperViewer.dll");
        addBuiltin("builtin.hypereditor", "HyperEditor", 2, Documents, hc::plugins::BuiltinDocumentSupport(), "편집", "HyperEditor.exe");
        addBuiltin("builtin.rhwp", "한글 문서", 2, {"hwp", "hwpx"}, "hwp · hwpx", "보기 · 편집", QDir::cleanPath(QDir(hc::layout::RhwpRuntime).filePath("runtime-inventory.json")));
        for (int Format = 0; Format < int(Formats.size()); ++Format) {
            const auto& Kind = Formats[Format]; const auto Items = plugins_.value(Kind.group_).toArray();
            for (int Index = 0; Index < qMin(Items.size(), qsizetype(1000)); ++Index) {
                const auto Record = Items[Index].toObject(); auto Path = Record.value("path").toString();
                const auto Exts = strings(Record.value("extensions"));
                const auto Support = Exts.isEmpty() ? (Kind.format_ == PluginFormat::Wfx || Kind.format_ == PluginFormat::DopusVfs ? QStringLiteral("파일 시스템 연결") : QStringLiteral("플러그인 감지로 판정")) : Exts.join(" · ");
                QString Actions = Kind.role_ == 2 ? QStringLiteral("보기") : Kind.role_ == 4 ? QStringLiteral("파일 정보") : Kind.role_ == 3 ? QStringLiteral("탐색") : QStringLiteral("압축 열기 · 해제");
                if (Kind.format_ == PluginFormat::DopusViewer) {
                    auto Reviewed = hc::plugins::ParseManifest(hc::settings::JsonBytes(Record.value("hostReviewed")));
                    if (Reviewed.HasValue()) { QStringList Parts; if (Reviewed.Value().extensions_.contains(hc::plugins::Extension::Viewer)) Parts.append(QStringLiteral("보기")); if (Reviewed.Value().extensions_.contains(hc::plugins::Extension::Metadata)) Parts.append(QStringLiteral("파일 정보")); if (Reviewed.Value().extensions_.contains(hc::plugins::Extension::ContentConvert)) Parts.append(QStringLiteral("텍스트 변환")); Actions = Parts.join(" · "); }
                    else Actions = QStringLiteral("문서 · 승인 후 확인");
                }
                entries_.append({key(Format, Index), QFileInfo(Path).fileName(), Kind.group_, resolvedPath(Path, root_, Kind.group_), Support,
                    Actions, Exts, Index, Format, Kind.role_, false});
            }
        }
    }
    QString stateText(const Entry& Value) const {
        {
            auto Rule = PluginApplicationRule::Parse(Value.builtin_ ? hc::plugins::BuiltinApplication(plugins_, Value.id_) : record(Value).value("application"));
            if (!Rule.HasValue()) return QStringLiteral("조건 확인 필요");
            if (!Rule.Value().Enabled()) return QStringLiteral("사용 안 함");
        }
        if (!QFileInfo::exists(Value.path_)) return QStringLiteral("구성 파일 없음");
        if (Value.builtin_) return QStringLiteral("제품 구성 · 실행 시 검증");
        const auto Record = record(Value);
        if (!Record.value("hostEnabled").toBool()) return QStringLiteral("기존 실행 방식");
        return hosts_[Value.format_]->VerificationState(Value.index_);
    }
    void rebuild(const QString& Select = {}) {
        const QSignalBlocker Block(list_); catalog(); list_->clear(); row_ = -1;
        QTreeWidgetItem *Builtin = nullptr, *External = nullptr, *Selected = nullptr;
        const auto Needle = search_->text().trimmed(); const auto Role = roles_->currentRow(); role_ = Role;
        for (int Index = 0; Index < entries_.size(); ++Index) {
            const auto& Value = entries_[Index];
            if ((Role > 0 && Value.role_ != Role && !(Role == 4 && Value.actions_.contains(QStringLiteral("파일 정보")))) || (!Needle.isEmpty() && !(Value.name_ + ' ' + Value.support_ + ' ' + Value.actions_ + ' ' + Value.extensions_.join(' ')).contains(Needle, Qt::CaseInsensitive))) continue;
            auto*& Group = Value.builtin_ ? Builtin : External;
            if (!Group) { Group = new QTreeWidgetItem(list_, {Value.builtin_ ? QStringLiteral("제품 내장") : QStringLiteral("추가한 플러그인")}); Group->setFlags(Qt::ItemIsEnabled); auto Font = Group->font(0); Font.setBold(true); Group->setFont(0, Font); }
            auto* Item = new QTreeWidgetItem(Group, {Value.name_ + "\n" + Value.support_ + "\n적용: " + PluginApplicationRule::Describe(Value.builtin_ ? hc::plugins::BuiltinApplication(plugins_, Value.id_) : record(Value).value("application")), Value.actions_}); Item->setData(0, Qt::UserRole, Index); Item->setData(0, Qt::UserRole + 1, Value.id_);
            Item->setToolTip(0, Value.path_ + '\n' + stateText(Value));
            Item->setIcon(0, owner_->style()->standardIcon(Value.role_ == 1 ? QStyle::SP_DriveHDIcon : QStyle::SP_FileIcon));
            if (Value.id_ == (Select.isEmpty() ? selectedId_ : Select)) Selected = Item;
        }
        list_->expandAll();
        if (!Selected) for (int Group = 0; Group < list_->topLevelItemCount(); ++Group) if (list_->topLevelItem(Group)->childCount()) { Selected = list_->topLevelItem(Group)->child(0); break; }
        if (Selected) { list_->setCurrentItem(Selected); row_ = Selected->data(0, Qt::UserRole).toInt(); }
        loadItem();
    }
    void loadItem();
    void storeItem();
    bool validate(bool Show = true);
    void add();
    bool addTc(const QString& Path, bool Review);
    bool registerFile(const QString& Path, int Format, const QStringList& Extensions, bool Review);
    void remove();
    void move(int Direction);
    void testFiles();
};

void QPluginManagementPage::State::loadItem() {
    loading_ = true;
    const bool Valid = row_ >= 0 && row_ < entries_.size();
    tabs_->setEnabled(Valid); remove_->setEnabled(Valid && !entries_[row_].builtin_);
    up_->setEnabled(Valid && !entries_[row_].builtin_); down_->setEnabled(up_->isEnabled());
    if (!Valid) { heading_->setText(QStringLiteral("플러그인을 선택하세요")); loading_ = false; return; }
    const auto& Value = entries_[row_]; selectedId_ = Value.id_; const auto Record = record(Value);
    up_->setEnabled(!Value.builtin_ && Value.index_ > 0);
    down_->setEnabled(!Value.builtin_ && Value.index_ + 1 < plugins_.value(Value.group_).toArray().size());
    heading_->setText(Value.name_); support_->setText(QStringLiteral("지원 형식: ") + Value.support_); support_->setToolTip(Value.support_);
    actions_->setText(QStringLiteral("지원 작업: ") + Value.actions_); status_->setText(QStringLiteral("상태: ") + stateText(Value));
    info_->setText(QStringLiteral("%1\n\n%2\n\n%3").arg(Value.builtin_ ? QStringLiteral("제품 내장 구성 요소") : QString::fromLatin1(Formats[Value.format_].token_), Value.path_ + "\n\n지원 형식: " + (Value.extensions_.isEmpty() ? Value.support_ : Value.extensions_.join("; ")),
        Value.builtin_ ? QStringLiteral("제품에 포함된 처리기입니다. 별도 플러그인을 등록하지 않아도 표시됩니다. 지원 형식은 제품 구성에 따릅니다.") : QStringLiteral("등록 형식은 설정에 기재한 지원 범위입니다. 실제 지원 여부는 플러그인의 감지·열기 결과로 확인합니다.")));
    auto Rule = Record.value("application").toObject(); const auto Parsed = PluginApplicationRule::Parse(Value.builtin_ ? hc::plugins::BuiltinApplication(plugins_, Value.id_) : Record.value("application"));
    enabled_->setChecked(Rule.value("enabled").toBool(true));
    auto Mode = Rule.value("mode").toString("all"); mode_->setCurrentIndex(Mode == "extensions" ? 1 : Mode == "tc" || Mode == "pattern" ? 2 : 0);
    dialect_->setCurrentIndex(Mode == "pattern" ? 1 : 0); expression_->setPlainText(Rule.value("expression").toString());
    registered_->setText(Value.extensions_.join("; ")); native_->setPlainText(Record.value("detect").toString());
    native_->setPlaceholderText(Record.contains("nativeDetect") && Record["nativeDetect"].toString().isEmpty()
        ? QStringLiteral("플러그인이 탐지식을 제공하지 않습니다. 필요하면 직접 입력하세요.")
        : QStringLiteral("WLX 탐지식 — 검사·승인된 플러그인에서 자동으로 읽습니다."));
    extensions_->clear(); auto Available = Value.extensions_;
    for (const auto& Ext : strings(Rule.value("extensions"))) if (!Available.contains(Ext, Qt::CaseInsensitive)) Available.append(Ext);
    for (const auto& Ext : Available) { auto* Item = new QListWidgetItem(Ext, extensions_); Item->setFlags(Item->flags() | Qt::ItemIsUserCheckable); Item->setCheckState(Mode != "extensions" || strings(Rule.value("extensions")).contains(Ext, Qt::CaseInsensitive) ? Qt::Checked : Qt::Unchecked); }
    editable_->setVisible(!Value.builtin_); editable_->setEnabled(!Value.builtin_ && Parsed.HasValue());
    enabled_->setEnabled(Parsed.HasValue());
    builtinPanel_->setVisible(Value.builtin_); builtinPanel_->setEnabled(Parsed.HasValue());
    builtinExtensions_->clear();
    QStringList Excluded = Parsed.HasValue() ? Parsed.Value().ExcludedExtensions() : QStringList{};
    if (Value.builtin_) {
        for (const auto& Ext : Value.extensions_) {
            auto* Item = new QListWidgetItem(Ext, builtinExtensions_);
            Item->setFlags(Item->flags() | Qt::ItemIsUserCheckable);
            Item->setCheckState(Excluded.contains(Ext, Qt::CaseInsensitive) ? Qt::Unchecked : Qt::Checked);
            Excluded.removeAll(Ext);
        }
        builtinExcluded_->setText(Excluded.join("; "));
    }
    // WFX is selected by connection, not by filename; only its enabled state applies.
    mode_->setEnabled(!Value.builtin_ && Value.role_ != 3 && Parsed.HasValue());
    conditionPages_->setCurrentIndex(mode_->currentIndex());
    registered_->setVisible(!Value.builtin_); native_->setVisible(!Value.builtin_ && Value.group_ == "wlx");
    readDetection_->setVisible(!Value.builtin_ && Value.group_ == "wlx");
    nativeLabel_->setVisible(!Value.builtin_ && Value.group_ == "wlx");
    ruleStatus_->setText(!Parsed.HasValue() ? QStringLiteral("지원하지 않는 조건입니다. 기존 값을 보존하며 실행 대상에서 제외합니다.") : Value.builtin_ ? QStringLiteral("목록은 알려진 지원 형식입니다. 체크 해제한 확장자는 감지 전에 제외하고 다른 처리기 후보를 사용합니다. 그 외 확장자는 기존 동적 감지를 유지합니다.") : Value.role_ == 3 ? QStringLiteral("파일 시스템은 연결 대상으로 선택합니다. 파일명 조건은 사용하지 않습니다.") : QStringLiteral("사용자 조건과 플러그인 고유 감지를 모두 통과해야 적용됩니다."));
    hosted_->setChecked(Record.value("hostEnabled").toBool()); hosted_->setEnabled(!Value.builtin_);
    hostPages_->setCurrentIndex(Value.builtin_ ? 0 : Value.format_ + 1);
    if (!Value.builtin_) hosts_[Value.format_]->SelectPlugin(Value.index_);
    loading_ = false;
    QTimer::singleShot(0, owner_, [this] { requestDetection(); });
}
void QPluginManagementPage::State::storeItem() {
    if (loading_ || row_ < 0 || row_ >= entries_.size()) return;
    const auto Value = entries_[row_]; auto Record = record(Value);
    if (Value.builtin_) {
        if (!builtinPanel_->isEnabled()) return;
        QStringList Excluded = builtinExcluded_->text().split(QRegularExpression("[;,|\\s]+"), Qt::SkipEmptyParts);
        for (auto& Ext : Excluded) { Ext = Ext.toLower(); if (Ext.startsWith("*.")) Ext.remove(0, 2); else if (Ext.startsWith('.')) Ext.remove(0, 1); }
        for (int I = 0; I < builtinExtensions_->count(); ++I)
            if (builtinExtensions_->item(I)->checkState() == Qt::Unchecked) Excluded.append(builtinExtensions_->item(I)->text());
        Excluded.removeDuplicates();
        auto Rule = Record["application"].toObject();
        Rule["version"] = 1; Rule["mode"] = "excludeExtensions"; Rule["enabled"] = enabled_->isChecked();
        Rule["extensions"] = QJsonArray::fromStringList(Excluded); Record["application"] = Rule;
        setRecord(Value, Record); return;
    }
    // Future or malformed schemas stay intact until a compatible editor is used.
    if (editable_->isEnabled()) {
        QStringList Chosen; for (int Index = 0; Index < extensions_->count(); ++Index) if (extensions_->item(Index)->checkState() == Qt::Checked) Chosen.append(extensions_->item(Index)->text());
        Record["application"] = ruleObject(enabled_->isChecked(), Value.role_ == 3 ? "all" : mode_->currentIndex() == 0 ? "all" : mode_->currentIndex() == 1 ? "extensions" : dialect_->currentIndex() == 0 ? "tc" : "pattern", Chosen, expression_->toPlainText());
        if (Value.role_ == 3) { auto Original = record(Value).value("application").toObject(); if (Original.isEmpty()) Original = ruleObject(true, "all", {}, {}); Original["enabled"] = enabled_->isChecked(); Record["application"] = Original; }
        auto Exts = registered_->text().split(QRegularExpression("[;,|\\s]+"), Qt::SkipEmptyParts);
        for (auto& Ext : Exts) { Ext = Ext.toLower(); if (Ext.startsWith("*.")) Ext.remove(0, 2); else if (Ext.startsWith('.')) Ext.remove(0, 1); }
        Exts.removeDuplicates(); Record["extensions"] = QJsonArray::fromStringList(Exts);
    }
    if (Value.group_ == "wlx" && native_->toPlainText() != Record["detect"].toString()) {
        Record["detect"] = native_->toPlainText(); Record["detectOverride"] = true;
    }
    setRecord(Value, Record);
}
bool QPluginManagementPage::State::validate(bool Show) {
    storeItem(); QString Message;
    if (row_ >= 0 && entries_[row_].builtin_ && builtinPanel_->isEnabled()) {
        const auto Parsed = PluginApplicationRule::Parse(hc::plugins::BuiltinApplication(plugins_, entries_[row_].id_));
        if (!Parsed.HasValue()) Message = QStringLiteral("제외 확장자는 pdf; epub처럼 ; 로 구분해 입력하세요.");
    }
    if (row_ >= 0 && !entries_[row_].builtin_) {
        const auto& Value = entries_[row_]; const auto Record = record(Value);
        auto Parsed = PluginApplicationRule::Parse(Record.value("application"));
        // Unknown original schemas are read-only and may be retained on Apply.
        if (editable_->isEnabled() && !Parsed.HasValue()) Message = QStringLiteral("적용 조건을 확인하세요: ") + QString::fromStdString(Parsed.Failure().code_);
        if (Parsed.HasValue() && !Parsed.Value().NameOnly() && ((Value.role_ != 2 && Value.role_ != 4) || !hosted_->isChecked())) Message = QStringLiteral("이 종류는 파일명 조건만 지원합니다. EXT 조건 또는 파일명 패턴을 사용하세요.");
        auto Exts = PluginApplicationRule::Parse(ruleObject(true, "extensions", strings(Record.value("extensions")), {}));
        if (!Exts.HasValue()) Message = QStringLiteral("등록 형식은 확장자를 ; 로 구분해 입력하세요.");
        if (Value.group_ == "wcx" && strings(Record.value("extensions")).isEmpty()) Message = QStringLiteral("압축 플러그인의 등록 형식을 입력하세요. 예: 7z; rar; lha");
        if (Value.group_ == "wlx" && Record["detectOverride"].toBool() && !native_->toPlainText().trimmed().isEmpty()) {
            const auto Bytes = native_->toPlainText().toLocal8Bit();
            auto Detect = hc::plugins::WlxDetectionExpression::Parse(Bytes);
            if (!Detect.HasValue() || QString::fromLocal8Bit(Bytes) != native_->toPlainText()) Message = QStringLiteral("탐지식 문법과 문자 인코딩을 확인하세요.");
        }
    }
    if (!Message.isEmpty()) { ruleStatus_->setText(Message); if (Show) { tabs_->setCurrentIndex(0); QMessageBox::warning(owner_, QStringLiteral("플러그인 적용 조건"), Message); } return false; }
    return true;
}
void QPluginManagementPage::State::add() {
    if (!validate()) return; storeHosts();
    QDialog Dialog(owner_); Dialog.setObjectName("pluginAddType"); Dialog.setWindowTitle(QStringLiteral("플러그인 추가")); auto* Form = new QFormLayout(&Dialog); QComboBox Format;
    Format.setObjectName("pluginAddFormat"); Format.setAccessibleName(QStringLiteral("플러그인 종류"));
    Format.addItem(QStringLiteral("토탈 커맨더 플러그인"), -1);
    Format.addItem(QStringLiteral("DOpus / 문서"), 2);
    Format.addItem(QStringLiteral("DOpus / 파일 시스템"), 4);
    Form->addRow(QStringLiteral("종류"), &Format); QDialogButtonBox Buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); Form->addRow(&Buttons);
    QObject::connect(&Buttons, &QDialogButtonBox::accepted, &Dialog, &QDialog::accept); QObject::connect(&Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
    if (Dialog.exec() != QDialog::Accepted) return;
    const auto Index = Format.currentData().toInt();
    const auto Filter = Index < 0 ? QStringLiteral("토탈 커맨더 플러그인 및 ZIP (*.wcx *.wcx64 *.wlx *.wlx64 *.wfx *.wfx64 *.wdx *.wdx64 *.zip);;ZIP 패키지 (*.zip);;모든 파일 (*)")
        : QStringLiteral("x64 플러그인 (*.dll);;모든 파일 (*)");
    const auto Path = QFileDialog::getOpenFileName(owner_, QStringLiteral("플러그인 파일 선택"), {}, Filter); if (Path.isEmpty()) return;
    if (Index < 0) { addTc(Path, true); return; }
    registerFile(Path, Index, {}, true);
}
bool QPluginManagementPage::State::registerFile(const QString& Path, int Index, const QStringList& Extensions, bool Review) {
    const auto& Kind = Formats[Index];
    auto Items = plugins_.value(Kind.group_).toArray();
    for (int At = 0; At < Items.size(); ++At) if (resolvedPath(Items[At].toObject().value("path").toString(), root_, Kind.group_).compare(Path, Qt::CaseInsensitive) == 0) { roles_->setCurrentRow(0); search_->clear(); rebuild(key(Index, At)); return true; }
    if (Items.size() >= 1000) { QMessageBox::warning(owner_, QStringLiteral("플러그인 추가"), QStringLiteral("같은 종류의 플러그인은 최대 1000개까지 등록할 수 있습니다.")); return false; }
    Items.append(QJsonObject{{"path", Path}, {"hostEnabled", true}, {"extensions", QJsonArray::fromStringList(Extensions)}}); plugins_[Kind.group_] = Items; loadHosts();
    roles_->setCurrentRow(Kind.role_); search_->clear(); rebuild(key(Index, Items.size() - 1));
    tabs_->setCurrentIndex(1);
    if (Review) hosts_[Index]->ReviewPlugin(Items.size() - 1);
    return true;
}
bool QPluginManagementPage::State::addTc(const QString& Source, bool Review) {
    if (!validate()) return false; storeHosts();
    QString Path = QFileInfo(Source).absoluteFilePath(), Group; QStringList Extensions;
    if (QFileInfo(Path).suffix().compare("zip", Qt::CaseInsensitive) == 0) {
        auto Installed = installPackage(owner_, Path, root_);
        if (!Installed.HasValue()) {
            if (Installed.Failure().category_ != hc::storage::ErrorCategory::Cancelled)
                QMessageBox::warning(owner_, QStringLiteral("플러그인 설치 실패"), QStringLiteral("ZIP에서 설치할 64비트 토탈 커맨더 플러그인을 확인하지 못했거나 설치 경로에 쓸 수 없습니다.\n%1").arg(QString::fromStdString(Installed.Failure().code_)));
            return false;
        }
        Path = Installed.Value().binary_; Group = Installed.Value().group_; Extensions = Installed.Value().extensions_;
    } else {
        Group = hc::plugins::TcPluginGroup(Path);
        if (!Group.isEmpty() && Path.endsWith('.' + Group, Qt::CaseInsensitive)) Path += "64";
        if (Group.isEmpty() || !hc::plugins::tcPackage::Binary64(Path)) {
            QMessageBox::warning(owner_, QStringLiteral("플러그인 추가"), QStringLiteral("64비트 WCX·WLX·WFX·WDX 파일 또는 플러그인 ZIP 패키지를 선택하세요. 32비트 파일을 선택한 경우 같은 위치의 64비트 파일이 필요합니다.")); return false;
        }
    }
    for (int Index = 0; Index < int(Formats.size()); ++Index)
        if (Group == QLatin1String(Formats[Index].group_)) return registerFile(Path, Index, Extensions, Review);
    return false;
}
void QPluginManagementPage::State::remove() {
    if (row_ < 0 || entries_[row_].builtin_) return;
    const auto Value = entries_[row_];
    if (QMessageBox::question(owner_, QStringLiteral("등록 제거"), QStringLiteral("%1 등록을 제거하시겠습니까? 파일은 삭제하지 않습니다.").arg(Value.name_)) != QMessageBox::Yes) return;
    storeHosts(); auto Items = plugins_.value(Value.group_).toArray(); Items.removeAt(Value.index_); plugins_[Value.group_] = Items; loadHosts(); rebuild();
}
void QPluginManagementPage::State::move(int Direction) {
    if (!validate() || row_ < 0 || entries_[row_].builtin_) return;
    storeHosts(); const auto Value = entries_[row_]; auto Items = plugins_.value(Value.group_).toArray(); const auto To = Value.index_ + Direction;
    if (To < 0 || To >= Items.size()) return;
    const auto Saved = Items[Value.index_]; Items[Value.index_] = Items[To]; Items[To] = Saved; plugins_[Value.group_] = Items;
    loadHosts(); rebuild(key(Value.format_, To));
}
void QPluginManagementPage::State::testFiles() {
    if (!validate() || testWatcher_.isRunning()) return;
    const auto Files = QFileDialog::getOpenFileNames(owner_, QStringLiteral("조건을 확인할 파일 선택")); if (Files.isEmpty()) return;
    if (Files.size() > 32) { QMessageBox::information(owner_, QStringLiteral("조건 확인"), QStringLiteral("한 번에 최대 32개 파일을 선택하세요.")); return; }
    storeHosts(); catalog(); auto Entries = entries_; const auto Plugins = plugins_; const auto Role = testAction_->currentData().toInt();
    std::stable_sort(Entries.begin(), Entries.end(), [](const Entry& Left, const Entry& Right) { return Left.builtin_ < Right.builtin_; });
    results_->clear(); results_->show(); test_->setEnabled(false);
    testWatcher_.setFuture(QtConcurrent::run([Files, Entries, Plugins, Role] {
        QVector<QStringList> Rows;
        for (const auto& Path : Files) {
            const QFileInfo File(Path); QStringList Candidates;
            for (const auto& Value : Entries) {
                if ((Value.role_ != (Role == 5 ? 2 : Role) && !(Role == 4 && Value.actions_.contains(QStringLiteral("파일 정보")))) || Role == 3) continue;
                if (Role == 2 && !Value.actions_.contains(QStringLiteral("보기"))) continue;
                if (Role == 5 && !Value.actions_.contains(QStringLiteral("편집"))) continue;
                if (Value.builtin_) {
                    const auto Rule = PluginApplicationRule::Parse(hc::plugins::BuiltinApplication(Plugins, Value.id_));
                    if (!Rule.HasValue()) continue;
                    const auto Allowed = Rule.Value().Matches(File.fileName());
                    if (!Allowed.HasValue() || !Allowed.Value()) continue;
                    if (Value.id_ == "builtin.hypereditor" && (File.suffix().compare("hwp", Qt::CaseInsensitive) == 0 || File.suffix().compare("hwpx", Qt::CaseInsensitive) == 0)) continue;
                    if ((Value.id_ == "builtin.hyperviewer" || Value.id_ == "builtin.hypereditor") || Value.extensions_.contains(File.suffix(), Qt::CaseInsensitive)) Candidates.append(Value.name_ + QStringLiteral(" (내장)")); continue;
                }
                const auto Record = Plugins.value(Value.group_).toArray()[Value.index_].toObject(); auto Rule = PluginApplicationRule::Parse(Record.value("application")); if (!Rule.HasValue()) continue;
                if (!Value.extensions_.isEmpty() && !Value.extensions_.contains(File.suffix(), Qt::CaseInsensitive)) continue;
                std::optional<QByteArray> Prefix;
                if (Rule.Value().NeedsPrefix()) { QFile Input(Path); if (!Input.open(QIODevice::ReadOnly)) continue; Prefix = Input.read(8192); }
                auto Match = Rule.Value().Matches(File.fileName(), quint64(File.size()), Prefix);
                if (Match.HasValue() && Match.Value() && Value.group_ == "wlx" && !Record["detect"].toString().trimmed().isEmpty()) {
                    const auto Detection = PluginApplicationRule::Parse(ruleObject(true, "tc", {}, Record["detect"].toString()));
                    if (!Detection.HasValue()) continue;
                    if (Detection.Value().NeedsPrefix() && !Prefix) { QFile Input(Path); if (!Input.open(QIODevice::ReadOnly)) continue; Prefix = Input.read(8192); }
                    Match = Detection.Value().Matches(File.fileName(), quint64(File.size()), Prefix);
                }
                if (Match.HasValue() && Match.Value()) Candidates.append(Value.name_);
            }
            Rows.append({File.fileName(), Candidates.isEmpty() ? QStringLiteral("조건 후보 없음") : Candidates.join(", "), QStringLiteral("고유 감지·승인은 실행 시 확인")});
        }
        return Rows;
    }));
}

QPluginManagementPage::QPluginManagementPage(QWidget* Parent, QString ProductDirectory) : QWidget(Parent), state_(std::make_unique<State>(this, ProductDirectory.isEmpty() ? QCoreApplication::applicationDirPath() : std::move(ProductDirectory))) {
    setObjectName("pluginManagementPage"); auto& S = *state_; auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0); Layout->setSpacing(10);
    auto* Header = new QLabel(tr("플러그인 관리")); auto Font = Header->font(); Font.setPointSize(Font.pointSize() + 3); Font.setBold(true); Header->setFont(Font); Layout->addWidget(Header);
    auto* Hint = new QLabel(tr("① 종류와 플러그인 선택   ② 적용 조건 설정   ③ 조건 확인 후 적용")); Hint->setWordWrap(true); Layout->addWidget(Hint);
    auto* Body = new QHBoxLayout; Layout->addLayout(Body, 1);
    S.roles_ = new QListWidget; S.roles_->setObjectName("pluginRoles"); S.roles_->addItems({tr("전체"), tr("압축"), tr("문서"), tr("파일 시스템"), tr("파일 정보")}); S.roles_->setFixedWidth(116); S.roles_->setSpacing(3); for (int Row = 0; Row < S.roles_->count(); ++Row) S.roles_->item(Row)->setSizeHint(QSize(0, 30)); Body->addWidget(S.roles_);
    auto* Catalog = new QVBoxLayout; Body->addLayout(Catalog, 3); S.search_ = new QLineEdit; S.search_->setPlaceholderText(tr("이름·지원 형식 검색")); S.search_->setClearButtonEnabled(true); S.search_->setObjectName("pluginSearch"); Catalog->addWidget(S.search_);
    S.list_ = new QTreeWidget; S.list_->setObjectName("pluginCatalog"); S.list_->setHeaderLabels({tr("처리기 / 지원 형식"), tr("지원 작업")}); S.list_->setRootIsDecorated(false); S.list_->setIndentation(10); S.list_->setUniformRowHeights(false); S.list_->header()->setSectionResizeMode(0, QHeaderView::Stretch); S.list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents); S.list_->setMinimumWidth(270); Catalog->addWidget(S.list_, 1);
    auto* CatalogButtons = new QHBoxLayout; Catalog->addLayout(CatalogButtons); auto* Add = new QPushButton(tr("추가…")); Add->setObjectName("pluginAdd"); CatalogButtons->addWidget(Add); S.remove_ = new QPushButton(tr("제거")); S.remove_->setObjectName("pluginRemove"); CatalogButtons->addWidget(S.remove_); S.up_ = new QPushButton(tr("위로")); CatalogButtons->addWidget(S.up_); S.down_ = new QPushButton(tr("아래로")); CatalogButtons->addWidget(S.down_);
    auto* Inspector = new QVBoxLayout; Body->addLayout(Inspector, 4); S.heading_ = new QLabel; S.heading_->setFont(Font); Inspector->addWidget(S.heading_); S.support_ = new QLabel; S.support_->setWordWrap(true); S.support_->setMaximumHeight(54); Inspector->addWidget(S.support_); S.actions_ = new QLabel; Inspector->addWidget(S.actions_); S.status_ = new QLabel; Inspector->addWidget(S.status_);
    S.tabs_ = new QTabWidget; S.tabs_->setObjectName("pluginInspectorTabs"); Inspector->addWidget(S.tabs_, 1);
    S.status_->setObjectName("pluginStatus"); S.status_->setWordWrap(true);
    auto* Conditions = new QWidget; S.tabs_->addTab(Conditions, tr("적용 조건")); auto* ConditionLayout = new QVBoxLayout(Conditions); S.enabled_ = new QCheckBox(tr("이 플러그인 사용")); S.enabled_->setObjectName("pluginApplicationEnabled"); ConditionLayout->addWidget(S.enabled_);
    S.editable_ = new QWidget; auto* EditLayout = new QVBoxLayout(S.editable_); EditLayout->setContentsMargins(0, 0, 0, 0); ConditionLayout->addWidget(S.editable_, 1);
    S.builtinPanel_ = new QWidget; auto* BuiltinLayout = new QVBoxLayout(S.builtinPanel_); BuiltinLayout->setContentsMargins(0, 0, 0, 0); ConditionLayout->addWidget(S.builtinPanel_, 1);
    auto* BuiltinHint = new QLabel(tr("알려진 지원 확장자 — 사용하지 않을 항목의 체크를 해제하세요.")); BuiltinHint->setWordWrap(true); BuiltinLayout->addWidget(BuiltinHint);
    S.builtinExtensions_ = new QListWidget; S.builtinExtensions_->setObjectName("builtinApplicationExtensions"); S.builtinExtensions_->setAccessibleName(tr("내장 처리기에 적용할 알려진 확장자")); BuiltinLayout->addWidget(S.builtinExtensions_, 1);
    S.builtinExcluded_ = new QLineEdit; S.builtinExcluded_->setObjectName("builtinExcludedExtensions"); S.builtinExcluded_->setPlaceholderText(tr("추가 제외 확장자: foo; bar; tar.gz")); S.builtinExcluded_->setAccessibleName(tr("추가 제외 확장자")); BuiltinLayout->addWidget(S.builtinExcluded_);
    S.registered_ = new QLineEdit; S.registered_->setObjectName("pluginRegisteredExtensions"); S.registered_->setPlaceholderText(tr("등록 형식: 7z; rar; lha (감지형은 비워 두기)")); S.registered_->setToolTip(tr("플러그인이 지원한다고 등록한 확장자입니다. 아래 적용 범위와 구분됩니다.")); EditLayout->addWidget(S.registered_);
    S.mode_ = new QComboBox; S.mode_->setObjectName("pluginRuleMode"); S.mode_->addItems({tr("기본 감지 범위"), tr("선택한 확장자만"), tr("고급 조건식")}); EditLayout->addWidget(S.mode_); S.conditionPages_ = new QStackedWidget; EditLayout->addWidget(S.conditionPages_, 1);
    auto* Default = new QLabel(tr("등록된 지원 범위에서 플러그인 고유 감지 결과에 따라 적용합니다.")); Default->setWordWrap(true); Default->setAlignment(Qt::AlignTop); S.conditionPages_->addWidget(Default);
    S.extensions_ = new QListWidget; S.extensions_->setObjectName("pluginApplicationExtensions"); S.conditionPages_->addWidget(S.extensions_);
    auto* Advanced = new QWidget; auto* AdvancedLayout = new QVBoxLayout(Advanced); AdvancedLayout->setContentsMargins(0, 0, 0, 0); S.dialect_ = new QComboBox; S.dialect_->setObjectName("pluginRuleDialect"); S.dialect_->addItems({tr("Total Commander 감지식"), tr("파일명 패턴 (DOpus 방식의 기본 문법)")}); AdvancedLayout->addWidget(S.dialect_); S.expression_ = new QPlainTextEdit; S.expression_->setObjectName("pluginApplicationExpression"); S.expression_->setMinimumHeight(70); S.expression_->setMaximumHeight(110); S.expression_->setPlaceholderText("EXT=\"RAR\" | EXT=\"LHA\""); AdvancedLayout->addWidget(S.expression_);
    auto* SyntaxButtons = new QHBoxLayout; auto* CheckSyntax = new QPushButton(tr("문법 확인")); CheckSyntax->setObjectName("pluginCheckSyntax"); SyntaxButtons->addWidget(CheckSyntax); auto* Example = new QPushButton(tr("예제 넣기")); SyntaxButtons->addWidget(Example); SyntaxButtons->addStretch(); AdvancedLayout->addLayout(SyntaxButtons);
    connect(CheckSyntax, &QPushButton::clicked, this, [this] { if (state_->validate(false)) state_->ruleStatus_->setText(tr("구문 정상 · 플러그인 고유 감지와 함께 적용합니다.")); });
    connect(Example, &QPushButton::clicked, this, [this] { state_->expression_->setPlainText(state_->dialect_->currentIndex() == 0 ? QStringLiteral("EXT=\"RAR\" | EXT=\"LHA\"") : QStringLiteral("*.(rar|lha)")); });
    auto* Syntax = new QLabel(tr("TC: EXT=\"RAR\" | EXT=\"LHA\"\n패턴: *.(rar|lha) · *, ?, (a|b), |, ~")); Syntax->setToolTip(tr("문서 보기와 파일 정보의 별도 프로세스 실행에서는 SIZE, FIND 및 바이트 조건도 사용할 수 있습니다. 압축과 기존 실행 방식에서는 파일명 조건을 사용하세요.")); Syntax->setWordWrap(true); AdvancedLayout->addWidget(Syntax); S.conditionPages_->addWidget(Advanced);
    S.nativeLabel_ = new QLabel(tr("WLX 탐지식 — 수정·저장 가능")); EditLayout->addWidget(S.nativeLabel_);
    S.native_ = new QPlainTextEdit; S.native_->setObjectName("pluginNativeDetect"); S.native_->setAccessibleName(tr("WLX 탐지식")); S.native_->setMaximumHeight(110); S.native_->setMinimumHeight(60); S.native_->setToolTip(tr("WLX 탐지식입니다. 수정한 식은 플러그인 원본 대신 사용하며 위 추가 적용 조건도 함께 적용합니다. 비우면 탐지식으로 제한하지 않습니다.")); S.native_->setPlaceholderText(tr("WLX 탐지식 — 검사·승인된 플러그인에서 자동으로 읽습니다.")); EditLayout->addWidget(S.native_);
    S.readDetection_ = new QPushButton(tr("플러그인 원본 읽기")); S.readDetection_->setObjectName("pluginReadNativeDetect"); EditLayout->addWidget(S.readDetection_);
    connect(S.readDetection_, &QPushButton::clicked, this, [this] { state_->storeItem(); state_->requestDetection(true); });
    connect(S.native_, &QPlainTextEdit::textChanged, this, [this] { state_->storeItem(); });
    const auto RefreshBuiltinRule = [this] {
        auto& S = *state_;
        if (S.loading_ || S.row_ < 0 || !S.entries_[S.row_].builtin_) return;
        S.storeItem(); const auto& Value = S.entries_[S.row_];
        S.status_->setText(tr("상태: ") + S.stateText(Value));
        if (auto* Item = S.list_->currentItem())
            Item->setText(0, Value.name_ + "\n" + Value.support_ + "\n적용: " + PluginApplicationRule::Describe(hc::plugins::BuiltinApplication(S.plugins_, Value.id_)));
    };
    connect(S.enabled_, &QCheckBox::toggled, this, RefreshBuiltinRule);
    connect(S.builtinExtensions_, &QListWidget::itemChanged, this, RefreshBuiltinRule);
    connect(S.builtinExcluded_, &QLineEdit::textEdited, this, RefreshBuiltinRule);
    S.ruleStatus_ = new QLabel; S.ruleStatus_->setWordWrap(true); ConditionLayout->addWidget(S.ruleStatus_);
    auto* Execution = new QWidget; S.tabs_->addTab(Execution, tr("실행·승인")); auto* ExecutionLayout = new QVBoxLayout(Execution); S.hosted_ = new QCheckBox(tr("별도 프로세스로 실행")); ExecutionLayout->addWidget(S.hosted_); S.hostPages_ = new QStackedWidget; ExecutionLayout->addWidget(S.hostPages_); auto* NativeInfo = new QLabel(tr("내장 구성은 제품 실행 경로에서 검증합니다.")); NativeInfo->setWordWrap(true); S.hostPages_->addWidget(NativeInfo);
    for (int Index = 0; Index < int(Formats.size()); ++Index) { S.hosts_[Index] = new QPluginHostSettings(Formats[Index].format_, S.hostPages_); S.hosts_[Index]->SetCompactMode(); S.hostPages_->addWidget(S.hosts_[Index]); }
    auto* SelectionLabel = new QLabel(tr("빠른 보기의 문서 선택 방식")); ExecutionLayout->addWidget(SelectionLabel); S.selection_ = new QComboBox; S.selection_->setObjectName("pluginViewerSelection"); S.selection_->addItem(tr("내장 우선 · 플러그인 수동 선택"), "manual"); S.selection_->addItem(tr("자동 · 내장 이미지 우선"), "automatic"); S.selection_->addItem(tr("자동 · 플러그인 우선"), "force"); ExecutionLayout->addWidget(S.selection_); auto* Priority = new QLabel(tr("위로·아래로는 같은 종류의 외부 플러그인 탐색 순서를 바꿉니다.\nF3/F4는 보기·편집 지원 작업에 따라 선택합니다.")); Priority->setWordWrap(true); ExecutionLayout->addWidget(Priority); ExecutionLayout->addStretch();
    S.info_ = new QLabel; S.info_->setWordWrap(true); S.info_->setTextInteractionFlags(Qt::TextSelectableByMouse); S.info_->setAlignment(Qt::AlignTop); S.tabs_->addTab(S.info_, tr("정보"));
    auto* TestBox = new QGroupBox(tr("적용 조건 확인")); auto* TestLayout = new QVBoxLayout(TestBox); auto* TestRow = new QHBoxLayout; TestLayout->addLayout(TestRow); S.testAction_ = new QComboBox; S.testAction_->addItem(tr("압축"), 1); S.testAction_->addItem(tr("문서 보기 (F3)"), 2); S.testAction_->addItem(tr("문서 편집 (F4)"), 5); S.testAction_->addItem(tr("파일 정보"), 4); TestRow->addWidget(S.testAction_); S.test_ = new QPushButton(tr("파일 선택하여 확인…")); S.test_->setObjectName("pluginTestFiles"); TestRow->addWidget(S.test_); TestRow->addStretch(); auto* TestHint = new QLabel(tr("조건 후보를 표시합니다. 실제 실행 전 플러그인 고유 감지와 승인을 다시 확인합니다.")); TestHint->setWordWrap(true); TestLayout->addWidget(TestHint); S.results_ = new QTreeWidget; S.results_->setHeaderLabels({tr("파일"), tr("조건에 맞는 후보"), tr("실행 확인")}); S.results_->setRootIsDecorated(false); S.results_->header()->setSectionResizeMode(QHeaderView::Stretch); S.results_->setMaximumHeight(90); S.results_->hide(); TestLayout->addWidget(S.results_); Layout->addWidget(TestBox);
    connect(S.list_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* Current, QTreeWidgetItem* Previous) { auto& S = *state_; if (S.loading_) return; if (!S.validate()) { QSignalBlocker Block(S.list_); S.list_->setCurrentItem(Previous); return; } S.storeHosts(); S.row_ = Current && Current->data(0, Qt::UserRole).isValid() ? Current->data(0, Qt::UserRole).toInt() : -1; S.loadItem(); });
    connect(S.roles_, &QListWidget::currentRowChanged, this, [this](int) { auto& S = *state_; if (S.loading_) return; if (!S.validate()) { const QSignalBlocker Block(S.roles_); S.roles_->setCurrentRow(S.role_); return; } S.storeHosts(); S.rebuild(); });
    connect(S.search_, &QLineEdit::textChanged, this, [this] { auto& S = *state_; if (S.loading_ || !S.validate(false)) return; S.storeHosts(); S.rebuild(); });
    connect(S.hosted_, &QCheckBox::toggled, this, [this](bool Checked) { auto& S = *state_; if (!S.loading_ && S.row_ >= 0 && !S.entries_[S.row_].builtin_) S.hosts_[S.entries_[S.row_].format_]->SetHostEnabled(S.entries_[S.row_].index_, Checked); });
    for (auto* Host : S.hosts_) Host->onChanged_ = [this] { auto& S = *state_; if (S.loading_ || S.row_ < 0 || S.entries_[S.row_].builtin_) return; S.storeHosts(); const QSignalBlocker Block(S.hosted_); S.hosted_->setChecked(S.record(S.entries_[S.row_]).value("hostEnabled").toBool()); S.status_->setText(S.stateText(S.entries_[S.row_])); QTimer::singleShot(0, this, [this] { state_->requestDetection(); }); };
    connect(S.mode_, &QComboBox::currentIndexChanged, S.conditionPages_, &QStackedWidget::setCurrentIndex);
    connect(S.registered_, &QLineEdit::editingFinished, this, [this] { auto& S = *state_; if (S.loading_) return; S.storeItem(); if (S.validate(false)) S.loadItem(); });
    connect(Add, &QPushButton::clicked, this, [this] { state_->add(); }); connect(S.remove_, &QPushButton::clicked, this, [this] { state_->remove(); }); connect(S.up_, &QPushButton::clicked, this, [this] { state_->move(-1); }); connect(S.down_, &QPushButton::clicked, this, [this] { state_->move(1); }); connect(S.test_, &QPushButton::clicked, this, [this] { state_->testFiles(); });
    connect(&S.testWatcher_, &QFutureWatcherBase::finished, this, [this] { auto& S = *state_; for (const auto& Row : S.testWatcher_.result()) new QTreeWidgetItem(S.results_, Row); S.test_->setEnabled(true); });
    S.roles_->setAccessibleName(tr("플러그인 종류")); S.list_->setAccessibleName(tr("플러그인 목록"));
    S.registered_->setAccessibleName(tr("등록된 지원 확장자")); S.mode_->setAccessibleName(tr("추가 적용 조건 방식"));
    S.dialect_->setAccessibleName(tr("조건식 문법")); S.expression_->setAccessibleName(tr("추가 적용 조건식"));
    S.extensions_->setAccessibleName(tr("사용할 확장자")); S.testAction_->setAccessibleName(tr("확인할 작업")); S.selection_->setAccessibleName(tr("빠른 보기 문서 선택 방식"));
    S.roles_->setCurrentRow(0);
}
QPluginManagementPage::~QPluginManagementPage() = default;
void QPluginManagementPage::Load(hc::settings::Repository& Settings) {
    state_->settingsDirectory_ = QFileInfo(Settings.FileName()).absolutePath();
    LoadDocument(Settings.Read(hc::settings::Section::Plugins).toObject());
    for (auto* Host : state_->hosts_) Host->Load(Settings);
}
void QPluginManagementPage::LoadDocument(const QJsonObject& Plugins) {
    auto& S = *state_; S.loading_ = true; S.plugins_ = Plugins; S.loadHosts(); const auto At = S.selection_->findData(Plugins.value("viewerSelection").toString("manual")); S.selection_->setCurrentIndex(qMax(At, 0)); S.rebuild(); S.loading_ = false;
}
bool QPluginManagementPage::Validate() { return state_->validate(); }
bool QPluginManagementPage::AddTotalCommanderPlugin(const QString& Path, bool Review) { return state_->addTc(Path, Review); }
QJsonObject QPluginManagementPage::Document() const { auto& S = *state_; S.storeItem(); S.storeHosts(); auto Result = S.plugins_; Result["viewerSelection"] = S.selection_->currentData().toString(); return Result; }
bool QPluginManagementPage::SelectPlugin(const QString& Binary, bool Review) {
    auto& S = *state_; S.roles_->setCurrentRow(0); S.search_->clear();
    for (const auto& Value : S.entries_) if (!Value.builtin_ && QFileInfo(Value.path_).absoluteFilePath().compare(QFileInfo(Binary).absoluteFilePath(), Qt::CaseInsensitive) == 0) {
        const auto Id = Value.id_; const auto Format = Value.format_; const auto Index = Value.index_;
        S.rebuild(Id); S.tabs_->setCurrentIndex(1);
        if (Review) QTimer::singleShot(0, S.hosts_[Format], [Host = S.hosts_[Format], Index] { Host->ReviewPlugin(Index); });
        return true;
    }
    return false;
}
bool QPluginManagementPage::Save(hc::settings::Repository& Settings) { if (!Validate()) return false; Settings.Put(hc::settings::Section::Plugins, Document()); return true; }
