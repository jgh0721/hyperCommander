#include "stdafx.h"
#include "cmnPath.hpp"
#include <QtTest>

class CPathTests : public QObject
{
    Q_OBJECT

private slots:
    void extension_data();
    void extension();
    void separators();
    void fileAndDirectoryQueries();
    void missingPathErrors();
};

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

QTEST_GUILESS_MAIN(CPathTests)
#include "tstPath.moc"
