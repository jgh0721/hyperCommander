#include "qtWcxHostSettings.hpp"
#include "pluginHost/manifestFile.hpp"
#include <QtWidgets>
#include <QtConcurrent>

QWcxHostSettings::QWcxHostSettings(QWidget* Parent) : QWcxHostSettings(hc::plugins::PluginFormat::Wcx, Parent) {}
QWcxHostSettings::QWcxHostSettings(hc::plugins::PluginFormat Format, QWidget* Parent) : QWidget(Parent), format_(Format)
{
    const bool Wlx = Format == hc::plugins::PluginFormat::Wlx;
    Q_ASSERT(Wlx || Format == hc::plugins::PluginFormat::Wcx);
    group_ = Wlx ? "ListerPlugins" : "PackerPlugins";
    const QString Prefix = Wlx ? "wlx" : "wcx";
    auto* Layout = new QVBoxLayout(this); Layout->setContentsMargins(0, 0, 0, 0);
    table_ = new QTableWidget(0, 4, this); table_->setObjectName(Prefix + "HostTable");
    table_->setHorizontalHeaderLabels({tr("플러그인"), Wlx ? tr("감지 조건") : tr("확장자"), tr("호스트"), tr("승인")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->verticalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int Column = 1; Column < 4; ++Column) table_->horizontalHeader()->setSectionResizeMode(Column, QHeaderView::ResizeToContents);
    Layout->addWidget(table_);
    auto* Buttons = new QHBoxLayout; Layout->addLayout(Buttons);
    review_ = new QPushButton(tr("호스트 실행 승인…"), this); review_->setObjectName(Prefix + "HostReview"); Buttons->addWidget(review_);
    auto* Revoke = new QPushButton(tr("승인 해제"), this); Revoke->setObjectName(Prefix + "HostRevoke"); Buttons->addWidget(Revoke); Buttons->addStretch();
    connect(review_, &QPushButton::clicked, this, [this] { review(); }); connect(Revoke, &QPushButton::clicked, this, [this] { revoke(); });
    connect(table_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* Item) { if (Item->column() == 2) updateState(Item->row()); });
}
void QWcxHostSettings::Load(QSettings& Settings)
{
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
void QWcxHostSettings::Save(QSettings& Settings)
{
    Settings.beginGroup(group_);
    for (std::size_t Index = 0; Index < rows_.size(); ++Index)
    {
        const auto Prefix = QString::number(rows_[Index].index_) + '_';
        Settings.setValue(Prefix + "HostEnabled", table_->item(static_cast<int>(Index), 2)->checkState() == Qt::Checked);
        Settings.setValue(Prefix + "HostManifest", rows_[Index].manifest_); Settings.setValue(Prefix + "HostReviewed", rows_[Index].reviewed_);
    }
    Settings.endGroup();
}
void QWcxHostSettings::updateState(int Row)
{
    if (Row < 0 || static_cast<std::size_t>(Row) >= rows_.size() || !table_->item(Row, 3)) return;
    table_->item(Row, 3)->setText(table_->item(Row, 2)->checkState() != Qt::Checked ? tr("기존 방식")
        : hc::plugins::ParseManifest(rows_[Row].reviewed_).HasValue() ? tr("승인됨") : tr("승인 필요"));
    table_->item(Row, 3)->setToolTip(rows_[Row].manifest_);
}
void QWcxHostSettings::revoke()
{
    const auto Row = table_->currentRow(); if (Row < 0) return;
    ++generation_; review_->setEnabled(true); review_->setText(tr("호스트 실행 승인…")); rows_[Row].reviewed_.clear();
    // Revoking trust must not silently switch execution into the application.
    table_->item(Row, 2)->setCheckState(Qt::Checked); updateState(Row);
}
void QWcxHostSettings::review()
{
    using namespace hc::plugins; using namespace hc::storage;
    const auto Row = table_->currentRow(); if (Row < 0) return;
    const auto BeforeDialog = generation_;
    const auto ManifestPath = QFileDialog::getOpenFileName(this, format_ == PluginFormat::Wlx ? tr("WLX 플러그인 manifest 선택") : tr("WCX 플러그인 manifest 선택"), rows_[Row].manifest_, tr("플러그인 manifest (*.json)"));
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
        Profile.extensions_ = Format == PluginFormat::Wlx ? std::set<Extension>{Extension::Viewer} : std::set<Extension>{Extension::ArchiveRead, Extension::ArchiveWrite};
        const auto Admitted = Admit(Approval.reviewed_, Approval, Profile); if (!Admitted.HasValue()) return Admitted.Failure();
        return Inspected;
    }));
}
std::vector<QtViewerSurface::WlxChoice> QWcxHostSettings::WlxChoices(QSettings& Settings)
{
    using namespace hc::plugins;
    std::vector<QtViewerSurface::WlxChoice> Choices;
    Settings.beginGroup("ListerPlugins"); const auto Count = std::min(Settings.value("Count").toUInt(), 1000u);
    for (unsigned Index = 0; Index < Count; ++Index)
    {
        const auto Prefix = QString::number(Index) + '_';
        if (!Settings.value(Prefix + "HostEnabled", false).toBool()) continue;
        QtViewerSurface::WlxChoice Choice; Choice.id_ = QString::number(Index);
        auto& Options = Choice.options_; Options.binaryPath_ = Settings.value(Prefix + "Path").toString();
        Options.binaryPath_.replace("%COMMANDER_PATH%", QCoreApplication::applicationDirPath(), Qt::CaseInsensitive);
        Choice.name_ = QFileInfo(Options.binaryPath_).fileName();
        if (Options.binaryPath_.endsWith(".wlx", Qt::CaseInsensitive)) Options.binaryPath_ += "64";
        if (QDir::isRelativePath(Options.binaryPath_)) Options.binaryPath_ = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(Options.binaryPath_);
        Options.executable_ = QCoreApplication::applicationDirPath() + "/hc-plugin-host-x64.exe";
        Options.manifestPath_ = Settings.value(Prefix + "HostManifest").toString();
        const auto Reviewed = ParseManifest(Settings.value(Prefix + "HostReviewed").toByteArray());
        if (Reviewed.HasValue() && Reviewed.Value().format_ == PluginFormat::Wlx)
        { Options.approval_.reviewed_ = Reviewed.Value(); Options.approval_.approved_ = true; Options.approval_.granted_ = Reviewed.Value().permissions_; }
        // Keep revoked entries selectable so execution reports the denial. No
        // DLL inspection/loading or content I/O occurs while building this menu.
        Choices.push_back(std::move(Choice));
    }
    Settings.endGroup(); return Choices;
}
