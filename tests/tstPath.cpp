#include "stdafx.h"
#include "cmnPath.hpp"
#include <QtTest>
#include <winioctl.h>

class CPathTests : public QObject
{
    Q_OBJECT

private slots:
    void extension_data();
    void extension();
    void separators();
    void fileAndDirectoryQueries();
    void missingPathErrors();
    void reparseLinks();
};

namespace
{
    // Mount point to a local folder; needs no symbolic-link privilege.
    bool MakeJunction(const std::wstring& Link, const std::wstring& Target)
    {
        if (!CreateDirectoryW(Link.c_str(), nullptr)) return false;
        const auto Substitute = std::wstring(L"\\??\\") + Target;
        const auto PathBytes = (Substitute.size() + Target.size() + 2) * sizeof(wchar_t);
        std::vector<BYTE> Buffer(16 + PathBytes);
        const auto Put16 = [&](size_t Offset, size_t Value) { const WORD Word = WORD(Value); memcpy(Buffer.data() + Offset, &Word, 2); };
        const DWORD Tag = IO_REPARSE_TAG_MOUNT_POINT; memcpy(Buffer.data(), &Tag, 4);
        Put16(4, 8 + PathBytes); Put16(8, 0); Put16(10, Substitute.size() * 2);
        Put16(12, (Substitute.size() + 1) * 2); Put16(14, Target.size() * 2);
        memcpy(Buffer.data() + 16, Substitute.c_str(), (Substitute.size() + 1) * 2);
        memcpy(Buffer.data() + 16 + (Substitute.size() + 1) * 2, Target.c_str(), (Target.size() + 1) * 2);
        const auto Handle = CreateFileW(Link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (Handle == INVALID_HANDLE_VALUE) return false;
        DWORD Returned = 0;
        const bool Set = DeviceIoControl(Handle, FSCTL_SET_REPARSE_POINT, Buffer.data(), DWORD(Buffer.size()), nullptr, 0, &Returned, nullptr);
        CloseHandle(Handle); return Set;
    }
    nsCmn::nsCmnPath::TyReparseLink ReadLink(const std::wstring& Path, bool* Ok)
    {
        const auto Handle = CreateFileW(Path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        *Ok = false; if (Handle == INVALID_HANDLE_VALUE) return {};
        const auto Link = nsCmn::nsCmnPath::ReadReparseLink(Handle); CloseHandle(Handle);
        *Ok = Link.Value.has_value(); return Link.Value.value_or(nsCmn::nsCmnPath::TyReparseLink{});
    }
}

void CPathTests::extension_data()
{
    QTest::addColumn<QString>("Name");
    QTest::addColumn<QString>("Expected");
    QTest::newRow("ordinary") << QStringLiteral("report.txt") << QStringLiteral("txt");
    QTest::newRow("multiple-dots") << QStringLiteral("archive.tar.gz") << QStringLiteral("gz");
    QTest::newRow("none") << QStringLiteral("README") << QString();
    QTest::newRow("trailing-dot") << QStringLiteral("name.") << QString();
    QTest::newRow("unicode") << QStringLiteral("한글 문서.TXT") << QStringLiteral("TXT");
    // These tests characterize existing filename-only behavior before provider migration.
    QTest::newRow("dotfile") << QStringLiteral(".gitignore") << QStringLiteral("gitignore");
}

void CPathTests::extension()
{
    QFETCH(QString, Name);
    QFETCH(QString, Expected);
    QCOMPARE(nsCmn::nsCmnPath::GetFileExtension(Name), Expected);
}

void CPathTests::separators()
{
    QVERIFY(nsCmn::nsCmnPath::IsContainPathSeps(QStringLiteral("folder/file")));
    QVERIFY(nsCmn::nsCmnPath::IsContainPathSeps(QStringLiteral("folder\\file")));
    QVERIFY(!nsCmn::nsCmnPath::IsContainPathSeps(QStringLiteral("한글 파일.txt")));
    QVERIFY(!nsCmn::nsCmnPath::IsContainPathSeps(QString()));
}

void CPathTests::fileAndDirectoryQueries()
{
    QTemporaryDir Fixture;
    QVERIFY(Fixture.isValid());
    QDir Root(Fixture.path());
    QVERIFY(Root.mkpath(QStringLiteral("한글 폴더/.nested")));

    const auto FilePath = Root.filePath(QStringLiteral("한글 폴더/file name.bin"));
    QFile File(FilePath);
    QVERIFY(File.open(QIODevice::WriteOnly));
    QCOMPARE(File.write(QByteArray("hello\0world", 11)), qint64(11));
    File.close();
    QFile Nested(Root.filePath(QStringLiteral("한글 폴더/.nested/data.bin")));
    QVERIFY(Nested.open(QIODevice::WriteOnly));
    QCOMPARE(Nested.write("123456"), qint64(6));
    Nested.close();

    const auto Size = nsCmn::nsCmnPath::GetFileSize(FilePath.toStdWString());
    QVERIFY(Size.Value.has_value());
    QCOMPARE(*Size.Value, int64_t(11));
    const auto IsFile = nsCmn::nsCmnPath::IsFileExists(FilePath.toStdWString());
    QVERIFY(IsFile.Value.has_value());
    QVERIFY(*IsFile.Value);
    const auto IsDirectory = nsCmn::nsCmnPath::IsDirExists(FilePath.toStdWString());
    QVERIFY(IsDirectory.Value.has_value());
    QVERIFY(!*IsDirectory.Value);
    const auto RootIsDirectory = nsCmn::nsCmnPath::IsDirExists(Fixture.path().toStdWString());
    QVERIFY(RootIsDirectory.Value.has_value());
    QVERIFY(*RootIsDirectory.Value);
    QCOMPARE(nsCmn::nsCmnPath::CalculateDirSize(Fixture.path().toStdWString()), int64_t(17));

    const auto Handle = CreateFileW(FilePath.toStdWString().c_str(), GENERIC_READ,
                                   FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    QVERIFY(Handle != INVALID_HANDLE_VALUE);
    const auto HandleSize = nsCmn::nsCmnPath::GetFileSize(Handle);
    const auto Links = nsCmn::nsCmnPath::GetHardLinkCount(Handle);
    CloseHandle(Handle);
    QVERIFY(HandleSize.Value.has_value());
    QCOMPARE(*HandleSize.Value, int64_t(11));
    QVERIFY(Links.Value.has_value());
    QCOMPARE(*Links.Value, uint32_t(1));
}

void CPathTests::missingPathErrors()
{
    QTemporaryDir Fixture;
    QVERIFY(Fixture.isValid());
    const auto Missing = QDir(Fixture.path()).filePath(QStringLiteral("absent.bin")).toStdWString();
    const auto Size = nsCmn::nsCmnPath::GetFileSize(Missing);
    QVERIFY(!Size.Value.has_value());
    QCOMPARE(Size.ErrorCode.Type, TyOsError::OS_WIN32_ERROR);
    QCOMPARE(Size.ErrorCode.ErrorCode, uint32_t(ERROR_FILE_NOT_FOUND));
    const auto Exists = nsCmn::nsCmnPath::IsFileExists(Missing);
    QVERIFY(!Exists.Value.has_value());
    QCOMPARE(Exists.ErrorCode.ErrorCode, uint32_t(ERROR_FILE_NOT_FOUND));
    // The legacy directory-size API has no error channel; preserve this baseline explicitly.
    QCOMPARE(nsCmn::nsCmnPath::CalculateDirSize(Missing), int64_t(0));
}

void CPathTests::reparseLinks()
{
    using Link = nsCmn::nsCmnPath::TyReparseLink;
    QTemporaryDir Fixture;
    QVERIFY(Fixture.isValid());
    QDir Root(Fixture.path());
    QVERIFY(Root.mkdir(QStringLiteral("대상 폴더")));
    const auto Target = QDir::toNativeSeparators(Root.filePath(QStringLiteral("대상 폴더"))).toStdWString();
    const auto Junction = QDir::toNativeSeparators(Root.filePath(QStringLiteral("접합"))).toStdWString();
    QVERIFY(MakeJunction(Junction, Target));

    bool Ok = false;
    const auto Mount = ReadLink(Junction, &Ok);
    QVERIFY(Ok);
    QCOMPARE(Mount.Kind, Link::REPARSE_LINK_JUNCTION);
    QCOMPARE(Mount.Tag, uint32_t(IO_REPARSE_TAG_MOUNT_POINT));
    QCOMPARE(QString::fromStdWString(Mount.Target), QString::fromStdWString(Target));
    QCOMPARE(nsCmn::nsCmnPath::GetReparsePointTo(QString::fromStdWString(Junction)), QString::fromStdWString(Target));

    // An ordinary folder is no reparse point: the read fails and the legacy query is empty.
    const auto Plain = ReadLink(Target, &Ok);
    QVERIFY(!Ok);
    QCOMPARE(Plain.Kind, Link::REPARSE_LINK_OTHER);
    QVERIFY(nsCmn::nsCmnPath::GetReparsePointTo(QString::fromStdWString(Target)).isEmpty());

    // A relative symbolic link keeps its stored text. Creating one needs Developer Mode or the privilege.
    const auto Symbolic = QDir::toNativeSeparators(Root.filePath(QStringLiteral("상대 링크"))).toStdWString();
    if (!CreateSymbolicLinkW(Symbolic.c_str(), L"대상 폴더", SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        QCOMPARE(GetLastError(), DWORD(ERROR_PRIVILEGE_NOT_HELD));
        qInfo("symbolic-link case not run: Developer Mode or the privilege is unavailable");
        return;
    }
    const auto Relative = ReadLink(Symbolic, &Ok);
    QVERIFY(Ok);
    QCOMPARE(Relative.Kind, Link::REPARSE_LINK_SYMLINK);
    QVERIFY(Relative.Relative);
    QCOMPARE(QString::fromStdWString(Relative.Target), QStringLiteral("대상 폴더"));
}

QTEST_GUILESS_MAIN(CPathTests)
#include "tstPath.moc"
