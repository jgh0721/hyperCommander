#include "builtinIso.hpp"
#include "hcIsoManifest.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>

namespace hc::plugins {
ApprovedWcxOptions BuiltinIsoOptions()
{
    ApprovedWcxOptions Options;
    const auto Root = QCoreApplication::applicationDirPath();
    Options.executable_ = Root + "/hc-plugin-host-x64.exe";
    Options.manifestPath_ = Root + "/BuiltInPlugin/IsoImage/isoimage.json";
    Options.binaryPath_ = Root + "/BuiltInPlugin/IsoImage/hc_isoimage_wcx.wcx64";
    auto Manifest = ParseManifest(QByteArray(HcBuiltinIsoManifest));
    if (Manifest.HasValue()) {
        Options.approval_.reviewed_ = std::move(Manifest.Value()); Options.approval_.approved_ = true;
        Options.approval_.granted_ = Options.approval_.reviewed_.permissions_;
    }
    Options.limits_.requestMs_ = 30000;
    Options.limits_.archiveWriteMs_ = 3600000;
    return Options;
}
storage::Result<BuiltinIsoInspection> InspectBuiltinIso(const QString& Path, bool ReadAll, storage::OperationContext Context)
{
    using namespace storage;
    const QFileInfo Before(Path);
    auto Backend = MakeApprovedWcxBackend(BuiltinIsoOptions());
    auto Cursor = Backend->Enumerate(Path.toStdString(), ReadAll, Context); if (!Cursor.HasValue()) return Cursor.Failure();
    BuiltinIsoInspection Result;
    for (;;) {
        auto Page = Cursor.Value()->Next(128, Context); if (!Page.HasValue()) return Page.Failure();
        for (const auto& Header : Page.Value().entries_) {
            if (Header.directory_) ++Result.directories_; else { ++Result.files_; Result.bytes_ += Header.size_; }
            if (Result.preview_.size() < 5000) Result.preview_.push_back(Header);
        }
        if (Page.Value().end_) break;
    }
    Cursor.Value().reset();
    QFile File(Path); if (!File.open(QIODevice::ReadOnly)) return Error{ErrorCategory::Unavailable, "iso-inspect-open"};
    auto BlockAt = [&](uint64_t Sector) { if (Sector >= uint64_t(File.size()) / 2048 || !File.seek(qint64(Sector * 2048))) return QByteArray{}; return File.read(2048); };
    auto Number = [](const QByteArray& Data, int Offset, int Width) { uint64_t Value = 0; for (int I = 0; I < Width && Offset + I < Data.size(); ++I) Value |= uint64_t(uint8_t(Data[Offset + I])) << (8 * I); return Value; };
    bool Udf = false;
    for (unsigned Sector = 16; Sector < 80; ++Sector) {
        const auto Data = BlockAt(Sector); if (Data.size() != 2048) break;
        if (Data.mid(1, 5) == "CD001" && Data[0] == 1) { Result.volumeLabel_ = QString::fromLatin1(Data.mid(40, 32)).trimmed(); Result.namespaces_ << "ISO 9660"; }
        if (Data.mid(1, 5) == "CD001" && Data[0] == 2 && Data.mid(88, 2) == "%/") Result.namespaces_ << "Joliet";
        if (Data.mid(1, 5) == "NSR02" || Data.mid(1, 5) == "NSR03") Udf = true;
    }
    if (Udf) {
        const auto Anchor = BlockAt(256); const auto Base = Number(Anchor, 20, 4);
        for (unsigned I = 0; I < 256; ++I) {
            const auto Data = BlockAt(Base + I); if (Data.size() != 2048) break;
            if (Number(Data, 0, 2) == 6) { Result.udfRevision_ = unsigned(Number(Data, 240, 2)); break; }
            if (Number(Data, 0, 2) == 8) break;
        }
        Result.namespaces_ << QString("UDF %1.%2").arg(Result.udfRevision_ >> 8).arg(Result.udfRevision_ & 255, 2, 16, QChar('0'));
    }
    const QFileInfo After(Path);
    if (Before.size() != After.size() || Before.lastModified() != After.lastModified()) return Error{ErrorCategory::StaleRevision, "iso-changed-during-inspection"};
    return Result;
}
}
