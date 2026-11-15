#include "stdafx.h"
#include "registryImportDialog.hpp"

/// 모듈
/// UIs
#include "Components/treeBranchStyle.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/registry/registryDescriptions.hpp"
#include "UniqueLibs/registry/registryLinks.hpp"
/// 공통 라이브러리
#include "commonLibs/system/processInfo.hpp"
/// 외부 라이브러리
#include <QtitanGrid.h>
#include <fmwidgets/ValueInputs.h>
#include <fmwidgets/Label.h>
#include <fmwidgets/Button.h>
#include <fmwidgets/WrapCheckBox.h>
#include <fmwidgets/SegmentedControl.h>
#include <fmwidgets/FlowLayout.h>
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QVBoxLayout>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QSysInfo>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QStringDecoder>
#include <QTimer>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryImportDialog, "hc.ui.registryImport")
using namespace hc::registry;
namespace {
struct LoadedImport { RegistryImportSource input; QByteArray bytes, hash; RegistryImportPreview preview; QString text; };
}
RegistryImportDialog::RegistryImportDialog(QString Path, RegistryDescriptions* Descriptions, QWidget* Parent)
    : ContentDialog(Parent), path_(std::move(Path)), descriptions_(Descriptions)
{
    setObjectName("hc.registry.importPreview"); setTitle(QFileInfo(path_).fileName() + tr(" 가져오기"));
    setPrimaryButtonText(tr("백업하고 가져오기")); setCloseButtonText(tr("취소")); setDefaultButton(CloseButton);
    auto* Body = new QWidget; auto* Layout = new QVBoxLayout(Body); Body->setMinimumSize(720, 400);
    notice_ = new fm::ui::Label(path_, fm::ui::Label::Help, Body); Layout->addWidget(notice_);
    auto* Bar = new fm::ui::FlowLayout(4, 4); Layout->addLayout(Bar);
    auto* Modes = new fm::ui::SegmentedControl({tr("변경 표"), tr("키 트리"), tr("원문 (.reg)")}, Body); Bar->addWidget(Modes); Modes->setCurrentIndex(0);
    bits_ = new fm::ui::ChoiceBox(Body); bits_->addItems({tr("64비트 보기"), tr("32비트 보기")}); Bar->addWidget(bits_);
    encoding_ = new fm::ui::ChoiceBox(Body); encoding_->addItems({tr("인코딩 자동"), tr("UTF-8"), tr("ANSI")}); Bar->addWidget(encoding_);
    classes_ = new fm::ui::ChoiceBox(Body); classes_->addItems({tr("HKCR 쓰기 위치 선택"), tr("HKCR → 현재 사용자"), tr("HKCR → 모든 사용자")}); Bar->addWidget(classes_);
    const auto Add = [&](const QString& Text, const QString& Name, auto Handler) {
        auto* Button = new fm::ui::Button(Text, Body); Button->setCompact(true); Button->setObjectName(Name); Bar->addWidget(Button); connect(Button, &QPushButton::clicked, this, Handler);
    };
    Add(tr("다시 읽기"), "hc.registry.import.reload", [this] { excluded_.clear(); reload(true); });
    Add(tr("HyperEditor로 열기"), "hc.registry.import.editor", [this] { if (openEditor) openEditor(path_); });
    Add(tr("선택 항목 제외"), "hc.registry.import.exclude", [this] {
        const auto Focus = view_->focusedRow(); if (!Focus.isValid()) return;
        const auto Source = model_->Row(Focus.modelIndex(0)); if (!Source) return;
        excluded_.insert(Source->payload["operation"].toInt()); reload(false);
    });
    Add(tr("제외 취소"), "hc.registry.import.includeAll", [this] { excluded_.clear(); reload(false); });
    Add(tr("키 열기"), "hc.registry.import.openKey", [this] {
        const auto Focus = view_->focusedRow(); if (!Focus.isValid()) return; const auto Row = model_->Row(Focus.modelIndex(0));
        if (Row && Row->location && openKey) openKey(*Row->location);
    });
    grid_ = new Qtitan::TreeGrid(Body); grid_->setObjectName("hc.registry.import.grid"); grid_->setViewType(Qtitan::TreeGrid::TreeView);
    model_ = new RegistryResultModel({tr("동작"), tr("키 › 값"), tr("지금"), tr("가져온 뒤"), tr("설명"), tr("권한")}, this);
    view_ = grid_->view<Qtitan::GridTreeView>(); view_->options().setAutoCreateColumns(false); view_->setModel(model_);
    view_->options().setGroupsHeader(false); view_->options().setGridLines(Qtitan::LinesNone); view_->setFileManagerMode(true);
    view_->tableOptions().setRowsQuickSelection(false); view_->tableOptions().setColumnsQuickCustomization(false);
    for (int I = 0; I < 6; ++I) { auto* Column = static_cast<Qtitan::GridTableColumn*>(view_->addColumn(I, model_->headerData(I, Qt::Horizontal).toString())); Column->setWidth(I == 1 ? 230 : I == 0 ? 90 : 150); }
    grid_->setStyle(new TreeBranchStyle(grid_)); Layout->addWidget(grid_, 1);
    source_ = new fm::ui::MultilineField(Body); source_->setReadOnly(true); source_->setObjectName("hc.registry.import.source"); Layout->addWidget(source_, 1); source_->hide();
    backup_ = new fm::ui::WrapCheckBox(tr("가져오기 전에 registry-backup에 원본과 .reg 백업을 저장합니다."), Body); backup_->setObjectName("hc.registry.import.backup"); Layout->addWidget(backup_); backup_->setChecked(true); backup_->setEnabled(false);
    connect(backup_, &QCheckBox::toggled, this, [this] { button(PrimaryButton)->setEnabled(!loading_ && preview_ && !preview_.plan.steps.isEmpty() && backup_->isChecked()); });
    connect(Modes, &fm::ui::SegmentedControl::currentIndexChanged, this, [this](int Mode) { tree_ = Mode == 1; grid_->setVisible(Mode != 2); source_->setVisible(Mode == 2); render(); });
    for (auto* Combo : {bits_, encoding_, classes_}) connect(Combo, &QComboBox::currentIndexChanged, this, [this] { reload(false); });
    setContentWidget(Body); button(PrimaryButton)->setEnabled(false); QTimer::singleShot(0, this, [this] { reload(true); });
}
RegistryImportDialog::~RegistryImportDialog() { if (cancel_) cancel_->store(true); view_->setModel(nullptr); }
void RegistryImportDialog::reload(bool ReadFile)
{
    if (cancel_) cancel_->store(true); cancel_ = std::make_shared<std::atomic_bool>(false);
    const auto Cancel = cancel_; const auto Generation = ++generation_;
    loading_ = true; button(PrimaryButton)->setEnabled(false);
    notice_->setText(tr("미리 보기를 준비하는 중…"));
    const auto Sid = QString::fromStdWString(hc::system::CurrentUserSid()), Machine = QString::fromLatin1(QSysInfo::machineUniqueId().toHex());
    const auto Bits = bits_->currentIndex() ? View::Wow32 : View::Native64;
    const auto Encoding = RegEncoding(encoding_->currentIndex()); const int Classes = classes_->currentIndex();
    auto* Watcher = new QFutureWatcher<LoadedImport>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Generation] {
        const auto Loaded = Watcher->result(); Watcher->deleteLater(); if (Generation != generation_) return;
        loading_ = false; input_ = Loaded.input; bytes_ = Loaded.bytes; hash_ = Loaded.hash; preview_ = Loaded.preview; source_->setPlainText(Loaded.text); render();
        notice_->setText(preview_ ? tr("%1 · 변경 %2단계 · 제외 %3건 · 적용 직전에 원본을 다시 확인합니다.").arg(path_).arg(preview_.plan.steps.size()).arg(preview_.skipped) : preview_.error);
        button(PrimaryButton)->setEnabled(preview_ && !preview_.plan.steps.isEmpty() && backup_->isChecked());
    });
    Watcher->setFuture(QtConcurrent::run([Path = path_, Input = input_, Loader = loadSource, Bytes = bytes_, ReadFile, Cancel, Sid, Machine, Bits, Encoding, Classes, Excluded = excluded_]() mutable {
        LoadedImport Result;
        if ((ReadFile || Bytes.isEmpty()) && Loader) {
            Input = Loader(Cancel.get());
            if (!Input.error.isEmpty() || Input.path.isEmpty()) { Result.preview.error = Input.error.isEmpty() ? QObject::tr("원본을 다시 읽지 못했습니다.") : Input.error; return Result; }
        }
        Result.input = Input;
        if (ReadFile || Bytes.isEmpty()) {
            QFile File(Input.path.isEmpty() ? Path : Input.path); if (!File.open(QIODevice::ReadOnly) || File.size() > RegFileParser::MaxFileBytes) { Result.preview.error = QObject::tr("파일을 읽지 못했거나 크기 한도를 넘었습니다."); return Result; }
            Bytes = File.read(RegFileParser::MaxFileBytes + 1);
            if (File.error() != QFile::NoError || Bytes.size() > RegFileParser::MaxFileBytes) { Result.preview.error = QObject::tr("파일을 끝까지 읽지 못했습니다."); return Result; }
        }
        Result.bytes = Bytes; Result.hash = QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256);
        QStringDecoder Decoder(Bytes.startsWith("\xff\xfe") || Bytes.startsWith("\xfe\xff") ? QStringConverter::Utf16 : Encoding == RegEncoding::Ansi ? QStringConverter::System : QStringConverter::Utf8);
        Result.text = Decoder(Bytes);
        if (Result.text.size() > 2 * 1024 * 1024) Result.text = Result.text.left(2 * 1024 * 1024) + QObject::tr("\n… 원문 표시는 2백만 자까지입니다. 전체 내용은 HyperEditor에서 확인하세요.");
        auto Parsed = RegFileParser::Parse(Bytes, Bits, Encoding);
        if (!Parsed) { Result.preview.error = Parsed.encodingConfirmation ? QObject::tr("인코딩을 UTF-8 또는 ANSI로 직접 선택하세요.") : QObject::tr("%1행: %2").arg(Parsed.line).arg(Parsed.error); return Result; }
        WinRegistryBackend Backend;
        for (auto& Operation : Parsed.operations) if (Operation.location.hive == Hive::ClassesRoot) {
            if (!Classes) { Result.preview.error = QObject::tr("HKCR 항목이 있습니다. 현재 사용자 또는 모든 사용자 쓰기 위치를 고르세요."); return Result; }
            const auto Physical = RegistryLinkResolver::PhysicalClasses(Backend, Classes == 1 ? Hive::CurrentUser : Hive::LocalMachine, Bits);
            if (!Physical) { Result.preview.error = QObject::tr("HKCR 쓰기 위치를 확인하지 못했습니다 (%1).").arg(Physical.error); return Result; }
            Operation.location = Operation.location.key.isEmpty() ? Physical.value : Physical.value.Child(Operation.location.key);
        }
        for (auto& Operation : Parsed.operations) if (Operation.location.hive == Hive::CurrentUser
            && (Operation.location.key.compare("Software\\Classes", Qt::CaseInsensitive) == 0
                || Operation.location.key.startsWith("Software\\Classes\\", Qt::CaseInsensitive))) {
            const auto Physical = RegistryLinkResolver::PhysicalClasses(Backend, Hive::CurrentUser, Bits);
            if (!Physical) { Result.preview.error = QObject::tr("사용자 Classes의 실제 위치를 확인하지 못했습니다 (%1).").arg(Physical.error); return Result; }
            const auto Relative = Operation.location.key.mid(QString("Software\\Classes").size());
            Operation.location = Relative.isEmpty() ? Physical.value : Physical.value.Child(Relative.mid(1));
        }
        Result.preview = RegistryImport::Prepare(Backend, Parsed.operations, Excluded, Sid, Machine, Cancel.get()); return Result;
    }));
}
void RegistryImportDialog::render()
{
    auto Rows = preview_.rows;
    for (auto& Row : Rows) {
        if (tree_ && Row.location) Row.groups = Row.location->PhysicalPath().split('\\'); else Row.groups.clear();
        if (descriptions_ && Row.location) Row.cells[4] = descriptions_->Find(*Row.location, Row.value ? std::optional<QString>(Row.value->name) : std::nullopt, Row.value ? std::optional<quint32>(Row.value->type) : std::nullopt).title;
    }
    model_->SetRows(std::move(Rows));
    if (tree_) for (int I = 0; I < model_->rowCount(); ++I) view_->expand(model_->index(I, 0));
}