#include "stdafx.h"
#include "registryCommands.hpp"

/// 모듈
/// UIs
#include "HyperCommander/UIs/registryImportDialog.hpp"
#include "HyperCommander/UIs/registryDialogs.hpp"
#include "HyperCommander/UIs/Components/cmpRegistryView.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
#include "registryBatch.hpp"
#include "registryCompareLease.hpp"
#include <fmwidgets/Button.h>
#include "UniqueLibs/commandUiBridge.hpp"
#include "UniqueLibs/settings/appSettings.hpp"
#include "UniqueLibs/system/systemSupport.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "UniqueLibs/scratchSession.hpp"
#include "HyperCommanderUpdater/updateController.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QMainWindow>
#include <QFileDialog>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QProcess>
#include <QStatusBar>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryTransfer, "hc.registry.transfer")
namespace hc::registry {
void RegistryCommands::OpenImport(const QString& Path, std::shared_ptr<void> KeepAlive, std::function<void()> Editor, std::function<bool()> SourceUnchanged, RegistryImportLoader Loader)
{
    if (busy_ || !main_ || Path.isEmpty()) return;
    auto* Descriptions = RegistryDescriptions::Shared(QDir(QFileInfo(hc::settings::AppSettings().FileName()).absolutePath()).filePath("registry-cache/admx"));
    RegistryImportDialog Dialog(Path, Descriptions, main_); Dialog.loadSource = std::move(Loader);
    Dialog.openKey = [this](const RegistryLocation& Location) { auto* View = Open(); if (View) { View->SetRegistryView(Location.view); View->Navigate(Location.Path()); } };
    Dialog.openEditor = [this, Editor](const QString& File) { if (Editor) Editor(); else bridge_->OpenRegistrySourceEditor(File); };
    if (Dialog.exec() != fm::ui::ContentDialog::Primary) return;
    const auto Preview = Dialog.Preview(); const auto Hash = Dialog.SourceHash();
    const auto Input = Dialog.Source(); const auto InputPath = Dialog.SourcePath();
    runMutation(ActiveView(), [Path = InputPath, Hash, Preview, KeepAlive, SourceUnchanged, Input](RegistryMutation& Mutation) {
        RegistryMutationResult Error;
        if ((Input.verify && !Input.verify()) || (SourceUnchanged && !SourceUnchanged())) { Error.applied.error = ERROR_REVISION_MISMATCH; Error.applied.conflict = true; return Error; }
        QFile File(Path);
        if (!File.open(QIODevice::ReadOnly) || File.size() > RegFileParser::MaxFileBytes) { Error.applied.error = ERROR_READ_FAULT; return Error; }
        const auto Bytes = File.read(RegFileParser::MaxFileBytes + 1);
        if (File.error() != QFile::NoError || QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256) != Hash) {
            Error.applied.error = ERROR_REVISION_MISMATCH; Error.applied.conflict = true; return Error;
        }
        return Mutation.ApplyPreview(Preview.operations, Preview.plan);
    });
}
void RegistryCommands::exportData(CRegistryView* View, bool Results)
{
    if (busy_ || !View || (Results && View->Search()->Running())) return;
    QString Filter = tr("레지스트리 파일 (*.reg)");
    const auto Path = QFileDialog::getSaveFileName(main_, tr("레지스트리 내보내기"), {}, Results ? Filter + ";;CSV (*.csv)" : Filter, &Filter);
    if (Path.isEmpty()) return;
    QList<RegistryLocation> Keys; QList<RegistryResultRow> Values;
    if (Results) {
        for (const auto& Row : View->Search()->Rows()) if (Row.location) {
            if (Row.value) Values.append(Row); else Keys.append(*Row.location);
        }
    } else {
        for (const auto& Index : View->SelectedSources()) {
            const auto Location = RegistryLocation::Parse(Index.data(RegistryTreeModel::LocationRole).toString(), View->SelectedRegistryView()); if (!Location) continue;
            if (Index.data(RegistryTreeModel::KindRole).toInt() == int(RegistryTreeModel::Kind::Value)) {
                RegistryResultRow Row; Row.location = *Location; Row.value = RegistryValue{Index.data(RegistryTreeModel::ValueNameRole).toString(), Index.data(RegistryTreeModel::ValueTypeRole).toUInt(), Index.data(RegistryTreeModel::RawValueRole).toByteArray()}; Values.append(Row);
            } else Keys.append(*Location);
        }
        if (Keys.isEmpty() && Values.isEmpty()) if (const auto Key = RegistryLocation::Parse(View->PathText(), View->SelectedRegistryView())) Keys.append(*Key);
    }
    const bool Csv = Results && Filter.startsWith("CSV"); const auto Rows = Results ? View->Search()->Rows() : QList<RegistryResultRow>{};
    busy_ = true; policyCancel_ = std::make_shared<std::atomic_bool>(false); const auto Cancel = policyCancel_;
    const auto CancelOnClose = connect(this, &QObject::destroyed, [Cancel] { Cancel->store(true); });
    policyCancelButton_->setProperty("registryTransfer", true); policyCancelButton_->setText(tr("내보내기 취소")); policyCancelButton_->setEnabled(true); policyCancelButton_->show(); RefreshState();
    hc::sys::RunAsync(this, [Keys, Values, Rows, Csv, Path, Cancel] {
        QByteArray Bytes;
        if (Csv) { const auto Data = RegistryBatch::Csv(Rows, Cancel.get()); if (!Data) return Data.error; Bytes = Data.value; }
        else { WinRegistryBackend Backend; const auto Export = RegistryBatch::Export(Backend, Keys, Values, Cancel.get()); if (!Export) return Export.error; Bytes = RegFileWriter::Write(Export.value); }
        if (Bytes.size() > RegFileParser::MaxFileBytes) return quint32(ERROR_FILE_TOO_LARGE);
        if (Cancel->load()) return quint32(ERROR_CANCELLED);
        QSaveFile File(Path);
        if (!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.commit()) return quint32(ERROR_WRITE_FAULT);
        return quint32(0);
    }, [this, Path, CancelOnClose](quint32 Error) {
        disconnect(CancelOnClose); busy_ = false; policyCancel_.reset(); policyCancelButton_->hide(); policyCancelButton_->setProperty("registryTransfer", false); policyCancelButton_->setText(tr("정책 작업 취소")); RefreshState();
        if (Error) hc::ui::MessageDialog::warning(main_, tr("레지스트리 내보내기"), hc::sys::Win32ErrorText(Error));
        else if (main_) main_->statusBar()->showMessage(tr("내보냄: %1").arg(Path), 6000);
    });
}
void RegistryCommands::compare(CRegistryView* View)
{
    if (busy_ || !View) return;
    auto Left = RegistryLocation::Parse(View->CurrentSource().data(RegistryTreeModel::LocationRole).toString(), View->SelectedRegistryView());
    if (!Left) Left = RegistryLocation::Parse(View->PathText(), View->SelectedRegistryView()); if (!Left) return;
    const auto Other = hc::ui::AskRegistryName(main_, tr("비교할 레지스트리 키 또는 .reg 파일 경로"), {}, false); if (!Other || Other->isEmpty()) return;
    const auto Right = RegistryLocation::Parse(*Other, View->SelectedRegistryView());
    const auto Program = QDir(QCoreApplication::applicationDirPath()).filePath("HyperCompare.exe");
    if (!hc::update::PrepareRootFiles(Program, main_)) return;
    auto Directory = std::make_shared<RegistryCompareLease>();
    if (!Directory->IsValid()) return;
    struct Result { quint32 error = 0; QStringList paths; };
    busy_ = true; RefreshState();
    hc::sys::RunAsync(this, [Left = *Left, Right, Other = *Other, Directory] {
        RegistryCompareLease::Sweep(); Result Output; WinRegistryBackend Backend;
        auto A = RegistryBatch::Export(Backend, {Left}); if (!A) { Output.error = A.error; return Output; }
        QList<RegistryOperation> B;
        if (Right) {
            auto Export = RegistryBatch::Export(Backend, {*Right}); if (!Export) { Output.error = Export.error; return Output; } B = Export.value;
        } else {
            QFile Input(Other); if (!Input.open(QIODevice::ReadOnly) || Input.size() > RegFileParser::MaxFileBytes) { Output.error = ERROR_READ_FAULT; return Output; }
            const auto Parsed = RegFileParser::Parse(Input.readAll(), Left.view);
            if (!Parsed) { Output.error = ERROR_INVALID_DATA; return Output; } B = Parsed.operations;
        }
        for (int Side = 0; Side < 2; ++Side) {
            const auto Path = Directory->FilePath(Side ? "right.reg" : "left.reg");
            const auto Bytes = RegFileWriter::Write(RegistryBatch::Normalize(Side ? B : A.value));
            QSaveFile File(Path);
            if (Bytes.size() > RegFileParser::MaxFileBytes || !File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.commit()) { Output.error = ERROR_WRITE_FAULT; return Output; }
            Output.paths.append(Path);
        }
        return Output;
    }, [this, Program, Directory](const Result& Output) {
        busy_ = false; RefreshState();
        if (Output.error) { hc::ui::MessageDialog::warning(main_, tr("레지스트리 비교"), hc::sys::Win32ErrorText(Output.error)); return; }
        if (!Directory->Starting()) { hc::ui::MessageDialog::warning(main_, tr("레지스트리 비교"), tr("임시 파일 소유 정보를 저장하지 못했습니다.")); return; }
        auto* Process = new QProcess(this); auto Released = std::make_shared<bool>(false); ++compareProcesses_;
        const auto Release = [this, Process, Directory, Released] { if (*Released) return; *Released = true; --compareProcesses_; Process->deleteLater(); };
        connect(Process, &QProcess::finished, this, [Release](int, QProcess::ExitStatus) { Release(); });
        connect(Process, &QProcess::errorOccurred, this, [this, Release, Directory](QProcess::ProcessError Error) {
            if (Error == QProcess::FailedToStart) { Directory->CancelStart(); hc::ui::MessageDialog::warning(main_, tr("레지스트리 비교"), tr("HyperCompare를 시작하지 못했습니다.")); Release(); }
        });
        connect(Process, &QProcess::started, this, [Process, Directory] { Directory->SetProcess(Process->processId()); });
        Process->setProgram(Program); Process->setArguments({"-mode", "compare", "-file1", Output.paths[0], "-file2", Output.paths[1]}); Process->start();
    });
}
}