#include "qtPluginHostSettings.hpp"
#include "UniqueLibs/errorPresentation.hpp"
#include "pluginHost/manifestFile.hpp"
#include "qtMessageDialog.hpp"
#include "archive/creationSettings.hpp"
#include "pluginHost/pluginApplicationRule.hpp"
#include <QtWidgets>
#include <QtConcurrent>
#include <QFileSystemWatcher>

namespace
{
    using hc::plugins::PluginFormat;
    bool StandaloneList(PluginFormat Format)
    { return Format == PluginFormat::Wfx || Format == PluginFormat::Wdx || Format == PluginFormat::DopusVfs || Format == PluginFormat::DopusViewer; }
    QString SettingsGroup(PluginFormat Format)
    {
        switch (Format)
        {
        case PluginFormat::Wlx: return "wlx";
        case PluginFormat::Wfx: return "wfx";
        case PluginFormat::Wdx: return "wdx";
        case PluginFormat::DopusVfs: return "dopusVfs";
        case PluginFormat::DopusViewer: return "dopusViewer";
        default: return "wcx";
        }
    }
    QString FormatToken(PluginFormat Format)
    {
        switch (Format)
        {
        case PluginFormat::Wlx: return "wlx";
        case PluginFormat::Wfx: return "wfx";
        case PluginFormat::Wdx: return "wdx";
        case PluginFormat::DopusVfs: return "dopus-vfs";
        case PluginFormat::DopusViewer: return "dopus-viewer";
        default: return "wcx";
        }
    }
}
QPluginHostSettings::QPluginHostSettings(QWidget* Parent) : QPluginHostSettings(hc::plugins::PluginFormat::Wcx, Parent) {}
QPluginHostSettings::QPluginHostSettings(hc::plugins::PluginFormat Format, QWidget* Parent) : QWidget(Parent), format_(Format)
{
    const bool Wlx = Format == hc::plugins::PluginFormat::Wlx;
    const bool Data = StandaloneList(Format);
    group_ = SettingsGroup(Format);
    settingsDirectory_ = QFileInfo(hc::settings::AppSettings().FileName()).absolutePath();
    files_ = new QFileSystemWatcher(this); checkTimer_ = new QTimer(this); checkTimer_->setSingleShot(true); checkTimer_->setInterval(200);
    connect(files_, &QFileSystemWatcher::fileChanged, checkTimer_, qOverload<>(&QTimer::start));
    connect(files_, &QFileSystemWatcher::directoryChanged, checkTimer_, qOverload<>(&QTimer::start));
    connect(checkTimer_, &QTimer::timeout, this, &QPluginHostSettings::checkFiles);
    const QString Prefix = FormatToken(Format);
    auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0);
    if (Wlx)
    {
        auto* PolicyRow = new QHBoxLayout; Layout->addLayout(PolicyRow);
        auto* Label = new QLabel(tr("파일을 열 때:"), this); PolicyRow->addWidget(Label);
        selection_ = new QComboBox(this); selection_->setObjectName("wlxSelectionMode"); Label->setBuddy(selection_);
        selection_->setAccessibleName(tr("뷰어 선택 방식"));
        selection_->addItem(tr("내장 뷰어 (플러그인은 수동 선택)"), "manual");
        selection_->addItem(tr("자동 선택 (내장 이미지 뷰어 우선)"), "automatic");
        selection_->addItem(tr("자동 선택 (플러그인 우선)"), "force");
        PolicyRow->addWidget(selection_, 1);
    }
    table_ = new QTableWidget(0, 4, this); table_->setObjectName(Prefix + "HostTable");
    table_->setHorizontalHeaderLabels({tr("플러그인"), Wlx ? tr("감지 조건") : tr("확장자"), tr("호스트"), tr("승인")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->verticalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int Column = 1; Column < 4; ++Column) table_->horizontalHeader()->setSectionResizeMode(Column, QHeaderView::ResizeToContents);
    Layout->addWidget(table_);
    auto* Buttons = new QHBoxLayout; Layout->addLayout(Buttons);
    if (Data)
    {
        table_->setColumnHidden(1, true);
        auto* Add = new QPushButton(tr("추가…"), this); Add->setObjectName(Prefix + "HostAdd"); Buttons->addWidget(Add);
        auto* Remove = new QPushButton(tr("제거"), this); Remove->setObjectName(Prefix + "HostRemove"); Buttons->addWidget(Remove);
        connect(Add, &QPushButton::clicked, this, [this, Prefix] {
            const auto Binary = QFileDialog::getOpenFileName(this, tr("x64 플러그인 선택"), {}, Prefix.startsWith("dopus-") ? tr("DOpus 플러그인 (*.dll)") : QString("%1 (*.%2)").arg(Prefix, Prefix + "64"));
            if (Binary.isEmpty()) return;
            ++generation_; const QSignalBlocker Block(table_); const auto At = table_->rowCount();
            rows_.push_back({At, Binary, {}, {}}); table_->insertRow(At);
            table_->setItem(At, 0, new QTableWidgetItem(QFileInfo(Binary).fileName())); table_->item(At, 0)->setToolTip(Binary);
            table_->setItem(At, 1, new QTableWidgetItem);
            auto* Enabled = new QTableWidgetItem; Enabled->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable); Enabled->setCheckState(Qt::Checked); table_->setItem(At, 2, Enabled);
            table_->setItem(At, 3, new QTableWidgetItem); updateState(At); table_->selectRow(At);
            review_->setEnabled(true); review_->setText(tr("검사·실행 승인…"));
        });
        connect(Remove, &QPushButton::clicked, this, [this] {
            const auto At = table_->currentRow(); if (At < 0) return;
            ++generation_; const QSignalBlocker Block(table_); rows_.erase(rows_.begin() + At); table_->removeRow(At);
            for (std::size_t Index = 0; Index < rows_.size(); ++Index) rows_[Index].index_ = int(Index);
            review_->setEnabled(true); review_->setText(tr("검사·실행 승인…"));
        });
    }
    review_ = new QPushButton(tr("검사·실행 승인…"), this); review_->setObjectName(Prefix + "HostReview"); Buttons->addWidget(review_);
    auto* Revoke = new QPushButton(tr("승인 해제"), this); Revoke->setObjectName(Prefix + "HostRevoke"); Buttons->addWidget(Revoke); Buttons->addStretch();
    connect(review_, &QPushButton::clicked, this, [this] { review(); }); connect(Revoke, &QPushButton::clicked, this, [this] { revoke(); });
    if (Format == PluginFormat::Wcx || Format == PluginFormat::DopusVfs) {
        auto* Verify = new QPushButton(tr("압축 생성 지원 확인…"), this); Verify->setObjectName(Prefix + "HostVerifyCreation"); Buttons->insertWidget(2, Verify);
        connect(Verify, &QPushButton::clicked, this, &QPluginHostSettings::verifyCreation);
    }
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* Item) { if (Item->column() == 2) updateState(Item->row()); });
    connect(table_, &QTableWidget::currentCellChanged, this, [this](int Current, int, int Previous, int) {
        if (Current != Previous && !loading_ && !review_->isEnabled()) {
            ++generation_; review_->setEnabled(true); QTimer::singleShot(0, this, &QPluginHostSettings::checkFiles);
        }
        const auto At = table_->currentRow(); if (At >= 0) review_->setText(rows_[At].reviewed_.isEmpty() ? tr("검사·실행 승인…") : tr("재검사·재승인…"));
    });
}
void QPluginHostSettings::Load(hc::settings::Repository& Settings)
{
    settingsDirectory_ = QFileInfo(Settings.FileName()).absolutePath();
    LoadPlugins(Settings.Read(hc::settings::Section::Plugins).toObject());
}
void QPluginHostSettings::LoadPlugins(const QJsonObject& Plugins)
{
    loading_ = true;
    if (selection_) selection_->setCurrentIndex(qMax(0, selection_->findData(Plugins.value("viewerSelection").toString("manual"))));
    ++generation_; review_->setEnabled(true); review_->setText(tr("검사·실행 승인…")); const QSignalBlocker Block(table_); rows_.clear(); table_->setRowCount(0);
    const auto Items = Plugins.value(group_).toArray(); const auto Count = qMin(Items.size(), qsizetype(1000));
    for (unsigned Index = 0; Index < Count; ++Index)
    {
        const auto Record = Items[Index].toObject(); Row Item{static_cast<int>(Index), Record["path"].toString(),
            Record["hostManifest"].toString(), hc::settings::JsonBytes(Record["hostReviewed"])};
        Item.creation_ = Record["archiveCreation"].toObject();
        const auto At = table_->rowCount(); rows_.push_back(Item); table_->insertRow(At);
        auto* Path = new QTableWidgetItem(QFileInfo(Item.path_).fileName()); Path->setToolTip(Item.path_); table_->setItem(At, 0, Path);
        table_->setItem(At, 1, new QTableWidgetItem(format_ == hc::plugins::PluginFormat::Wlx ? Record["detect"].toString() : hc::settings::Strings(Record["extensions"]).join('|')));
        auto* Enabled = new QTableWidgetItem; Enabled->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        Enabled->setCheckState(Record["hostEnabled"].toBool() ? Qt::Checked : Qt::Unchecked); table_->setItem(At, 2, Enabled);
        table_->setItem(At, 3, new QTableWidgetItem); updateState(At);
    }
    if (Count) table_->selectRow(0);
    loading_ = false; checkFiles();
}
void QPluginHostSettings::Save(hc::settings::Repository& Settings)
{
    auto Plugins = Settings.Read(hc::settings::Section::Plugins).toObject();
    SavePlugins(Plugins); Settings.Put(hc::settings::Section::Plugins, Plugins);
}
void QPluginHostSettings::SavePlugins(QJsonObject& Plugins) const
{
    const auto Previous = Plugins[group_].toArray(); QJsonArray Items;
    if (selection_) Plugins["viewerSelection"] = selection_->currentData().toString();
    for (std::size_t Index = 0; Index < rows_.size(); ++Index) {
        const auto& Row = rows_[Index];
        auto Record = Row.index_ < Previous.size() ? Previous[Row.index_].toObject() : QJsonObject{};
        Record["path"] = Row.path_;
        Record["hostEnabled"] = table_->item(static_cast<int>(Index), 2)->checkState() == Qt::Checked;
        Record["hostManifest"] = Row.manifest_; Record["hostReviewed"] = hc::settings::JsonObject(Row.reviewed_);
        if (!Row.creation_.isEmpty()) Record["archiveCreation"] = Row.creation_;
        Items.append(Record);
    }
    Plugins[group_] = Items;
}
void QPluginHostSettings::updateState(int Row)
{
    if (Row < 0 || static_cast<std::size_t>(Row) >= rows_.size() || !table_->item(Row, 3)) return;
    table_->item(Row, 3)->setText(table_->item(Row, 2)->checkState() != Qt::Checked ? (StandaloneList(format_) ? tr("사용 안 함") : tr("기존 방식"))
        : !hc::plugins::ParseManifest(rows_[Row].reviewed_).HasValue() ? tr("실행 승인 필요")
        : rows_[Row].verification_.isEmpty() ? tr("검사 중…") : rows_[Row].verification_);
    table_->item(Row, 3)->setToolTip(rows_[Row].manifest_);
    if (!loading_ && onChanged_) onChanged_();
}
void QPluginHostSettings::revoke()
{
    const auto Row = table_->currentRow(); if (Row < 0) return;
    ++generation_; review_->setEnabled(true); review_->setText(tr("검사·실행 승인…")); rows_[Row].reviewed_.clear(); rows_[Row].verification_.clear();
    // Revoking trust must not silently switch execution into the application.
    table_->item(Row, 2)->setCheckState(Qt::Checked); updateState(Row);
}
void QPluginHostSettings::verifyCreation()
{
    const auto At = table_->currentRow(); if (At < 0) return;
    const auto& Row = rows_[At];
    if (Row.reviewed_.isEmpty() || table_->item(At, 2)->checkState() != Qt::Checked) {
        hc::ui::MessageDialog::information(this, tr("압축 생성 지원 확인"), tr("먼저 플러그인을 검사하고 실행을 승인하십시오.")); return;
    }
    bool Accepted = false;
    const auto Text = hc::ui::InputDialog::getText(this, tr("압축 생성 지원 확인"),
        tr("확인할 생성 형식의 확장자를 입력하십시오 (예: 7z, zip).\n임시 파일을 생성하고 다시 읽어 내용과 경로가 일치하는 형식만 등록합니다."), QLineEdit::Normal,
        table_->item(At, 1)->text().replace('|', ", "), &Accepted);
    if (!Accepted) return;
    const auto Formats = Text.toLower().split(QRegularExpression("[,;|\\s]+"), Qt::SkipEmptyParts);
    if (Formats.isEmpty() || Formats.size() > 32) return;
    QJsonObject Record{{"path", Row.path_}, {"hostManifest", Row.manifest_}, {"hostReviewed", hc::settings::JsonObject(Row.reviewed_)}, {"hostEnabled", true}};
    auto Creator = hc::archive::ExternalCreator(Record, format_, settingsDirectory_);
    const auto Result = hc::archive::RunArchiveWorker(this, tr("압축 생성 지원 확인"), [Creator, Formats](hc::storage::OperationContext Context) { return hc::archive::VerifyCreationFormats(Creator, Formats, Context); });
    if (!Result.HasValue()) { if (Result.Failure().category_ != hc::storage::ErrorCategory::Cancelled) hc::ui::MessageDialog::warning(this, tr("압축 생성 지원 확인"), hc::ui::ErrorText(Result.Failure())); return; }
    rows_[At].creation_ = Result.Value(); updateState(At);
    QStringList Verified; for (const auto& Format : Result.Value()["formats"].toArray()) Verified << Format.toString().toUpper();
    hc::ui::MessageDialog::information(this, tr("압축 생성 지원 확인"), tr("생성 가능한 형식: %1").arg(Verified.join(", ")));
}
QString QPluginHostSettings::resolvedBinary(const QString& Path) const
{
    auto Binary = Path; Binary.replace("%COMMANDER_PATH%", QCoreApplication::applicationDirPath(), Qt::CaseInsensitive);
    const auto Group = SettingsGroup(format_);
    if (Binary.endsWith('.' + Group, Qt::CaseInsensitive)) Binary += "64";
    return QDir::isRelativePath(Binary) ? QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(Binary) : Binary;
}
QString QPluginHostSettings::VerificationState(int Index) const
{ return Index >= 0 && Index < table_->rowCount() ? table_->item(Index, 3)->text() : QString{}; }
void QPluginHostSettings::ReviewPlugin(int Index)
{ SelectPlugin(Index); if (table_->currentRow() == Index && review_->isEnabled()) review(); }
void QPluginHostSettings::checkFiles()
{
    using namespace hc::plugins; using namespace hc::storage;
    const auto Generation = generation_;
    const auto CheckGeneration = ++checkGeneration_;
    QStringList Paths;
    for (int At = 0; At < int(rows_.size()); ++At) {
        auto& Row = rows_[At]; const auto Reviewed = ParseManifest(Row.reviewed_);
        if (!Reviewed.HasValue()) continue;
        const auto Binary = resolvedBinary(Row.path_); const auto ManifestPath = Row.manifest_;
        Paths << Binary << QFileInfo(Binary).absolutePath() << ManifestPath;
        const auto Root = Reviewed.Value().packageRoot_.empty() ? QFileInfo(ManifestPath).absolutePath() : QString::fromStdString(Reviewed.Value().packageRoot_);
        for (const auto& [Name, Hash] : Reviewed.Value().dependencies_) Paths << QDir(Root).filePath(QString::fromStdString(Name));
        for (const auto& [Name, Hash] : Reviewed.Value().helpers_) Paths << QDir(Root).filePath(QString::fromStdString(Name));
        auto* Watcher = new QFutureWatcher<Result<InspectedPlugin>>(this);
        connect(Watcher, &QFutureWatcher<Result<InspectedPlugin>>::finished, this, [this, Watcher, At, Generation, CheckGeneration, Reviewed, Binary] {
            const auto Checked = Watcher->result(); Watcher->deleteLater(); if (Generation != generation_ || CheckGeneration != checkGeneration_ || At >= rows_.size()) return;
            rows_[At].verification_ = Checked.HasValue() && Checked.Value().manifest_ == Reviewed.Value()
                && QFileInfo(Binary).canonicalFilePath().compare(Checked.Value().binaryPath_, Qt::CaseInsensitive) == 0 ? tr("승인됨 · 파일 확인 완료")
                : tr("파일 또는 선언 변경됨 · 검사·재승인 필요");
            updateState(At);
        });
        Watcher->setFuture(QtConcurrent::run([ManifestPath] { OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10); return InspectManifestFile(ManifestPath, Context); }));
    }
    if (!files_->files().isEmpty()) files_->removePaths(files_->files());
    if (!files_->directories().isEmpty()) files_->removePaths(files_->directories());
    Paths.removeDuplicates(); Paths.removeAll(QString{});
    for (const auto& Path : Paths) if (QFileInfo::exists(Path)) files_->addPath(Path);
}
void QPluginHostSettings::review()
{
    using namespace hc::plugins; using namespace hc::storage;
    const auto At = table_->currentRow(); if (At < 0) return;
    const auto Binary = resolvedBinary(rows_[At].path_);
    const auto Old = ParseManifest(rows_[At].reviewed_);
    const auto Generation = ++generation_; review_->setEnabled(false); review_->setText(tr("검사 중…"));
    auto* Watcher = new QFutureWatcher<Result<InspectedPlugin>>(this);
    connect(Watcher, &QFutureWatcher<Result<InspectedPlugin>>::finished, this, [this, Watcher, At, Generation, Old, Binary] {
        const auto Checked = Watcher->result(); Watcher->deleteLater(); if (Generation != generation_) return;
        const auto Reset = [this] { review_->setEnabled(true); review_->setText(tr("검사·재승인…")); };
        if (!Checked.HasValue()) { Reset(); hc::ui::MessageDialog::warning(this, tr("플러그인 검사 실패"), tr("플러그인 파일을 검사하지 못했습니다. 파일과 함께 설치한 DLL을 확인하세요.\n%1").arg(hc::ui::ErrorText(Checked.Failure()))); return; }
        auto Plugin = Checked.Value(); const auto& Manifest = Plugin.manifest_;
        PluginApproval Approval{Manifest, true, false, Manifest.permissions_};
        HostProfile Profile; Profile.formats_ = {format_}; Profile.supportsIsolation_ = true; Profile.extensions_ = Manifest.extensions_;
        const auto Admitted = Admit(Manifest, Approval, Profile);
        if (!Admitted.HasValue()) { Reset(); hc::ui::MessageDialog::warning(this, tr("플러그인 검사 실패"), hc::ui::ErrorText(Admitted.Failure())); return; }
        const bool Changed = Old.HasValue() && Old.Value() != Manifest;
        QStringList Permissions{tr("파일 읽기")};
        if (Manifest.permissions_.contains(Permission::WriteContent)) Permissions << tr("파일 변경");
        if (Manifest.permissions_.contains(Permission::UserInterface)) Permissions << tr("플러그인 창 표시");
        if (Manifest.permissions_.contains(Permission::Network)) Permissions << tr("네트워크");
        if (Manifest.permissions_.contains(Permission::ChildProcess)) Permissions << tr("외부 헬퍼 실행");
        auto Message = (Changed ? tr("플러그인 파일 또는 구성이 변경되었습니다. 새 파일을 확인하고 다시 승인하세요.\n\n") : QString{})
            + tr("%1\n%2\n\n요청 권한: %3\nSHA-256: %4\n함께 검증할 DLL: %5개\n\n별도 프로세스에서 현재 사용자 권한으로 실행합니다. 신뢰할 수 있는 출처의 파일인지 확인하세요.")
                .arg(QFileInfo(Binary).fileName(), Binary, Permissions.join(", "), QString::fromStdString(Manifest.sha256_)).arg(Manifest.dependencies_.size());
        if (Old.HasValue()) Message += tr("\n이전 SHA-256: %1").arg(QString::fromStdString(Old.Value().sha256_));
        if (Changed) {
            QStringList Changes;
            if (Old.Value().sha256_ != Manifest.sha256_) Changes << QString::fromStdString(Manifest.binary_);
            const auto CompareFiles = [&](const auto& Before, const auto& After) {
                for (const auto& [Name, Hash] : After) if (!Before.contains(Name) || Before.at(Name) != Hash) Changes << QString::fromStdString(Name);
                for (const auto& [Name, Hash] : Before) if (!After.contains(Name)) Changes << tr("%1 (제거됨)").arg(QString::fromStdString(Name));
            };
            CompareFiles(Old.Value().dependencies_, Manifest.dependencies_); CompareFiles(Old.Value().helpers_, Manifest.helpers_);
            if (!Changes.isEmpty()) Message += tr("\n변경 파일: %1").arg(Changes.mid(0, 8).join(", ") + (Changes.size() > 8 ? tr(" 외 %1개").arg(Changes.size() - 8) : QString{}));
        }
        hc::ui::MessageDialog Prompt(QMessageBox::Question, Changed ? tr("플러그인 변경 확인·재승인") : tr("플러그인 실행 승인"), Message, QMessageBox::Yes | QMessageBox::No, this);
        Prompt.button(QMessageBox::Yes)->setText(tr("승인(&A)")); Prompt.button(QMessageBox::No)->setText(tr("취소"));
        Prompt.setObjectName("hc.plugin.reviewApproval"); Prompt.setTextFormat(Qt::PlainText); Prompt.setDefaultButton(QMessageBox::No);
        Prompt.setDetailedText(QString::fromUtf8(EncodeManifest(Manifest)));
        QCheckBox* AllowSettings = nullptr;
        if (format_ == PluginFormat::Wcx && !Manifest.permissions_.contains(Permission::UserInterface)) {
            AllowSettings = new QCheckBox(tr("플러그인 설정 창 표시 권한도 승인"), &Prompt);
            AllowSettings->setObjectName("hc.plugin.allowSettingsUi"); AllowSettings->setChecked(false); Prompt.setCheckBox(AllowSettings);
        }
        if (Prompt.exec() != QMessageBox::Yes || Generation != generation_) { Reset(); return; }
        if (AllowSettings && AllowSettings->isChecked()) Plugin.manifest_.permissions_.insert(Permission::UserInterface);
        auto* Save = new QFutureWatcher<Result<QString>>(this);
        connect(Save, &QFutureWatcher<Result<QString>>::finished, this, [this, Save, Plugin, At, Generation, Reset] {
            const auto Saved = Save->result(); Save->deleteLater(); if (Generation != generation_) return; Reset();
            if (!Saved.HasValue()) { hc::ui::MessageDialog::warning(this, tr("승인 저장 실패"), tr("검사 중 파일이 바뀌었거나 승인 정보를 저장할 수 없습니다. 다시 검사해 주세요.\n%1").arg(hc::ui::ErrorText(Saved.Failure()))); return; }
            rows_[At].manifest_ = Saved.Value(); rows_[At].reviewed_ = EncodeManifest(Plugin.manifest_); rows_[At].verification_ = tr("승인됨 · 파일 확인 완료");
            table_->item(At, 2)->setCheckState(Qt::Checked); updateState(At); checkFiles();
        });
        Save->setFuture(QtConcurrent::run([Plugin, Directory = QDir(settingsDirectory_).filePath("plugin-manifests")] {
            OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10); return SavePluginManifest(Plugin, Directory, Context);
        }));
    });
    Watcher->setFuture(QtConcurrent::run([Binary, Format = format_, Previous = Old.HasValue() ? std::optional<PluginManifest>(Old.Value()) : std::nullopt] {
        OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10); return InspectPluginBinary(Binary, Format, Previous, Context);
    }));
}
QtViewerSurface::PluginViewerSelectionMode QPluginHostSettings::ViewerSelection(hc::settings::Repository& Settings)
{
    const auto Mode = Settings.Read(hc::settings::Section::Plugins).toObject()["viewerSelection"].toString("manual");
    if (Mode == "automatic") return QtViewerSurface::PluginViewerSelectionMode::Automatic;
    if (Mode == "force") return QtViewerSurface::PluginViewerSelectionMode::Force;
    return QtViewerSurface::PluginViewerSelectionMode::Manual;
}
std::vector<QtViewerSurface::PluginViewerChoice> QPluginHostSettings::ViewerChoices(hc::settings::Repository& Settings)
{
    using namespace hc::plugins;
    std::vector<QtViewerSurface::PluginViewerChoice> Choices;
    const auto Items = Settings.Plugins("wlx"); const auto Count = qMin(Items.size(), qsizetype(1000));
    for (unsigned Index = 0; Index < Count; ++Index)
    {
        const auto Record = Items[Index].toObject();
        const auto Rule = PluginApplicationRule::Parse(Record.value("application"));
        if (!Rule.HasValue() || !Rule.Value().Enabled() || !Record["hostEnabled"].toBool()) continue;
        QtViewerSurface::PluginViewerChoice Choice; Choice.id_ = QString::number(Index);
        Choice.extensions_ = hc::settings::Strings(Record.value("extensions"));
        auto& Options = Choice.options_; Options.binaryPath_ = Record["path"].toString(); Options.applicationRule_ = Record.value("application");
        if (Record["detectOverride"].toBool() || (!Record.contains("detectOverride") && !Record["detect"].toString().isEmpty()))
            Options.detectionOverride_ = Record["detect"].toString();
        Options.binaryPath_.replace("%COMMANDER_PATH%", QCoreApplication::applicationDirPath(), Qt::CaseInsensitive);
        Choice.name_ = QFileInfo(Options.binaryPath_).fileName();
        if (Options.binaryPath_.endsWith(".wlx", Qt::CaseInsensitive)) Options.binaryPath_ += "64";
        if (QDir::isRelativePath(Options.binaryPath_)) Options.binaryPath_ = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(Options.binaryPath_);
        Options.executable_ = QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe";
        Options.defaultIniPath_ = QDir(QFileInfo(Settings.FileName()).absolutePath()).filePath("lsplugin.ini");
        Options.manifestPath_ = Record["hostManifest"].toString();
        const auto Reviewed = ParseManifest(hc::settings::JsonBytes(Record["hostReviewed"]));
        if (Reviewed.HasValue() && Reviewed.Value().format_ == PluginFormat::Wlx)
        { Options.approval_.reviewed_ = Reviewed.Value(); Options.approval_.approved_ = true; Options.approval_.granted_ = Reviewed.Value().permissions_; }
        // Keep revoked entries selectable so execution reports the denial. No
        // DLL inspection/loading or content I/O occurs while building this menu.
        Choices.push_back(std::move(Choice));
    }

    auto Dopus = DataChoices(Settings, PluginFormat::DopusViewer);
    for (auto& Choice : Dopus)
    {
        if (Choice.options_.approval_.approved_ && !Choice.options_.approval_.reviewed_.extensions_.contains(Extension::Viewer)) continue;
        Choice.id_ = "dopus-" + Choice.id_; Choices.push_back(std::move(Choice));
    }
    return Choices;
}

std::vector<QtViewerSurface::PluginViewerChoice> QPluginHostSettings::DataChoices(hc::settings::Repository& Settings, hc::plugins::PluginFormat Format)
{
    using namespace hc::plugins;
    std::vector<QtViewerSurface::PluginViewerChoice> Choices;
    const auto Items = Settings.Plugins(SettingsGroup(Format)); const auto Count = qMin(Items.size(), qsizetype(1000));
    for (unsigned Index = 0; Index < Count; ++Index)
    {
        const auto Record = Items[Index].toObject();
        const auto Rule = PluginApplicationRule::Parse(Record.value("application"));
        if (!Rule.HasValue() || !Rule.Value().Enabled() || !Record["hostEnabled"].toBool()) continue;
        if ((Format == PluginFormat::Wfx || Format == PluginFormat::DopusVfs) && Record.value("application").toObject().value("mode").toString("all") != "all") continue;
        QtViewerSurface::PluginViewerChoice Choice; Choice.id_ = QString::number(Index);
        Choice.extensions_ = hc::settings::Strings(Record.value("extensions"));
        auto& Options = Choice.options_; Options.binaryPath_ = Record["path"].toString(); Options.applicationRule_ = Record.value("application");
        Options.binaryPath_.replace("%COMMANDER_PATH%", QCoreApplication::applicationDirPath(), Qt::CaseInsensitive);
        Choice.name_ = QFileInfo(Options.binaryPath_).fileName();
        if ((Format == PluginFormat::Wfx || Format == PluginFormat::Wdx) && Options.binaryPath_.endsWith((Format == PluginFormat::Wfx ? ".wfx" : ".wdx"), Qt::CaseInsensitive)) Options.binaryPath_ += "64";
        if (QDir::isRelativePath(Options.binaryPath_)) Options.binaryPath_ = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(Options.binaryPath_);
        Options.executable_ = QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe";
        Options.defaultIniPath_ = QDir(QFileInfo(Settings.FileName()).absolutePath()).filePath((Format == PluginFormat::Wfx ? "fsplugin.ini" : "contentplugin.ini"));
        Options.manifestPath_ = Record["hostManifest"].toString();
        const auto Reviewed = ParseManifest(hc::settings::JsonBytes(Record["hostReviewed"]));
        if (Reviewed.HasValue() && Reviewed.Value().format_ == Format)
        { Options.approval_.reviewed_ = Reviewed.Value(); Options.approval_.approved_ = true; Options.approval_.granted_ = Reviewed.Value().permissions_; }
        // Keep revoked entries selectable so execution reports the denial. No
        // DLL inspection/loading or content I/O occurs while building this menu.
        Choices.push_back(std::move(Choice));
    }
    return Choices;
}

void QPluginHostSettings::SetCompactMode() {
    table_->hide();
    if (selection_) { selection_->hide(); if (auto* Label = qobject_cast<QWidget*>(layout()->itemAt(0)->layout()->itemAt(0)->widget())) Label->hide(); }
    const auto Prefix = FormatToken(format_);
    for (const auto* Suffix : {"HostAdd", "HostRemove"}) if (auto* Button = findChild<QPushButton*>(Prefix + Suffix)) Button->hide();
}
void QPluginHostSettings::SelectPlugin(int Index) { if (Index >= 0 && Index < table_->rowCount()) table_->selectRow(Index); }
void QPluginHostSettings::SetHostEnabled(int Index, bool Enabled) { if (Index >= 0 && Index < table_->rowCount()) table_->item(Index, 2)->setCheckState(Enabled ? Qt::Checked : Qt::Unchecked); }
