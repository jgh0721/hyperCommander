#pragma once

#include "HyperCommanderBuiltInPlugin/SevenZip/builtinSevenZip.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QStringDecoder>
#include <QTemporaryDir>
#include <QtEndian>
#include <qt_windows.h>

namespace hc::plugins {
struct TcPluginInstallation {
    QString group_, binary_, directory_;
    QStringList extensions_;
};
inline QString TcPluginGroup(const QString& Path) {
    auto Suffix = QFileInfo(Path).suffix().toLower();
    if (Suffix.endsWith("64")) Suffix.chop(2);
    return QStringList{"wcx", "wlx", "wfx", "wdx"}.contains(Suffix) ? Suffix : QString{};
}
namespace tcPackage {
inline storage::Error Failure(const char* Code) {
    return {storage::ErrorCategory::InvalidRequest, Code, {"tc-plugin-package"}};
}
inline bool Segment(const QString& Name) {
    static const QRegularExpression Invalid(R"([<>:"/\\|?*\x00-\x1f])");
    static const QRegularExpression Reserved(R"(^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$))", QRegularExpression::CaseInsensitiveOption);
    return !Name.isEmpty() && Name.size() <= 255 && Name != "." && Name != ".."
        && !Name.endsWith('.') && !Name.endsWith(' ') && !Name.contains(Invalid) && !Reserved.match(Name).hasMatch();
}
inline QString Member(QString Name, bool Directory = false) {
    Name.replace('\\', '/');
    if (Directory && Name.endsWith('/')) Name.chop(1);
    const auto Parts = Name.split('/');
    for (const auto& Part : Parts) if (!Segment(Part)) return {};
    return Name;
}
inline bool Binary64(const QString& Path) {
    QFile File(Path);
    if (!File.open(QIODevice::ReadOnly)) return false;
    const auto Dos = File.read(64);
    if (Dos.size() != 64 || !Dos.startsWith("MZ")) return false;
    const auto Offset = qFromLittleEndian<quint32>(Dos.constData() + 60);
    if (Offset > 1024 * 1024 || Offset > quint64(qMax(qint64(0), File.size() - 26)) || !File.seek(Offset)) return false;
    const auto Pe = File.read(26);
    return Pe.size() == 26 && Pe.first(4) == QByteArray("PE\0\0", 4)
        && qFromLittleEndian<quint16>(Pe.constData() + 4) == 0x8664
        && (qFromLittleEndian<quint16>(Pe.constData() + 22) & 0x2000)
        && qFromLittleEndian<quint16>(Pe.constData() + 24) == 0x20b;
}
inline storage::Result<QMap<QString, QString>> Metadata(const QByteArray& Bytes) {
    if (Bytes.size() > 256 * 1024) return Failure("plugin-package-metadata-size");
    QStringDecoder Decode(Bytes.startsWith("\xff\xfe") || Bytes.startsWith("\xfe\xff") ? QStringDecoder::Utf16 : QStringDecoder::Utf8);
    QString Text = Decode(Bytes);
    if (Decode.hasError()) Text = QString::fromLocal8Bit(Bytes);
    if (Text.startsWith(QChar(0xfeff))) Text.remove(0, 1);
    QMap<QString, QString> Values; bool Inside = false;
    for (auto Line : Text.split('\n')) {
        Line = Line.trimmed();
        if (Line.isEmpty() || Line.startsWith(';') || Line.startsWith('#')) continue;
        if (Line.startsWith('[')) { Inside = Line.compare("[plugininstall]", Qt::CaseInsensitive) == 0; continue; }
        if (!Inside) continue;
        const auto Equal = Line.indexOf('=');
        if (Equal < 1) return Failure("plugin-package-metadata-invalid");
        const auto Key = Line.left(Equal).trimmed().toLower(); auto Value = Line.mid(Equal + 1).trimmed();
        if (Value.size() >= 2 && Value.startsWith('"') && Value.endsWith('"')) Value = Value.mid(1, Value.size() - 2);
        if (Values.contains(Key)) return Failure("plugin-package-metadata-duplicate");
        Values.insert(Key, Value);
    }
    if (Values.isEmpty()) return Failure("plugin-package-metadata-invalid");
    return Values;
}
// A package can supply filenames, never a path outside the new private tree.
inline bool Category(const QString& Root, const QString& Group, QString& Output) {
    if (!QFileInfo(Root).isDir()) return false;
    auto Path = QDir(Root).absolutePath();
    for (const auto& Part : QStringList{"Addons", Group.toUpper()}) {
        Path = QDir(Path).filePath(Part);
        const auto Native = QDir::toNativeSeparators(Path);
        auto Attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(Native.utf16()));
        if (Attributes == INVALID_FILE_ATTRIBUTES) {
            if (!QDir().mkdir(Path)) return false;
            Attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(Native.utf16()));
        }
        if (Attributes == INVALID_FILE_ATTRIBUTES || !(Attributes & FILE_ATTRIBUTE_DIRECTORY)
            || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    Output = Path; return true;
}
}

// Worker-side installation. Settings and approval remain owned by the UI draft.
// Final rename publishes only a fully extracted package; existing folders stay intact.
inline storage::Result<TcPluginInstallation> InstallTcPluginPackage(const QString& Source, const QString& ProductDirectory,
    storage::OperationContext Context = {}, std::shared_ptr<IArchiveBackend> Backend = {}) {
    using namespace tcPackage;
    constexpr quint64 MaxBytes = 512ULL * 1024 * 1024;
    constexpr qsizetype MaxEntries = 10000;
    if (const auto Stop = Context.Check({"tc-plugin-package"})) return *Stop;
    QFile Zip(Source);
    if (!Zip.open(QIODevice::ReadOnly) || Zip.size() <= 0 || quint64(Zip.size()) > MaxBytes || Zip.read(2) != "PK")
        return Failure("plugin-package-not-zip");
    Zip.close();
    QTemporaryDir Scratch;
    if (!Scratch.isValid()) return Failure("plugin-package-staging");
    const auto Archive = Scratch.filePath("package.zip");
    if (!QFile::copy(Source, Archive)) return Failure("plugin-package-copy");
    if (quint64(QFileInfo(Archive).size()) > MaxBytes) return Failure("plugin-package-size-limit");
    if (!Backend) Backend = MakeApprovedWcxBackend(BuiltinSevenZipOptions());
    const auto Close = qScopeGuard([&] { CloseApprovedWcxBackend(Backend); });
    Context.maxEnumerationEntries_ = qMin(Context.maxEnumerationEntries_, size_t(MaxEntries));
    auto Listing = Backend->Enumerate(Archive.toUtf8().toStdString(), false, Context);
    if (!Listing.HasValue()) return Listing.Failure();
    QVector<ArchiveHeader> Entries; QStringList Names; QMap<QString, int> Files;
    QSet<QString> Directories; quint64 Total = 0; int Manifest = -1;
    for (;;) {
        auto Page = Listing.Value()->Next(128, Context);
        if (!Page.HasValue()) return Page.Failure();
        if (Entries.size() + qsizetype(Page.Value().entries_.size()) > MaxEntries) return Failure("plugin-package-entry-limit");
        for (const auto& Entry : Page.Value().entries_) {
            const auto Name = Member(QString::fromUtf8(Entry.name_), Entry.directory_);
            if (Name.isEmpty() || Entry.encrypted_ || (Entry.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT))
                return Failure("plugin-package-unsafe-entry");
            const auto Key = Name.toCaseFolded();
            if (Files.contains(Key) || (!Entry.directory_ && Directories.contains(Key))) return Failure("plugin-package-path-conflict");
            auto Parent = Key;
            while (Parent.contains('/')) {
                Parent = Parent.left(Parent.lastIndexOf('/'));
                if (Files.contains(Parent)) return Failure("plugin-package-path-conflict");
                Directories.insert(Parent);
            }
            if (Entry.directory_) Directories.insert(Key);
            else {
                if (Entry.size_ > MaxBytes - Total) return Failure("plugin-package-size-limit");
                Total += Entry.size_; Files.insert(Key, Entries.size());
                if (QFileInfo(Name).fileName().compare("pluginst.inf", Qt::CaseInsensitive) == 0) {
                    if (Manifest >= 0 || Entry.size_ > 256 * 1024) return Failure("plugin-package-metadata-ambiguous");
                    Manifest = Entries.size();
                }
            }
            Entries.append(Entry); Names.append(Name);
        }
        if (Page.Value().end_) break;
        if (Page.Value().entries_.empty()) return Failure("plugin-package-list-no-progress");
    }
    Listing.Value().reset();
    QMap<QString, QString> Meta; QString Prefix;
    if (Manifest >= 0) {
        const auto Inf = Scratch.filePath("pluginst.inf");
        auto Extracted = Backend->Extract(Archive.toUtf8().toStdString(), Entries[Manifest], Inf.toUtf8().toStdString(), Context);
        if (!Extracted.HasValue()) return Extracted.Failure();
        QFile File(Inf); if (!File.open(QIODevice::ReadOnly) || File.size() > 256 * 1024) return Failure("plugin-package-metadata-read");
        auto Parsed = Metadata(File.readAll()); if (!Parsed.HasValue()) return Parsed.Failure(); Meta = std::move(Parsed.Value());
        const auto Slash = Names[Manifest].lastIndexOf('/');
        if (Slash >= 0) Prefix = Names[Manifest].left(Slash + 1);
    }
    TcPluginInstallation Result; int Selected = -1;
    if (Manifest >= 0) {
        Result.group_ = Meta.value("type").toLower();
        if (!QStringList{"wcx", "wlx", "wfx", "wdx"}.contains(Result.group_)) return Failure("plugin-package-type");
        QStringList Candidates;
        if (!Meta.value("file64").isEmpty()) Candidates << Meta.value("file64");
        const auto File = Meta.value("file");
        if (File.isEmpty() && Candidates.isEmpty()) return Failure("plugin-package-binary-missing");
        if (File.endsWith('.' + Result.group_, Qt::CaseInsensitive)) Candidates << File + "64";
        if (!File.isEmpty()) Candidates << File;
        for (const auto& Candidate : Candidates) {
            const auto Name = Member(Candidate);
            if (Name.isEmpty()) return Failure("plugin-package-binary-path");
            const auto Found = Files.constFind((Prefix + Name).toCaseFolded());
            if (Found != Files.cend()) { Selected = Found.value(); break; }
        }
    } else {
        for (auto It = Files.cbegin(); It != Files.cend(); ++It) {
            const auto Group = TcPluginGroup(Names[It.value()]);
            if (Group.isEmpty() || !QFileInfo(Names[It.value()]).suffix().endsWith("64", Qt::CaseInsensitive)) continue;
            if (Selected >= 0) return Failure("plugin-package-binary-ambiguous");
            Selected = It.value(); Result.group_ = Group;
        }
    }
    if (Selected < 0) return Failure("plugin-package-binary-missing");
    const auto Folder = Manifest >= 0 ? Meta.value("defaultdir") : QFileInfo(Source).completeBaseName();
    if (!Segment(Folder)) return Failure("plugin-package-directory");
    static const QRegularExpression ExtensionSeparator("[,;|\\s]+");
    for (auto Extension : Meta.value("defaultextension").split(ExtensionSeparator, Qt::SkipEmptyParts)) {
        Extension = Extension.toLower(); if (Extension.startsWith("*.")) Extension.remove(0, 2); else if (Extension.startsWith('.')) Extension.remove(0, 1);
        if (Extension.size() > 128 || Extension.contains('/') || Extension.contains('\\') || Extension.contains(':')) return Failure("plugin-package-extension");
        Result.extensions_.append(Extension);
    }
    Result.extensions_.removeDuplicates();
    QString CategoryPath;
    if (!Category(ProductDirectory, Result.group_, CategoryPath)) return Failure("plugin-package-install-directory");
    QTemporaryDir Stage(QDir(CategoryPath).filePath(".install-XXXXXX"));
    if (!Stage.isValid()) return Failure("plugin-package-install-directory");
    const auto Raw = Stage.filePath("raw"), Payload = Stage.filePath("payload");
    if (!QDir().mkdir(Raw) || !QDir().mkdir(Payload)) return Failure("plugin-package-staging");
    auto Extracted = Backend->ExtractAll(Archive.toUtf8().toStdString(), Raw.toUtf8().toStdString(), Context);
    if (!Extracted.HasValue()) return Extracted.Failure();
    QString Binary;
    for (int Index = 0; Index < Entries.size(); ++Index) {
        if (const auto Stop = Context.Check({"tc-plugin-package"})) return *Stop;
        if (!Prefix.isEmpty() && Prefix.startsWith(Names[Index] + '/', Qt::CaseInsensitive) && Entries[Index].directory_) continue;
        if (!Names[Index].startsWith(Prefix, Qt::CaseInsensitive)) return Failure("plugin-package-root-conflict");
        const auto Name = Names[Index].mid(Prefix.size());
        if (Index == Manifest) continue;
        const auto To = QDir(Payload).filePath(Name);
        if (Entries[Index].directory_) { if (!QDir().mkpath(To)) return Failure("plugin-package-output-directory"); continue; }
        const auto From = QDir(Raw).filePath(QString::number(Index) + ".bin");
        if (QFileInfo(From).size() < 0 || quint64(QFileInfo(From).size()) != Entries[Index].size_
            || !QDir().mkpath(QFileInfo(To).absolutePath()) || !QFile::rename(From, To)) return Failure("plugin-package-output-file");
        if (Index == Selected) Binary = Name;
    }
    const auto StagedBinary = QDir(Payload).filePath(Binary);
    if (Binary.isEmpty() || !Binary64(StagedBinary)) return Failure("plugin-package-x64-required");
    // Legacy TC settings resolve .wcx/.wlx/.wfx/.wdx to their 64-bit companion.
    // Preserve the archive's original filename as well as that companion alias.
    if (Binary.endsWith('.' + Result.group_, Qt::CaseInsensitive)) {
        if (!QFile::copy(StagedBinary, StagedBinary + "64")) return Failure("plugin-package-binary-alias");
        Binary += "64";
    }
    for (int Suffix = 1; Suffix <= 10000; ++Suffix) {
        if (const auto Stop = Context.Check({"tc-plugin-package"})) return *Stop;
        const auto Name = Folder + (Suffix == 1 ? QString{} : '-' + QString::number(Suffix));
        const auto Destination = QDir(CategoryPath).filePath(Name);
        if (QFileInfo::exists(Destination)) continue;
        if (!QDir().rename(Payload, Destination)) return Failure("plugin-package-publish");
        Result.directory_ = Destination; Result.binary_ = QDir(Destination).filePath(Binary); return Result;
    }
    return Failure("plugin-package-directory-collision");
}
}
