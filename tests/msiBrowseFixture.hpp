#pragma once
#include <msi.h>
#include <msiquery.h>
#include <QtEndian>

namespace msiFixture {
inline QByteArray Cabinet() {
    QByteArray Files, Data("application\nplugin\n");
    const auto U16 = [](QByteArray& B, quint16 V) { char P[2]; qToLittleEndian(V, P); B.append(P, 2); };
    const auto U32 = [](QByteArray& B, quint32 V) { char P[4]; qToLittleEndian(V, P); B.append(P, 4); };
    for (const auto& Item : {std::tuple{"fil_APP", 12u, 0u}, std::tuple{"fil_PLUGIN", 7u, 12u}}) {
        U32(Files, std::get<1>(Item)); U32(Files, std::get<2>(Item)); U16(Files, 0);
        U16(Files, 0); U16(Files, 0); U16(Files, 0x20); Files.append(std::get<0>(Item)); Files.append('\0');
    }
    QByteArray Out("MSCF", 4); U32(Out, 0); U32(Out, 44 + Files.size() + 8 + Data.size());
    U32(Out, 0); U32(Out, 44); U32(Out, 0); Out.append(char(3)); Out.append(char(1));
    U16(Out, 1); U16(Out, 2); U16(Out, 0); U16(Out, 1); U16(Out, 0);
    U32(Out, 44 + Files.size()); U16(Out, 1); U16(Out, 0); Out += Files;
    U32(Out, 0); U16(Out, Data.size()); U16(Out, Data.size()); Out += Data; return Out;
}
inline UINT Sql(MSIHANDLE Database, const QString& Statement) {
    PMSIHANDLE View; const auto Open = MsiDatabaseOpenViewW(Database, reinterpret_cast<LPCWSTR>(Statement.utf16()), &View);
    return Open == ERROR_SUCCESS ? MsiViewExecute(View, 0) : Open;
}
inline UINT Create(const QString& Path, bool External = false, bool Loose = false, bool Unsafe = false, bool Cycle = false) {
    PMSIHANDLE Database; auto Code = MsiOpenDatabaseW(reinterpret_cast<LPCWSTR>(Path.utf16()), MSIDBOPEN_CREATE, &Database);
    if (Code != ERROR_SUCCESS) return Code;
    const QStringList Statements{
        "CREATE TABLE `Directory` (`Directory` CHAR(72) NOT NULL, `Directory_Parent` CHAR(72), `DefaultDir` CHAR(255) NOT NULL PRIMARY KEY `Directory`)",
        "CREATE TABLE `Component` (`Component` CHAR(72) NOT NULL, `Directory_` CHAR(72) NOT NULL PRIMARY KEY `Component`)",
        "CREATE TABLE `File` (`File` CHAR(72) NOT NULL, `Component_` CHAR(72) NOT NULL, `FileName` CHAR(255) NOT NULL, `FileSize` LONG NOT NULL, `Attributes` SHORT, `Sequence` SHORT NOT NULL PRIMARY KEY `File`)",
        "CREATE TABLE `Media` (`DiskId` SHORT NOT NULL, `LastSequence` SHORT NOT NULL, `Cabinet` CHAR(255) PRIMARY KEY `DiskId`)",
        "CREATE TABLE `CreateFolder` (`Directory_` CHAR(72) NOT NULL, `Component_` CHAR(72) NOT NULL PRIMARY KEY `Directory_`, `Component_`)",
        "INSERT INTO `Directory` (`Directory`, `DefaultDir`) VALUES ('TARGETDIR', 'SourceDir')",
        QString("INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES ('INSTALLDIR', '%1', 'MYAPP~1|MyApplication:source')").arg(Cycle ? "PLUGINSDIR" : "TARGETDIR"),
        "INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES ('PLUGINSDIR', 'INSTALLDIR', 'plugins')",
        "INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES ('SAME', 'PLUGINSDIR', '.')",
        "INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES ('EMPTY', 'INSTALLDIR', 'empty')",
        "INSERT INTO `CreateFolder` (`Directory_`, `Component_`) VALUES ('EMPTY', 'cmp_Main')",
        "INSERT INTO `Component` (`Component`, `Directory_`) VALUES ('cmp_Main', 'INSTALLDIR')",
        "INSERT INTO `Component` (`Component`, `Directory_`) VALUES ('cmp_Plugins', 'SAME')",
        QString("INSERT INTO `File` (`File`, `Component_`, `FileName`, `FileSize`, `Attributes`, `Sequence`) VALUES ('fil_APP', 'cmp_Main', '%1', 12, 0, 1)").arg(Unsafe ? "../escape.exe" : "MYAPP~1.EXE|MyApplication.exe"),
        "INSERT INTO `File` (`File`, `Component_`, `FileName`, `FileSize`, `Attributes`, `Sequence`) VALUES ('fil_PLUGIN', 'cmp_Plugins', 'image.dll', 7, 0, 2)",
        QString("INSERT INTO `Media` (`DiskId`, `LastSequence`, `Cabinet`) VALUES (1, 2, '%1')").arg(External ? "payload.cab" : "#payload.cab")
    };
    for (const auto& S : Statements) if ((Code = Sql(Database, S)) != ERROR_SUCCESS) return Code;
    PMSIHANDLE Summary; Code = MsiGetSummaryInformationW(Database, nullptr, 4, &Summary); if (Code != ERROR_SUCCESS) return Code;
    Code = MsiSummaryInfoSetPropertyW(Summary, 15, VT_I4, Loose ? 0 : 2, nullptr, nullptr); if (Code != ERROR_SUCCESS) return Code;
    Code = MsiSummaryInfoSetPropertyW(Summary, 14, VT_I4, 200, nullptr, nullptr); if (Code != ERROR_SUCCESS) return Code;
    Code = MsiSummaryInfoSetPropertyW(Summary, 7, VT_LPSTR, 0, nullptr, L"x64;1033"); if (Code != ERROR_SUCCESS) return Code;
    Code = MsiSummaryInfoPersist(Summary); if (Code != ERROR_SUCCESS) return Code;
    const auto CabPath = QFileInfo(Path).dir().filePath("payload.cab"); QFile Cab(CabPath);
    if (!Cab.open(QIODevice::WriteOnly) || Cab.write(Cabinet()) != Cabinet().size()) return ERROR_WRITE_FAULT; Cab.close();
    if (!External && !Loose) {
        PMSIHANDLE View, Record = MsiCreateRecord(2);
        Code = MsiDatabaseOpenViewW(Database, L"INSERT INTO `_Streams` (`Name`, `Data`) VALUES (?, ?)", &View); if (Code != ERROR_SUCCESS) return Code;
        Code = MsiRecordSetStringW(Record, 1, L"payload.cab"); if (Code != ERROR_SUCCESS) return Code;
        Code = MsiRecordSetStreamW(Record, 2, reinterpret_cast<LPCWSTR>(CabPath.utf16())); if (Code != ERROR_SUCCESS) return Code;
        Code = MsiViewExecute(View, Record); if (Code != ERROR_SUCCESS) return Code;
    }
    return MsiDatabaseCommit(Database);
}
}
