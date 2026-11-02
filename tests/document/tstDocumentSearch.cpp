#include "stdafx.h"
#include "UniqueLibs/document/search/core/textSearchMatcher.hpp"
#include "UniqueLibs/document/search/core/searchWorkerProtocol.hpp"
#include "UniqueLibs/document/search/documentSearchWorkerClient.hpp"
#include "UniqueLibs/document/search/documentSearchEngine.hpp"
#include "UniqueLibs/document/search/core/bodyProjection.hpp"
#include <QtTest>
#include <io.h>
#include <fcntl.h>

using namespace hc::document::search;
class DocumentSearchTest final : public QObject {
    Q_OBJECT
private slots:
    void recoverySkipsFullyUndoneJournal() {
        QTemporaryDir Directory;QMap<QString,QString> Texts{{"a","hello"},{"b","hello"}};
        const auto Attach=[&](Engine& Session){Session.SetJournalDirectory(Directory.path());connect(&Session,&Engine::EditBuffer,&Session,[&,Pointer=&Session](quint64 Ticket,const QJsonObject& Source,const QString& Before,const QString& After){
            const auto Id=Source["document"].toString();const auto Error=Texts[Id]==Before?QString{}:QStringLiteral("stale");if(Error.isEmpty())Texts[Id]=After;
            QTimer::singleShot(0,Pointer,[Pointer,Ticket,Error]{Pointer->BufferApplied(Ticket,Error);});});};
        {
            Engine First,Second;Attach(First);Attach(Second);
            for(auto Pair:{qMakePair(&First,QString("a")),qMakePair(&Second,QString("b"))}){
                QSignalSpy Progress(Pair.first,&Engine::Progress),Applied(Pair.first,&Engine::ApplyProgress);
                Pair.first->Begin(1,{{"book",true},{"query","hello"}},{QJsonObject{{"document",Pair.second},{"path",Pair.second},{"text",Texts[Pair.second]}}});
                QTRY_VERIFY_WITH_TIMEOUT(!Progress.isEmpty()&&Progress.last()[4].toBool(),10000);Pair.first->Apply(1,"all",{},"world");QTRY_VERIFY_WITH_TIMEOUT(!Applied.isEmpty()&&!Applied.last()[0].toBool(),5000);
            }
            QSignalSpy Applied(&Second,&Engine::ApplyProgress);Second.Undo();QTRY_VERIFY_WITH_TIMEOUT(!Applied.isEmpty()&&!Applied.last()[0].toBool(),5000);QCOMPARE(Texts["b"],QString("hello"));
        }
        QCOMPARE(Texts["a"],QString("world"));Engine Recovered;Attach(Recovered);Recovered.Begin(1,{{"query",""}},{});
        QSignalSpy Applied(&Recovered,&Engine::ApplyProgress);Recovered.Undo();QTRY_VERIFY_WITH_TIMEOUT(!Applied.isEmpty()&&!Applied.last()[0].toBool(),5000);QCOMPARE(Texts["a"],QString("hello"));
    }
    void staleDiskAndUndoConflict() {
        QTemporaryDir Directory;const auto Path=Directory.filePath("a.txt");
        const auto Put=[&](QByteArray Bytes){QFile File(Path);return File.open(QIODevice::WriteOnly)&&File.write(Bytes)==Bytes.size();};
        const auto Read=[&]{QFile File(Path);return File.open(QIODevice::ReadOnly)?File.readAll():QByteArray{};};
        QVERIFY(Put("hello\r\n"));Engine Session;Session.SetJournalDirectory(Directory.filePath("undo"));
        int Writes=0;Session.SetWriter([&](const QString& Target,const QByteArray& Hash,const QByteArray& Bytes)->QString{
            QFile Input(Target);if(!Input.open(QIODevice::ReadOnly))return "open";const auto Current=Input.readAll();Input.close();
            if(QCryptographicHash::hash(Current,QCryptographicHash::Sha256)!=Hash)return "stale";
            QSaveFile Output(Target);if(!Output.open(QIODevice::WriteOnly)||Output.write(Bytes)!=Bytes.size()||!Output.commit())return "write";++Writes;return {};
        });
        QSignalSpy Progress(&Session,&Engine::Progress),Applied(&Session,&Engine::ApplyProgress);
        const QJsonObject Request{{"root",Directory.path()},{"query","hello"},{"include","*.txt"}};
        Session.Begin(1,Request,{});QTRY_VERIFY_WITH_TIMEOUT(!Progress.isEmpty()&&Progress.last()[4].toBool(),10000);
        QVERIFY(Put("external change\r\n"));Session.Apply(1,"all",{},"world");QTRY_VERIFY_WITH_TIMEOUT(!Applied.isEmpty()&&!Applied.last()[0].toBool(),5000);
        QCOMPARE(Writes,0);QCOMPARE(Read(),QByteArray("external change\r\n"));
        QVERIFY(Put("hello\r\n"));Progress.clear();Applied.clear();Session.Begin(2,Request,{});QTRY_VERIFY_WITH_TIMEOUT(!Progress.isEmpty()&&Progress.last()[4].toBool(),10000);
        Session.Apply(2,"all",{},"world");QTRY_VERIFY_WITH_TIMEOUT(!Applied.isEmpty()&&!Applied.last()[0].toBool(),5000);QCOMPARE(Writes,1);QCOMPARE(Read(),QByteArray("world\r\n"));
        QVERIFY(Put("newer change\r\n"));Applied.clear();Session.Undo();QTRY_VERIFY_WITH_TIMEOUT(!Applied.isEmpty()&&!Applied.last()[0].toBool(),5000);QCOMPARE(Writes,1);QCOMPARE(Read(),QByteArray("newer change\r\n"));
        QVERIFY(Put("world\r\n"));Applied.clear();Session.Undo();QTRY_VERIFY_WITH_TIMEOUT(!Applied.isEmpty()&&!Applied.last()[0].toBool(),5000);QCOMPARE(Writes,2);QCOMPARE(Read(),QByteArray("hello\r\n"));
    }
    void bookBodyBatchAndUndo() {
        QTemporaryDir Directory; Engine Session; Session.SetJournalDirectory(Directory.path());
        QMap<QString,QString> Texts{{"a.xhtml","<html><body><p>hel<b>lo</b></p></body></html>"},{"b.xhtml","<html><body><p>hello</p></body></html>"}};
        QJsonArray Sources;for(auto It=Texts.cbegin();It!=Texts.cend();++It)Sources.append(QJsonObject{{"path",It.key()},{"resource",It.key()},{"document","book"},{"text",It.value()},{"encoding","UTF-8"}});
        int Batches=0;
        connect(&Session,&Engine::EditBuffer,this,[&](quint64 Ticket,const QJsonObject& Source,const QString&,const QString&){
            ++Batches;const auto Changes=Source["bookBatch"].toArray();QCOMPARE(Changes.size(),2);
            for(const auto& Value:Changes){const auto Item=Value.toObject();const auto Path=Item["source"].toObject()["resource"].toString();QCOMPARE(Texts[Path],Item["before"].toString());Texts[Path]=Item["after"].toString();}
            QTimer::singleShot(0,&Session,[&,Ticket]{Session.BufferApplied(Ticket,{});});
        });
        QSignalSpy Progress(&Session,&Engine::Progress),Applied(&Session,&Engine::ApplyProgress);
        Session.Begin(1,{{"book",true},{"body",true},{"query","hello"}},Sources);
        QTRY_VERIFY_WITH_TIMEOUT(!Progress.isEmpty()&&Progress.last()[4].toBool(),10000);QCOMPARE(Progress.last()[1].toInt(),2);
        Session.Apply(1,"all",{},"a&b");QTRY_VERIFY_WITH_TIMEOUT(!Applied.isEmpty()&&!Applied.last()[0].toBool(),5000);
        QCOMPARE(Batches,1);QCOMPARE(Texts["a.xhtml"],QString("<html><body><p>a&amp;b<b></b></p></body></html>"));
        Applied.clear();Session.Undo();QTRY_VERIFY_WITH_TIMEOUT(!Applied.isEmpty()&&!Applied.last()[0].toBool(),5000);
        QCOMPARE(Batches,2);QCOMPARE(Texts["a.xhtml"],Sources.first().toObject()["text"].toString());
    }
    void incrementalQuerySupersession() {
        QTemporaryDir Directory; Engine Session;Session.SetJournalDirectory(Directory.path());QSignalSpy Progress(&Session,&Engine::Progress);
        const QJsonArray Sources{QJsonObject{{"path","a.txt"},{"document","buffer"},{"text",QString("hello ").repeated(20000)}}};
        Session.Begin(1,{{"book",true},{"query","hello"}},Sources);
        QTRY_VERIFY_WITH_TIMEOUT(!Progress.isEmpty()&&Progress.last()[1].toInt()>0,10000);
        Session.Begin(2,{{"book",true},{"query","absent"}},Sources);
        QTRY_VERIFY_WITH_TIMEOUT(!Progress.isEmpty()&&Progress.last()[0].toInt()==2&&Progress.last()[4].toBool(),10000);
        QCOMPARE(Progress.last()[1].toInt(),0);QCOMPARE(Progress.last()[3].toInt(),0);
        Session.Begin(3,{{"book",true},{"query","hello"}},Sources);
        QTRY_VERIFY_WITH_TIMEOUT(Progress.last()[0].toInt()==3&&Progress.last()[4].toBool(),15000);
        QCOMPARE(Progress.last()[1].toInt(),20000);QCOMPARE(Progress.last()[3].toInt(),0);
    }
    void bodyProjection()
    {
        BodyProjection Body("<html><head><title>hidden</title></head><body><p>a&amp;<b>b</b>c</p><p>d<ruby>base<rt>ruby</rt></ruby></p><script>hidden</script><span hidden='hidden'>hidden</span></body></html>");
        QVERIFY2(Body.Error().isEmpty(), qPrintable(Body.Error())); QVERIFY(!Body.Text().contains("hidden")); QVERIFY(Body.Text().contains("a&bc\nd"));
        const auto Cross = Body.Map({1, 3}); QVERIFY(Cross); QCOMPARE(Cross->size(), 3);
        QVERIFY(!Body.Map({0, 6}));
        QCOMPARE(BodyProjection::Escape("<&>"), QString("&lt;&amp;&gt;"));
        BodyProjection Emoji("<html><body><p>&#x1f600;x</p></body></html>");
        QVERIFY(Emoji.Map({0, 2})); QVERIFY(!Emoji.Map({1, 1}));
        BodyProjection Invalid("<html><body><p>broken</body></html>"); QVERIFY(!Invalid.Error().isEmpty());
    }
    void diskSessionAndUndo()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid());
        const auto A = Directory.filePath("a.txt"), B = Directory.filePath("b.txt");
        const auto Put = [](const QString& Path, const QByteArray& Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); };
        QVERIFY(Put(A, "disk")); QVERIFY(Put(B, "hello\r\n"));
        Engine Session; Session.SetJournalDirectory(Directory.filePath("undo"));
        Session.SetWriter([](const QString& Path, const QByteArray& Hash, const QByteArray& Bytes) -> QString {
            QFile Input(Path); if (!Input.open(QIODevice::ReadOnly)) return "open"; const auto Original = Input.readAll(); Input.close();
            if (QCryptographicHash::hash(Original, QCryptographicHash::Sha256) != Hash) return "stale";
            QSaveFile Output(Path); if (!Output.open(QIODevice::WriteOnly) || Output.write(Bytes) != Bytes.size() || !Output.commit()) return "write"; return {};
        });
        QString Buffer = "hello hello";
        connect(&Session, &Engine::EditBuffer, this, [&](quint64 Ticket, const QJsonObject&, const QString& Before, const QString& After) {
            const auto Error = Buffer == Before ? QString{} : QStringLiteral("stale-buffer"); if (Error.isEmpty()) Buffer = After;
            QTimer::singleShot(0, &Session, [&, Ticket, Error] { Session.BufferApplied(Ticket, Error); });
        });
        QSignalSpy Progress(&Session, &Engine::Progress), Pages(&Session, &Engine::PageReady), Applied(&Session, &Engine::ApplyProgress);
        Session.Begin(1, {{"root", Directory.path()}, {"query", "hello"}, {"include", "*.txt"}, {"recursive", true}}, {QJsonObject{{"path", A}, {"document", "open"}, {"text", Buffer}, {"encoding", "UTF-8"}}});
        QTRY_VERIFY_WITH_TIMEOUT(!Progress.isEmpty() && Progress.last()[4].toBool(), 10000);
        QCOMPARE(Progress.last()[1].toLongLong(), 3); QCOMPARE(Progress.last()[3].toLongLong(), 0);
        Session.Page(1, 0, 64, "world", true, 1); QCOMPARE(Pages.size(), 1);
        QCOMPARE(Pages.first()[2].toJsonArray().size(), 3); QCOMPARE(Pages.first()[2].toJsonArray().first().toObject()["replacement"].toString(), QString("world"));
        Session.Apply(1, "all", {}, "world"); QTRY_VERIFY_WITH_TIMEOUT(!Applied.isEmpty() && !Applied.last()[0].toBool(), 5000);
        QCOMPARE(Buffer, QString("world world")); QFile File(B); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), QByteArray("world\r\n")); File.close();
        Applied.clear(); Session.Undo(); QTRY_VERIFY_WITH_TIMEOUT(!Applied.isEmpty() && !Applied.last()[0].toBool(), 5000);
        QCOMPARE(Buffer, QString("hello hello")); QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), QByteArray("hello\r\n"));
    }
    void unicodeAndLineEndings()
    {
        TextMap Text(QString::fromUtf8("가\r\n😀\r끝\n"));
        Matcher Search("\\n|😀|끝", {true, true}); Search.Start(Text);
        auto A = Search.Next(); QVERIFY(A); QCOMPARE(A->range_.start_, 1); QCOMPARE(A->range_.length_, 2);
        auto B = Search.Next(); QVERIFY(B); QCOMPARE(B->range_.start_, 3); QCOMPARE(B->range_.length_, 2); QCOMPARE(B->line_, 2);
        auto C = Search.Next(); QVERIFY(C); QCOMPARE(C->range_.start_, 5);
        auto D = Search.Next(); QVERIFY(D); QCOMPARE(D->range_.start_, 6); QCOMPARE(D->line_, 3);
    }
    void captureReplacement()
    {
        TextMap Text("abc\r\ndef"); Matcher Search("(?<word>abc)(x)?(\\n)", {true, true}); Search.Start(Text);
        const auto Hit = Search.Next(); QVERIFY(Hit);
        Replacement Replace("$<word>:$2:$3:$&:$$", true, Search.Groups()); QVERIFY(Replace.Error().isEmpty());
        QCOMPARE(*Replace.Expand(Text.Original(), *Hit), QString("abc::\r\n:abc\r\n:$"));
        QVERIFY(!Replacement("$99", true, Search.Groups()).Error().isEmpty());
        QVERIFY(!Replacement("$0", true, Search.Groups()).Error().isEmpty());
        QCOMPARE(*Replacement("$1\\n", false, {}).Expand(Text.Original(), *Hit), QString("$1\\n"));
    }
    void zeroWidthAndLookbehind()
    {
        TextMap Text("ab"); Matcher Search("(?<=(a))(?=b)", {true, true}); Search.Start(Text);
        const auto Hit = Search.Next(); QVERIFY(Hit); QCOMPARE(Hit->range_.start_, 1); QCOMPARE(Hit->range_.length_, 0);
        QCOMPARE(*Replacement("$1", true, Search.Groups()).Expand(Text.Original(), *Hit), QString("a"));
        QVERIFY(!Search.Next());
    }
    void framing()
    {
        const auto Bytes = wire::Encode({wire::Type::Hello, 7, 9, 0, QByteArray("payload")});
        wire::Decoder Decoder;
        for (qsizetype I = 0; I < Bytes.size() - 1; ++I) { QVERIFY(Decoder.Append(Bytes.mid(I, 1))); QVERIFY(!Decoder.Take()); }
        QVERIFY(Decoder.Append(Bytes.last(1))); const auto Frame = Decoder.Take(); QVERIFY(Frame); QCOMPARE(Frame->generation_, 9u); QCOMPARE(Frame->payload_, QByteArray("payload"));
        auto Invalid = Bytes; Invalid[8] = char(0xff); Invalid[9] = char(0xff); Invalid[10] = char(0xff);
        QVERIFY(Decoder.Append(Invalid)); QVERIFY(!Decoder.Take()); QVERIFY(!Decoder.Error().isEmpty());
    }
    void immutableReplacementPlan()
    {
        TextMap Text("abc\r\ndef\nabc"); Matcher Search("(abc)", {true, true}); Search.Start(Text);
        QVector<Match> Hits; while (auto Hit = Search.Next()) Hits.append(*Hit);
        Replacement Replace("$1\\nX", true, Search.Groups());
        ReplacementPlan Plan(Text.Original(), Hits, Replace, "\r\n");
        QVERIFY(Plan.Error().isEmpty()); QCOMPARE(Plan.After(), QString("abc\r\nX\r\ndef\nabc\r\nX"));
        QCOMPARE(*Plan.ApplyTo(Text.Original()), Plan.After()); QVERIFY(!Plan.ApplyTo("changed"));
        Hits.append(Hits.first()); QVERIFY(!ReplacementPlan(Text.Original(), Hits, Replace).Error().isEmpty());
        Match Broken; Broken.range_ = {1, 1};
        QVERIFY(!ReplacementPlan(QString::fromUtf8("😀"), {Broken}, Replacement("x", false, {})).Error().isEmpty());
        Broken.range_ = {1, 1};
        QVERIFY(!ReplacementPlan("x\r\ny", {Broken}, Replacement("x", false, {})).Error().isEmpty());
        TextMap CaptureText("a\r\nb"); Matcher Capture("(a\\nb)", {true, true}); Capture.Start(CaptureText);
        QCOMPARE(*Replacement("$1\\n", true, Capture.Groups()).Expand(CaptureText.Original(), *Capture.Next(), "\n"), QString("a\r\nb\n"));
    }
    void cancelFromResults()
    {
        WorkerClient Client; QSignalSpy Registered(&Client, &WorkerClient::Registered), Canceled(&Client, &WorkerClient::Canceled), Errors(&Client, &WorkerClient::Failed), Done(&Client, &WorkerClient::Finished);
        bool Requested = false;
        connect(&Client, &WorkerClient::Matches, this, [&](quint64, const QVector<Match>&) { if (!Requested) { Requested = true; Client.Cancel(); } });
        Client.Start(); QTRY_VERIFY_WITH_TIMEOUT(Client.Ready(), 5000);
        Client.RegisterSnapshot("v", QString("hello ").repeated(1000)); QTRY_COMPARE_WITH_TIMEOUT(Registered.size(), 1, 5000);
        Client.Search("v", {}, "hello", {}, 1); QTRY_COMPARE_WITH_TIMEOUT(Canceled.size(), 1, 5000);
        QTest::qWait(1700); QVERIFY(Errors.isEmpty()); QVERIFY(!Client.Busy()); QVERIFY(Done.isEmpty());
        Client.Search("v", {}, "absent", {}, 2); QTRY_COMPARE_WITH_TIMEOUT(Done.size(), 1, 5000);
        QVERIFY(Errors.isEmpty());
    }
    void smallChunksAndIncrementalResults()
    {
        WorkerClient Client; Client.SetChunkBytes(64);
        QSignalSpy Registered(&Client, &WorkerClient::Registered), Done(&Client, &WorkerClient::Finished), Errors(&Client, &WorkerClient::Failed);
        int Rows = 0, Batches = 0; bool Premature = false;
        connect(&Client, &WorkerClient::Matches, this, [&](quint64 Generation, const QVector<Match>& Hits) {
            QCOMPARE(Generation, 1u); ++Batches;
            for (const auto& Hit : Hits) { QCOMPARE(Hit.range_.start_, Rows * 7); QCOMPARE(Hit.range_.length_, 5); QCOMPARE(Hit.line_, Rows + 1); ++Rows; }
            Premature |= !Done.isEmpty();
        });
        Client.Start(); QTRY_VERIFY_WITH_TIMEOUT(Client.Ready(), 10000);
        Client.RegisterSnapshot("v1", QString("hello\r\n").repeated(1200)); QTRY_VERIFY_WITH_TIMEOUT(!Registered.isEmpty() || !Errors.isEmpty(), 10000); QVERIFY2(Errors.isEmpty(), Errors.isEmpty() ? "" : qPrintable(Errors.first().last().toString())); QCOMPARE(Registered.size(), 1);
        Client.Search("v1", {}, "hello", {}, 1); QTRY_COMPARE_WITH_TIMEOUT(Done.size(), 1, 15000);
        QVERIFY2(Errors.isEmpty(), Errors.isEmpty() ? "" : qPrintable(Errors.first().last().toString()));
        QCOMPARE(Rows, 1200); QVERIFY(Batches > 1); QVERIFY(!Premature); QVERIFY(Client.Resident("v1"));
    }
    void cancelAndRestart()
    {
        WorkerClient Client; QSignalSpy Registered(&Client, &WorkerClient::Registered), Cancel(&Client, &WorkerClient::Canceled), Errors(&Client, &WorkerClient::Failed);
        Client.Start(); QTRY_VERIFY_WITH_TIMEOUT(Client.Ready(), 10000);
        Client.RegisterSnapshot("large", QString('a').repeated(1000000) + '!'); QTRY_VERIFY_WITH_TIMEOUT(!Registered.isEmpty() || !Errors.isEmpty(), 10000); QVERIFY2(Errors.isEmpty(), Errors.isEmpty() ? "" : qPrintable(Errors.first().last().toString())); QCOMPARE(Registered.size(), 1);
        Client.Search("large", {}, "(*NO_AUTO_POSSESS)(a+)+$", {true, true}, 2);
        QTest::qWait(20); Client.Cancel(); QTRY_VERIFY_WITH_TIMEOUT(!Cancel.isEmpty(), 2000);
        // The engine may hit its own regex match limit before cancellation is
        // delivered. That is a reported safe failure, not an IPC/cancel defect.
        for(const auto& Error:Errors)QCOMPARE(Error.last().toString(),QString::fromUtf8("정규식 실행에 실패했습니다."));
    }
    void diskSearch()
    {
        QTemporaryDir Directory; QVERIFY(Directory.isValid()); QFile File(Directory.filePath("unicode.txt"));
        QVERIFY(File.open(QIODevice::WriteOnly)); const auto Bytes = QByteArray("\xff\xfe", 2) + wire::EncodeText(QString::fromUtf8("가\r\n😀needle"));
        QCOMPARE(File.write(Bytes), Bytes.size()); File.close();
        WorkerClient Client; QSignalSpy Done(&Client, &WorkerClient::Finished), Errors(&Client, &WorkerClient::Failed);
        QVector<Match> Found; connect(&Client, &WorkerClient::Matches, this, [&](quint64, const QVector<Match>& Hits) { Found += Hits; });
        Client.Start(); QTRY_VERIFY_WITH_TIMEOUT(Client.Ready(), 10000);
        Client.Search("disk", File.fileName(), "needle", {}, 1); QTRY_VERIFY_WITH_TIMEOUT(!Done.isEmpty() || !Errors.isEmpty(), 5000);
        QVERIFY(Errors.isEmpty()); QCOMPARE(Found.size(), 1); QCOMPARE(Found.first().range_.start_, 5); QCOMPARE(Found.first().line_, 2);
    }
    void forcedKillAndFreshRegistration()
    {
        WorkerClient Client; QSignalSpy Cancel(&Client, &WorkerClient::Canceled), Registered(&Client, &WorkerClient::Registered);
        Client.Start(QCoreApplication::applicationFilePath()); QTRY_VERIFY_WITH_TIMEOUT(Client.Ready(), 5000);
        Client.Search("fixture", {}, "x", {}, 3); QTest::qWait(30); Client.Cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!Cancel.isEmpty(), 2000); QTRY_VERIFY_WITH_TIMEOUT(!Client.IsRunning(), 2000);
        QVERIFY(!Client.Ready()); QVERIFY(!Client.Resident("fixture"));
        Client.Start(); QTRY_VERIFY_WITH_TIMEOUT(Client.Ready(), 5000);
        Client.RegisterSnapshot("fixture", "fresh"); QTRY_COMPARE_WITH_TIMEOUT(Registered.size(), 1, 5000);
        QVERIFY(Client.Resident("fixture"));
    }
    void watchdog()
    {
        WorkerClient Client; QSignalSpy Errors(&Client, &WorkerClient::Failed);
        Client.Start(QCoreApplication::applicationFilePath()); QTRY_VERIFY_WITH_TIMEOUT(Client.Ready(), 5000);
        Client.Search("fixture", {}, "x", {}, 4); QTRY_COMPARE_WITH_TIMEOUT(Errors.size(), 1, 4000);
        QVERIFY(!Client.Ready()); QTRY_VERIFY_WITH_TIMEOUT(!Client.IsRunning(), 2000);
    }
    void invalidPattern()
    {
        WorkerClient Client; QSignalSpy Errors(&Client, &WorkerClient::Failed), Registered(&Client, &WorkerClient::Registered), Done(&Client, &WorkerClient::Finished);
        Client.Start(); QTRY_VERIFY_WITH_TIMEOUT(Client.Ready(), 5000);
        Client.RegisterSnapshot("v", "abc"); QTRY_COMPARE_WITH_TIMEOUT(Registered.size(), 1, 5000);
        Client.Search("v", {}, "[", {true, false}, 5); QTRY_COMPARE_WITH_TIMEOUT(Errors.size(), 1, 5000);
        QVERIFY(Done.isEmpty());
    }
};
int main(int Argc, char** Argv)
{
    QCoreApplication App(Argc, Argv);
    if (App.arguments().contains("--document-search")) {
        // A deliberately non-cooperative peer proves the parent can kill and re-register.
        _setmode(_fileno(stdin), _O_BINARY); _setmode(_fileno(stdout), _O_BINARY);
        QFile Output; if (!Output.open(stdout, QIODevice::WriteOnly)) return 2;
        wire::Decoder Decoder; quint32 Sequence = 0;
        for (;;) {
            char Byte = 0; if (_read(_fileno(stdin), &Byte, 1) != 1) return 0; Decoder.Append(QByteArray(1, Byte));
            while (auto Frame = Decoder.Take()) {
                wire::Frame Reply{wire::Type::Ready, Frame->instance_, Frame->generation_, Sequence++};
                if (Frame->type_ == wire::Type::Search) { Reply.type_ = wire::Type::Started; wire::Writer Data; Data.Text("matching"); Reply.payload_ = Data.bytes_; }
                Output.write(wire::Encode(Reply)); Output.flush();
                if (Frame->type_ == wire::Type::Search) for (;;) QThread::msleep(1000);
            }
        }
    }
    DocumentSearchTest Test; return QTest::qExec(&Test, Argc, Argv);
}
#include "tstDocumentSearch.moc"
