#include "stdafx.h"
#include "creationSettings.hpp"

#include "UniqueLibs/settings/appSettings.hpp"
#include "commonLibs/Qt/elidedLabel.hpp"
#include "UniqueLibs/qtDialogFrame.hpp"
#include "pluginHost/archiveCodec.hpp"
#include <QtConcurrent>

namespace hc::archive {
using namespace plugins;
using namespace storage;
namespace {
QString Key(const CreatorRuntime& C, const std::string& Format) { return QString::fromStdString(C.descriptor_.id_ + '/' + Format); }
QString Bytes(uint64_t Value) { return QLocale().formattedDataSize(qint64(Value), 1, QLocale::DataSizeTraditionalFormat); }
const char* MethodNames[]{"자동", "Copy", "LZMA", "LZMA2", "PPMd", "BZip2", "Deflate", "Deflate64"};
QString MethodName(SevenZipMethod Method) { return QString::fromUtf8(MethodNames[std::clamp(int(Method), 0, 7)]); }
QJsonObject Overlay(QJsonObject Base, const QJsonObject& Next) { for (auto It = Next.begin(); It != Next.end(); ++It) Base[It.key()] = It.value(); return Base; }
}
QJsonObject CreationPreferences(settings::Repository& Settings)
{
    auto Out = Settings.Read(settings::Section::Plugins).toObject()["archiveCreation"].toObject();
    if (!Out.contains("schemaVersion")) Out["schemaVersion"] = 1;
    if (!Out.contains("lastFormat")) Out["lastFormat"] = "7z"; return Out;
}
Result<Unit> SaveCreationPreferences(settings::Repository& Settings, QJsonObject Preferences)
{
    auto Plugins = Settings.Read(settings::Section::Plugins).toObject(); const auto Existing = Plugins["archiveCreation"].toObject();
    if ((Existing.contains("schemaVersion") && Existing["schemaVersion"] != 1) || Preferences["schemaVersion"] != 1)
        return Error{ErrorCategory::Unsupported, "archive-create-settings-version"};
    Plugins["archiveCreation"] = Overlay(Existing, Preferences);
    if (!Settings.Put(settings::Section::Plugins, Plugins)) return Error{ErrorCategory::Unavailable, "archive-create-settings-save"};
    return Unit{};
}
ArchiveCompositionOptions ReadComposition(const QJsonObject& Preferences)
{
    const auto Value = Preferences["composition"].toObject();
    return {Value["preservePaths"].toBool(true), Value["includeSubdirectories"].toBool(true), Value["separateArchives"].toBool(false), Value["omitSelectedFolderRoot"].toBool(false)};
}
void WriteComposition(QJsonObject& Preferences, ArchiveCompositionOptions O)
{
    Preferences["composition"] = Overlay(Preferences["composition"].toObject(), {{"preservePaths", O.preservePaths_}, {"includeSubdirectories", O.includeSubdirectories_},
        {"separateArchives", O.separateArchives_}, {"omitSelectedFolderRoot", O.OmitSelectedFolderRoot()}});
}
Result<Unit> ValidateIsoSettings(const IsoCreateSettings& S)
{
    const auto Label = QString::fromStdString(S.volumeLabel_); const auto Limit = S.profile_ == IsoCreateProfile::Iso9660 ? 32 : S.profile_ == IsoCreateProfile::UdfWithoutJoliet ? 30 : 16;
    const auto Date = QDateTime::fromSecsSinceEpoch(S.timestampUtc_, QTimeZone::UTC).date();
    if (int(S.profile_) < 0 || int(S.profile_) > 3 || Label.isEmpty() || Label.size() > Limit
        || !std::ranges::all_of(S.volumeLabel_, [](unsigned char C) { return C >= 32 && C < 127; })
        || !Date.isValid() || Date.year() < 1900 || Date.year() > 2155 || !S.maxImageBytes_ || S.maxImageBytes_ > (128ull << 30))
        return Error{ErrorCategory::InvalidRequest, "iso-invalid-settings"};
    return Unit{};
}
Result<ArchiveCreateSettings> ReadCreatorSettings(const QJsonObject& Preferences, const CreatorRuntime& Creator, const std::string& Format)
{
    const auto Kind = Creator.descriptor_.kind_;
    if (Kind == ArchiveCreatorKind::Wcx || Kind == ArchiveCreatorKind::DopusVfs) return ArchiveCreateSettings(ExternalArchiveCreateSettings{});
    const auto Stored = Preferences["creators"].toObject()[Key(Creator, Format)].toObject();
    if (Stored.isEmpty()) {
        if (Kind == ArchiveCreatorKind::BuiltinIso) return ArchiveCreateSettings(IsoCreateSettings{});
        SevenZipCreateSettings S; if (Format == "zip") S.solid_ = ArchiveSolidMode::Off; return ArchiveCreateSettings(S);
    }
    if (Kind == ArchiveCreatorKind::BuiltinSevenZip) {
        const auto Value = Stored["value"].toObject(); QJsonObject Known;
        if (Value["version"] != 1) return Error{ErrorCategory::Unsupported, "archive-create-settings-version"};
        for (const auto Name : {"version", "level", "method", "dictionary", "word", "threads", "solidMode", "solidBytes", "encrypt", "header"}) if (Value.contains(Name)) Known[Name] = Value[Name];
        auto Decoded = DecodeSevenZipSettings(Known); if (!Decoded.HasValue()) return Decoded.Failure();
        auto Checked = ValidateSevenZipSettings(Format, Decoded.Value()); if (!Checked.HasValue()) return Checked.Failure(); return ArchiveCreateSettings(Decoded.Value());
    }
    const auto Value = Stored["value"].toObject(); if (Value["version"] != 1) return Error{ErrorCategory::Unsupported, "archive-create-settings-version"};
    IsoCreateSettings S; const auto Profile = Value["profile"].toInt(-1);
    if (Profile < 0 || Profile > 3 || Value["profile"].toDouble(-1) != Profile || !Value["label"].isString() || !Value["share"].isBool() || !Value["timestamp"].isString()) return Error{ErrorCategory::InvalidRequest, "iso-invalid-settings"};
    auto Limit = WireUnsigned(Value["maxImageBytes"]); bool TimeOk = false; S.timestampUtc_ = Value["timestamp"].toString().toLongLong(&TimeOk);
    if (!Limit.HasValue() || !TimeOk) return Error{ErrorCategory::InvalidRequest, "iso-invalid-settings"};
    S.profile_ = IsoCreateProfile(Profile); S.volumeLabel_ = Value["label"].toString().toStdString(); S.shareDuplicateContents_ = Value["share"].toBool(); S.maxImageBytes_ = Limit.Value();
    auto Checked = ValidateIsoSettings(S); if (!Checked.HasValue()) return Checked.Failure(); return ArchiveCreateSettings(S);
}
void WriteCreatorSettings(QJsonObject& Preferences, const CreatorRuntime& Creator, const std::string& Format, const ArchiveCreateSettings& Settings)
{
    if (std::holds_alternative<ExternalArchiveCreateSettings>(Settings)) return;
    QJsonObject Value;
    if (const auto* S = std::get_if<SevenZipCreateSettings>(&Settings)) Value = EncodeSevenZipSettings(*S);
    else { const auto& Iso = std::get<IsoCreateSettings>(Settings); Value = {{"version", 1}, {"profile", int(Iso.profile_)}, {"label", QString::fromStdString(Iso.volumeLabel_)}, {"share", Iso.shareDuplicateContents_}, {"timestamp", QString::number(Iso.timestampUtc_)}, {"maxImageBytes", QString::number(Iso.maxImageBytes_)}}; }
    auto Creators = Preferences["creators"].toObject(); auto Record = Creators[Key(Creator, Format)].toObject(); Record["value"] = Overlay(Record["value"].toObject(), Value);
    Creators[Key(Creator, Format)] = Record; Preferences["creators"] = Creators;
}
QString CreationSettingsSummary(const ArchiveCreateSettings& Settings, const std::string& Format)
{
    if (const auto* S = std::get_if<SevenZipCreateSettings>(&Settings)) {
        auto Text = QObject::tr("%1 · 수준 %2 · 사전 %3 · 스레드 %4").arg(MethodName(EffectiveSevenZipMethod(Format, *S))).arg(S->level_)
            .arg(S->dictionaryBytes_ ? Bytes(*S->dictionaryBytes_) : QObject::tr("자동"), S->threads_ ? QString::number(*S->threads_) : QObject::tr("자동"));
        if (S->encrypt_) Text += Format == "7z" && S->encryptHeaders_ ? QObject::tr(" · AES-256 및 파일 이름 암호화") : QObject::tr(" · AES-256"); return Text;
    }
    if (const auto* S = std::get_if<IsoCreateSettings>(&Settings)) return QObject::tr("%1 · 중복 내용 공유 %2 · 최대 %3\n%4 UTC")
        .arg(QString::fromStdString(S->volumeLabel_), S->shareDuplicateContents_ ? QObject::tr("사용") : QObject::tr("사용 안 함"), Bytes(S->maxImageBytes_), QDateTime::fromSecsSinceEpoch(S->timestampUtc_, QTimeZone::UTC).toString("yyyy-MM-dd HH:mm:ss"));
    return QObject::tr("플러그인 자체 설정을 사용합니다.\n대기열을 포함하여 작업을 시작할 때의 설정을 적용합니다.");
}
Result<QJsonObject> RunArchiveWorker(QWidget* Parent, const QString& Title, std::function<Result<QJsonObject>(OperationContext)> Work)
{
    class WaitDialog final : public QDialog {
    public:
        std::shared_ptr<std::stop_source> stop_ = std::make_shared<std::stop_source>();
        QLabel* status_ = nullptr; QPushButton* cancel_ = nullptr;
        explicit WaitDialog(QWidget* Parent) : QDialog(Parent) {}
        void reject() override { stop_->request_stop(); if (cancel_) cancel_->setEnabled(false); if (status_) status_->setText(tr("작업을 취소하고 창을 닫는 중입니다…")); }
    } Dialog(Parent);
    Dialog.setWindowTitle(Title); auto* Layout = new QVBoxLayout(&Dialog); Dialog.status_ = new QLabel(Title, &Dialog); Dialog.status_->setWordWrap(true); Layout->addWidget(Dialog.status_);
    auto* Busy = new QProgressBar(&Dialog); Busy->setRange(0, 0); Layout->addWidget(Busy); Dialog.cancel_ = new QPushButton(QObject::tr("취소"), &Dialog); Layout->addWidget(Dialog.cancel_);
    QObject::connect(Dialog.cancel_, &QPushButton::clicked, &Dialog, &QDialog::reject);
    hc::ui::DialogFrameOptions Frame; Frame.header_ = false; hc::ui::MoveToFooter(hc::ui::ApplyDialogFrame(&Dialog, Frame), Dialog.cancel_);
    QFutureWatcher<Result<QJsonObject>> Watcher; Result<QJsonObject> Result = Error{ErrorCategory::Cancelled, "cancelled"};
    QObject::connect(&Watcher, &QFutureWatcher<hc::storage::Result<QJsonObject>>::finished, &Dialog, [&] { Result = Watcher.result(); Dialog.accept(); });
    Watcher.setFuture(QtConcurrent::run([Work = std::move(Work), Stop = Dialog.stop_] { OperationContext Context; Context.stopToken_ = Stop->get_token(); return Work(Context); }));
    Dialog.exec(); return Result;
}
Result<QJsonObject> ConfigureCreator(QWidget* Parent, CreatorRuntime Creator)
{
    const auto Owner = uint64_t(Parent->winId());
    return RunArchiveWorker(Parent, QObject::tr("플러그인 설정 창을 사용하고 있습니다."), [Creator = std::move(Creator), Owner](OperationContext Context) mutable -> Result<QJsonObject> {
        auto SettingsLease = AcquireCreatorSettings(Creator, Context); if (!SettingsLease.HasValue()) return SettingsLease.Failure();
        auto Valid = RevalidateCreator(Creator, Context); if (!Valid.HasValue()) return Valid.Failure();
        auto Limits = Creator.options_.limits_; Limits.purpose_ = HostPurpose::Configuration;
        auto Started = PluginProcess::Start(Creator.options_.executable_, Creator.options_.manifestPath_, Creator.options_.approval_, Limits, Context, Creator.options_.defaultIniPath_); if (!Started.HasValue()) return Started.Failure();
        return Started.Value()->Configure(Owner, Context);
    });
}
bool EditBuiltinCreationSettings(QWidget* Parent, const std::string& Format, ArchiveCreateSettings& Settings)
{
    QDialog Dialog(Parent); Dialog.setObjectName(QString("hc.archive.settings.") + QString::fromStdString(Format)); Dialog.setWindowTitle(Format == "iso" ? QObject::tr("ISO 플러그인 설정") : QObject::tr("7-Zip 플러그인 설정")); Dialog.resize(620, 550);
    auto* Layout = new QVBoxLayout(&Dialog); auto* Scroll = new QScrollArea(&Dialog); Scroll->setWidgetResizable(true); Scroll->setFrameShape(QFrame::NoFrame); Layout->addWidget(Scroll, 1);
    auto* Content = new QWidget(Scroll); Scroll->setWidget(Content); auto* Form = new QFormLayout(Content); Form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto* Footer = new QHBoxLayout; Layout->addLayout(Footer);
    auto* ErrorLabel = new hc::qt::ElidedLabel(&Dialog); ErrorLabel->setObjectName("hc.archive.settings.error"); Footer->addWidget(ErrorLabel, 1);
    auto* Buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &Dialog); Footer->addWidget(Buttons); Buttons->button(QDialogButtonBox::Ok)->setText(QObject::tr("확인")); Buttons->button(QDialogButtonBox::Cancel)->setText(QObject::tr("취소"));
    QObject::connect(Buttons, &QDialogButtonBox::rejected, &Dialog, &QDialog::reject);
    {
        auto* Band = hc::ui::ApplyDialogFrame(&Dialog, {{}, QObject::tr("이 형식으로 새로 만드는 압축 파일에 적용합니다. 대기열의 작업은 시작할 때의 설정을 씁니다."), fm::ui::glyph::File});
        ErrorLabel->setParent(Band->parentWidget()); Band->insertWidget(0, ErrorLabel, 1); hc::ui::MoveToFooter(Band, Buttons);
    }
    ArchiveCreateSettings Draft = Settings;
    if (auto* S = std::get_if<SevenZipCreateSettings>(&Draft)) {
        auto* Level = new QComboBox(Content); Level->setObjectName("hc.archive.settings.level"); for (unsigned Value : {0u, 1u, 3u, 5u, 7u, 9u}) Level->addItem(QString::number(Value), Value); Level->setCurrentIndex(Level->findData(S->level_)); Form->addRow(QObject::tr("압축 수준(&L)"), Level);
        auto* Method = new QComboBox(Content); Method->setObjectName("hc.archive.settings.method"); for (int I = 0; I < 8; ++I) if (Format == "7z" || I != int(SevenZipMethod::Lzma2)) Method->addItem(MethodName(SevenZipMethod(I)), I); Method->setCurrentIndex(Method->findData(int(S->method_))); Form->addRow(QObject::tr("알고리즘(&M)"), Method);
        auto* Dictionary = new QComboBox(Content); Dictionary->setObjectName("hc.archive.settings.dictionary"); auto* DictionaryLabel = new QLabel(Content); Form->addRow(DictionaryLabel, Dictionary); DictionaryLabel->setBuddy(Dictionary);
        auto* Word = new QSpinBox(Content); Word->setObjectName("hc.archive.settings.word"); Word->setSpecialValueText(QObject::tr("자동")); auto* WordLabel = new QLabel(Content); Form->addRow(WordLabel, Word); WordLabel->setBuddy(Word);
        auto* Threads = new QSpinBox(Content); Threads->setObjectName("hc.archive.settings.threads"); Threads->setSpecialValueText(QObject::tr("자동")); Form->addRow(QObject::tr("멀티 스레드(&T)"), Threads);
        auto* Solid = new QComboBox(Content); Solid->setObjectName("hc.archive.settings.solid"); Solid->addItems({QObject::tr("사용 안 함"), QObject::tr("전체"), QObject::tr("블록 크기 지정")}); Solid->setCurrentIndex(Format == "7z" ? int(S->solid_) : 0); Solid->setEnabled(Format == "7z"); Form->addRow(QObject::tr("솔리드 압축(&S)"), Solid);
        auto* Block = new QComboBox(Content); Block->setObjectName("hc.archive.settings.solidBlock"); for (uint64_t Size : {2ull << 20, 64ull << 20, 512ull << 20, 1ull << 30, 2ull << 30, 4ull << 30, 8ull << 30, 16ull << 30, 64ull << 30}) Block->addItem(Bytes(Size), QVariant::fromValue<qulonglong>(Size)); Form->addRow(QObject::tr("솔리드 블록 크기"), Block);
        if (S->solidBlockBytes_) { const auto At = Block->findData(QVariant::fromValue<qulonglong>(*S->solidBlockBytes_)); if (At >= 0) Block->setCurrentIndex(At); }
        auto* Encrypt = new QCheckBox(QObject::tr("AES-256 암호화(&E)"), Content); Encrypt->setObjectName("hc.archive.settings.encrypt"); Encrypt->setChecked(S->encrypt_); Form->addRow(Encrypt);
        auto* Header = new QCheckBox(QObject::tr("파일 이름도 암호화(&H)"), Content); Header->setObjectName("hc.archive.settings.encryptHeaders"); Header->setChecked(S->encryptHeaders_); Header->setVisible(Format == "7z"); Form->addRow(Header);
        const auto Refresh = [=] {
            SevenZipCreateSettings Current; Current.level_ = Level->currentData().toUInt(); Current.method_ = SevenZipMethod(Method->currentData().toInt()); const auto Codec = EffectiveSevenZipMethod(Format, Current);
            Dictionary->clear(); Dictionary->addItem(QObject::tr("자동"), QVariant::fromValue<qulonglong>(0));
            DictionaryLabel->setText(Codec == SevenZipMethod::Ppmd ? QObject::tr("메모리 크기(&D)") : Codec == SevenZipMethod::Bzip2 ? QObject::tr("블록 크기(&D)") : QObject::tr("사전 크기(&D)"));
            const auto Add = [Dictionary](uint64_t Size) { Dictionary->addItem(Bytes(Size), QVariant::fromValue<qulonglong>(Size)); };
            if (Codec == SevenZipMethod::Lzma || Codec == SevenZipMethod::Lzma2) { for (unsigned Shift = 16; Shift <= 31; ++Shift) Add(1ull << Shift); Add(15ull << 28); }
            else if (Codec == SevenZipMethod::Ppmd) { for (unsigned Shift = 20; Shift <= (Format == "zip" ? 28u : 31u); ++Shift) Add(1ull << Shift); if (Format == "7z") Add((1ull << 32) - 1024); }
            else if (Codec == SevenZipMethod::Bzip2) for (unsigned I = 1; I <= 9; ++I) Add(I * 100 * 1024);
            else if (Codec == SevenZipMethod::Deflate || Codec == SevenZipMethod::Deflate64) { Dictionary->clear(); Add(Codec == SevenZipMethod::Deflate ? 32768 : 65536); }
            Dictionary->setEnabled(Codec != SevenZipMethod::Copy && Codec != SevenZipMethod::Deflate && Codec != SevenZipMethod::Deflate64);
            WordLabel->setText(Codec == SevenZipMethod::Ppmd ? QObject::tr("모델 차수(&W)") : QObject::tr("워드 크기(&W)"));
            const auto MaxWord = Codec == SevenZipMethod::Ppmd ? (Format == "zip" ? 16 : 32) : Codec == SevenZipMethod::Deflate ? 258 : Codec == SevenZipMethod::Deflate64 ? 257 : Codec == SevenZipMethod::Lzma || Codec == SevenZipMethod::Lzma2 ? 273 : 0;
            Word->setRange(0, MaxWord); Word->setValue(0); Word->setEnabled(MaxWord > 0); Threads->setRange(0, int(SevenZipThreadLimit(Format, Codec))); Threads->setValue(0);
            Method->setEnabled(Current.level_ != 0); Block->setEnabled(Format == "7z" && Solid->currentIndex() == 2); Header->setEnabled(Encrypt->isChecked());
        };
        Refresh(); if (S->dictionaryBytes_) { auto At = Dictionary->findData(QVariant::fromValue<qulonglong>(*S->dictionaryBytes_)); if (At < 0) { Dictionary->addItem(Bytes(*S->dictionaryBytes_), QVariant::fromValue<qulonglong>(*S->dictionaryBytes_)); At = Dictionary->count() - 1; } Dictionary->setCurrentIndex(At); }
        Word->setValue(int(S->wordSize_.value_or(0))); Threads->setValue(int(S->threads_.value_or(0)));
        QObject::connect(Level, &QComboBox::currentIndexChanged, &Dialog, Refresh); QObject::connect(Method, &QComboBox::currentIndexChanged, &Dialog, Refresh);
        QObject::connect(Solid, &QComboBox::currentIndexChanged, &Dialog, [=] { Block->setEnabled(Format == "7z" && Solid->currentIndex() == 2); }); QObject::connect(Encrypt, &QCheckBox::toggled, Header, &QWidget::setEnabled);
        QObject::connect(Buttons, &QDialogButtonBox::accepted, &Dialog, [&, S, Level, Method, Dictionary, Word, Threads, Solid, Block, Encrypt, Header] {
            S->level_ = Level->currentData().toUInt(); S->method_ = SevenZipMethod(Method->currentData().toInt()); const auto Codec = EffectiveSevenZipMethod(Format, *S);
            S->dictionaryBytes_.reset(); if (Codec != SevenZipMethod::Copy && Dictionary->currentData().toULongLong()) S->dictionaryBytes_ = Dictionary->currentData().toULongLong();
            S->wordSize_ = Word->isEnabled() && Word->value() ? std::optional<unsigned>(unsigned(Word->value())) : std::nullopt; S->threads_ = Threads->value() ? std::optional<unsigned>(unsigned(Threads->value())) : std::nullopt;
            S->solid_ = ArchiveSolidMode(Solid->currentIndex()); S->solidBlockBytes_ = S->solid_ == ArchiveSolidMode::BlockSize ? std::optional<uint64_t>(Block->currentData().toULongLong()) : std::nullopt;
            S->encrypt_ = Encrypt->isChecked(); S->encryptHeaders_ = Format == "7z" && Header->isChecked(); auto Checked = ValidateSevenZipSettings(Format, *S);
            if (!Checked.HasValue()) { ErrorLabel->SetText(QObject::tr("알고리즘에 맞는 사전 크기·워드 크기·스레드를 확인하십시오.")); return; } Dialog.accept();
        });
    } else if (auto* S = std::get_if<IsoCreateSettings>(&Draft)) {
        auto* Profile = new QComboBox(Content); Profile->setObjectName("hc.archive.settings.profile"); Profile->addItems({QObject::tr("ISO 9660"), QObject::tr("ISO 9660 + Joliet"), QObject::tr("ISO 9660 + Joliet + UDF 1.02"), QObject::tr("ISO 9660 + UDF 1.02 (긴 파일 이름)")}); Profile->setCurrentIndex(int(S->profile_)); Form->addRow(QObject::tr("파일 시스템(&F)"), Profile);
        auto* Label = new QLineEdit(QString::fromStdString(S->volumeLabel_), Content); Label->setObjectName("hc.archive.settings.label"); Label->setMaxLength(32); Form->addRow(QObject::tr("볼륨 이름(&V)"), Label);
        auto* Share = new QCheckBox(QObject::tr("중복 파일 내용을 한 번만 저장(&D)"), Content); Share->setObjectName("hc.archive.settings.share"); Share->setChecked(S->shareDuplicateContents_); Form->addRow(Share);
        auto* Fixed = new QCheckBox(QObject::tr("고정 시각 사용 (2000-01-01 00:00:00 UTC)"), Content); Fixed->setObjectName("hc.archive.settings.fixedTime"); Fixed->setChecked(S->timestampUtc_ == 946684800); Form->addRow(Fixed);
        auto* Time = new QDateTimeEdit(QDateTime::fromSecsSinceEpoch(S->timestampUtc_, QTimeZone::UTC), Content); Time->setObjectName("hc.archive.settings.timestamp"); Time->setTimeZone(QTimeZone::UTC); Time->setDisplayFormat("yyyy-MM-dd HH:mm:ss 'UTC'"); Time->setDateRange(QDate(1900, 1, 1), QDate(2155, 12, 31)); Time->setEnabled(!Fixed->isChecked()); Form->addRow(QObject::tr("사용자 지정 시각(&T)"), Time);
        auto* Maximum = new QDoubleSpinBox(Content); Maximum->setObjectName("hc.archive.settings.maxImage"); Maximum->setDecimals(3); Maximum->setRange(0.001, 128); Maximum->setSuffix(" GiB"); Maximum->setValue(double(S->maxImageBytes_) / double(1ull << 30)); Form->addRow(QObject::tr("최대 이미지 크기(&M)"), Maximum);
        auto* Help = new QLabel(QObject::tr("중복 내용 공유는 모든 경로를 유지합니다. 작성 후에는 각 경로의 전체 내용을 원본과 대조합니다."), Content); Help->setWordWrap(true); Help->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); Form->addRow(Help);
        QObject::connect(Fixed, &QCheckBox::toggled, Time, [Time](bool On) { Time->setEnabled(!On); });
        const auto Validate = [=] { IsoCreateSettings Value; Value.profile_ = IsoCreateProfile(Profile->currentIndex()); Value.volumeLabel_ = Label->text().toStdString(); Value.timestampUtc_ = Fixed->isChecked() ? 946684800 : Time->dateTime().toSecsSinceEpoch(); Value.maxImageBytes_ = uint64_t(Maximum->value() * double(1ull << 30));
            const auto Valid = ValidateIsoSettings(Value).HasValue(); Buttons->button(QDialogButtonBox::Ok)->setEnabled(Valid); ErrorLabel->SetText(Valid ? QString{} : QObject::tr("볼륨 이름은 ISO 32자, Joliet 16자, UDF 30자 이내의 ASCII 문자로 입력하십시오.")); };
        QObject::connect(Profile, &QComboBox::currentIndexChanged, &Dialog, Validate); QObject::connect(Label, &QLineEdit::textChanged, &Dialog, Validate); QObject::connect(Time, &QDateTimeEdit::dateTimeChanged, &Dialog, Validate); QObject::connect(Maximum, &QDoubleSpinBox::valueChanged, &Dialog, Validate); Validate();
        QObject::connect(Buttons, &QDialogButtonBox::accepted, &Dialog, [&, S, Profile, Label, Share, Fixed, Time, Maximum] { S->profile_ = IsoCreateProfile(Profile->currentIndex()); S->volumeLabel_ = Label->text().toStdString(); S->shareDuplicateContents_ = Share->isChecked(); S->timestampUtc_ = Fixed->isChecked() ? 946684800 : Time->dateTime().toSecsSinceEpoch(); S->maxImageBytes_ = uint64_t(Maximum->value() * double(1ull << 30)); if (ValidateIsoSettings(*S).HasValue()) Dialog.accept(); });
    } else return false;
    if (Dialog.exec() != QDialog::Accepted) return false; Settings = std::move(Draft); return true;
}
}
