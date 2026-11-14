#include "UniqueLibs/document/completion/completionMatch.hpp"
#include "UniqueLibs/document/completion/completionPopup.hpp"
#include "UniqueLibs/document/sqlite/sqliteApi.hpp"
#include "UniqueLibs/document/sqlite/sqliteCompletion.hpp"
#include "UniqueLibs/document/sqlite/sqliteConnection.hpp"
#include "UniqueLibs/document/sqlite/sqliteDocumentView.hpp"
#include "UniqueLibs/document/sqlite/sqliteFormat.hpp"
#include "UniqueLibs/document/sqlite/sqliteGrid.hpp"
#include "UniqueLibs/document/sqlite/sqliteKeyManager.hpp"
#include "UniqueLibs/document/sqlite/sqliteKeyStore.hpp"
#include "UniqueLibs/document/sqlite/sqliteQueryRunner.hpp"
#include "UniqueLibs/document/sqlite/sqliteQueryTab.hpp"
#include "UniqueLibs/document/sqlite/sqliteResultModel.hpp"
#include "UniqueLibs/document/sqlite/sqliteRuntime.hpp"
#include "UniqueLibs/document/sqlite/sqliteSchema.hpp"
#include "UniqueLibs/document/sqlite/sqliteSeeDialog.hpp"
#include "UniqueLibs/document/sqlite/sqliteSql.hpp"
#include "UniqueLibs/document/sqlite/sqliteDatabaseInfo.hpp"
#include "UniqueLibs/document/sqlite/sqliteTableDesigner.hpp"
#include "UniqueLibs/document/sqlite/sqliteTableTab.hpp"
#include "UniqueLibs/document/sqlite/sqliteTransfer.hpp"
#include "UniqueLibs/document/sqlite/sqliteValuePanel.hpp"
#include "text/ScintillaQtDirectBackend.h"

#include <fmstyle/ThemeManager.h>
#include <fmwidgets/SegmentedControl.h>

#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QFontDatabase>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLineEdit>
#include <QRadioButton>
#include <QToolButton>
#include <QSignalSpy>
#include <QTabBar>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <Scintilla.h>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <thread>

// SQLite 문서의 화면 없는 핵심(설계 261113 10.1): 키트 DLL 불러오기 · SEE 활성화, 파일 머리 검사, 연결 · 문,
// SEE 왕복(키 형식별), 스키마 읽기, keyset 테이블 모델. 키는 시험이 만든 임시 값만 쓰고 출력하지 않는다.
using namespace hc::sqlite;

namespace
{
    std::unique_ptr<Connection> Create(const QString& Path)
    {
        OpenOptions Options;
        Options.create_ = true;
        QString Error;
        auto Db = Connection::Open(Path, Options, &Error);
        if (!Db) qWarning() << "open failed:" << Error;
        return Db;
    }

    bool Run(Connection& Db, const QString& Sql)
    {
        QString Error;
        if (Db.Exec(Sql, &Error)) return true;
        qWarning() << "sql failed:" << Error;
        return false;
    }

    // 자동 완성 시험용 작은 chinook(외래 키 포함).
    const char* MusicSchema =
        "CREATE TABLE artists(ArtistId INTEGER PRIMARY KEY, Name NVARCHAR(120));"
        "CREATE TABLE media_types(MediaTypeId INTEGER PRIMARY KEY, Name NVARCHAR(120));"
        "CREATE TABLE albums(AlbumId INTEGER PRIMARY KEY, Title NVARCHAR(160) NOT NULL, ArtistId INTEGER NOT NULL REFERENCES artists(ArtistId));"
        "CREATE TABLE tracks(TrackId INTEGER PRIMARY KEY, Name NVARCHAR(200) NOT NULL, AlbumId INTEGER REFERENCES albums(AlbumId),"
        " MediaTypeId INTEGER NOT NULL REFERENCES media_types(MediaTypeId), Milliseconds INTEGER NOT NULL);"
        "CREATE VIEW v_long AS SELECT TrackId, Name FROM tracks WHERE Milliseconds > 300000;"
        "INSERT INTO artists VALUES (1, 'AC/DC'); INSERT INTO media_types VALUES (1, 'MPEG');"
        "INSERT INTO albums VALUES (1, 'Rock', 1), (2, 'Jazz', 1);"
        "INSERT INTO tracks VALUES (1, 'One', 1, 1, 343719), (2, 'Two', 1, 1, 200000), (3, 'Three', 2, 1, 400000);";

    QStringList Labels(const CompletionResult& Result)
    {
        QStringList Names;
        for (const auto& Item : Result.candidates_) Names.append(Item.label_);
        return Names;
    }

    const CompletionCandidate* Candidate(const CompletionResult& Result, const QString& Label)
    {
        for (const auto& Item : Result.candidates_) if (Item.label_ == Label) return &Item;
        return nullptr;
    }
}

class SqliteCoreTests final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        // 화면 확인용 PNG 를 남길 때만 제품 스타일(시안3 Paper Fluent)을 깐다.
        if (!qEnvironmentVariable("HC_SQLITE_SHOT").isEmpty()) {
            auto& Theme = fm::style::ThemeManager::instance();
            Theme.setDesign(fm::style::Design::PaperFluent);
            if (auto* App = qobject_cast<QApplication*>(QCoreApplication::instance())) Theme.install(*App);
            auto Font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
            Font.setPixelSize(13);
            QApplication::setFont(Font);
        }
    }

    void runtimeLoadsKitLibrary()
    {
        QVERIFY(Runtime::Load());
        QVERIFY(Runtime::LibraryPath().endsWith(QStringLiteral("sqlite3.dll"), Qt::CaseInsensitive));
        QCOMPARE(Runtime::Version(), QStringLiteral("3.54.0"));
#if __has_include("UniqueLibs/sqlite_see_activate.hpp")
        QCOMPARE(int(Runtime::See()), int(SeeState::Available));
#else
        QCOMPARE(int(Runtime::See()), int(SeeState::NotInBuild));
#endif
        QVERIFY(!Runtime::SeeText(Runtime::See()).isEmpty());
    }

    void headerInspection()
    {
        QByteArray Plain("SQLite format 3", 16);
        Plain.append(QByteArray::fromHex("1000010100402020"));
        Plain.resize(100);
        QCOMPARE(int(InspectHeader(Plain, 8192).kind_), int(FileKind::Plain));
        QCOMPARE(InspectHeader(Plain, 8192).pageSize_, 4096);
        QCOMPARE(int(InspectHeader({}, 0).kind_), int(FileKind::Empty));

        // SEE: 0–15 은 암호문, 16–23 은 평문(페이지 4096, 형식 2 · 2 = WAL, 예약 12, 64 · 32 · 32).
        QByteArray See = QByteArray::fromHex("8a1fc307 5e92d441 0b77e219 a63c58f0").replace(' ', "");
        See = QByteArray::fromHex("8a1fc3075e92d4410b77e219a63c58f0");
        See.append(QByteArray::fromHex("100002020c402020"));
        See.resize(100);
        const auto Info = InspectHeader(See, 4096 * 3);
        QCOMPARE(int(Info.kind_), int(FileKind::SeeEncrypted));
        QCOMPARE(Info.reservedBytes_, 12);
        QVERIFY(Info.wal_);
        // 크기가 페이지 배수가 아니면 SQLite 아님.
        QCOMPARE(int(InspectHeader(See, 4096 * 3 + 1).kind_), int(FileKind::NotSqlite));
        // 페이로드 비율이 다르면 SQLite 아님.
        auto Broken = See; Broken[21] = char(63);
        QCOMPARE(int(InspectHeader(Broken, 4096).kind_), int(FileKind::NotSqlite));
        QVERIFY(!HasPlainHeader(See));
        QVERIFY(HasPlainHeader(Plain));
    }

    void connectionAndStatements()
    {
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        const auto Path = Dir.filePath(QStringLiteral("basic.db"));
        auto Db = Create(Path);
        QVERIFY(Db);
        QVERIFY(Run(*Db, QStringLiteral("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score REAL, data BLOB);"
                                         "INSERT INTO t VALUES(1, 'a''b', 1.5, X'00FF'), (2, NULL, NULL, NULL);")));
        Statement Query(*Db, QStringLiteral("SELECT id, name, score, data FROM t ORDER BY id; SELECT 2"));
        QVERIFY(Query.Valid());
        QVERIFY(Query.ReadOnly());
        QCOMPARE(Query.Tail().trimmed(), QStringLiteral("SELECT 2"));
        QCOMPARE(Query.Step(), SQLITE_ROW);
        QCOMPARE(Query.Value(0).toLongLong(), 1);
        QCOMPARE(Query.Value(1).toString(), QStringLiteral("a'b"));
        QCOMPARE(Query.Value(2).toDouble(), 1.5);
        QCOMPARE(Query.Value(3).toByteArray(), QByteArray::fromHex("00FF"));
        QCOMPARE(Query.TableName(1), QStringLiteral("t"));
        QCOMPARE(Query.Step(), SQLITE_ROW);
        QVERIFY(!Query.Value(1).isValid());
        QCOMPARE(Query.Step(), SQLITE_DONE);
        Statement Bad(*Db, QStringLiteral("SELECT nope FROM t"));
        QVERIFY(!Bad.Valid());
        QVERIFY(Bad.Error().contains(QStringLiteral("no such column")));
        QVERIFY(Db->ErrorOffset() >= 7);
        QCOMPARE(SqlLiteral(QVariant()), QStringLiteral("NULL"));
        QCOMPARE(SqlLiteral(QByteArray::fromHex("0aff")), QStringLiteral("X'0AFF'"));
        QCOMPARE(QuoteIdentifier(QStringLiteral("a\"b")), QStringLiteral("\"a\"\"b\""));

        // 읽기 전용 + query_only: 쓰기가 막힌다.
        Db.reset();
        OpenOptions ReadOnly;
        ReadOnly.readOnly_ = true;
        QString Error;
        auto Ro = Connection::Open(Path, ReadOnly, &Error);
        QVERIFY2(Ro, qPrintable(Error));
        QVERIFY(!Ro->Exec(QStringLiteral("DELETE FROM t"), &Error));
        QCOMPARE(int(InspectFile(Path).kind_), int(FileKind::Plain));
    }

    void seeRoundTripPerKeyFormat()
    {
        if (Runtime::See() != SeeState::Available) QSKIP("SEE is not available in this build");
        QTemporaryDir Dir;
        QVERIFY(Dir.isValid());
        const QList<std::pair<KeyFormat, QByteArray>> Cases{
            {KeyFormat::Text, QByteArrayLiteral("test-only passphrase")},
            {KeyFormat::Raw, QByteArrayLiteral("test-only raw key")},
            {KeyFormat::Hex, QByteArrayLiteral("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff")}};
        int Index = 0;
        for (const auto& [Format, Key] : Cases) {
            const auto Path = Dir.filePath(QStringLiteral("see-%1.db").arg(Index++));
            {
                auto Db = Create(Path);
                QVERIFY(Db);
                QCOMPARE(Db->ApplyKey(Key, Format), SQLITE_OK);
                QVERIFY(Run(*Db, QStringLiteral("CREATE TABLE secret(v); INSERT INTO secret VALUES(42);")));
            }
            QCOMPARE(int(InspectFile(Path).kind_), int(FileKind::SeeEncrypted));
            OpenOptions Options;
            QString Error;
            auto NoKey = Connection::Open(Path, Options, &Error);
            QVERIFY(NoKey);
            QCOMPARE(NoKey->Probe(), SQLITE_NOTADB);
            NoKey.reset();
            // 같은 글자라도 다른 형식이면 다른 키다.
            auto Wrong = Connection::Open(Path, Options, &Error);
            QCOMPARE(Wrong->ApplyKey(Key, Format == KeyFormat::Raw ? KeyFormat::Text : KeyFormat::Raw), SQLITE_OK);
            QCOMPARE(Wrong->Probe(), SQLITE_NOTADB);
            Wrong.reset();
            auto Right = Connection::Open(Path, Options, &Error);
            QCOMPARE(Right->ApplyKey(Key, Format), SQLITE_OK);
            QCOMPARE(Right->Probe(), SQLITE_OK);
            Statement Read(*Right, QStringLiteral("SELECT v FROM secret"));
            QCOMPARE(Read.Step(), SQLITE_ROW);
            QCOMPARE(Read.Value(0).toLongLong(), 42);
        }
        // 잘못된 16진 키는 넣지 않는다.
        auto Db = Create(Dir.filePath(QStringLiteral("hex-bad.db")));
        QCOMPARE(Db->ApplyKey(QByteArrayLiteral("abc"), KeyFormat::Hex), SQLITE_MISUSE);
    }

    void seeChangeEncryption()
    {
        // 설계 8.1: 평문 → 암호화(rekey 한 번, DELETE · WAL 모두), 키 바꾸기, 해제, 바이트 백업.
        if (Runtime::See() != SeeState::Available) QSKIP("SEE is not available in this build");
        QTemporaryDir Dir;
        const auto Opens = [](const QString& Path, const QByteArray& Key, KeyFormat Format) {
            auto Db = Connection::Open(Path, OpenOptions{}, nullptr);
            if (!Db) return -1;
            if (!Key.isEmpty() && Db->ApplyKey(Key, Format) != SQLITE_OK) return -2;
            if (const int Rc = Db->Probe(); Rc != SQLITE_OK) return Rc;
            Statement Read(*Db, QStringLiteral("SELECT v FROM t"));
            return Read.Step() == SQLITE_ROW ? int(Read.Value(0).toLongLong()) : -3;
        };
        for (const auto Journal : {QStringLiteral("DELETE"), QStringLiteral("WAL")}) {
            const auto Path = Dir.filePath(QStringLiteral("rekey-%1.db").arg(Journal));
            auto Db = Create(Path);
            QVERIFY(Run(*Db, QStringLiteral("PRAGMA journal_mode=%1; CREATE TABLE t(v); INSERT INTO t VALUES(7);").arg(Journal)));
            QString Error;
            QVERIFY2(ChangeEncryption(*Db, QByteArrayLiteral("test-only first"), KeyFormat::Text, &Error), qPrintable(Error));
            // 연결을 연 채로도 본 파일이 암호화되어 있다(WAL 은 곧바로 체크포인트).
            QCOMPARE(int(InspectFile(Path).kind_), int(FileKind::SeeEncrypted));
            QCOMPARE(QFileInfo(Path + QStringLiteral("-wal")).size(), 0);
            QCOMPARE(Opens(Path, {}, KeyFormat::Text), SQLITE_NOTADB);
            QCOMPARE(Opens(Path, QByteArrayLiteral("test-only first"), KeyFormat::Text), 7);
            // 바이트 백업은 옛 키로 열린다.
            const auto Backup = BackupPathFor(Path);
            QVERIFY(Backup.endsWith(QStringLiteral(".bak")));
            QVERIFY2(CopyDatabaseFile(*Db, Backup, &Error), qPrintable(Error));
            QCOMPARE(BackupPathFor(Path), Path + QStringLiteral(".bak2"));
            QVERIFY2(ChangeEncryption(*Db, QByteArrayLiteral("0011223344556677"), KeyFormat::Hex, &Error), qPrintable(Error));
            QCOMPARE(Opens(Path, QByteArrayLiteral("test-only first"), KeyFormat::Text), SQLITE_NOTADB);
            QCOMPARE(Opens(Path, QByteArrayLiteral("0011223344556677"), KeyFormat::Hex), 7);
            QCOMPARE(Opens(Backup, QByteArrayLiteral("test-only first"), KeyFormat::Text), 7);
            // 해제: 평문 파일이 된다. 열린 트랜잭션이 있으면 바꾸지 않는다.
            QVERIFY(Run(*Db, QStringLiteral("BEGIN")));
            QVERIFY(!ChangeEncryption(*Db, {}, KeyFormat::Text, &Error));
            QVERIFY(Run(*Db, QStringLiteral("ROLLBACK")));
            QVERIFY2(ChangeEncryption(*Db, {}, KeyFormat::Text, &Error), qPrintable(Error));
            Db.reset();
            QCOMPARE(int(InspectFile(Path).kind_), int(FileKind::Plain));
            QCOMPARE(Opens(Path, {}, KeyFormat::Text), 7);
        }
        QVERIFY(ValidHexKey(QByteArrayLiteral("00aaFF")));
        QVERIFY(!ValidHexKey(QByteArrayLiteral("0aF")));
        QVERIFY(!ValidHexKey(QByteArrayLiteral("zz")));
    }

    void keyStoreDpapiAndFileId()
    {
        // 설계 8.3: DPAPI 왕복, 파일에 평문 키가 없음, 경로 → 파일 ID 로 찾고 경로 고침, 정리, 두 스레드 동시 쓰기.
        QTemporaryDir Dir;
        const auto Profile = Dir.filePath(QStringLiteral("profile"));
        const auto Db = Dir.filePath(QStringLiteral("a.db"));
        { QFile File(Db); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("x"); }
        const auto Key = QByteArrayLiteral("test-only stored key");
        const auto Protected = ProtectKey(Key);
        QVERIFY(!Protected.isEmpty());
        QVERIFY(!Protected.contains(Key));
        QCOMPARE(RevealKey(Protected), Key);
        QVERIFY(RevealKey(QByteArrayLiteral("garbage")).isEmpty());
        QVERIFY(!FileIdentity(Db).isEmpty());
        QCOMPARE(FileIdentity(Db), FileIdentity(QDir::toNativeSeparators(Db).toUpper()));

        KeyStore Store(Profile);
        QVERIFY(Store.Entries().isEmpty());
        QVERIFY(!Store.Find(Db));
        QVERIFY(Store.Save(Db, Key, KeyFormat::Raw));
        {
            QFile File(Store.FileName());
            QVERIFY(File.open(QIODevice::ReadOnly));
            const auto Bytes = File.readAll();
            QVERIFY(!Bytes.contains(Key));
            QVERIFY(Bytes.contains("\"protection\": \"dpapi-user\""));
            QVERIFY(Bytes.contains("\"keyFormat\": \"key\""));
        }
        auto Found = Store.Find(QDir::toNativeSeparators(Db).toUpper());
        QVERIFY(Found);
        QCOMPARE(int(Found->format_), int(KeyFormat::Raw));
        QCOMPARE(RevealKey(Found->protected_), Key);
        // 같은 볼륨 안에서 이름을 바꾸면 파일 ID 로 찾고 경로를 고친다.
        const auto Moved = Dir.filePath(QStringLiteral("renamed.db"));
        QVERIFY(QFile::rename(Db, Moved));
        Found = Store.Find(Moved);
        QVERIFY(Found);
        QCOMPARE(QFileInfo(Found->path_).fileName(), QStringLiteral("renamed.db"));
        QCOMPARE(Store.Entries().size(), 1);
        QCOMPARE(QFileInfo(Store.Entries().first().path_).fileName(), QStringLiteral("renamed.db"));
        // 다시 저장하면 바꾼다(항목은 하나).
        QVERIFY(Store.Save(Moved, QByteArrayLiteral("test-only second"), KeyFormat::Text));
        QCOMPARE(Store.Entries().size(), 1);
        QCOMPARE(RevealKey(Store.Find(Moved)->protected_), QByteArrayLiteral("test-only second"));
        QVERIFY(Store.Touch(Moved));
        QVERIFY(Store.Entries().first().used_.isValid());
        // 없는 파일 정리.
        const auto Gone = Dir.filePath(QStringLiteral("gone.db"));
        { QFile File(Gone); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("y"); }
        QVERIFY(Store.Save(Gone, Key, KeyFormat::Hex));
        QVERIFY(QFile::remove(Gone));
        QCOMPARE(int(KeyStore::StateOf(Store.Entries().last())), int(KeyStore::State::Missing));
        QCOMPARE(Store.RemoveMissing(), 1);
        QCOMPARE(Store.Entries().size(), 1);
        // 두 스레드(빠른 보기 · 편집기 프로세스를 흉내)가 함께 써도 항목을 잃지 않는다(QLockFile).
        const auto Writer = [&](int Base) {
            KeyStore Own(Profile);
            for (int Index = 0; Index < 15; ++Index) Own.Save(Dir.filePath(QStringLiteral("c-%1.db").arg(Base + Index)), Key, KeyFormat::Text);
        };
        std::thread First(Writer, 0), Second(Writer, 100);
        First.join();
        Second.join();
        QCOMPARE(Store.Entries().size(), 31);
        QVERIFY(Store.Remove(Moved));
        QVERIFY(!Store.Find(Moved));
        QVERIFY(Store.RemoveAll());
        QVERIFY(Store.Entries().isEmpty());
    }

    void seeKeyDialogStates()
    {
        // 키 대화(시안 ‘SEE’ 보드): 틀린 키면 대화를 닫지 않고 시도한 형식을 알린 뒤 ‘다시 시도’, 16진 꼴 검사, 맞으면 키를 내준다.
        int Tries = 0;
        SeeKeyDialog Dialog(QStringLiteral("C:/x/chinook_secure.db"), KeyFormat::Text, false, false,
            [&](const QByteArray& Key, KeyFormat Format, bool) {
                ++Tries;
                return Key == "right" && Format == KeyFormat::Raw ? SeeKeyDialog::Outcome{true} : SeeKeyDialog::Outcome{false, true, {}};
            });
        Dialog.show();
        QCOMPARE(Dialog.windowTitle(), QStringLiteral("암호화된 데이터베이스 열기"));
        QCOMPARE(Dialog.Key()->Edit()->echoMode(), QLineEdit::Password);
        Dialog.Key()->findChild<QToolButton*>()->click();
        QCOMPARE(Dialog.Key()->Edit()->echoMode(), QLineEdit::Normal);
        Dialog.Key()->Edit()->setText(QStringLiteral("right"));
        Dialog.accept();
        QVERIFY(Dialog.isVisible());
        QVERIFY(Dialog.ShowingError());
        QCOMPARE(Dialog.OpenButton()->text(), QStringLiteral("다시 시도"));
        if (const auto Shot = qEnvironmentVariable("HC_SQLITE_SHOT"); !Shot.isEmpty()) { Dialog.resize(520, 470); QTest::qWait(100); Dialog.grab().save(Shot + QStringLiteral("-see-error.png")); }
        QVERIFY(Dialog.findChild<QLabel*>(QStringLiteral("sqliteSeeErrorText"))->text().contains(QStringLiteral("암호문 형식")));
        Dialog.SetFormat(KeyFormat::Hex);
        Dialog.Key()->Edit()->setText(QStringLiteral("abc"));
        Dialog.accept();
        QCOMPARE(Tries, 1);
        QVERIFY(Dialog.findChild<QLabel*>(QStringLiteral("sqliteSeeErrorText"))->text().contains(QStringLiteral("16진")));
        Dialog.SetFormat(KeyFormat::Raw);
        Dialog.Key()->Edit()->setText(QStringLiteral("right"));
        Dialog.accept();
        QCOMPARE(Dialog.result(), int(QDialog::Accepted));
        QCOMPARE(Dialog.TakeKey(), QByteArrayLiteral("right"));
        QVERIFY(Dialog.TakeKey().isEmpty());
        QVERIFY(Dialog.Key()->Edit()->text().isEmpty());

        // 암호화 대화: 두 칸이 같아야 하고, 해제는 키를 묻지 않는다.
        SeeEncryptDialog Encrypt(QStringLiteral("C:/x/chinook_secure.db"), true, KeyFormat::Text, KeyFormat::Text, true);
        QCOMPARE(int(Encrypt.Chosen()), int(SeeEncryptDialog::Operation::Change));
        Encrypt.NewKey()->Edit()->setText(QStringLiteral("one"));
        Encrypt.ConfirmKey()->Edit()->setText(QStringLiteral("two"));
        QVERIFY(Encrypt.Problem().contains(QStringLiteral("다릅니다")));
        Encrypt.accept();
        QCOMPARE(Encrypt.result(), int(QDialog::Rejected));
        Encrypt.ConfirmKey()->Edit()->setText(QStringLiteral("one"));
        QVERIFY(Encrypt.Problem().isEmpty());
        QVERIFY(Encrypt.Backup());
        if (const auto Shot = qEnvironmentVariable("HC_SQLITE_SHOT"); !Shot.isEmpty()) { Encrypt.show(); Encrypt.resize(520, 470); QTest::qWait(100); Encrypt.grab().save(Shot + QStringLiteral("-see-rekey.png")); }
        Encrypt.accept();
        QCOMPARE(Encrypt.result(), int(QDialog::Accepted));
        QCOMPARE(Encrypt.TakeKey(), QByteArrayLiteral("one"));
        SeeEncryptDialog Decrypt(QStringLiteral("C:/x/a.db"), true, KeyFormat::Hex, KeyFormat::Text, false);
        Decrypt.Choose(SeeEncryptDialog::Operation::Decrypt);
        QVERIFY(!Decrypt.NewKey()->isEnabled());
        QVERIFY(Decrypt.Problem().isEmpty());
        QCOMPARE(Decrypt.ApplyButton()->text(), QStringLiteral("해제"));
        SeeEncryptDialog Plain(QStringLiteral("C:/x/a.db"), false, KeyFormat::Text, KeyFormat::Hex, false);
        QCOMPARE(int(Plain.Chosen()), int(SeeEncryptDialog::Operation::Encrypt));
        QCOMPARE(int(Plain.Format()), int(KeyFormat::Hex));
        QVERIFY(!Plain.findChild<QRadioButton*>());

        // 데이터베이스 정보의 암호화 칸: 방식 · 저장한 키와 ‘암호화…’ · ‘관리…’(읽기 전용이어도 관리는 연다).
        QTemporaryDir Dir;
        const auto Path = Dir.filePath(QStringLiteral("info.db"));
        auto Db = Create(Path);
        int Changed = 0, Managed = 0;
        DatabaseInfoDialog Info(*Db, [Path](QString* Error) { return Connection::Open(Path, OpenOptions{}, Error); }, Path, [] { return true; }, true);
        QCOMPARE(Info.findChild<QLabel*>(QStringLiteral("sqliteInfo.방식"))->text(), QStringLiteral("없음 (평문 파일)"));
        Info.SetEncryption([&] {
            DatabaseInfoDialog::Encryption Crypto;
            Crypto.method_ = QStringLiteral("SEE · AES-256 · 암호문 (textkey)");
            Crypto.savedKey_ = QStringLiteral("이 PC 에 저장됨 (DPAPI)");
            Crypto.change_ = [&] { ++Changed; return true; };
            Crypto.manageKeys_ = [&] { ++Managed; };
            return Crypto;
        });
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(Info.findChild<QLabel*>(QStringLiteral("sqliteInfo.방식"))->text(), QStringLiteral("SEE · AES-256 · 암호문 (textkey)"));
        auto* Manage = Info.findChild<QPushButton*>(QStringLiteral("sqliteInfoAction.저장한 키"));
        QVERIFY(Manage && Manage->isEnabled());
        Manage->click();
        QCOMPARE(Managed, 1);
        auto* Change = Info.findChild<QPushButton*>(QStringLiteral("sqliteInfoAction.방식"));
        QVERIFY(Change);
        QVERIFY(!Change->isEnabled());   // 읽기 전용
    }

    void seeDocumentUnlockAndRekey()
    {
        // 작업 창(설계 8.2 · 8.3): 잠김 → 틀린 키 → 맞는 키(저장) → 작업 스레드 연결(표 · 쿼리 탭)도 같은 키, 상태 표시줄 ·
        // 스키마 머리글 ‘SEE · AES-256’, 빠른 보기는 저장한 키로만 조용히 연다, 키 바꾸기(백업 · 저장 키 갱신) · 해제(저장 키 삭제).
        if (Runtime::See() != SeeState::Available) QSKIP("SEE is not available in this build");
        QTemporaryDir Dir;
        const auto Path = Dir.filePath(QStringLiteral("secure.db"));
        const auto Profile = Dir.filePath(QStringLiteral("profile"));
        const auto Key = QByteArrayLiteral("test-only document key");
        {
            auto Db = Create(Path);
            QCOMPARE(Db->ApplyKey(Key, KeyFormat::Text), SQLITE_OK);
            QVERIFY(Run(*Db, QStringLiteral("CREATE TABLE secret(id INTEGER PRIMARY KEY, v); INSERT INTO secret(v) VALUES(42), (43);")));
        }
        const auto RunQuery = [](DocumentView& View, const QString& Sql) {
            auto* Query = View.NewQuery(Sql);
            View.RunCommand(QStringLiteral("sqlite.run"));
            if (!QTest::qWaitFor([Query] { return !Query->Running(); }, 10000)) return -2;
            return Query->CurrentGrid() && Query->CurrentGrid()->model() ? Query->CurrentGrid()->model()->rowCount() : -1;
        };
        {
            DocumentView View(Path, DocumentView::Mode::Editor);
            View.SetKeyPromptEnabled(false);
            View.SetProfileDirectory(Profile);
            View.resize(1100, 700);
            View.show();
            View.Open();
            QVERIFY(View.IsLocked());
            QVERIFY(View.findChild<QPushButton*>(QStringLiteral("sqliteKeyButton"))->isVisible());
            QVERIFY(!View.CanRunCommand(QStringLiteral("sqlite.newQuery")));
            bool Wrong = false;
            QVERIFY(!View.Unlock(Key, KeyFormat::Raw, false, false, &Wrong));
            QVERIFY(Wrong);
            QVERIFY(View.IsLocked());
            QVERIFY(View.Unlock(Key, KeyFormat::Text, false, true, &Wrong));
            QVERIFY(!View.IsLocked());
            QVERIFY(View.IsEncrypted());
            QVERIFY(!View.findChild<QPushButton*>(QStringLiteral("sqliteKeyButton"))->isVisible());
            QVERIFY(View.Schema().Find(QStringLiteral("main"), QStringLiteral("secret")));
            bool Status = false;
            for (const auto& Item : View.StatusItems()) Status |= Item.id_ == QLatin1String("sqliteEncryption") && Item.text_ == QStringLiteral("SEE · AES-256");
            QVERIFY(Status);
            bool Chip = false;
            for (auto* Tag : View.findChildren<QWidget*>()) Chip |= Tag->metaObject()->className() == QByteArray("fm::ui::Tag") && Tag->property("text").toString() == QStringLiteral("SEE · AES-256");
            QVERIFY(Chip);
            // 표 탭(keyset 작업 연결)과 쿼리 탭(실행기 연결)이 같은 키로 읽는다.
            View.OpenObject(QStringLiteral("main"), QStringLiteral("secret"));
            QTRY_COMPARE(View.CurrentModel()->rowCount(), 2);
            QCOMPARE(RunQuery(View, QStringLiteral("SELECT v FROM secret ORDER BY v;")), 2);
            // 키 저장소: 저장했고 평문은 없다.
            KeyStore Store(Profile);
            QVERIFY(Store.Find(Path));
            QCOMPARE(RevealKey(Store.Find(Path)->protected_), Key);

            // 빠른 보기: 키를 묻지 않고 저장한 키로만 연다. 끄면 잠김.
            {
                DocumentView Preview(Path, DocumentView::Mode::QuickPreview);
                Preview.SetProfileDirectory(Profile);
                Preview.Open();
                QVERIFY(!Preview.IsLocked());
                QVERIFY(Preview.UsedSavedKey());
                QVERIFY(Preview.IsReadOnly());
                QVERIFY(Preview.findChild<QLabel*>(QStringLiteral("sqlitePreviewHeader"))->text().contains(QStringLiteral("SEE")));
            }
            {
                DocumentView Preview(Path, DocumentView::Mode::QuickPreview);
                Preview.SetProfileDirectory(Profile);
                QueryPreferences Off;
                Off.previewSavedKeys_ = false;
                Preview.ApplyQueryPreferences(Off);
                Preview.Open();
                QVERIFY(Preview.IsLocked());
                QVERIFY(!Preview.RequestKey());
            }

            // 키 바꾸기: 다른 연결(쿼리 탭 실행기)을 닫고 rekey, 백업은 옛 키, 저장 키는 새 키로.
            const auto NewKey = QByteArrayLiteral("test-only rekeyed");
            QString Error;
            QVERIFY2(View.ChangeEncryption(SeeEncryptDialog::Operation::Change, NewKey, KeyFormat::Raw, true, &Error), qPrintable(Error));
            QCOMPARE(int(View.EncryptionFormat()), int(KeyFormat::Raw));
            QVERIFY(QFile::exists(Path + QStringLiteral(".bak")));
            QCOMPARE(int(Store.Find(Path)->format_), int(KeyFormat::Raw));
            QCOMPARE(RevealKey(Store.Find(Path)->protected_), NewKey);
            QCOMPARE(RunQuery(View, QStringLiteral("SELECT v FROM secret;")), 2);
            {
                auto Old = Connection::Open(Path + QStringLiteral(".bak"), OpenOptions{}, nullptr);
                QCOMPARE(Old->ApplyKey(Key, KeyFormat::Text), SQLITE_OK);
                QCOMPARE(Old->Probe(), SQLITE_OK);
                auto Now = Connection::Open(Path, OpenOptions{}, nullptr);
                QCOMPARE(Now->ApplyKey(NewKey, KeyFormat::Raw), SQLITE_OK);
                QCOMPARE(Now->Probe(), SQLITE_OK);
            }
            // 데이터베이스 정보의 암호화 칸.
            View.OpenObject(QStringLiteral("main"), QStringLiteral("secret"));
            QTRY_COMPARE(View.CurrentModel()->rowCount(), 2);
            // 해제: 평문이 되고 저장한 키를 지운다.
            QVERIFY2(View.ChangeEncryption(SeeEncryptDialog::Operation::Decrypt, {}, KeyFormat::Text, false, &Error), qPrintable(Error));
            QVERIFY(!View.IsEncrypted());
            QCOMPARE(int(InspectFile(Path).kind_), int(FileKind::Plain));
            QVERIFY(!Store.Find(Path));
            QCOMPARE(RunQuery(View, QStringLiteral("SELECT v FROM secret;")), 2);
            for (const auto& Item : View.StatusItems()) QVERIFY(Item.id_ != QLatin1String("sqliteEncryption"));
            // 평문을 다시 암호화: ‘저장을 미리 켜 두기’ 가 꺼져 있으면 저장하지 않는다.
            QVERIFY2(View.ChangeEncryption(SeeEncryptDialog::Operation::Encrypt, QByteArrayLiteral("00112233"), KeyFormat::Hex, false, &Error), qPrintable(Error));
            QVERIFY(View.IsEncrypted());
            QCOMPARE(int(InspectFile(Path).kind_), int(FileKind::SeeEncrypted));
            QVERIFY(!Store.Find(Path));
            QCOMPARE(RunQuery(View, QStringLiteral("SELECT v FROM secret;")), 2);
        }
        // 저장한 키가 없으면 편집기도 잠긴 채 연다(키 대화는 시험에서 끈다).
        DocumentView Again(Path, DocumentView::Mode::Editor);
        Again.SetKeyPromptEnabled(false);
        Again.SetProfileDirectory(Profile);
        Again.Open();
        QVERIFY(Again.IsLocked());
        QVERIFY(Again.Unlock(QByteArrayLiteral("00112233"), KeyFormat::Hex, true, false));
        QVERIFY(Again.IsReadOnly());
    }

    void transferReadAndInfer()
    {
        // CSV(RFC 4180): BOM, 구분자 추정, 따옴표 안 구분자 · 줄바꿈 · 겹친 따옴표, 따옴표 없는 빈 칸 = NULL, 빈 줄 건너뜀, 들쭉날쭉한 행.
        const QByteArray Csv = "\xEF\xBB\xBFid;name;note\r\n1;\"a;b\";\r\n2;\"multi\nline\";\"\"\r\n\r\n3;x;\"say \"\"hi\"\"\";extra\n";
        auto Source = ReadCsv(Csv, CsvOptions{});
        QVERIFY2(Source.error_.isEmpty(), qPrintable(Source.error_));
        QCOMPARE(Source.delimiter_, QChar(u';'));
        QCOMPARE(Source.columns_, (QStringList{QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("note"), QStringLiteral("column4")}));
        QCOMPARE(Source.rows_.size(), 3);
        QCOMPARE(Source.rows_[0][1].toString(), QStringLiteral("a;b"));
        QVERIFY(!Source.rows_[0][2].isValid());
        QCOMPARE(Source.rows_[1][1].toString(), QStringLiteral("multi\nline"));
        QVERIFY(Source.rows_[1][2].isValid());
        QCOMPARE(Source.rows_[1][2].toString(), QString());
        QCOMPARE(Source.rows_[2][2].toString(), QStringLiteral("say \"hi\""));
        QCOMPARE(Source.rows_[2][3].toString(), QStringLiteral("extra"));
        CsvOptions NoHeader;
        NoHeader.header_ = false;
        NoHeader.delimiter_ = u',';
        Source = ReadCsv("a,b\n1,2\n", NoHeader);
        QCOMPARE(Source.columns_, (QStringList{QStringLiteral("column1"), QStringLiteral("column2")}));
        QCOMPARE(Source.rows_.size(), 2);
        QVERIFY(!ReadCsv("a,b\n\"open,2\n", CsvOptions{}).error_.isEmpty());
        QCOMPARE(ReadCsv("a\tb\tc\n1\t2\t3\n", CsvOptions{}).delimiter_, QChar(u'\t'));
        QCOMPARE(CsvField(QStringLiteral("a,b"), u','), QStringLiteral("\"a,b\""));
        QCOMPARE(CsvField(QStringLiteral("q\""), u','), QStringLiteral("\"q\"\"\""));
        QCOMPARE(CsvField(QVariant(), u','), QString());
        QCOMPARE(CsvField(QByteArray::fromHex("0aff"), u','), QStringLiteral("0AFF"));

        // JSON: 객체 배열(열은 모든 객체의 열쇠), 중첩은 JSON 글, bool → 1/0, 큰 정수 그대로. JSON Lines. 배열의 배열(첫 줄 열 이름).
        Source = ReadJson(R"([{"id":1,"tags":["a"],"ok":true},{"id":9007199254740993,"score":1.5,"name":null}])");
        QVERIFY2(Source.error_.isEmpty(), qPrintable(Source.error_));
        QCOMPARE(Source.columns_.size(), 5);
        const auto Column = [&](const QString& Name) { return int(Source.columns_.indexOf(Name)); };
        QCOMPARE(Source.rows_[0][Column(QStringLiteral("tags"))].toString(), QStringLiteral("[\"a\"]"));
        QCOMPARE(Source.rows_[0][Column(QStringLiteral("ok"))].toLongLong(), 1);
        QCOMPARE(Source.rows_[1][Column(QStringLiteral("id"))].toLongLong(), 9007199254740993ll);
        QCOMPARE(Source.rows_[1][Column(QStringLiteral("score"))].toDouble(), 1.5);
        QVERIFY(!Source.rows_[1][Column(QStringLiteral("name"))].isValid());
        QVERIFY(!Source.rows_[0][Column(QStringLiteral("score"))].isValid());
        Source = ReadJson("{\"a\":1}\n{\"a\":2,\"b\":\"x\"}\n");
        QVERIFY2(Source.error_.isEmpty(), qPrintable(Source.error_));
        QCOMPARE(Source.rows_.size(), 2);
        QCOMPARE(Source.columns_.size(), 2);
        Source = ReadJson(R"([["x","y"],[1,"p"],[2,"q"]])");
        QCOMPARE(Source.columns_, (QStringList{QStringLiteral("x"), QStringLiteral("y")}));
        QCOMPARE(Source.rows_.size(), 2);
        QVERIFY(!ReadJson("{bad").error_.isEmpty());

        // 형식 추정 · 열 맞춤.
        const QVector<QVector<QVariant>> Rows{{QStringLiteral("1"), QStringLiteral("1.5"), QStringLiteral("x"), QVariant()}, {QStringLiteral("-2"), QStringLiteral("3"), QStringLiteral("2"), QVariant()}};
        QCOMPARE(InferType(Rows, 0), QStringLiteral("INTEGER"));
        QCOMPARE(InferType(Rows, 1), QStringLiteral("REAL"));
        QCOMPARE(InferType(Rows, 2), QStringLiteral("TEXT"));
        QCOMPARE(InferType(Rows, 3), QStringLiteral("TEXT"));
        int Matched = 0;
        QCOMPARE(MatchColumns({QStringLiteral("NAME"), QStringLiteral("zzz")}, {QStringLiteral("id"), QStringLiteral("name")}, &Matched), (QStringList{QStringLiteral("name"), QString()}));
        QCOMPARE(Matched, 1);
        QCOMPARE(MatchColumns({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}, {QStringLiteral("id"), QStringLiteral("name")}, &Matched), (QStringList{QStringLiteral("id"), QStringLiteral("name"), QString()}));
        QCOMPARE(Matched, 2);
    }

    void transferRoundTrips()
    {
        // 내보내기 → 다시 읽기 · 가져오기: NULL · 따옴표 · 쉼표 · 줄바꿈 · 한글 · 실수 · BLOB. 실패는 모두 되돌리고, 취소는 파일을 남기지 않는다.
        QTemporaryDir Dir;
        auto Db = Create(Dir.filePath(QStringLiteral("t.db")));
        QVERIFY(Run(*Db, QStringLiteral("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT NOT NULL, score REAL, data BLOB, note TEXT);"
            "INSERT INTO t VALUES (1, 'a,b \"q\"', 0.1, X'00FF', NULL), (2, '한글' || char(10) || '둘째 줄', -2.5, NULL, ''), (3, 'x', NULL, NULL, 'end');")));
        std::atomic_bool Never = false;
        std::atomic<qint64> Count = 0;
        ExportRequest Request;
        Request.sql_ = QStringLiteral("SELECT * FROM t ORDER BY id");
        Request.source_ = Request.table_ = QStringLiteral("t");

        Request.path_ = Dir.filePath(QStringLiteral("t.csv"));
        auto Result = ExportData(Db.get(), Request, Never, Count);
        QVERIFY2(Result.ok_, qPrintable(Result.error_));
        QCOMPARE(Result.rows_, 3);
        QFile CsvFile(Request.path_);
        QVERIFY(CsvFile.open(QIODevice::ReadOnly));
        const auto CsvBytes = CsvFile.readAll();
        QVERIFY(CsvBytes.startsWith("\xEF\xBB\xBFid,name,score,data,note\r\n"));
        auto Back = ReadCsv(CsvBytes, CsvOptions{});
        QCOMPARE(Back.rows_.size(), 3);
        QCOMPARE(Back.rows_[0][1].toString(), QStringLiteral("a,b \"q\""));
        QCOMPARE(Back.rows_[1][1].toString(), QStringLiteral("한글\n둘째 줄"));
        QCOMPARE(Back.rows_[0][2].toString(), QStringLiteral("0.1"));
        QCOMPARE(Back.rows_[0][3].toString(), QStringLiteral("00FF"));
        QVERIFY(!Back.rows_[0][4].isValid());
        // CSV → 새 테이블(형식 추정).
        ImportRequest Into;
        Into.table_ = QStringLiteral("t_csv");
        Into.create_ = true;
        for (int Column = 0; Column < Back.columns_.size(); ++Column) Into.types_.append(InferType(Back.rows_, Column));
        QCOMPARE(Into.types_[0], QStringLiteral("INTEGER"));
        QCOMPARE(Into.types_[2], QStringLiteral("REAL"));
        Result = ImportData(*Db, Back, Into, Never, Count);
        QVERIFY2(Result.ok_, qPrintable(Result.error_));
        const auto Value = [&](const QString& Sql) { Statement Query(*Db, Sql); return Query.Step() == SQLITE_ROW ? Query.Value(0) : QVariant(QStringLiteral("<none>")); };
        QCOMPARE(Value(QStringLiteral("SELECT count(*) FROM t_csv")).toLongLong(), 3);
        QCOMPARE(Value(QStringLiteral("SELECT typeof(score) FROM t_csv WHERE id = 2")).toString(), QStringLiteral("real"));
        QCOMPARE(Value(QStringLiteral("SELECT name FROM t_csv WHERE id = 2")).toString(), QStringLiteral("한글\n둘째 줄"));
        QCOMPARE(Value(QStringLiteral("SELECT note IS NULL FROM t_csv WHERE id = 1")).toLongLong(), 1);

        // JSON: 정수는 정수, NULL 은 null, BLOB 은 Base64.
        Request.format_ = TransferFormat::Json;
        Request.path_ = Dir.filePath(QStringLiteral("t.json"));
        Result = ExportData(Db.get(), Request, Never, Count);
        QVERIFY2(Result.ok_, qPrintable(Result.error_));
        QFile JsonFile(Request.path_);
        QVERIFY(JsonFile.open(QIODevice::ReadOnly));
        Back = ReadJson(JsonFile.readAll());
        QVERIFY2(Back.error_.isEmpty(), qPrintable(Back.error_));
        QCOMPARE(Back.rows_.size(), 3);
        const int Data = int(Back.columns_.indexOf(QStringLiteral("data"))), Id = int(Back.columns_.indexOf(QStringLiteral("id")));
        QCOMPARE(Back.rows_[0][Id].typeId(), int(QMetaType::LongLong));
        QCOMPARE(Back.rows_[0][Data].toString(), QStringLiteral("AP8="));
        QVERIFY(!Back.rows_[1][Data].isValid());

        // SQL: CREATE + INSERT 를 새 파일에서 실행하면 같은 값.
        Request.format_ = TransferFormat::Sql;
        Request.path_ = Dir.filePath(QStringLiteral("t.sql"));
        Request.createSql_ = Value(QStringLiteral("SELECT sql FROM sqlite_schema WHERE name = 't'")).toString();
        Result = ExportData(Db.get(), Request, Never, Count);
        QVERIFY2(Result.ok_, qPrintable(Result.error_));
        QFile SqlFile(Request.path_);
        QVERIFY(SqlFile.open(QIODevice::ReadOnly));
        auto Copy = Create(Dir.filePath(QStringLiteral("copy.db")));
        QVERIFY(Run(*Copy, QString::fromUtf8(SqlFile.readAll())));
        Statement Left(*Db, QStringLiteral("SELECT id, name, score, data, note FROM t ORDER BY id"));
        Statement Right(*Copy, QStringLiteral("SELECT id, name, score, data, note FROM t ORDER BY id"));
        while (Left.Step() == SQLITE_ROW) {
            QCOMPARE(Right.Step(), SQLITE_ROW);
            for (int Column = 0; Column < 5; ++Column) QCOMPARE(Right.Value(Column), Left.Value(Column));
        }
        QCOMPARE(Right.Step(), SQLITE_DONE);

        // 기존 테이블에 이름으로 맞춰 넣다가 NOT NULL 에 걸리면 모두 되돌린다.
        ImportSource Bad;
        Bad.columns_ = {QStringLiteral("NAME"), QStringLiteral("ignored")};
        Bad.rows_ = {{QStringLiteral("ok"), 1}, {QVariant(), 2}};
        ImportRequest Existing;
        Existing.table_ = QStringLiteral("t");
        Existing.targets_ = MatchColumns(Bad.columns_, {QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("score")});
        Result = ImportData(*Db, Bad, Existing, Never, Count);
        QVERIFY(!Result.ok_);
        QVERIFY2(Result.error_.contains(QStringLiteral("원본 2번째 행")), qPrintable(Result.error_));
        QCOMPARE(Value(QStringLiteral("SELECT count(*) FROM t")).toLongLong(), 3);
        QVERIFY(sqlite3_get_autocommit(Db->Handle()) != 0);
        // 취소: 파일을 남기지 않는다.
        std::atomic_bool Stop = true;
        Request.format_ = TransferFormat::Csv;
        Request.path_ = Dir.filePath(QStringLiteral("canceled.csv"));
        Result = ExportData(Db.get(), Request, Stop, Count);
        QVERIFY(Result.canceled_ || Result.ok_);
        if (Result.canceled_) QVERIFY(!QFile::exists(Request.path_));
        Into.table_ = QStringLiteral("t_canceled");
        Result = ImportData(*Db, Back, Into, Stop, Count);
        QVERIFY(Result.canceled_);
        QCOMPARE(Value(QStringLiteral("SELECT count(*) FROM sqlite_schema WHERE name = 't_canceled'")).toLongLong(), 0);
        // 쓰기 문은 내보내지 않는다.
        Request.sql_ = QStringLiteral("DELETE FROM t");
        QVERIFY(!ExportData(Db.get(), Request, Never, Count).ok_);
        QCOMPARE(Value(QStringLiteral("SELECT count(*) FROM t")).toLongLong(), 3);
    }

    void transferInWorkWindow()
    {
        // 작업 창: 테이블 · 쿼리 결과 내보내기(작업 스레드 연결), 가져오기 뒤 스키마 · 표 다시 읽기, 수동 트랜잭션 중에는 막기, 대화 상자.
        QTemporaryDir Dir;
        const auto Path = Dir.filePath(QStringLiteral("work.db"));
        {
            auto Db = Create(Path);
            QVERIFY(Run(*Db, QStringLiteral("CREATE TABLE people(id INTEGER PRIMARY KEY, name TEXT, age INTEGER, full TEXT GENERATED ALWAYS AS (name || '!') VIRTUAL);"
                "INSERT INTO people(name, age) VALUES ('Ann', 30), ('Bo', 41);")));
        }
        DocumentView View(Path, DocumentView::Mode::Editor);
        View.resize(1100, 700);
        View.show();
        View.Open();
        QVERIFY(View.IsReady());
        auto Request = View.ExportRequestFor(QStringLiteral("main"), QStringLiteral("people"));
        QVERIFY(Request.createSql_.startsWith(QStringLiteral("CREATE TABLE people")));
        Request.format_ = TransferFormat::Sql;
        Request.path_ = Dir.filePath(QStringLiteral("people.sql"));
        auto Result = View.Export(Request);
        QVERIFY2(Result.ok_, qPrintable(Result.error_));
        QCOMPARE(Result.rows_, 2);
        {
            // 생성 열은 INSERT 에서 뺀다.
            QFile File(Request.path_);
            QVERIFY(File.open(QIODevice::ReadOnly));
            const auto Text = QString::fromUtf8(File.readAll());
            QVERIFY(Text.contains(QStringLiteral("INSERT INTO \"people\" (\"id\", \"name\", \"age\") VALUES (1, 'Ann', 30);")));
        }
        // 쿼리 결과: 읽기 문이면 다시 읽어 모든 행.
        auto* Query = View.NewQuery(QStringLiteral("SELECT name, age FROM people ORDER BY id;"));
        QVERIFY(!View.CanRunCommand(QStringLiteral("sqlite.export")));
        QVERIFY(View.RunCommand(QStringLiteral("sqlite.run")));
        QTRY_VERIFY(!Query->Running());
        QVERIFY(Query->Result()->findChild<QToolButton*>(QStringLiteral("sqliteResultExport"))->isEnabled());
        QVERIFY(View.CanRunCommand(QStringLiteral("sqlite.export")));
        ExportRequest FromResult;
        QVERIFY(View.CurrentResultExport(&FromResult));
        QCOMPARE(FromResult.sql_, QStringLiteral("SELECT name, age FROM people ORDER BY id;"));
        QCOMPARE(FromResult.table_, QStringLiteral("people"));
        FromResult.format_ = TransferFormat::Json;
        FromResult.path_ = Dir.filePath(QStringLiteral("result.json"));
        Result = View.Export(FromResult);
        QVERIFY2(Result.ok_, qPrintable(Result.error_));
        QCOMPARE(Result.rows_, 2);

        // 가져오기: 새 테이블 → 스키마에 나타난다. 기존 테이블 → 열린 표 탭을 다시 읽는다.
        const auto Csv = Dir.filePath(QStringLiteral("more.csv"));
        { QFile File(Csv); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("name,age\nCy,25\nDee,\n"); }
        auto Source = ReadImportFile(Csv, CsvOptions{});
        ImportRequest Fresh;
        Fresh.table_ = QStringLiteral("imported");
        Fresh.create_ = true;
        Fresh.types_ = {InferType(Source.rows_, 0), InferType(Source.rows_, 1)};
        Result = View.Import(Source, Fresh);
        QVERIFY2(Result.ok_, qPrintable(Result.error_));
        QVERIFY(View.Schema().Find(QStringLiteral("main"), QStringLiteral("imported")));
        View.OpenObject(QStringLiteral("main"), QStringLiteral("people"));
        QTRY_COMPARE(View.CurrentModel()->rowCount(), 2);
        // 스크립트 만들기(스키마 메뉴): 생성 열은 SELECT 에만, 키는 기본 키.
        QVERIFY(View.ScriptFor(QStringLiteral("main"), QStringLiteral("people"), QStringLiteral("SELECT")).startsWith(QStringLiteral("SELECT id, name, age, ")));
        QCOMPARE(View.ScriptFor(QStringLiteral("main"), QStringLiteral("people"), QStringLiteral("INSERT")), QStringLiteral("INSERT INTO people (id, name, age)\nVALUES (NULL, NULL, NULL);\n"));
        QCOMPARE(View.ScriptFor(QStringLiteral("main"), QStringLiteral("people"), QStringLiteral("UPDATE")), QStringLiteral("UPDATE people\nSET name = NULL,\n    age = NULL\nWHERE id = NULL;\n"));
        QCOMPARE(View.ScriptFor(QStringLiteral("main"), QStringLiteral("people"), QStringLiteral("DELETE")), QStringLiteral("DELETE FROM people\nWHERE id = NULL;\n"));
        QVERIFY(View.ScriptFor(QStringLiteral("main"), QStringLiteral("people"), QStringLiteral("CREATE")).startsWith(QStringLiteral("CREATE TABLE people")));
        // 생성 열은 표에서 고칠 수 없다(복제에서도 뺀다).
        QVERIFY(View.CurrentModel()->flags(View.CurrentModel()->index(0, 1)) & Qt::ItemIsEditable);
        QVERIFY(!(View.CurrentModel()->flags(View.CurrentModel()->index(0, 3)) & Qt::ItemIsEditable));
        ImportRequest Existing;
        Existing.table_ = QStringLiteral("people");
        Existing.targets_ = MatchColumns(Source.columns_, {QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("age")});
        Result = View.Import(Source, Existing);
        QVERIFY2(Result.ok_, qPrintable(Result.error_));
        QTRY_COMPARE(View.CurrentModel()->rowCount(), 4);
        // 수동 커밋 쿼리 탭의 열린 트랜잭션이 있으면 막는다.
        auto* Manual = View.NewQuery(QStringLiteral("UPDATE people SET age = age + 1 WHERE id = 1;"));
        Manual->SetManualCommit(true);
        QVERIFY(View.RunCommand(QStringLiteral("sqlite.run")));
        QTRY_VERIFY(Manual->InTransaction());
        Result = View.Import(Source, Existing);
        QVERIFY(!Result.ok_);
        QVERIFY(Result.error_.contains(QStringLiteral("트랜잭션")));
        QVERIFY(Manual->EndTransaction(false, true));

        // 대화 상자: 기존 테이블이면 이름이 맞는 열, 새 테이블이면 추정 형식. 내보내기는 형식에 맞춰 확장자를 바꾼다.
        {
            ImportDialog Dialog(Csv, {{QStringLiteral("people"), {QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("age")}}}, QStringLiteral("people"));
            QCOMPARE(Dialog.Preview()->rowCount(), 2);
            QVERIFY2(Dialog.Summary()->text().contains(QStringLiteral("이름이 맞는 열 2개")), qPrintable(Dialog.Summary()->text()));
            QVERIFY(!Dialog.Request().create_);
            Dialog.SetNewTable(QStringLiteral("people"));
            Dialog.accept();
            QCOMPARE(Dialog.result(), int(QDialog::Rejected));
            Dialog.SetNewTable(QStringLiteral("people2"));
            QVERIFY(Dialog.Request().create_);
            QCOMPARE(Dialog.Request().types_, (QStringList{QStringLiteral("TEXT"), QStringLiteral("INTEGER")}));
            QVERIFY(Dialog.Summary()->text().contains(QStringLiteral("age INTEGER")));
            if (const auto Shot = qEnvironmentVariable("HC_SQLITE_SHOT"); !Shot.isEmpty()) { Dialog.show(); QTest::qWait(100); Dialog.grab().save(Shot + QStringLiteral("-import.png")); }
        }
        {
            ExportDialog Dialog(View.ExportRequestFor(QStringLiteral("main"), QStringLiteral("people")), true);
            QVERIFY(Dialog.PathEdit()->text().endsWith(QStringLiteral("people.csv")));
            Dialog.SetFormat(TransferFormat::Json);
            QVERIFY(Dialog.PathEdit()->text().endsWith(QStringLiteral("people.json")));
            QCOMPARE(int(Dialog.Request().format_), int(TransferFormat::Json));
            Dialog.SetFormat(TransferFormat::Sql);
            QVERIFY(!Dialog.Request().createSql_.isEmpty());
            if (const auto Shot = qEnvironmentVariable("HC_SQLITE_SHOT"); !Shot.isEmpty()) { Dialog.SetFormat(TransferFormat::Csv); Dialog.show(); QTest::qWait(100); Dialog.grab().save(Shot + QStringLiteral("-export.png")); }
        }
    }

    void keyManagerPage()
    {
        // 설정 › 암호화 (SEE) › 비밀번호 관리: 목록 · 검색 · 상태, 키 보기는 Windows 로그인 확인 뒤, 삭제 · 정리 · 모두 삭제.
        QTemporaryDir Dir;
        const auto Profile = Dir.filePath(QStringLiteral("profile"));
        const auto Alpha = Dir.filePath(QStringLiteral("alpha.db")), Beta = Dir.filePath(QStringLiteral("beta.db"));
        for (const auto& Path : {Alpha, Beta}) { QFile File(Path); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("x"); }
        {
            KeyStore Store(Profile);
            QVERIFY(Store.Save(Alpha, QByteArrayLiteral("test-only alpha"), KeyFormat::Text));
            QVERIFY(Store.Save(Beta, QByteArrayLiteral("test-only beta"), KeyFormat::Hex));
            QVERIFY(Store.Save(Dir.filePath(QStringLiteral("missing.db")), QByteArrayLiteral("k"), KeyFormat::Raw));
        }
        KeyManagerPage Page(Profile);
        Page.show();
        QCOMPARE(Page.List()->topLevelItemCount(), 3);
        QCOMPARE(Page.Title()->text(), QStringLiteral("저장한 키 3개"));
        if (const auto Shot = qEnvironmentVariable("HC_SQLITE_SHOT"); !Shot.isEmpty()) { Page.resize(640, 300); QTest::qWait(100); Page.grab().save(Shot + QStringLiteral("-keys.png")); }
        QCOMPARE(Page.List()->topLevelItem(0)->text(3), QStringLiteral("확인됨"));
        QCOMPARE(Page.List()->topLevelItem(2)->text(3), QStringLiteral("파일 없음"));
        Page.Search()->setText(QStringLiteral("beta"));
        QCOMPARE(Page.List()->topLevelItemCount(), 1);
        QCOMPARE(Page.List()->topLevelItem(0)->text(1), QStringLiteral("16진 (hexkey)"));
        Page.List()->topLevelItem(0)->setSelected(true);
        // Windows 로그인 확인이 실패하면 보이지 않는다.
        SetLoginCheck([](QWidget*, const QString&) { return false; });
        Page.RevealSelected();
        QVERIFY(!Page.findChild<QDialog*>(QStringLiteral("sqliteKeyRevealDialog")));
        SetLoginCheck([](QWidget*, const QString&) { return true; });
        Page.RevealSelected();
        auto* Reveal = Page.findChild<QDialog*>(QStringLiteral("sqliteKeyRevealDialog"));
        QVERIFY(Reveal);
        QCOMPARE(Reveal->findChild<QLineEdit*>(QStringLiteral("sqliteKeyRevealValue"))->text(), QStringLiteral("test-only beta"));
        Reveal->reject();
        QCOMPARE(Reveal->findChild<QLineEdit*>(QStringLiteral("sqliteKeyRevealValue"))->text(), QString());
        SetLoginCheck({});
        Page.RemoveSelected(false);
        Page.Search()->clear();
        QCOMPARE(Page.List()->topLevelItemCount(), 2);
        QCOMPARE(Page.RemoveMissing(), 1);
        QCOMPARE(Page.List()->topLevelItemCount(), 1);
        Page.RemoveAll(false);
        QCOMPARE(Page.List()->topLevelItemCount(), 0);
        QCOMPARE(Page.Title()->text(), QStringLiteral("저장한 키 0개"));
    }

    void schemaObjectsAndKeys()
    {
        QTemporaryDir Dir;
        auto Db = Create(Dir.filePath(QStringLiteral("schema.db")));
        QVERIFY(Db);
        QVERIFY(Run(*Db, QStringLiteral(
            "CREATE TABLE artists(ArtistId INTEGER PRIMARY KEY, Name NVARCHAR(120) NOT NULL);"
            "CREATE TABLE albums(AlbumId INTEGER PRIMARY KEY, Title TEXT, ArtistId INTEGER REFERENCES artists(ArtistId));"
            "CREATE TABLE pairs(a INTEGER, b TEXT, PRIMARY KEY(b, a)) WITHOUT ROWID;"
            "CREATE INDEX albums_artist ON albums(ArtistId);"
            "CREATE VIEW album_names AS SELECT Title FROM albums;"
            "CREATE TRIGGER albums_ai AFTER INSERT ON albums BEGIN SELECT 1; END;")));
        SchemaInfo Schema;
        QString Error;
        QVERIFY2(ReadSchema(*Db, Schema, &Error), qPrintable(Error));
        const auto* Main = Schema.Database(QStringLiteral("main"));
        QVERIFY(Main);
        QCOMPARE(Main->Objects(ObjectType::Table).size(), 3);
        QCOMPARE(Main->Objects(ObjectType::View).size(), 1);
        QCOMPARE(Main->Objects(ObjectType::Index).size(), 1);
        QCOMPARE(Main->Objects(ObjectType::Trigger).size(), 1);
        const auto* Albums = Main->Find(QStringLiteral("albums"));
        QVERIFY(Albums && Albums->HasRowid());
        QCOMPARE(Albums->columns_.size(), 3);
        QCOMPARE(Albums->columns_[2].foreignTable_, QStringLiteral("artists"));
        QCOMPARE(Albums->PrimaryKey(), QStringList{QStringLiteral("AlbumId")});
        const auto* Pairs = Main->Find(QStringLiteral("pairs"));
        QVERIFY(Pairs && Pairs->withoutRowid_ && !Pairs->HasRowid());
        QCOMPARE(Pairs->PrimaryKey(), (QStringList{QStringLiteral("b"), QStringLiteral("a")}));
        QVERIFY(Main->Find(QStringLiteral("artists"))->columns_[1].notNull_);
        QVERIFY(Schema.schemaVersion_ > 0);
        QCOMPARE(CountRows(*Db, QStringLiteral("main"), QStringLiteral("albums")), 0);
    }

    void tableModelKeysetSortAndFilter()
    {
        QTemporaryDir Dir;
        const auto Path = Dir.filePath(QStringLiteral("model.db"));
        auto Db = Create(Path);
        QVERIFY(Run(*Db, QStringLiteral(
            "CREATE TABLE tracks(TrackId INTEGER PRIMARY KEY, Name TEXT, Milliseconds INTEGER, Price NUMERIC);"
            "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 5000) "
            "INSERT INTO tracks SELECT i, 'track ' || i, i * 10, 0.99 FROM n;"
            "UPDATE tracks SET Milliseconds = 'oops' WHERE TrackId = 7;"
            "UPDATE tracks SET Name = NULL WHERE TrackId = 8;"
            "CREATE VIEW long_tracks AS SELECT TrackId, Name FROM tracks WHERE Milliseconds > 40000;")));
        SchemaInfo Schema;
        QVERIFY(ReadSchema(*Db, Schema, nullptr));
        const auto Opener = [Path](QString* Error) { return Connection::Open(Path, OpenOptions{}, Error); };
        TableModel Model(*Db, Opener);
        Model.SetSource(*Schema.Find(QStringLiteral("main"), QStringLiteral("tracks")));
        QVERIFY(Model.KeysetMode());
        QSignalSpy Loaded(&Model, &TableModel::Loaded);
        Model.Reload();
        QVERIFY(Loaded.wait(10000));
        QCOMPARE(Model.rowCount(), 5000);
        QCOMPARE(Model.columnCount(), 4);
        QCOMPARE(Model.index(4999, 0).data(ValueRole).toLongLong(), 5000);
        QCOMPARE(Model.index(2600, 1).data().toString(), QStringLiteral("track 2601"));
        QCOMPARE(Model.index(6, 2).data(MismatchRole).toBool(), true);
        QCOMPARE(Model.index(5, 2).data(MismatchRole).toBool(), false);
        QCOMPARE(Model.index(7, 1).data(NullRole).toBool(), true);
        QCOMPARE(Model.index(7, 1).data().toString(), QStringLiteral("NULL"));
        QCOMPARE(Model.index(0, 2).data(NumericRole).toBool(), true);
        QCOMPARE(Model.index(10, 0).data(RowIdRole).toLongLong(), 11);

        QSignalSpy Order(&Model, &TableModel::OrderChanged);
        Model.sort(0, Qt::DescendingOrder);
        QCOMPARE(Order.size(), 1);
        QVERIFY(Loaded.wait(10000));
        QCOMPARE(Model.index(0, 0).data(ValueRole).toLongLong(), 5000);

        Model.SetFilter(QStringLiteral("TrackId <= 100 AND TrackId % 2 = 0"), QStringLiteral("TrackId"));
        Model.Reload();
        QVERIFY(Loaded.wait(10000));
        QCOMPARE(Model.rowCount(), 50);
        QCOMPARE(Model.index(49, 0).data(ValueRole).toLongLong(), 100);
        QVERIFY(Model.GeneratedSql().contains(QStringLiteral("WHERE TrackId <= 100")));

        QSignalSpy Failed(&Model, &TableModel::Failed);
        Model.SetFilter(QStringLiteral("NoSuchColumn = 1"), {});
        Model.Reload();
        QVERIFY(Failed.wait(10000));
        QVERIFY(Model.LastError().contains(QStringLiteral("no such column")));

        // 뷰는 rowid 가 없어 행 수를 센 뒤 LIMIT/OFFSET 으로 읽는다.
        Model.SetSource(*Schema.Find(QStringLiteral("main"), QStringLiteral("long_tracks")));
        QVERIFY(!Model.KeysetMode());
        Model.Reload();
        QVERIFY(Loaded.wait(10000));
        // 4001–5000 의 1000행 + 7번 행: 'oops'(TEXT)는 SQLite 비교에서 모든 정수보다 크다.
        QCOMPARE(Model.rowCount(), 1001);
        QCOMPARE(Model.index(0, 0).data(ValueRole).toLongLong(), 7);
        QCOMPARE(Model.index(1000, 0).data(ValueRole).toLongLong(), 5000);
    }

    void documentViewOpensSchemaAndTables()
    {
        // 작업 창: 열기 → 스키마 트리 → 테이블 탭 → 그리드. HC_SQLITE_SHOT 이 있으면 화면을 PNG 로 남긴다(눈으로 확인용).
        QTemporaryDir Dir;
        const auto Path = Dir.filePath(QStringLiteral("chinook.db"));
        {
            auto Db = Create(Path);
            QVERIFY(Run(*Db, QStringLiteral(
                "PRAGMA journal_mode=WAL;"
                "CREATE TABLE artists(ArtistId INTEGER PRIMARY KEY, Name NVARCHAR(120));"
                "CREATE TABLE genres(GenreId INTEGER PRIMARY KEY, Name NVARCHAR(120));"
                "CREATE TABLE albums(AlbumId INTEGER PRIMARY KEY, Title NVARCHAR(160) NOT NULL, ArtistId INTEGER NOT NULL REFERENCES artists(ArtistId));"
                "CREATE TABLE tracks(TrackId INTEGER PRIMARY KEY, Name NVARCHAR(200) NOT NULL, AlbumId INTEGER REFERENCES albums(AlbumId),"
                " GenreId INTEGER REFERENCES genres(GenreId), Composer NVARCHAR(220), Milliseconds INTEGER NOT NULL, Bytes INTEGER, UnitPrice NUMERIC(10,2) NOT NULL);"
                "CREATE INDEX IFK_TrackAlbumId ON tracks(AlbumId);"
                "CREATE VIEW long_tracks AS SELECT TrackId, Name FROM tracks WHERE Milliseconds > 300000;"
                "CREATE TRIGGER tracks_ai AFTER INSERT ON tracks BEGIN SELECT 1; END;"
                "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 275) INSERT INTO artists SELECT i, 'Artist ' || i FROM n;"
                "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 25) INSERT INTO genres SELECT i, 'Genre ' || i FROM n;"
                "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 347) INSERT INTO albums SELECT i, 'Album ' || i, 1 + i % 275 FROM n;"
                "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 3503) INSERT INTO tracks "
                " SELECT i, 'Track ' || i, 1 + i % 347, 1 + i % 25, CASE WHEN i % 7 = 0 THEN NULL ELSE 'Composer ' || (i % 50) END, 180000 + i * 37, 5000000 + i * 101, 0.99 FROM n;"
                "UPDATE tracks SET Bytes = X'89504E470D0A1A0A' WHERE TrackId = 3;")));
        }
        DocumentView View(Path, DocumentView::Mode::Editor);
        View.resize(1320, 760);
        View.show();
        QSignalSpy State(&View, &DocumentView::StateChanged);
        View.Open();
        QVERIFY(View.IsReady());
        QVERIFY(View.Error().isEmpty());
        QCOMPARE(View.Schema().Database(QStringLiteral("main"))->Objects(ObjectType::Table).size(), 4);
        View.OpenObject(QStringLiteral("main"), QStringLiteral("tracks"));
        QCOMPARE(View.OpenTables(), QStringList{QStringLiteral("tracks")});
        auto* Model = View.CurrentModel();
        QVERIFY(Model);
        QSignalSpy Loaded(Model, &TableModel::Loaded);
        QVERIFY(Model->rowCount() == 3503 || Loaded.wait(10000));
        QCOMPARE(Model->rowCount(), 3503);
        QCOMPARE(Model->index(2, 6).data(StorageRole).toInt(), SQLITE_BLOB);
        // 같은 테이블을 다시 열면 새 탭을 만들지 않는다.
        View.OpenObject(QStringLiteral("main"), QStringLiteral("tracks"));
        QCOMPARE(View.OpenTables().size(), 1);
        bool HasRows = false;
        for (const auto& Item : View.StatusItems()) HasRows |= Item.id_ == QStringLiteral("sqliteRows") && Item.text_.contains(QStringLiteral("3,503"));
        QVERIFY(HasRows);
        QTest::qWait(400);  // 행 수 세기(작업 스레드)가 트리에 들어갈 시간
        if (const auto Shot = qEnvironmentVariable("HC_SQLITE_SHOT"); !Shot.isEmpty()) View.grab().save(Shot + QStringLiteral("-editor.png"));

        // 빠른 보기: 읽기 전용, 목록 + 데이터.
        DocumentView Preview(Path, DocumentView::Mode::QuickPreview);
        Preview.resize(900, 520);
        Preview.show();
        Preview.Open();
        QVERIFY(Preview.IsReady());
        QVERIFY(Preview.IsReadOnly());
        auto* PreviewModel = Preview.CurrentModel();
        QVERIFY(PreviewModel);
        QSignalSpy PreviewLoaded(PreviewModel, &TableModel::Loaded);
        QVERIFY(PreviewModel->rowCount() > 0 || PreviewLoaded.wait(10000));
        QTest::qWait(300);
        if (const auto Shot = qEnvironmentVariable("HC_SQLITE_SHOT"); !Shot.isEmpty()) Preview.grab().save(Shot + QStringLiteral("-preview.png"));
    }

    void documentViewRejectsOtherFiles()
    {
        QTemporaryDir Dir;
        const auto Path = Dir.filePath(QStringLiteral("Thumbs.db"));
        QFile File(Path);
        QVERIFY(File.open(QIODevice::WriteOnly));
        File.write(QByteArray::fromHex("D0CF11E0A1B11AE1") + QByteArray(504, '\0'));
        File.close();
        DocumentView View(Path, DocumentView::Mode::Editor);
        View.Open();
        QVERIFY(!View.IsReady());
        QVERIFY(View.Error().contains(QStringLiteral("SQLite 데이터베이스가 아닙니다")));
    }

    void tableModelCancelOnDestroy()
    {
        // 읽는 도중에 모델이 사라져도 작업 스레드의 결과가 사라진 모델로 가지 않는다.
        QTemporaryDir Dir;
        const auto Path = Dir.filePath(QStringLiteral("cancel.db"));
        auto Db = Create(Path);
        QVERIFY(Run(*Db, QStringLiteral("CREATE TABLE big(x);"
            "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 200000) INSERT INTO big SELECT i FROM n;")));
        SchemaInfo Schema;
        QVERIFY(ReadSchema(*Db, Schema, nullptr));
        const auto Opener = [Path](QString* Error) { return Connection::Open(Path, OpenOptions{}, Error); };
        for (int Round = 0; Round < 5; ++Round) {
            auto* Model = new TableModel(*Db, Opener);
            Model->SetSource(*Schema.Find(QStringLiteral("main"), QStringLiteral("big")));
            Model->Reload();
            delete Model;
        }
        QTest::qWait(500);
    }

    void splitStatementsAndGuards()
    {
        // 문 경계(설계 5.3): 문자열 · 주석 안의 ; 는 경계가 아니고, 트리거 본문은 END; 까지 한 문이다. 위치는 UTF-8 바이트.
        const QByteArray Sql = "SELECT 1;  SELECT ';' -- x ; y\n, 2;\n"
                               "CREATE TRIGGER t AFTER INSERT ON a BEGIN INSERT INTO b VALUES(1); UPDATE b SET x = 2; END;\n"
                               "/* ; */ SELECT '\xED\x95\x9C\xEA\xB8\x80'; SELECT 3";
        const auto Ranges = SplitStatements(Sql);
        QCOMPARE(Ranges.size(), 5);
        const auto Body = [&](int Index) { return QString::fromUtf8(Sql.mid(Ranges[Index].bodyStart_, Ranges[Index].bodyEnd_ - Ranges[Index].bodyStart_)); };
        QCOMPARE(Body(0), QStringLiteral("SELECT 1;"));
        QCOMPARE(Body(1), QStringLiteral("SELECT ';' -- x ; y\n, 2;"));
        QVERIFY(Body(2).startsWith(QStringLiteral("CREATE TRIGGER")));
        QVERIFY(Body(2).endsWith(QStringLiteral("END;")));
        QCOMPARE(Body(3), QString::fromUtf8("SELECT '\xED\x95\x9C\xEA\xB8\x80';"));
        QCOMPARE(Body(4), QStringLiteral("SELECT 3"));
        QCOMPARE(Ranges[4].bodyStart_, int(Sql.indexOf("SELECT 3")));
        // 커서가 문 사이 공백에 있으면 앞 문, 문 안이면 그 문.
        QCOMPARE(StatementAt(Ranges, 0), 0);
        QCOMPARE(StatementAt(Ranges, Ranges[0].end_ + 1), 0);
        QCOMPARE(StatementAt(Ranges, int(Sql.indexOf("UPDATE b"))), 2);
        QCOMPARE(StatementAt(Ranges, int(Sql.size())), 4);
        QVERIFY(SplitStatements("  ;  ; -- only comments\n").isEmpty());

        // WHERE 없는 UPDATE · DELETE 는 실행 전에 묻는다(설계 6.3). 주석 · 문자열 안의 WHERE 는 세지 않는다.
        QVERIFY(IsUnboundedWrite(QStringLiteral("UPDATE t SET x = 1")));
        QVERIFY(IsUnboundedWrite(QStringLiteral("  delete from t -- WHERE id = 1")));
        QVERIFY(IsUnboundedWrite(QStringLiteral("WITH c AS (SELECT 1) DELETE FROM t")));
        QVERIFY(IsUnboundedWrite(QStringLiteral("UPDATE t SET note = 'no WHERE here'")));
        QVERIFY(!IsUnboundedWrite(QStringLiteral("UPDATE t SET x = 1 WHERE id = 2")));
        QVERIFY(!IsUnboundedWrite(QStringLiteral("SELECT * FROM t")));
        QVERIFY(!IsUnboundedWrite(QStringLiteral("INSERT INTO t VALUES (1)")));
        const auto Words = Keywords();
        QVERIFY(Words.size() > 100);
        QVERIFY(Words.contains(QStringLiteral("select")));
        QCOMPARE(Summary(QStringLiteral("  -- note\n  SELECT   a,\n b FROM t"), 60), QStringLiteral("SELECT a, b FROM t"));
    }

    void queryRunnerResultsFetchErrorsAndInterrupt()
    {
        // 쿼리 탭 실행기(설계 5.3): 행 제한만큼 받고 나머지는 fetchMore, 오류는 편집기 위치로, 중지는 sqlite3_interrupt.
        QTemporaryDir Dir;
        const auto Path = Dir.filePath(QStringLiteral("runner.db"));
        {
            auto Db = Create(Path);
            QVERIFY(Run(*Db, QStringLiteral("CREATE TABLE t(x INTEGER); CREATE TABLE log(a);"
                "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 2500) INSERT INTO t SELECT i FROM n;")));
        }
        struct Message { int index_; QString error_; int offset_; qint64 changes_; };
        QList<int> Started;
        QList<std::tuple<int, int, QList<QueryModel::Column>, int, bool>> Results;
        QList<std::tuple<quint64, int, int, bool>> Fetched;
        QList<Message> Messages;
        QList<std::pair<bool, bool>> Finished;
        QObject Receiver;
        RunSink Sink;
        Sink.started_ = [&](int Index) { Started.append(Index); };
        Sink.result_ = [&](int Index, int Result, QList<QueryModel::Column> Columns, QVector<QVector<QVariant>> Rows, bool More, qint64) {
            Results.append({Index, Result, Columns, int(Rows.size()), More});
        };
        Sink.rows_ = [&](quint64 Batch, int Result, QVector<QVector<QVariant>> Rows, bool More) { Fetched.append({Batch, Result, int(Rows.size()), More}); };
        Sink.message_ = [&](int Index, QString Error, int Offset, qint64 Changes, qint64, bool) { Messages.append({Index, Error, Offset, Changes}); };
        Sink.finished_ = [&](bool Interrupted, bool Wrote, qint64) { Finished.append({Interrupted, Wrote}); };
        QueryRunner Runner([Path](QString* Error) { return Connection::Open(Path, OpenOptions{}, Error); }, &Receiver, Sink);

        Runner.Run(1, {{0, QStringLiteral("SELECT x, x * 2 AS twice FROM t ORDER BY x")}, {60, QStringLiteral("INSERT INTO log VALUES (1)")}}, 1000);
        QTRY_COMPARE(Finished.size(), 1);
        QCOMPARE(Started, (QList<int>{0, 1}));
        QCOMPARE(Results.size(), 1);
        const auto& [Index, Result, Columns, Count, More] = Results[0];
        QCOMPARE(Index, 0);
        QCOMPARE(Result, 0);
        QCOMPARE(Count, 1000);
        QVERIFY(More);
        QCOMPARE(Columns.size(), 2);
        QCOMPARE(Columns[0].name_, QStringLiteral("x"));
        QCOMPARE(Columns[0].declaredType_, QStringLiteral("INTEGER"));
        QCOMPARE(Columns[0].table_, QStringLiteral("t"));
        QVERIFY(Columns[1].table_.isEmpty());
        QCOMPARE(Messages.size(), 2);
        QVERIFY(Messages[0].error_.isEmpty());
        QCOMPARE(Messages[0].changes_, -1);
        QCOMPARE(Messages[1].changes_, 1);
        QCOMPARE(Finished[0], std::make_pair(false, true));
        // 나머지 1500행: 1000 + 500, 그다음은 끝.
        Runner.FetchMore(1, 0, 1000);
        Runner.FetchMore(1, 0, 1000);
        Runner.FetchMore(1, 0, 1000);
        QTRY_COMPARE(Fetched.size(), 3);
        QCOMPARE(Fetched[0], std::make_tuple(quint64(1), 0, 1000, true));
        QCOMPARE(Fetched[1], std::make_tuple(quint64(1), 0, 500, false));
        QCOMPARE(Fetched[2], std::make_tuple(quint64(1), 0, 0, false));

        // 오류는 그 문에서 멈추고, 위치는 문 시작 + sqlite3_error_offset.
        Messages.clear();
        Results.clear();
        Runner.Run(2, {{0, QStringLiteral("SELECT 1")}, {10, QStringLiteral("SELECT nope FROM t")}, {40, QStringLiteral("SELECT 2")}}, 100);
        QTRY_COMPARE(Finished.size(), 2);
        QCOMPARE(Finished[1], std::make_pair(false, false));
        QCOMPARE(Results.size(), 1);
        QCOMPARE(Messages.size(), 2);
        QCOMPARE(Messages[1].index_, 1);
        QVERIFY(Messages[1].error_.contains(QStringLiteral("nope")));
        QCOMPARE(Messages[1].offset_, 10 + 7);
        // 앞 실행의 결과는 새 실행이 닫았다.
        Runner.FetchMore(1, 0, 1000);
        QTRY_COMPARE(Fetched.size(), 4);
        QCOMPARE(std::get<2>(Fetched[3]), 0);

        // 끝나지 않는 문은 중지(Ctrl+.)로 멈춘다.
        Started.clear();
        Runner.Run(3, {{0, QStringLiteral("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n) SELECT count(*) FROM n")}}, 100);
        QTRY_COMPARE(Started.size(), 1);
        QVERIFY(Runner.Busy());
        for (int Round = 0; Round < 100 && Finished.size() < 3; ++Round) { Runner.Interrupt(); QTest::qWait(30); }
        QCOMPARE(Finished.size(), 3);
        QVERIFY(Finished[2].first);
        QVERIFY(!Runner.Busy());
        // 닫으면 연결을 놓는다.
        Runner.Release();
        QVERIFY(QFile::rename(Path, Path + QStringLiteral(".moved")));
    }

    void queryTabRunsInWorkWindow()
    {
        // 작업 창의 쿼리 탭(설계 5절): 명령 · 단축키 범위, 실행 · 결과 탭 · 메시지, 쓰기 뒤 표 탭 다시 읽기, 현재 문,
        // 오류 위치, 주석 전환, WHERE 없는 DELETE 확인, 스키마 바뀜, .sql 파일 탭, 스키마에서 SELECT.
        QTemporaryDir Dir;
        const auto Path = Dir.filePath(QStringLiteral("work.db"));
        const auto SqlPath = Dir.filePath(QStringLiteral("q.sql"));
        {
            auto Db = Create(Path);
            QVERIFY(Run(*Db, QStringLiteral("CREATE TABLE notes(id INTEGER PRIMARY KEY, body TEXT);"
                "INSERT INTO notes(body) VALUES('first'), ('second'), (NULL);")));
            QFile Sql(SqlPath);
            QVERIFY(Sql.open(QIODevice::WriteOnly));
            Sql.write("SELECT 1;\n");
        }
        {
            DocumentView View(Path, DocumentView::Mode::Editor);
            View.resize(1200, 720);
            View.show();
            View.Open();
            QVERIFY(View.IsReady());
            View.OpenObject(QStringLiteral("main"), QStringLiteral("notes"));
            QTRY_COMPARE(View.CurrentModel()->rowCount(), 3);
            QVERIFY(!View.CanRunCommand(QStringLiteral("sqlite.run")));
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.newQuery")));
            QCOMPARE(View.TabCount(), 2);
            auto* Query = qobject_cast<QueryTab*>(View.CurrentTab());
            QVERIFY(Query);
            QCOMPARE(Query->Title(), QStringLiteral("쿼리 1"));
            QVERIFY(View.CanRunCommand(QStringLiteral("sqlite.run")));

            // 단축키 범위: 편집기는 sqlite · sqlite.query, 스키마 트리는 sqlite · sqlite.schema.
            auto* Editor = Query->FocusTarget();
            auto* Tree = View.findChild<QTreeWidget*>(QStringLiteral("sqliteSchemaTree"));
            QVERIFY(Tree);
            QVERIFY(View.OwnsShortcutScope(QStringLiteral("sqlite"), Editor));
            QVERIFY(View.OwnsShortcutScope(QStringLiteral("sqlite.query"), Editor));
            QVERIFY(!View.OwnsShortcutScope(QStringLiteral("sqlite.grid"), Editor));
            QVERIFY(View.OwnsShortcutScope(QStringLiteral("sqlite.schema"), Tree));
            QVERIFY(!View.OwnsShortcutScope(QStringLiteral("sqlite.query"), Tree));
            QVERIFY(!View.OwnsShortcutScope(QStringLiteral("sqlite"), nullptr));

            // 실행: SELECT 두 개와 쓰기 하나. 결과 탭은 SELECT 마다, 메시지는 문마다.
            Query->SetText(QStringLiteral("SELECT id, body FROM notes ORDER BY id;\nUPDATE notes SET body = 'changed' WHERE id = 1;\nSELECT count(*) AS n FROM notes;\n"));
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.run")));
            QTRY_VERIFY(!Query->Running());
            auto* Grid = Query->CurrentGrid();
            QVERIFY(Grid);
            QCOMPARE(Grid->model()->rowCount(), 3);
            QCOMPARE(Grid->model()->index(0, 1).data(ValueRole).toString(), QStringLiteral("first"));
            auto* ResultTabs = Query->Result()->findChild<QTabBar*>();
            QCOMPARE(ResultTabs->count(), 3);
            auto* Messages = Query->Result()->findChild<QListWidget*>(QStringLiteral("sqliteMessages"));
            QCOMPARE(Messages->count(), 3);
            QVERIFY(Messages->item(1)->text().contains(QStringLiteral("1행 바뀜")));
            QVERIFY(View.OwnsShortcutScope(QStringLiteral("sqlite.grid"), Grid->viewport()));
            // 쓰기가 끝나면 열린 표 탭을 다시 읽는다.
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.prevTab")));
            QVERIFY(qobject_cast<TableTab*>(View.CurrentTab()));
            QTRY_COMPARE(View.CurrentModel()->index(0, 1).data(ValueRole).toString(), QStringLiteral("changed"));
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.nextTab")));
            QCOMPARE(View.CurrentTab(), static_cast<WorkTab*>(Query));

            // 현재 문만(Ctrl+Enter): 커서가 있는 셋째 문.
            const auto Utf8 = Query->Text().toUtf8();
            Query->Editor()->Send(SCI_GOTOPOS, Utf8.indexOf("SELECT count"));
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.runCurrent")));
            QTRY_VERIFY(!Query->Running());
            QCOMPARE(ResultTabs->count(), 2);
            QCOMPARE(Query->CurrentGrid()->model()->index(0, 0).data(ValueRole).toInt(), 3);

            // 오류: 메시지에 줄 · 열, 오류 위치로 이동.
            Query->SetText(QStringLiteral("SELECT 1;\nSELECT nope FROM notes;\n"));
            Query->RunAll();
            QTRY_VERIFY(!Query->Running());
            QVERIFY(Messages->item(Messages->count() - 1)->text().contains(QStringLiteral("2행 8열")));
            Query->GoToError();
            QCOMPARE(int(Query->Editor()->Send(SCI_GETCURRENTPOS)), int(Query->Text().toUtf8().indexOf("nope")));

            // 주석 전환(Ctrl+/).
            Query->SetText(QStringLiteral("SELECT 1;\n  SELECT 2;"));
            Query->Editor()->Send(SCI_SELECTALL);
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.comment")));
            QCOMPARE(Query->Text(), QStringLiteral("-- SELECT 1;\n  -- SELECT 2;"));
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.comment")));
            QCOMPARE(Query->Text(), QStringLiteral("SELECT 1;\n  SELECT 2;"));

            // 설정 › 실행: 행 제한은 새 탭부터, Ctrl+Enter 는 고른 글 먼저(끄면 현재 문 전체), 현재 문 막대 끄기.
            QueryPreferences Preferences;
            Preferences.rowLimit_ = 1500;
            Preferences.statementBar_ = false;
            View.ApplyQueryPreferences(Preferences);
            QCOMPARE(Query->RowLimit(), 1000);
            auto* Limited = View.NewQuery(QStringLiteral("SELECT 1;\nSELECT 2 + 40;"));
            QVERIFY(Limited);
            QCOMPARE(Limited->RowLimit(), 1500);
            QCOMPARE(Limited->Editor()->Send(SCI_MARKERNEXT, 0, 1 << 24), -1);
            const auto Part = Limited->Text().toUtf8().indexOf("SELECT 2");
            Limited->Editor()->Send(SCI_SETSEL, Part, Part + 8);
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.runCurrent")));
            QTRY_VERIFY(!Limited->Running() && Limited->CurrentGrid());
            QCOMPARE(Limited->CurrentGrid()->model()->index(0, 0).data(ValueRole).toInt(), 2);
            Preferences.selectionFirst_ = false;
            View.ApplyQueryPreferences(Preferences);
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.runCurrent")));
            QTRY_VERIFY(!Limited->Running() && Limited->CurrentGrid());
            QCOMPARE(Limited->CurrentGrid()->model()->index(0, 0).data(ValueRole).toInt(), 42);
            View.ApplyQueryPreferences(QueryPreferences{});

            // WHERE 없는 DELETE 는 묻는다. ‘아니요’면 실행하지 않는다.
            Query->SetText(QStringLiteral("DELETE FROM notes"));
            bool Asked = false;
            QTimer Closer;
            Closer.setInterval(20);
            connect(&Closer, &QTimer::timeout, this, [&] {
                if (auto* Box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) { Asked = true; Box->button(QMessageBox::No)->click(); }
            });
            Closer.start();
            Query->RunAll();
            Closer.stop();
            QVERIFY(Asked);
            QVERIFY(!Query->Running());
            {
                auto Check = Connection::Open(Path, OpenOptions{}, nullptr);
                Statement Count(*Check, QStringLiteral("SELECT count(*) FROM notes"));
                QVERIFY(Count.Step() == SQLITE_ROW);
                QCOMPARE(Count.Value(0).toInt(), 3);
            }

            // 스키마가 바뀌면 다시 읽는다.
            Query->SetText(QStringLiteral("CREATE TABLE extra(a);"));
            Query->RunAll();
            QTRY_VERIFY(View.Schema().Find(QStringLiteral("main"), QStringLiteral("extra")));

            // .sql 파일 탭: 이름, 수정 표시, 저장. 같은 파일은 같은 탭.
            auto* FileTab = View.NewQuery({}, SqlPath);
            QVERIFY(FileTab);
            QCOMPARE(FileTab->Title(), QStringLiteral("q.sql"));
            QVERIFY(!FileTab->Modified());
            FileTab->SetText(QStringLiteral("SELECT 2;\n"));
            QVERIFY(FileTab->Modified());
            auto* Bar = View.findChild<QTabBar*>(QStringLiteral("sqliteDocumentTabs"));
            QVERIFY(Bar->tabText(Bar->currentIndex()).endsWith(QStringLiteral("●")));
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.saveQuery")));
            QVERIFY(!FileTab->Modified());
            QCOMPARE(Bar->tabText(Bar->currentIndex()), QStringLiteral("q.sql"));
            {
                QFile Saved(SqlPath);
                QVERIFY(Saved.open(QIODevice::ReadOnly));
                QCOMPARE(Saved.readAll(), QByteArray("SELECT 2;\n"));
            }
            const int Count = View.TabCount();
            QCOMPARE(View.NewQuery({}, SqlPath), FileTab);
            QCOMPARE(View.TabCount(), Count);
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.closeTab")));
            QCOMPARE(View.TabCount(), Count - 1);

            // 스키마에서 SELECT(Ctrl+Enter): 새 쿼리 탭에 SELECT 를 쓰고 실행한다.
            QTreeWidgetItem* Notes = nullptr;
            for (QTreeWidgetItemIterator It(Tree); *It; ++It)
                if ((*It)->data(0, Qt::UserRole + 5).toString() == QStringLiteral("notes")) { Notes = *It; break; }
            QVERIFY(Notes);
            Tree->setCurrentItem(Notes);
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.schemaSelect")));
            auto* Selected = qobject_cast<QueryTab*>(View.CurrentTab());
            QVERIFY(Selected && Selected != Query);
            QCOMPARE(Selected->Text(), QStringLiteral("SELECT id, body\nFROM notes\nLIMIT 1000;\n"));
            QTRY_VERIFY(Selected->CurrentGrid() && !Selected->Running());
            QCOMPARE(Selected->CurrentGrid()->model()->rowCount(), 3);
            // 사람이 읽을 이름: 키워드 · 특수 문자만 감싼다.
            QCOMPARE(IdentifierText(QStringLiteral("notes")), QStringLiteral("notes"));
            QCOMPARE(IdentifierText(QStringLiteral("order")), QStringLiteral("\"order\""));
            QCOMPARE(IdentifierText(QStringLiteral("unit price")), QStringLiteral("\"unit price\""));
            QCOMPARE(IdentifierText(QStringLiteral("2nd")), QStringLiteral("\"2nd\""));
            // INSERT 문으로 복사(Ctrl+Shift+C): 결과 열이 한 테이블에서 왔으면 그 테이블 이름.
            auto* ResultGrid = Selected->CurrentGrid();
            ResultGrid->setCurrentIndex(ResultGrid->model()->index(0, 1));
            QVERIFY(View.OwnsShortcutScope(QStringLiteral("sqlite.grid"), ResultGrid));
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.copyInsert")));
            QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("INSERT INTO notes (id, body) VALUES (1, 'changed');"));
            // 설정 글꼴을 받기 전에도 편집기 글자가 보인다(2026-11-14: 스타일 크기 -1 로 줄 높이 5px 이던 결함).
            QVERIFY(Selected->Editor()->Send(SCI_TEXTHEIGHT, 0) >= 10);
            ViewFonts Fonts;
            Fonts.mono_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            Fonts.mono_.setPointSize(11);
            View.ApplyFonts(Fonts);
            QVERIFY(Selected->Editor()->Send(SCI_STYLEGETSIZE, STYLE_DEFAULT) == 11);
            if (const auto Shot = qEnvironmentVariable("HC_SQLITE_SHOT"); !Shot.isEmpty()) { QTest::qWait(300); View.grab().save(Shot + QStringLiteral("-query.png")); }
        }
        // 작업 창이 닫히면 쿼리 탭의 연결도 닫힌다.
        QVERIFY(QFile::rename(Path, Path + QStringLiteral(".moved")));
    }

    void historyAndTabRestore()
    {
        // 실행 기록 · 쿼리 탭 보관(설계 5.5): 데이터베이스 파일마다 프로필에, 새것이 위, 키를 넣는 문은 어디에도 남기지 않는다.
        QVERIFY(ProfileStore::Sensitive(QStringLiteral("PRAGMA key = 'secret'")));
        QVERIFY(ProfileStore::Sensitive(QStringLiteral("  pragma main.hexkey='00ff'")));
        QVERIFY(ProfileStore::Sensitive(QStringLiteral("PRAGMA textrekey = 'x'")));
        QVERIFY(ProfileStore::Sensitive(QStringLiteral("ATTACH 'file:b.db?hexkey=00ff' AS b")));
        QVERIFY(ProfileStore::Sensitive(QStringLiteral("ATTACH DATABASE 'b.db' AS b KEY 'secret'")));
        QVERIFY(!ProfileStore::Sensitive(QStringLiteral("PRAGMA journal_mode")));
        QVERIFY(!ProfileStore::Sensitive(QStringLiteral("SELECT keyword FROM t")));
        QCOMPARE(ProfileStore::WithoutSensitive(QStringLiteral("SELECT 1;\nPRAGMA key='x';\nSELECT 2;")), QStringLiteral("SELECT 1;\n\nSELECT 2;"));

        QTemporaryDir Dir, Profile;
        const auto Path = Dir.filePath(QStringLiteral("music.db"));
        {
            auto Db = Create(Path);
            QVERIFY(Run(*Db, QString::fromLatin1(MusicSchema)));
        }
        {
            DocumentView View(Path, DocumentView::Mode::Editor);
            View.SetProfileDirectory(Profile.path());
            View.show();
            View.Open();
            QVERIFY(View.IsReady());
            auto* Query = View.NewQuery(QStringLiteral("SELECT count(*) FROM tracks;\nSELECT 'file:x.db?key=secret';\nUPDATE albums SET Title = Title WHERE AlbumId = 1;"));
            Query->RunAll();
            QTRY_VERIFY(!Query->Running());
            const auto Saved = ProfileStore(Profile.path(), Path).LoadHistory();
            QCOMPARE(Saved.size(), 2);
            QVERIFY(Saved[0].sql_.startsWith(QStringLiteral("UPDATE albums")));
            QCOMPARE(Saved[0].changes_, 1);
            QCOMPARE(Saved[1].sql_, QStringLiteral("SELECT count(*) FROM tracks;"));
            QCOMPARE(Saved[1].rows_, 1);
            auto* List = View.findChild<QListWidget*>(QStringLiteral("sqliteHistoryList"));
            QVERIFY(List);
            QCOMPARE(List->count(), 2);
            QVERIFY(List->item(0)->text().contains(QStringLiteral("1행 바뀜")));
            // 같은 문을 다시 실행하면 겹치지 않고 맨 위로.
            Query->SetText(QStringLiteral("SELECT count(*) FROM tracks;"));
            Query->RunAll();
            QTRY_VERIFY(!Query->Running());
            QTRY_COMPARE(List->count(), 2);
            QCOMPARE(List->item(0)->data(Qt::UserRole + 1).toString(), QStringLiteral("SELECT count(*) FROM tracks;"));
            // 실행 계획은 기록하지 않는다.
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.explain")));
            QTRY_VERIFY(!Query->Running());
            QCOMPARE(ProfileStore(Profile.path(), Path).LoadHistory().size(), 2);
            // 기록에서 고르면 지금 쿼리 탭의 커서 자리에(줄 중간이면 새 줄로) 넣는다.
            Query->SetText(QStringLiteral("SELECT 9"));
            Query->Editor()->Send(SCI_DOCUMENTEND);
            emit List->itemActivated(List->item(1));
            QCOMPARE(Query->Text(), QStringLiteral("SELECT 9\nUPDATE albums SET Title = Title WHERE AlbumId = 1;"));

            // 보관할 탭: 키 문이 든 이름 없는 쿼리, 테이블 탭.
            Query->SetText(QStringLiteral("SELECT 2;\nPRAGMA key='secret';"));
            Query->SetCaret(3);
            View.OpenObject(QStringLiteral("main"), QStringLiteral("albums"));
            View.NewQuery(QStringLiteral("SELECT 3;"));
            QCOMPARE(View.TabCount(), 3);
        }
        {
            QFile Tabs(ProfileStore(Profile.path(), Path).TabsFile());
            QVERIFY(Tabs.open(QIODevice::ReadOnly));
            QVERIFY(!Tabs.readAll().contains("secret"));
        }
        {
            DocumentView View(Path, DocumentView::Mode::Editor);
            View.SetProfileDirectory(Profile.path());
            View.show();
            View.Open();
            QCOMPARE(View.TabCount(), 3);
            QCOMPARE(View.OpenTables(), QStringList{QStringLiteral("albums")});
            QVERIFY(qobject_cast<QueryTab*>(View.CurrentTab()));
            QCOMPARE(static_cast<QueryTab*>(View.CurrentTab())->Text(), QStringLiteral("SELECT 3;"));
            View.RunCommand(QStringLiteral("sqlite.prevTab"));
            View.RunCommand(QStringLiteral("sqlite.prevTab"));
            auto* Restored = qobject_cast<QueryTab*>(View.CurrentTab());
            QVERIFY(Restored);
            QCOMPARE(Restored->Text(), QStringLiteral("SELECT 2;\n"));
            QCOMPARE(Restored->Title(), QStringLiteral("쿼리 1"));
            QCOMPARE(Restored->Caret(), 3);
            // 다음 새 쿼리는 이름이 겹치지 않는다.
            QCOMPARE(View.NewQuery()->Title(), QStringLiteral("쿼리 3"));
            QCOMPARE(View.findChild<QListWidget*>(QStringLiteral("sqliteHistoryList"))->count(), 2);
        }
        // 다른 프로필(빈 폴더)은 아무것도 되살리지 않는다.
        QTemporaryDir Empty;
        DocumentView Fresh(Path, DocumentView::Mode::Editor);
        Fresh.SetProfileDirectory(Empty.path());
        Fresh.Open();
        QCOMPARE(Fresh.TabCount(), 0);
    }

    void tableEditsCommitAndConflicts()
    {
        // 보류 편집(설계 6.2): 파일에 쓰지 않고 쌓다가 커밋에서 한 트랜잭션으로. rowid 로 묶어 정렬을 바꿔도 유지,
        // 다른 곳에서 바뀐 칸은 충돌로 되돌린다. WITHOUT ROWID 는 기본 키로 찾고, 뷰는 읽기만.
        QTemporaryDir Dir;
        const auto Path = Dir.filePath(QStringLiteral("edit.db"));
        {
            auto Db = Create(Path);
            QVERIFY(Run(*Db, QStringLiteral("CREATE TABLE notes(id INTEGER PRIMARY KEY, body TEXT, n INTEGER DEFAULT 5);"
                "INSERT INTO notes(body, n) VALUES ('first', 1), ('second', 2), ('third', 3);"
                "CREATE TABLE kv(k TEXT PRIMARY KEY, v) WITHOUT ROWID; INSERT INTO kv VALUES ('a', 1), ('b', 2);"
                "CREATE VIEW v_notes AS SELECT id, body FROM notes;")));
        }
        const auto Read = [&](const QString& Sql) {
            auto Db = Connection::Open(Path, OpenOptions{}, nullptr);
            Statement Query(*Db, Sql);
            return Query.Step() == SQLITE_ROW ? Query.Value(0) : QVariant();
        };
        DocumentView View(Path, DocumentView::Mode::Editor);
        View.resize(1200, 720);
        View.show();
        View.Open();
        QVERIFY(View.IsReady());
        View.OpenObject(QStringLiteral("main"), QStringLiteral("notes"));
        auto* Table = qobject_cast<TableTab*>(View.CurrentTab());
        QVERIFY(Table);
        Table->SetEditing(false);
        auto* Model = Table->Model();
        QTRY_COMPARE(Model->rowCount(), 3);
        QVERIFY(Model->Editable());
        QVERIFY(Model->flags(Model->index(0, 1)) & Qt::ItemIsEditable);

        // 고치기: 수정 칸 표시, 원래 값으로 되돌리면 보류가 사라진다.
        QVERIFY(Model->setData(Model->index(0, 1), QStringLiteral("changed")));
        QCOMPARE(Model->index(0, 1).data(PendingRole).toInt(), int(TableModel::PendingState::Modified));
        QCOMPARE(Model->index(0, 1).data(ValueRole).toString(), QStringLiteral("changed"));
        QCOMPARE(Model->Pending().modified_, 1);
        QVERIFY(Model->setData(Model->index(0, 1), QStringLiteral("first")));
        QVERIFY(!Model->HasPending());
        QVERIFY(Model->setData(Model->index(0, 1), QStringLiteral("changed")));
        // 정수 열의 글은 정수로.
        QVERIFY(Model->setData(Model->index(1, 2), QStringLiteral("42")));
        QCOMPARE(Model->index(1, 2).data(ValueRole).typeId(), int(QMetaType::LongLong));
        // 새 행: rowid 별칭은 ‘자동’, 기본값이 있는 칸은 그 식.
        QVERIFY(View.RunCommand(QStringLiteral("sqlite.addRow")));
        QCOMPARE(Model->rowCount(), 4);
        QCOMPARE(Model->index(3, 0).data(Qt::DisplayRole).toString(), QStringLiteral("자동"));
        QCOMPARE(Model->index(3, 2).data(Qt::DisplayRole).toString(), QStringLiteral("5"));
        QCOMPARE(Model->headerData(3, Qt::Vertical).toString(), QStringLiteral("+"));
        QVERIFY(Model->setData(Model->index(3, 1), QStringLiteral("new")));
        // 지우기 표시.
        Table->Grid()->setCurrentIndex(Model->index(2, 1));
        QVERIFY(View.RunCommand(QStringLiteral("sqlite.deleteRows")));
        QCOMPARE(Model->index(2, 1).data(PendingRole).toInt(), int(TableModel::PendingState::Deleted));
        QVERIFY(!(Model->flags(Model->index(2, 1)) & Qt::ItemIsEditable));
        QCOMPARE(Model->Pending().Total(), 4);
        bool Warned = false;
        for (const auto& Item : View.StatusItems()) Warned |= Item.id_ == QStringLiteral("sqlitePending") && Item.warning_ && Item.text_.contains(QStringLiteral("4건"));
        QVERIFY(Warned);
        // 정렬을 바꿔도 rowid 로 묶여 남는다.
        Model->sort(1, Qt::DescendingOrder);
        QTRY_VERIFY(!Model->Loading());
        bool Kept = false;
        for (int Row = 0; Row < Model->rowCount(); ++Row)
            Kept |= Model->index(Row, 0).data(ValueRole).toLongLong() == 1 && Model->index(Row, 1).data(ValueRole).toString() == QStringLiteral("changed");
        QVERIFY(Kept);
        QCOMPARE(Model->Pending().Total(), 4);
        const auto Statements = Model->PendingStatements();
        QCOMPARE(Statements.first(), QStringLiteral("BEGIN IMMEDIATE;"));
        QVERIFY(Statements.contains(QStringLiteral("UPDATE notes SET body = 'changed' WHERE rowid = 1 AND body IS 'first';")));
        QVERIFY(Statements.contains(QStringLiteral("DELETE FROM notes WHERE rowid = 3;")));
        QVERIFY(Statements.contains(QStringLiteral("INSERT INTO notes (body) VALUES ('new');")));
        if (const auto Shot = qEnvironmentVariable("HC_SQLITE_SHOT"); !Shot.isEmpty()) {
            Table->ToggleValuePanel();
            Table->Grid()->setCurrentIndex(Model->index(0, 1));
            QTest::qWait(200);
            View.grab().save(Shot + QStringLiteral("-pending.png"));
            Table->ToggleValuePanel();
        }
        // 아직 파일에는 쓰지 않았다.
        QCOMPARE(Read(QStringLiteral("SELECT body FROM notes WHERE id = 1")).toString(), QStringLiteral("first"));

        // Ctrl+S(작업 창의 커밋): 한 트랜잭션으로 쓰고 다시 읽는다.
        QVERIFY(View.RunCommand(QStringLiteral("sqlite.commit")));
        QVERIFY(!View.HasPending());
        QCOMPARE(Read(QStringLiteral("SELECT body FROM notes WHERE id = 1")).toString(), QStringLiteral("changed"));
        QCOMPARE(Read(QStringLiteral("SELECT n FROM notes WHERE id = 2")).toLongLong(), 42);
        // 새 행은 지운 행의 rowid(3)를 다시 받을 수 있다. 지운 행은 내용으로 본다.
        QCOMPARE(Read(QStringLiteral("SELECT count(*) FROM notes WHERE body = 'third'")).toLongLong(), 0);
        QCOMPARE(Read(QStringLiteral("SELECT n FROM notes WHERE body = 'new'")).toLongLong(), 5);
        QTRY_COMPARE(Model->rowCount(), 3);

        // 충돌: 다른 연결이 그 사이에 같은 칸을 바꿨으면 쓰지 않고 칸을 표시한다. 보류는 남는다.
        int Row = -1;
        for (int Index = 0; Index < Model->rowCount(); ++Index) if (Model->index(Index, 0).data(ValueRole).toLongLong() == 2) Row = Index;
        QVERIFY(Row >= 0);
        QVERIFY(Model->setData(Model->index(Row, 1), QStringLiteral("local")));
        {
            auto Other = Connection::Open(Path, OpenOptions{}, nullptr);
            QVERIFY(Run(*Other, QStringLiteral("UPDATE notes SET body = 'remote' WHERE id = 2")));
        }
        QString Error;
        QVERIFY(!Table->Commit(&Error));
        QVERIFY(Error.contains(QStringLiteral("1개")));
        QCOMPARE(Model->ConflictCount(), 1);
        QVERIFY(Model->index(Row, 1).data(ConflictRole).toBool());
        QCOMPARE(Read(QStringLiteral("SELECT body FROM notes WHERE id = 2")).toString(), QStringLiteral("remote"));
        QVERIFY(Model->HasPending());
        Table->RevertAll();
        QVERIFY(!Model->HasPending());
        QCOMPARE(Model->ConflictCount(), 0);

        // NULL 넣기(Ctrl+Shift+N)와 값 패널.
        Table->Grid()->setCurrentIndex(Model->index(0, 1));
        Table->Grid()->selectionModel()->select(Model->index(0, 1), QItemSelectionModel::ClearAndSelect);
        QVERIFY(View.RunCommand(QStringLiteral("sqlite.setNull")));
        QVERIFY(Model->index(0, 1).data(NullRole).toBool());
        QVERIFY(View.RunCommand(QStringLiteral("sqlite.valuePanel")));
        QVERIFY(Table->ValuePanelVisible());
        auto* Panel = Table->Panel();
        Panel->SetCell(Model->index(0, 1));
        Panel->Editor()->setPlainText(QStringLiteral("{\"a\": [1, 2]}"));
        QTest::keyClick(Panel->Editor(), Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(Model->index(0, 1).data(ValueRole).toString(), QStringLiteral("{\"a\": [1, 2]}"));
        Panel->findChild<fm::ui::SegmentedControl*>(QStringLiteral("sqliteValueModes"))->setCurrentIndex(1);
        QVERIFY(Panel->Editor()->toPlainText().contains(QStringLiteral("\n")));
        Table->RevertAll();

        // WITHOUT ROWID: 기본 키로 찾아 고친다.
        View.OpenObject(QStringLiteral("main"), QStringLiteral("kv"));
        auto* Pairs = qobject_cast<TableTab*>(View.CurrentTab());
        QVERIFY(Pairs);
        QTRY_COMPARE(Pairs->Model()->rowCount(), 2);
        QVERIFY(Pairs->Model()->Editable());
        QVERIFY(Pairs->Model()->setData(Pairs->Model()->index(1, 1), QStringLiteral("9")));
        QVERIFY(Pairs->Model()->PendingStatements().contains(QStringLiteral("UPDATE kv SET v = '9' WHERE k IS 'b' AND v IS 2;")));
        QVERIFY(View.Commit());
        QCOMPARE(Read(QStringLiteral("SELECT v FROM kv WHERE k = 'b'")).toLongLong(), 9);

        // 뷰는 읽기만.
        View.OpenObject(QStringLiteral("main"), QStringLiteral("v_notes"));
        auto* Notes = qobject_cast<TableTab*>(View.CurrentTab());
        QTRY_VERIFY(Notes->Model()->rowCount() > 0);
        QVERIFY(!Notes->Model()->Editable());
        QVERIFY(!(Notes->Model()->flags(Notes->Model()->index(0, 1)) & Qt::ItemIsEditable));
        QVERIFY(!View.CanRunCommand(QStringLiteral("sqlite.addRow")));

        // 닫을 때 커밋하지 않은 편집이 있으면 묻는다(‘버리기’).
        View.OpenObject(QStringLiteral("main"), QStringLiteral("notes"));
        QVERIFY(Model->setData(Model->index(0, 2), QStringLiteral("77")));
        QTimer Closer;
        Closer.setInterval(20);
        bool Asked = false;
        connect(&Closer, &QTimer::timeout, this, [&] {
            if (auto* Box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                for (auto* Button : Box->buttons()) if (Button->text() == QStringLiteral("버리기")) { Asked = true; Button->click(); }
        });
        Closer.start();
        QVERIFY(View.RequestClose());
        Closer.stop();
        QVERIFY(Asked);
        QVERIFY(!View.HasPending());
    }

    void manualCommitMode()
    {
        // 수동 커밋(설계 6.3): 첫 쓰기 문 앞에서 BEGIN, Ctrl+S 가 커밋, ‘롤백’ 이 버린다. VACUUM 은 먼저 커밋할지 묻는다.
        QTemporaryDir Dir;
        const auto Path = Dir.filePath(QStringLiteral("manual.db"));
        {
            auto Db = Create(Path);
            QVERIFY(Run(*Db, QStringLiteral("PRAGMA journal_mode=WAL; CREATE TABLE t(id INTEGER PRIMARY KEY, n INTEGER); INSERT INTO t VALUES (1, 1);")));
        }
        const auto ReadN = [&] {
            auto Db = Connection::Open(Path, OpenOptions{}, nullptr);
            Statement Query(*Db, QStringLiteral("SELECT n FROM t WHERE id = 1"));
            return Query.Step() == SQLITE_ROW ? Query.Value(0).toLongLong() : -1;
        };
        DocumentView View(Path, DocumentView::Mode::Editor);
        View.show();
        View.Open();
        auto* Query = View.NewQuery();
        QVERIFY(!Query->ManualCommit());
        QVERIFY(View.RunCommand(QStringLiteral("sqlite.toggleCommitMode")));
        QVERIFY(Query->ManualCommit());
        Query->SetText(QStringLiteral("UPDATE t SET n = 7 WHERE id = 1;"));
        Query->RunAll();
        QTRY_VERIFY(!Query->Running());
        QTRY_VERIFY(Query->InTransaction());
        QCOMPARE(ReadN(), 1);
        bool Open = false;
        for (const auto& Item : View.StatusItems()) Open |= Item.id_ == QStringLiteral("sqliteTransaction") && Item.warning_;
        QVERIFY(Open);
        QVERIFY(View.HasPending());
        QVERIFY(View.Commit());
        QTRY_VERIFY(!Query->InTransaction());
        QCOMPARE(ReadN(), 7);

        // 롤백.
        Query->SetText(QStringLiteral("UPDATE t SET n = 9 WHERE id = 1;"));
        Query->RunAll();
        QTRY_VERIFY(Query->InTransaction() && !Query->Running());
        QTimer Closer;
        Closer.setInterval(20);
        connect(&Closer, &QTimer::timeout, this, [&] {
            if (auto* Box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) Box->button(QMessageBox::Yes)->click();
        });
        Closer.start();
        QVERIFY(View.RunCommand(QStringLiteral("sqlite.rollback")));
        QTRY_VERIFY(!Query->InTransaction());
        QCOMPARE(ReadN(), 7);

        // VACUUM 은 트랜잭션 안에서 못 돈다: 먼저 커밋하고 실행한다.
        Query->SetText(QStringLiteral("UPDATE t SET n = 8 WHERE id = 1;"));
        Query->RunAll();
        QTRY_VERIFY(Query->InTransaction() && !Query->Running());
        Query->SetText(QStringLiteral("VACUUM;"));
        Query->RunAll();
        Closer.stop();
        QTRY_VERIFY(!Query->Running());
        QVERIFY(!Query->InTransaction());
        QCOMPARE(ReadN(), 8);
        auto* Messages = Query->Result()->findChild<QListWidget*>(QStringLiteral("sqliteMessages"));
        QVERIFY(!Messages->item(Messages->count() - 1)->text().contains(QStringLiteral("cannot")));

        // 읽기 전용 문서의 실행기는 쓰기 문을 실행 전에 막는다.
        QString LastError;
        QObject Receiver;
        RunSink Sink;
        Sink.message_ = [&](int, QString Error, int, qint64, qint64, bool) { if (!Error.isEmpty()) LastError = Error; };
        bool Done = false;
        Sink.finished_ = [&](bool, bool, qint64) { Done = true; };
        QueryRunner Runner([Path](QString* Error) { return Connection::Open(Path, OpenOptions{}, Error); }, &Receiver, Sink);
        Runner.Run(1, {{0, QStringLiteral("UPDATE t SET n = 0")}}, 10, QueryRunner::Options{false, false});
        QTRY_VERIFY(Done);
        QVERIFY(LastError.contains(QStringLiteral("읽기 전용")));
        QCOMPARE(ReadN(), 8);
    }

    void tableDesignerAndMaintenance()
    {
        // 구조 바꾸기(설계 7절): ALTER 로 되는 것은 ALTER, 그 밖은 12단계 재작성(원래 열 정의 · 표 제약 · 색인 · 트리거 보존,
        // 외래 키가 어긋나면 전체 되돌림), 재작성 전 사본. 데이터베이스 정보의 유지 관리.
        QTemporaryDir Dir;
        const auto Path = Dir.filePath(QStringLiteral("design.db"));
        {
            auto Db = Create(Path);
            QVERIFY(Run(*Db, QStringLiteral(
                "CREATE TABLE artists(ArtistId INTEGER PRIMARY KEY, Name NVARCHAR(120));"
                "CREATE TABLE albums(AlbumId INTEGER PRIMARY KEY AUTOINCREMENT, Title TEXT NOT NULL COLLATE NOCASE, ArtistId INTEGER REFERENCES artists(ArtistId),"
                " CHECK (length(Title) > 0));"
                "CREATE INDEX albums_artist ON albums(ArtistId);"
                "CREATE TRIGGER albums_touch AFTER UPDATE ON albums BEGIN SELECT 1; END;"
                "CREATE VIEW v_albums AS SELECT Title FROM albums;"
                "INSERT INTO artists VALUES (1, 'A'), (2, 'B');"
                "INSERT INTO albums(Title, ArtistId) VALUES ('Rock', 1), ('Jazz', 2), ('Pop', 1);"
                "CREATE TABLE tracks(TrackId INTEGER PRIMARY KEY, AlbumId INTEGER REFERENCES albums(AlbumId));"
                "INSERT INTO tracks VALUES (1, 1);")));
        }
        auto Db = Connection::Open(Path, OpenOptions{}, nullptr);
        SchemaInfo Schema;
        QVERIFY(ReadSchema(*Db, Schema, nullptr));
        const auto* Albums = Schema.Find(QStringLiteral("main"), QStringLiteral("albums"));
        QVERIFY(Albums);
        auto Design = DesignFrom(*Albums);
        QCOMPARE(Design.columns_.size(), 3);
        QCOMPARE(Design.tableConstraints_, QStringList{QStringLiteral("CHECK (length(Title) > 0)")});
        QVERIFY(Design.columns_[1].originalDefinition_.contains(QStringLiteral("COLLATE NOCASE")));
        QCOMPARE(Design.columns_[2].references_, QStringLiteral("artists(ArtistId)"));

        // 이름 바꾸기 + 끝에 열 추가: ALTER TABLE.
        auto Altered = Design;
        Altered.columns_[1].name_ = QStringLiteral("Heading");
        DesignColumn Year;
        Year.name_ = QStringLiteral("Year");
        Year.type_ = QStringLiteral("INTEGER");
        Altered.columns_.append(Year);
        auto Plan = PlanDesign(Altered, Schema, QStringLiteral("backup.db"));
        QVERIFY(Plan.changed_);
        QVERIFY(!Plan.rebuild_);
        QVERIFY(Plan.statements_.contains(QStringLiteral("ALTER TABLE \"albums\" RENAME COLUMN \"Title\" TO \"Heading\"")));
        QVERIFY(Plan.statements_.contains(QStringLiteral("ALTER TABLE \"albums\" ADD COLUMN \"Year\" INTEGER")));
        QVERIFY(!Plan.statements_.join(QLatin1Char('\n')).contains(QStringLiteral("VACUUM INTO")));

        // 형식 바꾸기: 재작성. 바뀌지 않은 열은 원래 정의(AUTOINCREMENT · COLLATE) 그대로.
        auto Rebuilt = Design;
        Rebuilt.columns_[2].type_ = QStringLiteral("TEXT");
        Plan = PlanDesign(Rebuilt, Schema, Dir.filePath(QStringLiteral("albums.bak.db")));
        QVERIFY(Plan.rebuild_);
        const auto Script = Plan.statements_.join(QStringLiteral(";\n"));
        QVERIFY(Plan.statements_.first().startsWith(QStringLiteral("VACUUM INTO")));
        QVERIFY(Script.contains(QStringLiteral("\"AlbumId\" INTEGER PRIMARY KEY AUTOINCREMENT")));
        QVERIFY(Script.contains(QStringLiteral("\"Title\" TEXT NOT NULL COLLATE NOCASE")));
        QVERIFY(Script.contains(QStringLiteral("\"ArtistId\" TEXT REFERENCES \"artists\" (\"ArtistId\")")));
        QVERIFY(Script.contains(QStringLiteral("CHECK (length(Title) > 0)")));
        QVERIFY(Script.contains(QStringLiteral("CREATE INDEX albums_artist ON albums(ArtistId)")));
        QVERIFY(Script.contains(QStringLiteral("PRAGMA foreign_key_check")));
        QVERIFY(Plan.summary_.contains(QStringLiteral("v_albums")));

        // 창으로 적용: ArtistId 형식을 TEXT 로.
        {
            TableDesigner Designer(*Db, Schema, Albums, QStringLiteral("main"), Dir.filePath(QStringLiteral("albums.bak.db")));
            auto* Grid = Designer.Columns();
            QCOMPARE(Grid->rowCount(), 3);
            Grid->item(2, 1)->setText(QStringLiteral("TEXT"));
            QVERIFY(Designer.Plan().rebuild_);
            QString Error;
            QVERIFY2(Designer.Apply(&Error), qPrintable(Error));
        }
        const auto Value = [&](const QString& Sql) { Statement Query(*Db, Sql); return Query.Step() == SQLITE_ROW ? Query.Value(0) : QVariant(); };
        QCOMPARE(Value(QStringLiteral("SELECT type FROM pragma_table_info('albums') WHERE name = 'ArtistId'")).toString(), QStringLiteral("TEXT"));
        QCOMPARE(Value(QStringLiteral("SELECT count(*) FROM albums")).toLongLong(), 3);
        QCOMPARE(Value(QStringLiteral("SELECT count(*) FROM sqlite_schema WHERE name IN ('albums_artist', 'albums_touch')")).toLongLong(), 2);
        QVERIFY(Value(QStringLiteral("SELECT sql FROM sqlite_schema WHERE name = 'albums'")).toString().contains(QStringLiteral("COLLATE NOCASE")));
        QVERIFY(QFile::exists(Dir.filePath(QStringLiteral("albums.bak.db"))));
        QCOMPARE(Value(QStringLiteral("PRAGMA foreign_keys")).toInt(), 0);

        // 외래 키가 어긋나면 되돌린다(tracks 에 없는 앨범을 가리키는 행).
        QVERIFY(Run(*Db, QStringLiteral("INSERT INTO tracks VALUES (2, 999)")));
        QVERIFY(ReadSchema(*Db, Schema, nullptr));
        {
            TableDesigner Designer(*Db, Schema, Schema.Find(QStringLiteral("main"), QStringLiteral("tracks")), QStringLiteral("main"), QString());
            Designer.Columns()->item(1, 1)->setText(QStringLiteral("TEXT"));
            QString Error;
            QVERIFY(!Designer.Apply(&Error));
            QVERIFY(Error.contains(QStringLiteral("외래 키")));
        }
        QCOMPARE(Value(QStringLiteral("SELECT type FROM pragma_table_info('tracks') WHERE name = 'AlbumId'")).toString(), QStringLiteral("INTEGER"));
        QCOMPARE(Value(QStringLiteral("SELECT count(*) FROM sqlite_schema WHERE name LIKE 'tracks_new%'")).toLongLong(), 0);

        // 새 테이블.
        {
            TableDesigner Designer(*Db, Schema, nullptr, QStringLiteral("main"), QString());
            Designer.NameEdit()->setText(QStringLiteral("fresh"));
            QVERIFY(Designer.Plan().statements_.first().startsWith(QStringLiteral("CREATE TABLE \"fresh\" (\n    \"id\" INTEGER PRIMARY KEY")));
            QString Error;
            QVERIFY2(Designer.Apply(&Error), qPrintable(Error));
        }
        QCOMPARE(Value(QStringLiteral("SELECT count(*) FROM sqlite_schema WHERE name = 'fresh'")).toLongLong(), 1);

        // 데이터베이스 정보 · 유지 관리.
        {
            DatabaseInfoDialog Info(*Db, [Path](QString* Error) { return Connection::Open(Path, OpenOptions{}, Error); }, Path, [] { return true; }, false);
            QCOMPARE(Info.findChild<QLabel*>(QStringLiteral("sqliteInfo.인코딩"))->text(), QStringLiteral("UTF-8"));
            QVERIFY(Info.RunMaintenance(QStringLiteral("integrity"), true).contains(QStringLiteral("— ok")));
            QVERIFY(Info.RunMaintenance(QStringLiteral("foreignKeys"), true).contains(QStringLiteral("어긋난 행 1개")));
            QVERIFY(Info.RunMaintenance(QStringLiteral("checkpoint"), true).contains(QStringLiteral("WAL 모드가 아닙니다")));
            QVERIFY(Info.RunMaintenance(QStringLiteral("analyze"), true).contains(QStringLiteral("끝냈습니다")));
            const auto Vacuumed = Info.RunMaintenance(QStringLiteral("vacuum"), true);
            QVERIFY2(Vacuumed.contains(QStringLiteral("VACUUM")) && Vacuumed.startsWith(QStringLiteral("✓")), qPrintable(Vacuumed));
        }
        Db.reset();

        // 작업 창의 이름 바꾸기(F2) · 삭제(Delete).
        DocumentView View(Path, DocumentView::Mode::Editor);
        View.show();
        View.Open();
        QVERIFY(View.IsReady());
        auto* Tree = View.findChild<QTreeWidget*>(QStringLiteral("sqliteSchemaTree"));
        const auto Select = [&](const QString& Name) {
            for (QTreeWidgetItemIterator It(Tree); *It; ++It)
                if ((*It)->data(0, Qt::UserRole + 5).toString() == Name && (*It)->data(0, Qt::UserRole + 3).toString() == QStringLiteral("table")) { Tree->setCurrentItem(*It); return true; }
            return false;
        };
        QTimer Closer;
        Closer.setInterval(20);
        connect(&Closer, &QTimer::timeout, this, [&] {
            if (auto* Input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) { Input->setTextValue(QStringLiteral("renamed")); Input->accept(); }
            else if (auto* Box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) Box->button(QMessageBox::Yes)->click();
        });
        Closer.start();
        QVERIFY(Select(QStringLiteral("fresh")));
        QVERIFY(View.RunCommand(QStringLiteral("sqlite.renameObject")));
        QVERIFY(View.Schema().Find(QStringLiteral("main"), QStringLiteral("renamed")));
        QVERIFY(Select(QStringLiteral("renamed")));
        View.OpenObject(QStringLiteral("main"), QStringLiteral("renamed"));
        QVERIFY(Select(QStringLiteral("renamed")));
        QVERIFY(View.RunCommand(QStringLiteral("sqlite.dropObject")));
        Closer.stop();
        QVERIFY(!View.Schema().Find(QStringLiteral("main"), QStringLiteral("renamed")));
        // 지운 테이블의 탭은 닫혔다.
        QVERIFY(!View.OpenTables().contains(QStringLiteral("renamed")));
    }

    void completionMatchRules()
    {
        // 접두어, 그다음 대문자 머리 글자(설계 5.2: mi → Milliseconds · MediaTypeId 의 M…I). 흩어진 글자에는 붙지 않는다.
        using namespace hc::document::completion;
        int Prefix = 0, Initials = 0;
        QVector<int> At;
        QVERIFY(InitialsMatch(QStringLiteral("mi"), QStringLiteral("Milliseconds"), &Prefix, &At));
        QCOMPARE(At, (QVector<int>{0, 1}));
        QVERIFY(InitialsMatch(QStringLiteral("mi"), QStringLiteral("MediaTypeId"), &Initials, &At));
        QCOMPARE(At, (QVector<int>{0, 9}));
        QVERIFY(Prefix > Initials);
        QVERIFY(InitialsMatch(QStringLiteral("mtid"), QStringLiteral("MediaTypeId"), nullptr, &At));
        QCOMPARE(At, (QVector<int>{0, 5, 9, 10}));
        QVERIFY(InitialsMatch(QStringLiteral("ui"), QStringLiteral("unit_id"), nullptr, &At));
        QCOMPARE(At, (QVector<int>{0, 5}));
        QVERIFY(!InitialsMatch(QStringLiteral("ed"), QStringLiteral("MediaTypeId")));
        QVERIFY(!InitialsMatch(QStringLiteral("mx"), QStringLiteral("MediaTypeId")));
        // reST 의 흩어진 글자 일치는 그대로(공용으로 옮긴 규칙).
        QVERIFY(FuzzyMatch(QStringLiteral("cb"), QStringLiteral("code-block"), nullptr, &At));
        QCOMPARE(At, (QVector<int>{0, 5}));
    }

    void completionContexts()
    {
        QTemporaryDir Dir;
        auto Db = Create(Dir.filePath(QStringLiteral("music.db")));
        QVERIFY(Run(*Db, QString::fromLatin1(MusicSchema)));
        CompletionSource Source;
        QVERIFY(ReadSchema(*Db, Source.schema_, nullptr));
        Source.functions_ = {QStringLiteral("substr"), QStringLiteral("count"), QStringLiteral("max")};
        Source.pragmas_ = {QStringLiteral("journal_mode"), QStringLiteral("table_info")};
        Source.rowCounts_.insert(QStringLiteral("main.albums"), 347);
        const auto At = [&](const QString& Text) {
            const int Caret = int(Text.indexOf(QLatin1Char('|')));
            auto Statement = Text;
            Statement.remove(Caret, 1);
            return Complete(Statement, Caret, Source, true);
        };

        // FROM 뒤: 같은 문의 CTE 가 맨 위, 테이블은 행 수와 함께, 뷰는 ‘뷰’.
        auto Result = At(QStringLiteral("WITH recent AS (SELECT 1) SELECT * FROM |"));
        QCOMPARE(int(Result.context_), int(CompletionContext::Tables));
        QCOMPARE(Result.candidates_.first().label_, QStringLiteral("recent"));
        QCOMPARE(Result.candidates_.first().detail_, QStringLiteral("CTE · 이 문"));
        QCOMPARE(Candidate(Result, QStringLiteral("albums"))->detail_, QStringLiteral("347행"));
        QCOMPARE(Candidate(Result, QStringLiteral("v_long"))->detail_, QStringLiteral("뷰"));
        QVERIFY(Candidate(Result, QStringLiteral("tracks"))->description_.startsWith(QStringLiteral("tracks — 열 5개 (TrackId, Name")));

        // 별칭 뒤: 그 테이블의 열(형식 · 외래 키 · 설명 줄).
        Result = At(QStringLiteral("SELECT t.mi| FROM tracks AS t"));
        QCOMPARE(int(Result.context_), int(CompletionContext::Columns));
        QCOMPARE(Result.prefix_, QStringLiteral("mi"));
        QCOMPARE(Candidate(Result, QStringLiteral("Milliseconds"))->detail_, QStringLiteral("INTEGER"));
        QCOMPARE(Candidate(Result, QStringLiteral("Milliseconds"))->description_, QStringLiteral("tracks.Milliseconds · INTEGER NOT NULL"));
        QCOMPARE(Candidate(Result, QStringLiteral("MediaTypeId"))->detail_, QStringLiteral("INTEGER · → media_types"));
        QCOMPARE(int(Candidate(Result, QStringLiteral("MediaTypeId"))->kind_), int(CompletionKind::ForeignKey));
        QCOMPARE(int(Candidate(Result, QStringLiteral("TrackId"))->kind_), int(CompletionKind::PrimaryKey));

        // JOIN … ON 뒤: 외래 키 조건이 첫 줄(이번 JOIN 쪽이 앞), 그다음 두 테이블의 한정된 열.
        Result = At(QStringLiteral("SELECT * FROM tracks AS t JOIN albums AS a ON |"));
        QCOMPARE(int(Result.context_), int(CompletionContext::JoinCondition));
        const auto* Join = Candidate(Result, QStringLiteral("a.AlbumId = t.AlbumId"));
        QVERIFY(Join);
        QVERIFY(Join->bias_ > 0);
        QCOMPARE(Join->description_, QStringLiteral("tracks.AlbumId REFERENCES albums(AlbumId)"));
        QVERIFY(Candidate(Result, QStringLiteral("a.Title")));
        QVERIFY(Candidate(Result, QStringLiteral("t.Milliseconds")));

        // 식: 범위 테이블의 열(겹치는 이름은 한정어를 붙여 넣는다), 함수, 접두어가 있으면 키워드.
        Result = At(QStringLiteral("SELECT Ti| FROM tracks t JOIN albums a ON a.AlbumId = t.AlbumId"));
        QCOMPARE(int(Result.context_), int(CompletionContext::Expression));
        QVERIFY(Candidate(Result, QStringLiteral("Title")));
        QVERIFY(Candidate(Result, QStringLiteral("TrackId")));
        QVERIFY(Candidate(Result, QStringLiteral("substr")));
        bool Qualified = false;
        for (const auto& Item : Result.candidates_) Qualified |= Item.label_ == QStringLiteral("AlbumId") && Item.insertText_.startsWith(QStringLiteral("t."));
        QVERIFY(Qualified);
        Result = At(QStringLiteral("UPDATE tracks SET Mi|"));
        QVERIFY(Candidate(Result, QStringLiteral("Milliseconds")));

        // 그 밖의 문맥.
        QCOMPARE(int(At(QStringLiteral("INSERT INTO albums (|")).context_), int(CompletionContext::Columns));
        QVERIFY(Candidate(At(QStringLiteral("INSERT INTO albums (Title, |")), QStringLiteral("ArtistId")));
        QVERIFY(Candidate(At(QStringLiteral("PRAGMA |")), QStringLiteral("journal_mode")));
        QVERIFY(Candidate(At(QStringLiteral("SELECT Name FROM artists ORDER BY Name COLLATE |")), QStringLiteral("NOCASE")));
        QCOMPARE(int(At(QStringLiteral("SELECT * FROM tracks t wh|")).context_), int(CompletionContext::Clause));
        QVERIFY(Candidate(At(QStringLiteral("SELECT * FROM tracks t wh|")), QStringLiteral("WHERE")));
        QVERIFY(Candidate(At(QStringLiteral("sel|")), QStringLiteral("SELECT")));
        Source.upperKeywords_ = false;
        QVERIFY(Candidate(At(QStringLiteral("sel|")), QStringLiteral("select")));
        Source.upperKeywords_ = true;
        // 문자열 · 주석 · 숫자 안에서는 열지 않는다.
        QCOMPARE(int(At(QStringLiteral("SELECT 'ab|")).context_), int(CompletionContext::None));
        QCOMPARE(int(At(QStringLiteral("SELECT 1 -- note |")).context_), int(CompletionContext::None));
        QCOMPARE(int(At(QStringLiteral("SELECT /* x | */ 1")).context_), int(CompletionContext::None));
        QCOMPARE(int(At(QStringLiteral("SELECT 12|")).context_), int(CompletionContext::None));
        QVERIFY(Labels(At(QStringLiteral("SELECT * FROM \"tra|"))).isEmpty());

        // 콜팁: 커서가 든 함수와 몇 번째 인자인지.
        const QString Call = QStringLiteral("SELECT substr(Name, max(1, 2), ");
        const auto Info = EnclosingCall(Call, int(Call.size()));
        QCOMPARE(Info.name_, QStringLiteral("substr"));
        QCOMPARE(Info.argument_, 2);
        const auto Signature = FunctionSignature(QStringLiteral("SUBSTR"));
        QVERIFY(Signature.startsWith(QStringLiteral("substr(X, Y, Z)")));
        const auto [From, To] = SignatureArgument(Signature, 2);
        QCOMPARE(Signature.mid(From, To - From), QStringLiteral("Z"));
        QCOMPARE(EnclosingCall(QStringLiteral("SELECT max(1) + "), 16).name_, QString());

        // 빠른 수정: 메시지의 이름과 편집 거리가 가까운 이름.
        auto Missing = ParseMissingName(QStringLiteral("no such column: t.Nme"));
        QCOMPARE(int(Missing.kind_), int(MissingName::Kind::Column));
        QCOMPARE(Missing.name_, QStringLiteral("Nme"));
        Missing = ParseMissingName(QStringLiteral("no such table: main.trax"));
        QCOMPARE(int(Missing.kind_), int(MissingName::Kind::Table));
        QCOMPARE(Missing.name_, QStringLiteral("trax"));
        QCOMPARE(int(ParseMissingName(QStringLiteral("near \"x\": syntax error")).kind_), int(MissingName::Kind::None));
        QCOMPARE(EditDistance(QStringLiteral("Nme"), QStringLiteral("name")), 1);
        QCOMPARE(Suggestions(QStringLiteral("trax"), NamesFor(Missing, QStringLiteral("SELECT * FROM trax"), Source)), QStringList{QStringLiteral("tracks")});
        Missing = ParseMissingName(QStringLiteral("no such column: Titel"));
        QCOMPARE(Suggestions(Missing.name_, NamesFor(Missing, QStringLiteral("SELECT Titel FROM albums"), Source)), QStringList{QStringLiteral("Title")});

        // SQL 서식(Alt+Shift+F).
        QCOMPARE(FormatSql(QStringLiteral("select t.Name, count(*) as n from tracks t join albums a on a.AlbumId = t.AlbumId "
                                          "where t.Milliseconds > 1000 and a.Title like 'R%' group by t.Name order by n desc limit 10;"), true),
                 QStringLiteral("SELECT t.Name, count(*) AS n\nFROM tracks t\nJOIN albums a ON a.AlbumId = t.AlbumId\nWHERE t.Milliseconds > 1000\n"
                                "    AND a.Title LIKE 'R%'\nGROUP BY t.Name\nORDER BY n DESC\nLIMIT 10;"));
        QCOMPARE(FormatSql(QStringLiteral("select * from (select a from t) x left join u on u.id=x.a where x.v between 1 and 2"), false),
                 QStringLiteral("select *\nfrom (\n    select a\n    from t\n) x\nleft join u on u.id = x.a\nwhere x.v between 1 and 2"));
        QCOMPARE(FormatSql(QStringLiteral("SELECT 'a -- not comment', \"from\" FROM t -- tail\n;"), true),
                 QStringLiteral("SELECT 'a -- not comment', \"from\"\nFROM t -- tail\n;"));
    }

    void queryCompletionPopupPlanAndCallTip()
    {
        QTemporaryDir Dir;
        const auto Path = Dir.filePath(QStringLiteral("music.db"));
        {
            auto Db = Create(Path);
            QVERIFY(Run(*Db, QString::fromLatin1(MusicSchema)));
        }
        {
            DocumentView View(Path, DocumentView::Mode::Editor);
            View.resize(1200, 720);
            View.show();
            View.Open();
            QVERIFY(View.IsReady());
            auto* Query = View.NewQuery();
            QVERIFY(Query);
            auto* Editor = Query->FocusTarget();
            auto* Popup = Query->CompletionPopup();
            QVERIFY(Popup);

            // Ctrl+Space: FROM 뒤 테이블, Enter 로 넣는다.
            Query->SetText(QStringLiteral("SELECT * FROM tr"));
            Query->Editor()->Send(SCI_DOCUMENTEND);
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.complete")));
            QVERIFY(Query->CompletionVisible());
            QCOMPARE(Popup->currentItem().label, QStringLiteral("tracks"));
            QVERIFY(!Popup->currentItem().icon.isNull());
            QTest::keyClick(Editor, Qt::Key_Return);
            QTRY_VERIFY(!Query->CompletionVisible());
            QCOMPARE(Query->Text(), QStringLiteral("SELECT * FROM tracks"));

            // 입력 중: . 뒤 열 목록, 글자를 치면 접두어 · 머리 글자로 거른다. 아래 줄은 고른 열의 설명.
            QTest::keyClicks(Editor, QStringLiteral(" AS t WHERE t."));
            QTRY_VERIFY(Query->CompletionVisible());
            QTest::keyClicks(Editor, QStringLiteral("mi"));
            QTRY_COMPARE(Popup->visibleCount(), 2);
            QCOMPARE(Popup->currentItem().label, QStringLiteral("Milliseconds"));
            QCOMPARE(Popup->currentItem().description, QStringLiteral("tracks.Milliseconds · INTEGER NOT NULL"));
            auto* Footer = Popup->findChild<QLabel*>(QStringLiteral("completionFooter"));
            QVERIFY(Footer && !Footer->isHidden());
            QTest::keyClick(Editor, Qt::Key_Down);
            QCOMPARE(Popup->currentItem().label, QStringLiteral("MediaTypeId"));
            QTest::keyClick(Editor, Qt::Key_Tab);
            QTRY_VERIFY(!Query->CompletionVisible());
            QCOMPARE(Query->Text(), QStringLiteral("SELECT * FROM tracks AS t WHERE t.MediaTypeId"));
            // 공백을 치면 닫히고, Esc 는 닫기만 한다.
            QTest::keyClicks(Editor, QStringLiteral(" = 1 AND t.N"));
            QTRY_VERIFY(Query->CompletionVisible());
            QTest::keyClick(Editor, Qt::Key_Escape);
            QTRY_VERIFY(!Query->CompletionVisible());
            QCOMPARE(Query->Text(), QStringLiteral("SELECT * FROM tracks AS t WHERE t.MediaTypeId = 1 AND t.N"));

            // 콜팁: ( 를 치면 함수 서명, 쉼표마다 인자 강조.
            QTest::keyClicks(Editor, QStringLiteral("ame = substr("));
            QTRY_VERIFY(Query->Editor()->Send(SCI_CALLTIPACTIVE) != 0);
            QTest::keyClick(Editor, Qt::Key_Escape);

            // 실행 계획(Ctrl+L): 색인 없는 검색은 SCAN, 기본 키는 SEARCH.
            Query->SetText(QStringLiteral("SELECT * FROM tracks WHERE Milliseconds > 1000;"));
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.explain")));
            QTRY_VERIFY(!Query->Running() && Query->PlanTree());
            QVERIFY(Query->PlanTree()->topLevelItem(0)->text(0).startsWith(QStringLiteral("SCAN tracks")));
            auto* Note = Query->Result()->findChild<QLabel*>(QStringLiteral("sqliteQueryNote"));
            QCOMPARE(Note->text(), QStringLiteral("실행 계획 — 1단계, 전체 검색 1"));
            QCOMPARE(Query->Result()->findChild<QTabBar*>()->tabText(0), QStringLiteral("실행 계획"));
            Query->SetText(QStringLiteral("SELECT * FROM tracks WHERE TrackId = 3;"));
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.explain")));
            QTRY_VERIFY(!Query->Running() && Query->PlanTree());
            QVERIFY(Query->PlanTree()->topLevelItem(0)->text(0).startsWith(QStringLiteral("SEARCH tracks")));
            QCOMPARE(Note->text(), QStringLiteral("실행 계획 — 1단계, 전체 검색 0"));
            // 바이트코드(EXPLAIN)는 표로.
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.explainBytecode")));
            QTRY_VERIFY(!Query->Running() && Query->CurrentGrid());
            QVERIFY(Query->CurrentGrid()->model()->rowCount() > 3);
            QVERIFY(Query->Result()->findChild<QTabBar*>()->tabText(0).startsWith(QStringLiteral("바이트코드")));
            QVERIFY(!Query->PlanTree());

            // 오류 빠른 수정(Alt+Enter): 없는 열 이름을 가까운 이름으로.
            Query->SetText(QStringLiteral("SELECT a.Titel FROM albums AS a;"));
            Query->RunAll();
            QTRY_VERIFY(!Query->Running());
            QVERIFY(Query->Top()->findChild<QLabel*>(QStringLiteral("sqliteEditorNote"))->text().contains(QStringLiteral("Alt+Enter")));
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.quickFix")));
            QVERIFY(Query->CompletionVisible());
            QCOMPARE(Popup->currentItem().label, QStringLiteral("Title"));
            QCOMPARE(Popup->currentItem().description, QStringLiteral("Titel → Title"));
            QTest::keyClick(Editor, Qt::Key_Return);
            QTRY_VERIFY(!Query->CompletionVisible());
            QCOMPARE(Query->Text(), QStringLiteral("SELECT a.Title FROM albums AS a;"));

            // SQL 서식(Alt+Shift+F): 커서가 든 문.
            Query->SetText(QStringLiteral("select name from artists where artistid=1;"));
            QVERIFY(View.RunCommand(QStringLiteral("sqlite.format")));
            QCOMPARE(Query->Text(), QStringLiteral("SELECT name\nFROM artists\nWHERE artistid = 1;"));
            if (const auto Shot = qEnvironmentVariable("HC_SQLITE_SHOT"); !Shot.isEmpty()) {
                Query->SetText(QStringLiteral("SELECT t.mi FROM tracks AS t"));
                Query->Editor()->Send(SCI_GOTOPOS, 11);
                Query->Complete(true);
                QTest::qWait(200);
                View.grab().save(Shot + QStringLiteral("-complete-view.png"));
                Popup->grab().save(Shot + QStringLiteral("-complete-popup.png"));
            }
        }
        QVERIFY(QFile::rename(Path, Path + QStringLiteral(".moved")));
    }
};

QTEST_MAIN(SqliteCoreTests)
#include "tstSqliteCore.moc"
