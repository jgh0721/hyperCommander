#include "stdafx.h"
#include "creationDialog.hpp"

#include "UniqueLibs/settings/appSettings.hpp"
#include <QtConcurrent>

namespace hc::archive {
using namespace plugins;
using namespace storage;
namespace {
class OptionLabel final : public QLabel {
public:
    OptionLabel(const QString& Text, QCheckBox* Check, QWidget* Parent) : QLabel(Text, Parent), check_(Check) { setBuddy(Check); }
protected:
    void mouseReleaseEvent(QMouseEvent* Event) override { if (Event->button() == Qt::LeftButton && check_->isEnabled()) check_->click(); QLabel::mouseReleaseEvent(Event); }
private:
    QCheckBox* check_;
};
}
CreationDialog::CreationDialog(settings::Repository& Settings, Locator SourceRoot, QString Directory, uint64_t SelectionCount,
    QWidget* Parent, const std::string& InitialFormat)
    : QDialog(Parent), settingsRepository_(Settings), sourceRoot_(std::move(SourceRoot)), directory_(std::move(Directory)),
      catalog_(CreatorCatalog(Settings)), checked_(catalog_.size(), false), preferences_(CreationPreferences(Settings))
{
    setObjectName("hc.archive.create"); setWindowTitle(tr("파일 압축")); resize(720, 540); setMinimumWidth(340);
    auto* Layout = new QVBoxLayout(this);
    auto* Selection = new QLabel(tr("선택한 %1개 파일·폴더로 압축 파일 또는 ISO 이미지를 생성합니다.").arg(QLocale().toString(SelectionCount)), this); Selection->setWordWrap(true); Selection->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); Layout->addWidget(Selection);
    auto* Scroll = new QScrollArea(this); Scroll->setWidgetResizable(true); Scroll->setFrameShape(QFrame::NoFrame); Layout->addWidget(Scroll, 1);
    auto* Content = new QWidget(Scroll); Scroll->setWidget(Content); auto* Rows = new QVBoxLayout(Content); auto* Form = new QFormLayout; Form->setRowWrapPolicy(QFormLayout::WrapLongRows); Rows->addLayout(Form);
    auto* OutputRow = new QWidget(Content); auto* OutputLayout = new QHBoxLayout(OutputRow); OutputLayout->setContentsMargins(0, 0, 0, 0);
    output_ = new QLineEdit(OutputRow); output_->setObjectName("hc.archive.create.path"); output_->setAccessibleName(tr("압축 파일 경로")); OutputLayout->addWidget(output_, 1);
    auto* Browse = new QPushButton(tr("찾아보기…"), OutputRow); Browse->setAutoDefault(false); OutputLayout->addWidget(Browse); outputLabel_ = new QLabel(tr("압축 파일 경로(&P)"), Content); outputLabel_->setBuddy(output_); Form->addRow(outputLabel_, OutputRow);
    formats_ = new QComboBox(Content); formats_->setObjectName("hc.archive.create.format");
    std::set<std::string> Formats; for (const auto& Creator : catalog_) for (const auto& Format : Creator.descriptor_.formats_) Formats.insert(Format.format_);
    for (const auto Name : {"7z", "zip", "iso"}) if (Formats.erase(Name)) formats_->addItem(QString::fromLatin1(Name).toUpper(), Name);
    for (const auto& Name : Formats) formats_->addItem(QString::fromStdString(Name).toUpper(), QString::fromStdString(Name)); Form->addRow(tr("형식(&F)"), formats_);
    auto* CreatorRow = new QWidget(Content); auto* CreatorLayout = new QHBoxLayout(CreatorRow); CreatorLayout->setContentsMargins(0, 0, 0, 0);
    creators_ = new QComboBox(CreatorRow); creators_->setObjectName("hc.archive.create.creator"); creators_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); CreatorLayout->addWidget(creators_, 1);
    configure_ = new QPushButton(tr("플러그인 설정…(&G)"), CreatorRow); configure_->setObjectName("hc.archive.create.settings"); configure_->setAutoDefault(false); CreatorLayout->addWidget(configure_); Form->addRow(tr("플러그인(&I)"), CreatorRow);
    summary_ = new QLabel(Content); summary_->setObjectName("hc.archive.create.summary"); summary_->setTextFormat(Qt::PlainText); summary_->setWordWrap(true); summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); Form->addRow(tr("현재 설정"), summary_);
    notice_ = new QLabel(Content); notice_->setWordWrap(true); notice_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); Rows->addWidget(notice_);
    const auto Options = ReadComposition(preferences_);
    const auto AddOption = [&](const QString& Text, const char* Name, bool Checked) {
        auto* Row = new QWidget(Content); auto* Line = new QHBoxLayout(Row); Line->setContentsMargins(0, 0, 0, 0);
        auto* Check = new QCheckBox(Row); Check->setObjectName(Name); Check->setChecked(Checked); Check->setAccessibleName(Text);
        auto* Label = new OptionLabel(Text, Check, Row); Label->setWordWrap(true); Label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        Line->addWidget(Check, 0, Qt::AlignTop); Line->addWidget(Label, 1); Rows->addWidget(Row); return Check;
    };
    paths_ = AddOption(tr("경로명 압축 · 폴더 구조 보존(&N)"), "hc.archive.create.paths", Options.preservePaths_);
    recursive_ = new QCheckBox(tr("하위 폴더 재귀 압축(&R)"), Content); recursive_->setObjectName("hc.archive.create.recursive"); recursive_->setChecked(Options.includeSubdirectories_); Rows->addWidget(recursive_);
    individual_ = AddOption(tr("선택한 파일·폴더를 각각의 압축 파일로 만들기(&A)"), "hc.archive.create.individual", Options.separateArchives_);
    omitRoot_ = new QCheckBox(tr("폴더 압축 시 기본 폴더 빼기(&B)"), Content); omitRoot_->setObjectName("hc.archive.create.omitRoot"); omitRoot_->setChecked(Options.omitSelectedFolderRoot_); Rows->addWidget(omitRoot_); Rows->addStretch();
    error_ = new QLabel(this); error_->setObjectName("hc.archive.create.error"); error_->setWordWrap(true); error_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); Layout->addWidget(error_);
    auto* Footer = new QHBoxLayout; Layout->addLayout(Footer);
    queue_ = new QPushButton(tr("대기열에 추가(&Q)"), this); queue_->setObjectName("hc.archive.create.queue"); queue_->setAutoDefault(false); Footer->addWidget(queue_); Footer->addStretch();
    auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this); Footer->addWidget(Buttons);
    create_ = Buttons->button(QDialogButtonBox::Ok); create_->setObjectName("hc.archive.create.submit"); create_->setText(tr("생성(&S)")); create_->setDefault(true); Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소"));
    connect(Buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(Buttons, &QDialogButtonBox::accepted, this, [this] { validate(); if (create_->isEnabled()) { queued_ = false; remember(); accept(); } });
    connect(queue_, &QPushButton::clicked, this, [this] { validate(); if (queue_->isEnabled()) { queued_ = true; remember(); accept(); } });
    connect(formats_, &QComboBox::currentIndexChanged, this, &CreationDialog::formatChanged); connect(creators_, &QComboBox::currentIndexChanged, this, &CreationDialog::creatorChanged);
    connect(configure_, &QPushButton::clicked, this, &CreationDialog::configure); connect(output_, &QLineEdit::textChanged, this, &CreationDialog::validate);
    connect(individual_, &QCheckBox::toggled, this, &CreationDialog::separateChanged);
    for (auto* Check : {paths_, recursive_, omitRoot_}) connect(Check, &QCheckBox::toggled, this, &CreationDialog::validate);
    connect(Browse, &QPushButton::clicked, this, [this] {
        const auto Path = individual_->isChecked() ? QFileDialog::getExistingDirectory(this, tr("압축 파일을 저장할 폴더"), outputPath())
            : QFileDialog::getSaveFileName(this, tr("압축 파일 저장 위치"), outputPath(), QString::fromStdString(format_) + " (*." + QString::fromStdString(format_) + ')', nullptr, QFileDialog::DontConfirmOverwrite);
        if (!Path.isEmpty()) output_->setText(QDir::toNativeSeparators(Path));
    });
    const auto Initial = InitialFormat.empty() ? preferences_["lastFormat"].toString("7z") : QString::fromStdString(InitialFormat);
    auto Index = formats_->findData(Initial); if (Index < 0) Index = 0;
    formats_->blockSignals(true); formats_->setCurrentIndex(Index); formats_->blockSignals(false);
    combinedPath_ = QDir(directory_).filePath((Initial == "iso" ? "image." : "archive.") + formats_->currentData().toString()); folderPath_ = directory_;
    separate_ = individual_->isChecked(); output_->setText(QDir::toNativeSeparators(separate_ ? folderPath_ : combinedPath_)); formatChanged(); separateChanged();
    for (size_t I = 0; I < catalog_.size(); ++I) {
        auto* Watcher = new QFutureWatcher<Result<Unit>>(this); const auto Creator = catalog_[I];
        connect(Watcher, &QFutureWatcher<Result<Unit>>::finished, this, [this, Watcher, I] { const auto Result = Watcher->result(); Watcher->deleteLater(); checked_[I] = true; if (!Result.HasValue()) { catalog_[I].available_ = false; catalog_[I].status_ = tr("플러그인 파일 또는 실행 승인을 확인하십시오."); } validate(); });
        Watcher->setFuture(QtConcurrent::run([Creator, Stop = stop_] { OperationContext Context; Context.stopToken_ = Stop->get_token(); Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10); return RevalidateCreator(Creator, Context); }));
    }
    output_->setFocus(); output_->selectAll();
}
CreationDialog::~CreationDialog() { stop_->request_stop(); }
void CreationDialog::remember()
{
    if (creator_ < 0 || !value_) return;
    WriteCreatorSettings(preferences_, catalog_[creator_], format_, *value_);
    auto Last = preferences_["lastCreatorByFormat"].toObject(); Last[QString::fromStdString(format_)] = QString::fromStdString(catalog_[creator_].descriptor_.id_); preferences_["lastCreatorByFormat"] = Last;
    preferences_["lastFormat"] = QString::fromStdString(format_);
    WriteComposition(preferences_, {paths_->isChecked(), recursive_->isChecked(), individual_->isChecked(), omitRoot_->isChecked()});
}
void CreationDialog::formatChanged()
{
    remember(); const auto Previous = QString::fromStdString(format_); format_ = formats_->currentData().toString().toStdString();
    if (!individual_->isChecked() && !Previous.isEmpty()) {
        auto Path = output_->text(); if (Path.endsWith('.' + Previous, Qt::CaseInsensitive)) { Path.chop(Previous.size()); Path += QString::fromStdString(format_); output_->setText(Path); }
    }
    creators_->blockSignals(true); creators_->clear(); const auto Last = preferences_["lastCreatorByFormat"].toObject()[QString::fromStdString(format_)].toString(); int Choice = -1;
    for (size_t I = 0; I < catalog_.size(); ++I) {
        const auto& C = catalog_[I]; if (std::ranges::none_of(C.descriptor_.formats_, [&](const auto& F) { return F.format_ == format_; })) continue;
        const auto Kind = C.descriptor_.kind_; const auto Tag = Kind == ArchiveCreatorKind::Wcx ? "WCX" : Kind == ArchiveCreatorKind::DopusVfs ? "DOpus" : "기본 제공";
        creators_->addItem(QString::fromStdString(C.descriptor_.displayName_) + " [" + QString::fromUtf8(Tag) + ']', int(I));
        if (Last == QString::fromStdString(C.descriptor_.id_)) Choice = creators_->count() - 1;
    }
    notice_->setText(!Last.isEmpty() && Choice < 0 ? tr("이전에 선택한 플러그인을 사용할 수 없어 다른 처리기를 선택했습니다.") : QString{});
    creators_->setCurrentIndex(Choice >= 0 ? Choice : 0); creators_->blockSignals(false); creator_ = -1; value_.reset(); creatorChanged();
}
void CreationDialog::creatorChanged()
{
    if (creator_ >= 0 && value_) WriteCreatorSettings(preferences_, catalog_[creator_], format_, *value_);
    creator_ = creators_->currentIndex() >= 0 ? creators_->currentData().toInt() : -1; value_.reset(); settingsUnsupported_ = false;
    creators_->setToolTip(creators_->currentText());
    if (creator_ >= 0) {
        auto Settings = ReadCreatorSettings(preferences_, catalog_[creator_], format_);
        if (Settings.HasValue()) value_ = std::move(Settings.Value()); else settingsUnsupported_ = Settings.Failure().category_ == ErrorCategory::Unsupported;
    }
    validate();
}
void CreationDialog::separateChanged()
{
    if (separate_ != individual_->isChecked()) {
        if (separate_) folderPath_ = output_->text(); else combinedPath_ = output_->text(); separate_ = individual_->isChecked();
        output_->setText(separate_ ? folderPath_ : combinedPath_);
    }
    outputLabel_->setText(separate_ ? tr("저장 폴더(&P)") : tr("압축 파일 경로(&P)")); output_->setAccessibleName(separate_ ? tr("압축 파일 저장 폴더") : tr("압축 파일 경로")); validate();
}
QString CreationDialog::outputPath() const
{
    auto Path = ResolveArchiveDestination(output_->text(), directory_); if (Path.isEmpty() || individual_->isChecked()) return Path;
    const auto Input = QDir::fromNativeSeparators(output_->text());
    if (Input.endsWith('/') || Input == "." || Input == ".." || Path.endsWith('/')) Path = QDir(Path).filePath("archive." + QString::fromStdString(format_));
    else if (QFileInfo(Path).suffix().isEmpty()) Path += '.' + QString::fromStdString(format_); return Path;
}
void CreationDialog::validate()
{
    omitRoot_->setEnabled(individual_->isChecked() && paths_->isChecked());
    bool Valid = creator_ >= 0 && creator_ < int(catalog_.size()); QString Error;
    if (!Valid) Error = tr("이 형식으로 생성할 수 있는 플러그인이 없습니다.");
    else if (!checked_[creator_]) { Valid = false; Error = tr("플러그인을 확인하고 있습니다…"); }
    else if (!catalog_[creator_].available_) { Valid = false; Error = catalog_[creator_].status_; }
    if (!value_ || preferences_["schemaVersion"] != 1) { Valid = false; Error = tr("저장된 설정을 읽을 수 없습니다. 플러그인 설정을 확인하십시오."); }
    const auto Path = outputPath();
    if (Path.isEmpty()) { Valid = false; Error = tr("저장할 경로를 입력하십시오."); }
    else if (!individual_->isChecked() && QFileInfo(Path).suffix().compare(QString::fromStdString(format_), Qt::CaseInsensitive)) { Valid = false; Error = tr("선택한 형식의 확장자 .%1을 사용하십시오.").arg(QString::fromStdString(format_)); }
    error_->setText(Error); create_->setEnabled(Valid); queue_->setEnabled(Valid);
    configure_->setEnabled(creator_ >= 0 && checked_[creator_] && catalog_[creator_].available_ && !settingsUnsupported_
        && catalog_[creator_].descriptor_.configuration_ != ArchiveConfigurationKind::None && preferences_["schemaVersion"] == 1);
    summary_->setText(value_ ? CreationSettingsSummary(*value_, format_) : tr("설정을 확인하십시오."));
}
void CreationDialog::configure()
{
    if (creator_ < 0) return;
    if (catalog_[creator_].descriptor_.configuration_ == ArchiveConfigurationKind::BuiltinDialog) {
        if (!value_) { if (format_ == "iso") value_ = IsoCreateSettings{}; else { SevenZipCreateSettings V; if (format_ == "zip") V.solid_ = ArchiveSolidMode::Off; value_ = V; } }
        auto Draft = *value_; if (EditBuiltinCreationSettings(this, format_, Draft)) { value_ = std::move(Draft); WriteCreatorSettings(preferences_, catalog_[creator_], format_, *value_); }
    } else {
        auto Result = ConfigureCreator(this, catalog_[creator_]); if (!Result.HasValue() && Result.Failure().category_ != ErrorCategory::Cancelled) QMessageBox::warning(this, tr("플러그인 설정"), QString::fromStdString(Result.Failure().code_));
    }
    validate();
}
ArchiveCreateRequest CreationDialog::Request() const
{
    ArchiveCreateRequest R; R.sourceRoot_ = sourceRoot_; R.creator_ = catalog_[creator_].descriptor_; R.format_ = format_;
    const auto Path = outputPath(); R.output_ = {Path.startsWith("//") ? LocatorKind::Smb : LocatorKind::Local, Path.toStdString()};
    R.composition_ = {paths_->isChecked(), recursive_->isChecked(), individual_->isChecked(), omitRoot_->isChecked()}; R.composition_.omitSelectedFolderRoot_ = R.composition_.OmitSelectedFolderRoot();
    R.settings_ = *value_; R.schedule_ = queued_ ? ArchiveCreateSchedule::Queued : ArchiveCreateSchedule::Immediate; R.correlationId_ = QUuid::createUuid().toString(QUuid::Id128).toStdString(); return R;
}
CreatorRuntime CreationDialog::Creator() const { return catalog_[creator_]; }
Result<Unit> CreationDialog::SavePreferences() { remember(); return SaveCreationPreferences(settingsRepository_, preferences_); }
}
