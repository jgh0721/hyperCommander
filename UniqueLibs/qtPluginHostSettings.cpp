#include "qtPluginHostSettings.hpp"
#include "pluginHost/manifestFile.hpp"
#include <QtWidgets>
#include <QtConcurrent>

namespace
{
    using hc::plugins::PluginFormat;
    bool StandaloneList(PluginFormat Format)
    { return Format == PluginFormat::Wfx || Format == PluginFormat::Wdx || Format == PluginFormat::DopusVfs || Format == PluginFormat::DopusViewer; }
    QString SettingsGroup(PluginFormat Format)
    {
        switch (Format)
        {
        case PluginFormat::Wlx: return "ListerPlugins";
        case PluginFormat::Wfx: return "FileSystemPlugins";
        case PluginFormat::Wdx: return "ContentPlugins";
        case PluginFormat::DopusVfs: return "DopusVfsPlugins";
        case PluginFormat::DopusViewer: return "DopusViewerPlugins";
        default: return "PackerPlugins";
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
            review_->setEnabled(true); review_->setText(tr("호스트 실행 승인…"));
        });
        connect(Remove, &QPushButton::clicked, this, [this] {
            const auto At = table_->currentRow(); if (At < 0) return;
            ++generation_; const QSignalBlocker Block(table_); rows_.erase(rows_.begin() + At); table_->removeRow(At);
            for (std::size_t Index = 0; Index < rows_.size(); ++Index) rows_[Index].index_ = int(Index);
            review_->setEnabled(true); review_->setText(tr("호스트 실행 승인…"));
        });
    }
    review_ = new QPushButton(tr("호스트 실행 승인…"), this); review_->setObjectName(Prefix + "HostReview"); Buttons->addWidget(review_);
    auto* Revoke = new QPushButton(tr("승인 해제"), this); Revoke->setObjectName(Prefix + "HostRevoke"); Buttons->addWidget(Revoke); Buttons->addStretch();
    connect(review_, &QPushButton::clicked, this, [this] { review(); }); connect(Revoke, &QPushButton::clicked, this, [this] { revoke(); });
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* Item) { if (Item->column() == 2) updateState(Item->row()); });
}
void QPluginHostSettings::Load(QSettings& Settings)
{
    if (selection_) selection_->setCurrentIndex(static_cast<int>(ViewerSelection(Settings)));
    ++generation_; review_->setEnabled(true); review_->setText(tr("호스트 실행 승인…")); const QSignalBlocker Block(table_); rows_.clear(); table_->setRowCount(0);
    Settings.beginGroup(group_); const auto Count = std::min(Settings.value("Count").toUInt(), 1000u);
    for (unsigned Index = 0; Index < Count; ++Index)
    {
        const auto Prefix = QString::number(Index) + '_'; Row Item{static_cast<int>(Index), Settings.value(Prefix + "Path").toString(),
            Settings.value(Prefix + "HostManifest").toString(), Settings.value(Prefix + "HostReviewed").toByteArray()};
        const auto At = table_->rowCount(); rows_.push_back(Item); table_->insertRow(At);
        auto* Path = new QTableWidgetItem(QFileInfo(Item.path_).fileName()); Path->setToolTip(Item.path_); table_->setItem(At, 0, Path);
        table_->setItem(At, 1, new QTableWidgetItem(Settings.value(Prefix + (format_ == hc::plugins::PluginFormat::Wlx ? "Detect" : "Exts")).toString()));
        auto* Enabled = new QTableWidgetItem; Enabled->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        Enabled->setCheckState(Settings.value(Prefix + "HostEnabled", false).toBool() ? Qt::Checked : Qt::Unchecked); table_->setItem(At, 2, Enabled);
        table_->setItem(At, 3, new QTableWidgetItem); updateState(At);
    }
    Settings.endGroup(); if (Count) table_->selectRow(0);
}
void QPluginHostSettings::Save(QSettings& Settings)
{
    Settings.beginGroup(group_);
    const bool Data = StandaloneList(format_);
    if (Data) { Settings.remove(""); Settings.setValue("Count", int(rows_.size())); }
    if (selection_) Settings.setValue("SelectionMode", selection_->currentData().toString());
    for (std::size_t Index = 0; Index < rows_.size(); ++Index)
    {
        const auto Prefix = QString::number(rows_[Index].index_) + '_';
        if (Data) Settings.setValue(Prefix + "Path", rows_[Index].path_);
        Settings.setValue(Prefix + "HostEnabled", table_->item(static_cast<int>(Index), 2)->checkState() == Qt::Checked);
        Settings.setValue(Prefix + "HostManifest", rows_[Index].manifest_); Settings.setValue(Prefix + "HostReviewed", rows_[Index].reviewed_);
    }
    Settings.endGroup();
}
void QPluginHostSettings::updateState(int Row)
{
    if (Row < 0 || static_cast<std::size_t>(Row) >= rows_.size() || !table_->item(Row, 3)) return;
    table_->item(Row, 3)->setText(table_->item(Row, 2)->checkState() != Qt::Checked ? (StandaloneList(format_) ? tr("사용 안 함") : tr("기존 방식"))
        : hc::plugins::ParseManifest(rows_[Row].reviewed_).HasValue() ? tr("승인됨") : tr("승인 필요"));
    table_->item(Row, 3)->setToolTip(rows_[Row].manifest_);
}
void QPluginHostSettings::revoke()
{
    const auto Row = table_->currentRow(); if (Row < 0) return;
    ++generation_; review_->setEnabled(true); review_->setText(tr("호스트 실행 승인…")); rows_[Row].reviewed_.clear();
    // Revoking trust must not silently switch execution into the application.
    table_->item(Row, 2)->setCheckState(Qt::Checked); updateState(Row);
}
void QPluginHostSettings::review()
{
    using namespace hc::plugins; using namespace hc::storage;
    const auto Row = table_->currentRow(); if (Row < 0) return;
    const auto BeforeDialog = generation_;
    const auto ManifestPath = QFileDialog::getOpenFileName(this, tr("플러그인 manifest 선택"), rows_[Row].manifest_, tr("플러그인 manifest (*.json)"));
    if (ManifestPath.isEmpty() || BeforeDialog != generation_) return;
    auto Binary = rows_[Row].path_; Binary.replace("%COMMANDER_PATH%", QCoreApplication::applicationDirPath(), Qt::CaseInsensitive);
    if (Binary.endsWith(format_ == PluginFormat::Wlx ? ".wlx" : ".wcx", Qt::CaseInsensitive)) Binary += "64";
    if (QDir::isRelativePath(Binary)) Binary = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(Binary);
    const auto Generation = ++generation_; review_->setEnabled(false); review_->setText(tr("검사 중…"));
    auto* Watcher = new QFutureWatcher<Result<InspectedPlugin>>(this);
    connect(Watcher, &QFutureWatcher<Result<InspectedPlugin>>::finished, this, [this, Watcher, Row, Generation, ManifestPath] {
        const auto Result = Watcher->result(); Watcher->deleteLater(); if (Generation != generation_) return;
        review_->setEnabled(true); review_->setText(tr("호스트 실행 승인…"));
        if (!Result.HasValue()) { QMessageBox::warning(this, tr("플러그인 검사 실패"), QString::fromStdString(Result.Failure().code_)); return; }
        const auto& Manifest = Result.Value().manifest_;
        QStringList Permissions{tr("파일 읽기")};
        if (Manifest.permissions_.contains(Permission::WriteContent)) Permissions << tr("파일 변경");
        if (Manifest.permissions_.contains(Permission::Network)) Permissions << tr("네트워크");
        if (Manifest.permissions_.contains(Permission::UserInterface)) Permissions << tr("뷰어 화면 표시");
        const auto Message = tr("%1\n요청 권한: %2\n\n별도 프로세스에서 실행합니다. 플러그인은 현재 사용자 권한으로 파일에 접근할 수 있습니다.\n파일이나 선언이 변경되면 다시 승인해야 합니다.")
            .arg(QString::fromStdString(Manifest.id_), Permissions.join(", "));
        if (QMessageBox::question(this, tr("호스트 실행 승인"), Message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        if (Generation != generation_) return;
        rows_[Row].manifest_ = ManifestPath; rows_[Row].reviewed_ = EncodeManifest(Manifest);
        table_->item(Row, 2)->setCheckState(Qt::Checked); updateState(Row);
    });
    Watcher->setFuture(QtConcurrent::run([ManifestPath, Binary, Format = format_]() -> Result<InspectedPlugin> {
        OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        auto Inspected = InspectManifestFile(ManifestPath, Context); if (!Inspected.HasValue()) return Inspected;
        if (QFileInfo(Binary).canonicalFilePath().compare(Inspected.Value().binaryPath_, Qt::CaseInsensitive) != 0)
            return Error{ErrorCategory::InvalidRequest, "plugin-configured-binary-mismatch"};
        PluginApproval Approval; Approval.reviewed_ = Inspected.Value().manifest_; Approval.approved_ = true; Approval.granted_ = Approval.reviewed_.permissions_;
        HostProfile Profile; Profile.formats_ = {Format}; Profile.supportsIsolation_ = true;
        Profile.extensions_ = Format == PluginFormat::DopusViewer ? std::set<Extension>{Extension::Viewer, Extension::Metadata, Extension::ContentConvert} : Format == PluginFormat::DopusVfs ? std::set<Extension>{Extension::StorageRead, Extension::StorageWrite} : Format == PluginFormat::Wlx ? std::set<Extension>{Extension::Viewer} : Format == PluginFormat::Wfx ? std::set<Extension>{Extension::StorageRead, Extension::StorageWrite} : Format == PluginFormat::Wdx ? std::set<Extension>{Extension::Metadata} : std::set<Extension>{Extension::ArchiveRead, Extension::ArchiveWrite};
        const auto Admitted = Admit(Approval.reviewed_, Approval, Profile); if (!Admitted.HasValue()) return Admitted.Failure();
        return Inspected;
    }));
}
QtViewerSurface::PluginViewerSelectionMode QPluginHostSettings::ViewerSelection(QSettings& Settings)
{
    const auto Mode = Settings.value("ListerPlugins/SelectionMode", "manual").toString();
    if (Mode == "automatic") return QtViewerSurface::PluginViewerSelectionMode::Automatic;
    if (Mode == "force") return QtViewerSurface::PluginViewerSelectionMode::Force;
    return QtViewerSurface::PluginViewerSelectionMode::Manual;
}
std::vector<QtViewerSurface::PluginViewerChoice> QPluginHostSettings::ViewerChoices(QSettings& Settings)
{
    using namespace hc::plugins;
    std::vector<QtViewerSurface::PluginViewerChoice> Choices;
    Settings.beginGroup("ListerPlugins"); const auto Count = std::min(Settings.value("Count").toUInt(), 1000u);
    for (unsigned Index = 0; Index < Count; ++Index)
    {
        const auto Prefix = QString::number(Index) + '_';
        if (!Settings.value(Prefix + "HostEnabled", false).toBool()) continue;
        QtViewerSurface::PluginViewerChoice Choice; Choice.id_ = QString::number(Index);
        auto& Options = Choice.options_; Options.binaryPath_ = Settings.value(Prefix + "Path").toString();
        Options.binaryPath_.replace("%COMMANDER_PATH%", QCoreApplication::applicationDirPath(), Qt::CaseInsensitive);
        Choice.name_ = QFileInfo(Options.binaryPath_).fileName();
        if (Options.binaryPath_.endsWith(".wlx", Qt::CaseInsensitive)) Options.binaryPath_ += "64";
        if (QDir::isRelativePath(Options.binaryPath_)) Options.binaryPath_ = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(Options.binaryPath_);
        Options.executable_ = QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe";
        Options.defaultIniPath_ = QDir(QFileInfo(Settings.fileName()).absolutePath()).filePath("lsplugin.ini");
        Options.manifestPath_ = Settings.value(Prefix + "HostManifest").toString();
        const auto Reviewed = ParseManifest(Settings.value(Prefix + "HostReviewed").toByteArray());
        if (Reviewed.HasValue() && Reviewed.Value().format_ == PluginFormat::Wlx)
        { Options.approval_.reviewed_ = Reviewed.Value(); Options.approval_.approved_ = true; Options.approval_.granted_ = Reviewed.Value().permissions_; }
        // Keep revoked entries selectable so execution reports the denial. No
        // DLL inspection/loading or content I/O occurs while building this menu.
        Choices.push_back(std::move(Choice));
    }
    Settings.endGroup();
    auto Dopus = DataChoices(Settings, PluginFormat::DopusViewer);
    for (auto& Choice : Dopus)
    {
        if (Choice.options_.approval_.approved_ && !Choice.options_.approval_.reviewed_.extensions_.contains(Extension::Viewer)) continue;
        Choice.id_ = "dopus-" + Choice.id_; Choices.push_back(std::move(Choice));
    }
    return Choices;
}

std::vector<QtViewerSurface::PluginViewerChoice> QPluginHostSettings::DataChoices(QSettings& Settings, hc::plugins::PluginFormat Format)
{
    using namespace hc::plugins;
    std::vector<QtViewerSurface::PluginViewerChoice> Choices;
    Settings.beginGroup(SettingsGroup(Format)); const auto Count = std::min(Settings.value("Count").toUInt(), 1000u);
    for (unsigned Index = 0; Index < Count; ++Index)
    {
        const auto Prefix = QString::number(Index) + '_';
        if (!Settings.value(Prefix + "HostEnabled", false).toBool()) continue;
        QtViewerSurface::PluginViewerChoice Choice; Choice.id_ = QString::number(Index);
        auto& Options = Choice.options_; Options.binaryPath_ = Settings.value(Prefix + "Path").toString();
        Options.binaryPath_.replace("%COMMANDER_PATH%", QCoreApplication::applicationDirPath(), Qt::CaseInsensitive);
        Choice.name_ = QFileInfo(Options.binaryPath_).fileName();
        if ((Format == PluginFormat::Wfx || Format == PluginFormat::Wdx) && Options.binaryPath_.endsWith((Format == PluginFormat::Wfx ? ".wfx" : ".wdx"), Qt::CaseInsensitive)) Options.binaryPath_ += "64";
        if (QDir::isRelativePath(Options.binaryPath_)) Options.binaryPath_ = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(Options.binaryPath_);
        Options.executable_ = QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe";
        Options.defaultIniPath_ = QDir(QFileInfo(Settings.fileName()).absolutePath()).filePath((Format == PluginFormat::Wfx ? "fsplugin.ini" : "contentplugin.ini"));
        Options.manifestPath_ = Settings.value(Prefix + "HostManifest").toString();
        const auto Reviewed = ParseManifest(Settings.value(Prefix + "HostReviewed").toByteArray());
        if (Reviewed.HasValue() && Reviewed.Value().format_ == Format)
        { Options.approval_.reviewed_ = Reviewed.Value(); Options.approval_.approved_ = true; Options.approval_.granted_ = Reviewed.Value().permissions_; }
        // Keep revoked entries selectable so execution reports the denial. No
        // DLL inspection/loading or content I/O occurs while building this menu.
        Choices.push_back(std::move(Choice));
    }
    Settings.endGroup(); return Choices;
}
