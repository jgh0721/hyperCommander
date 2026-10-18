#include "UniqueLibs/document/hwpBundleHost.hpp"
#include "UniqueLibs/document/hwpPackageAdmission.hpp"
#include "UniqueLibs/document/hwpLoadRetirement.hpp"
#include "UniqueLibs/document/hwpInputTransfer.hpp"
#include "UniqueLibs/document/hwpInputSender.hpp"
#include "UniqueLibs/document/hwpOpenWindow.hpp"
#include "UniqueLibs/document/hwpHostSession.hpp"
#include "UniqueLibs/document/hwpHelperDocument.hpp"
#include "hwpLoadFixtureData.hpp"
#include "UniqueLibs/document/hwpStartupTrace.hpp"
#include "UniqueLibs/nativeMaterializer.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include <QDirIterator>
#include <QBuffer>
#include <QtCore/private/qzipwriter_p.h>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWebEngineView>
#include "UniqueLibs/document/hwpPrintSnapshot.hpp"
#include "UniqueLibs/document/hwpPrintBridge.hpp"
#include "UniqueLibs/document/hwpExportTransfer.hpp"
#include <QWebEnginePage>
#include <QtTest>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <QScopeGuard>
using namespace hc::document;
namespace
{
    class InputLease final : public hc::storage::IMaterializedFileLease
    {
    public:
        explicit InputLease(std::string Path) : path_(std::move(Path)) {}
        std::string LocalPath() const override { return path_; }
        hc::storage::EntrySnapshot Describe() const override { return {}; }
        std::size_t Depth() const override { return 0; }
    private:
        std::string path_;
    };
    class RetirementLease final : public hc::storage::IMaterializedFileLease
    {
    public:
        std::string LocalPath() const override { return "fixture.hwp"; }
        hc::storage::EntrySnapshot Describe() const override { return {}; }
        std::size_t Depth() const override { return 0; }
    };
    QByteArray ValidInput(bool Restricted = false)
    {
        QByteArray Bytes; QBuffer Buffer(&Bytes);
        if (!Buffer.open(QIODevice::WriteOnly)) return {};
        QZipWriter Writer(&Buffer); Writer.setCompressionPolicy(QZipWriter::NeverCompress);
        Writer.addFile("Contents/content.hpf", QByteArray("<package/>"));
        Writer.addFile("Contents/header.xml", QByteArray("<header/>"));
        Writer.addFile("BinData/payload.bin", QByteArray(400 * 1024, 'x'));
        if (Restricted) Writer.addFile("META-INF/signatures.xml", QByteArray("<signature/>"));
        Writer.close(); return Writer.status() == QZipWriter::NoError ? Bytes : QByteArray{};
    }
    const QByteArray Html("<!doctype html><title>pinned</title><script>document.title='executed';</script>");
    QByteArray Hash(const QByteArray& Bytes) { return QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex(); }
    bool Write(const QString& Path, const QByteArray& Bytes)
    { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
}
class BundleTests final : public QObject
{
    Q_OBJECT
private slots:
    void smallResourceHasBoundedCapacity()
    {
        QTemporaryDir Dir; QString Error;
        QVERIFY(Write(Dir.filePath("index.html"), Html));
        auto Bundle = HwpBundle::LoadPinned(Dir.path(), {{"index.html", Hash(Html)}}, Error);
        QVERIFY2(Bundle, qPrintable(Error));
        const auto Bytes = Bundle->Bytes(QUrl("rhwp://bundle/index.html"));
        QCOMPARE(Bytes, Html);
        QVERIFY2(Bytes.capacity() < 4096, "Small resources must not retain the 64 MiB per-file ceiling");
    }
    void embeddedBundle()
    {
#ifndef HC_RHWP_EMBEDDED_BUNDLE
        QSKIP("No embedded bundle configured");
#else
        const auto Root = QStringLiteral(":/hc/rhwp/bundle");
        QFile Manifest(Root + "/asset-manifest.json"); QVERIFY(Manifest.open(QIODevice::ReadOnly));
        const auto Bytes = Manifest.readAll();
        const auto Object = QJsonDocument::fromJson(Bytes).object();
        QMap<QString, QByteArray> Pins;
        for (const auto& Value : Object.value("files").toArray()) {
            const auto Entry = Value.toObject();
            Pins.insert(Entry.value("path").toString(), Entry.value("sha256").toString().toLatin1());
        }
        QString Error;
        auto Bundle = HwpBundle::LoadPinned(Root, Pins, Error); QVERIFY2(Bundle, qPrintable(Error));
        QVERIFY(!Bundle->Bytes(QUrl("rhwp://bundle/rhwp_bg.wasm")).isEmpty());
        QVERIFY(!Bundle->Allows(QUrl("qrc:/hc/rhwp/bundle/index.html")));
        QVERIFY(!LoadApprovedHwpPackage(Root, {}, Error));
        QCOMPARE(Error, QStringLiteral("rhwp-package-policy-unavailable"));
        const auto Approved = LoadApprovedHwpPackage(Root, Hash(Bytes), Error);
        QCOMPARE(bool(Approved), Object.value("releaseEligible").toBool());
        if (!Approved) QCOMPARE(Error, QStringLiteral("rhwp-package-not-release-eligible"));
        const auto Corpus = qEnvironmentVariable("HC_RHWP_TEST_CORPUS");
        QVERIFY2(!Corpus.isEmpty(), "Embedded engine test requires the pinned corpus");
        QFile Input(Corpus + "/c02.hwp"); QVERIFY(Input.open(QIODevice::ReadOnly));
        const auto InputBytes = Input.readAll();
        QCOMPARE(Hash(InputBytes), QByteArray("86e607233a2218928fabe5969155116a46d4ab4400db1402fe45b0082149496c"));
        HwpHelperDocument Document(Bundle);
        const QJsonObject Identity{{"documentId", "embedded-document"}, {"generation", "2"},
            {"token", "3"}, {"bindingOwner", "4"}, {"bindingGeneration", "5"}, {"epoch", "6"}};
        auto Command = [&](const char* Type) {
            return QJsonObject{{"version", 1}, {"type", Type}, {"identity", Identity}};
        };
        auto Begin = Command("InputBegin"); Begin["format"] = "hwp"; Begin["readOnly"] = true;
        Begin["byteLength"] = InputBytes.size(); Begin["sha256"] = QString::fromLatin1(Hash(InputBytes));
        QCOMPARE(Document.Handle(Begin)["type"].toString(), QString("InputAccepted"));
        for (qsizetype Offset = 0; Offset < InputBytes.size(); Offset += 192 * 1024) {
            auto Chunk = Command("InputChunk"); Chunk["offset"] = Offset;
            Chunk["data"] = QString::fromLatin1(InputBytes.mid(Offset, 192 * 1024).toBase64());
            QCOMPARE(Document.Handle(Chunk)["type"].toString(), QString("InputChunkAccepted"));
        }
        QCOMPARE(Document.Handle(Command("InputCommit"))["type"].toString(), QString("InputVerified"));
        QCOMPARE(Document.Handle(Command("DocumentStart"))["state"].toString(), QString("loading"));
        QJsonObject Status;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            Status = Document.Handle(Command("DocumentStatus"));
            return Status["state"] == QJsonValue("ready") || Status["state"] == QJsonValue("failed");
        })(), 20000);
        QCOMPARE(Status["state"].toString(), QString("ready"));
        Pins["index.html"] = QByteArray(64, '0');
        QVERIFY(!HwpBundle::LoadPinned(Root, Pins, Error));
        QCOMPARE(Error, QStringLiteral("rhwp-bundle-hash"));
#endif
    }

    void viewportBundle_data()
    {
        QTest::addColumn<QString>("Format");QTest::newRow("hwp")<<"hwp";QTest::newRow("hwpx")<<"hwpx";
    }
    void viewportBundle()
    {
        QFETCH(QString,Format);
        const auto Root=qEnvironmentVariable("HC_RHWP_TEST_EDITOR_BUNDLE"),Corpus=qEnvironmentVariable("HC_RHWP_TEST_CORPUS"),Evidence=qEnvironmentVariable("HC_RHWP_TEST_PRINT_EVIDENCE");
        QVERIFY(!Root.isEmpty());QFile Manifest(Root+"/asset-manifest.json");QVERIFY(Manifest.open(QIODevice::ReadOnly));
        QMap<QString,QByteArray>Pins;for(const auto& V:QJsonDocument::fromJson(Manifest.readAll()).object()["files"].toArray()){
            auto O=V.toObject();Pins[O["path"].toString()]=O["sha256"].toString().toLatin1();}
        QString Error;auto Bundle=HwpBundle::LoadPinned(Root,Pins,Error);QVERIFY2(Bundle,qPrintable(Error));
        QFile Input(Corpus+(Format=="hwp"?"/c06.hwp":"/c07.hwpx"));QVERIFY(Input.open(QIODevice::ReadOnly));const auto Bytes=Input.readAll();
        QCOMPARE(Hash(Bytes),QByteArray(Format=="hwp"?"a854b822c3150d9731b8ba0687160b2120be1078fc5c91221566a15176de3e20":"4405f89db156da688d1a10ae0c0829f0159ad24ad4ad5e083902577a8304338a"));
        HwpHelperDocument Document(Bundle);
        const QJsonObject Identity{{"documentId","viewport-test"},{"generation","2"},{"token","3"},{"bindingOwner","4"},{"bindingGeneration","5"},{"epoch","4"}};
        const auto Command=[&](const char* Type){return QJsonObject{{"version",1},{"type",Type},{"identity",Identity}};};
        auto Begin=Command("InputBegin");Begin["format"]=Format;Begin["readOnly"]=true;Begin["byteLength"]=Bytes.size();Begin["sha256"]=QString::fromLatin1(Hash(Bytes));
        QCOMPARE(Document.Handle(Begin)["type"].toString(),QString("InputAccepted"));
        for(qsizetype Offset=0;Offset<Bytes.size();Offset+=192*1024){auto C=Command("InputChunk");C["offset"]=Offset;C["data"]=QString::fromLatin1(Bytes.mid(Offset,192*1024).toBase64());QCOMPARE(Document.Handle(C)["type"].toString(),QString("InputChunkAccepted"));}
        QCOMPARE(Document.Handle(Command("InputCommit"))["type"].toString(),QString("InputVerified"));
        QCOMPARE(Document.Handle(Command("DocumentStart"))["state"].toString(),QString("loading"));
        QTRY_COMPARE_WITH_TIMEOUT(Document.Handle(Command("DocumentStatus"))["state"].toString(),QString("ready"),20000);
        QWebEngineView* View=nullptr;for(auto* W:QApplication::allWidgets())if(auto* C=qobject_cast<QWebEngineView*>(W);C&&C->url().path()=="/index.html")View=C;
        QVERIFY(View);
        const auto Eval=[&](const QString& Script){struct Reply{bool done=false;QVariant value;};auto R=std::make_shared<Reply>();
            View->page()->runJavaScript(Script,[R](const QVariant& V){R->value=V;R->done=true;});QElapsedTimer T;T.start();while(!R->done&&T.elapsed()<5000)QTest::qWait(10);return R->value;};
        QCOMPARE(Eval("document.querySelectorAll('.rhwp-page').length").toInt(),5);
        QVERIFY(Eval("document.querySelector('[data-page=\"0\"]').dataset.painted==='true'").toBool());
        QVERIFY(!Eval("document.querySelector('[data-page=\"4\"]').dataset.painted==='true'").toBool());
        if(!Evidence.isEmpty())QVERIFY(View->grab().save(Evidence+"/viewport-first-"+Format+".png"));
        // Print the complete immutable snapshot while the last view page is still unpainted.
        const DocumentOperationTicket Ticket{"viewport-test",2,7,5,DocumentOperation::Printing,{4,0,Hash(Bytes).toStdString()},{}};
        HwpPrintTransfer Printed(Ticket,{},30000,5);auto Print=Command("DocumentPrint");Print["printIdentity"]=HwpPrintIdentity(Ticket,5);Print["pages"]=QJsonArray{};
        QVERIFY(Document.Handle(Print)["ok"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(([&]{const auto R=Document.Handle(Command("DocumentPrintPoll"));if(R["printMessage"].isObject())Printed.Handle(R["printMessage"].toObject());return Printed.Ready()||!Printed.Error().isEmpty();})(),30000);
        QVERIFY2(Printed.Ready(),qPrintable(Printed.Error()));QCOMPARE(Printed.Pages().size(),size_t(5));
        const auto Pdf=BuildHwpPdf(Ticket,Printed.Pages(),Printed.Frames());QVERIFY(Pdf.HasValue());
        if(!Evidence.isEmpty())QVERIFY(Write(Evidence+"/viewport-offscreen-"+Format+".pdf",Pdf.Value()));
        QJsonArray Navigation;
        for(const auto Page:{2,4,0,4,2,0}){
            Eval(QString("document.querySelector('[data-page=\"%1\"]').scrollIntoView();true").arg(Page));
            QTRY_VERIFY_WITH_TIMEOUT(Eval(QString("document.querySelector('[data-page=\"%1\"]').dataset.painted==='true'").arg(Page)).toBool(),10000);
            QCOMPARE(Document.Handle(Command("DocumentStatus"))["state"].toString(),QString("ready"));
            Navigation.append(Page);if(!Evidence.isEmpty())QVERIFY(View->grab().save(Evidence+QString("/viewport-%1-%2.png").arg(Format).arg(Page)));
        }
        // Rapid changes replace priorities; first/middle/last stay accessible.
        Eval("for(const P of [4,0,3,1,4])document.querySelector('[data-page=\"'+P+'\"]').scrollIntoView();true");
        QTRY_VERIFY_WITH_TIMEOUT(Eval("document.querySelector('[data-page=\"4\"]').dataset.painted==='true'").toBool(),10000);
        if(!Evidence.isEmpty())QVERIFY(Write(Evidence+"/viewport-"+Format+".json",QJsonDocument(QJsonObject{{"firstPainted",true},{"lastUnpaintedBeforePrint",true},{"printedPages",5},{"navigation",Navigation},{"readyMaintained",true}}).toJson()));
        Input.seek(0);QCOMPARE(Input.readAll(),Bytes);
    }

    void nativeEditorBundle_data()
    {
        QTest::addColumn<QString>("Fixture"); QTest::addColumn<QString>("Format");
        QTest::newRow("hwp") << "c02.hwp" << "hwp";
        QTest::newRow("hwpx") << "c05.hwpx" << "hwpx";
    }
    void nativeEditorBundle()
    {
        QFETCH(QString, Fixture); QFETCH(QString, Format);
        const auto Root = qEnvironmentVariable("HC_RHWP_TEST_EDITOR_BUNDLE");
        if (Root.isEmpty()) QSKIP("Native editor candidate not requested");
        QFile Manifest(Root + "/asset-manifest.json"); QVERIFY(Manifest.open(QIODevice::ReadOnly));
        const auto Bytes = Manifest.readAll();
        const auto Object = QJsonDocument::fromJson(Bytes).object();
        QMap<QString, QByteArray> Pins;
        for (const auto& Value : Object.value("files").toArray()) {
            const auto Entry = Value.toObject();
            Pins.insert(Entry.value("path").toString(), Entry.value("sha256").toString().toLatin1());
        }
        QString Error;
        auto Bundle = HwpBundle::LoadPinned(Root, Pins, Error); QVERIFY2(Bundle, qPrintable(Error));
        QVERIFY(!Bundle->Bytes(QUrl("rhwp://bundle/rhwp_bg.wasm")).isEmpty());
        QVERIFY(!Bundle->Allows(QUrl("qrc:/hc/rhwp/bundle/index.html")));
        QVERIFY(!LoadApprovedHwpPackage(Root, {}, Error));
        QCOMPARE(Error, QStringLiteral("rhwp-package-policy-unavailable"));
        const auto Approved = LoadApprovedHwpPackage(Root, Hash(Bytes), Error);
        QCOMPARE(bool(Approved), Object.value("releaseEligible").toBool());
        if (!Approved) QCOMPARE(Error, QStringLiteral("rhwp-package-not-release-eligible"));
        const auto Corpus = qEnvironmentVariable("HC_RHWP_TEST_CORPUS");
        QVERIFY2(!Corpus.isEmpty(), "Embedded engine test requires the pinned corpus");
        QFile Input(Corpus + "/" + Fixture); QVERIFY(Input.open(QIODevice::ReadOnly));
        const auto InputBytes = Input.readAll();
        QCOMPARE(Hash(InputBytes), QByteArray(Format == "hwp" ? "86e607233a2218928fabe5969155116a46d4ab4400db1402fe45b0082149496c" : "8d9df52bb09803fa1754317360e27709621555b3e1163a0d7eb4ad9abab5b26b"));
        HwpHelperDocument Document(Bundle, 0, 30000, true);
        const QJsonObject Identity{{"documentId", "embedded-document"}, {"generation", "2"},
            {"token", "3"}, {"bindingOwner", "4"}, {"bindingGeneration", "5"}, {"epoch", "4"}};
        auto Command = [&](const char* Type) {
            return QJsonObject{{"version", 1}, {"type", Type}, {"identity", Identity}};
        };
        auto Begin = Command("InputBegin"); Begin["format"] = Format; Begin["readOnly"] = true;
        Begin["byteLength"] = InputBytes.size(); Begin["sha256"] = QString::fromLatin1(Hash(InputBytes));
        QCOMPARE(Document.Handle(Begin)["type"].toString(), QString("InputAccepted"));
        for (qsizetype Offset = 0; Offset < InputBytes.size(); Offset += 192 * 1024) {
            auto Chunk = Command("InputChunk"); Chunk["offset"] = Offset;
            Chunk["data"] = QString::fromLatin1(InputBytes.mid(Offset, 192 * 1024).toBase64());
            QCOMPARE(Document.Handle(Chunk)["type"].toString(), QString("InputChunkAccepted"));
        }
        QCOMPARE(Document.Handle(Command("InputCommit"))["type"].toString(), QString("InputVerified"));
        QCOMPARE(Document.Handle(Command("DocumentStartEdit"))["state"].toString(), QString("loading"));
        QJsonObject Status;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            Status = Document.Handle(Command("DocumentStatus"));
            return Status["state"] == QJsonValue("ready") || Status["state"] == QJsonValue("failed");
        })(), 20000);
        QCOMPARE(Status["state"].toString(), QString("ready"));
        QVERIFY(Status["editor"].isObject());
        QCOMPARE(Status["editor"].toObject()["sequence"].toString(), QString("0"));
        QWebEngineView* EditorView = nullptr;
        for (auto* Widget : QApplication::allWidgets()) {
            auto* Candidate = qobject_cast<QWebEngineView*>(Widget);
            if (Candidate && Candidate->url().path() == "/editor/index.html") EditorView = Candidate;
        }
        QVERIFY(EditorView);
        bool Inserted = false;
        EditorView->page()->runJavaScript(R"JS((() => {
            const input = document.querySelector('textarea[aria-label="문서 편집 입력"]');
            if (!input) return false;
            input.focus(); input.value = 'H2NativeEdit';
            input.dispatchEvent(new InputEvent('input', {bubbles:true, inputType:'insertText', data:'H2NativeEdit'}));
            return true;
        })())JS", [&](const QVariant& Value) { Inserted = Value.toBool(); });
        QTRY_VERIFY_WITH_TIMEOUT(Inserted, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            Status = Document.Handle(Command("DocumentStatus"));
            // Studio publishes both the input mutation and document change.
            // Export only after both reports reach the native endpoint.
            return Status["editor"].toObject()["sequence"].toString().toULongLong() >= 2;
        })(), 5000);
        // Qt/WebEngine event delivery is distinct from installed OS key acceptance.
        EditorView->window()->activateWindow();EditorView->setFocus();
        for(int Focus=0;Focus<2;++Focus) {
            bool Focused=false;
            EditorView->page()->runJavaScript(Focus==0?
                "(()=>{const I=document.querySelector('textarea[aria-label=\"문서 편집 입력\"]');I?.focus();return document.activeElement===I;})()":
                "(()=>{const I=document.querySelector('#font-size');I?.focus();return !!I&&document.activeElement===I;})()",
                [&](const QVariant& V){Focused=V.toBool();});
            QTRY_VERIFY2_WITH_TIMEOUT(Focused,Focus==0?"body focus":"ribbon font-size focus",5000);
            auto* Receiver=EditorView->focusProxy()?EditorView->focusProxy():EditorView;
            Receiver->setFocus();QTest::keyClick(Receiver,Qt::Key_S,Qt::ControlModifier);
            QString Action;
            QTRY_VERIFY_WITH_TIMEOUT(([&]{if(!Action.isEmpty())return true;Action=Document.Handle(Command("DocumentEditorPoll"))["action"].toString();return !Action.isEmpty();})(),5000);
            QCOMPARE(Action,QString("save"));
            QCOMPARE(Document.Handle(Command("DocumentEditorPoll"))["action"].toString(),QString{});
            Action.clear();QTest::keyClick(Receiver,Qt::Key_S,Qt::ControlModifier|Qt::ShiftModifier);
            QTRY_VERIFY_WITH_TIMEOUT(([&]{if(!Action.isEmpty())return true;Action=Document.Handle(Command("DocumentEditorPoll"))["action"].toString();return !Action.isEmpty();})(),5000);
            QCOMPARE(Action,QString("saveAs"));
            QCOMPARE(Document.Handle(Command("DocumentEditorPoll"))["action"].toString(),QString{});
            Action.clear();QTest::keyClick(Receiver,Qt::Key_P,Qt::ControlModifier);
            QTRY_VERIFY_WITH_TIMEOUT(([&]{if(!Action.isEmpty())return true;Action=Document.Handle(Command("DocumentEditorPoll"))["action"].toString();return !Action.isEmpty();})(),5000);
            QCOMPARE(Action,QString("print"));
            QCOMPARE(Document.Handle(Command("DocumentEditorPoll"))["action"].toString(),QString{});
        }
        const auto Model = Status["editor"].toObject();
        const DocumentOperationTicket ExportTicket{"embedded-document", 2, 4, 5, DocumentOperation::Exporting,
            {4, Model["sequence"].toString().toULongLong(), Model["savepoint"].toString().toStdString()}, {}};
        HwpExportTransfer Transfer(ExportTicket, 4, 5, Format);
        auto Export = Command("DocumentEditorExport"); Export["format"] = Format;
        Export["exportIdentity"] = Transfer.Identity();
        const auto Accepted = Document.Handle(Export);
        QVERIFY2(Accepted["ok"].toBool(), QJsonDocument(Accepted).toJson().constData());
        QString ExportError;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            const auto Reply = Document.Handle(Command("DocumentEditorPoll"));
            ExportError = Reply["editor"].toObject()["error"].toString();
            if (Reply["message"].isObject()) Transfer.Handle(Reply["message"].toObject());
            return !ExportError.isEmpty() || Transfer.CurrentState() == HwpExportTransfer::State::Ready
                || Transfer.CurrentState() == HwpExportTransfer::State::Failed;
        })(), 30000);
        QVERIFY2(ExportError.isEmpty(), qPrintable(ExportError));
        QCOMPARE(Transfer.CurrentState(), HwpExportTransfer::State::Ready);
        const auto Payload = Transfer.Take(); QVERIFY(Payload);
        QVERIFY(!Payload->bytes_.empty());
        const auto Encoded = QString::fromLatin1(QByteArray::fromStdString(Payload->bytes_).toBase64());
        // Reopen the exported container with a fresh engine document. This checks
        // persistence of the actual input, beyond transport integrity alone.
        EditorView->page()->runJavaScript(QString(R"JS((async () => {
            try {
                const Engine = await import('rhwp://bundle/rhwp.js'); await Engine.default();
                const Bytes = Uint8Array.from(atob('%1'), C => C.charCodeAt(0));
                const Doc = new Engine.HwpDocument(Bytes);
                try { window.hcRoundtrip = Doc.getTextFileText().includes('H2NativeEdit') ? 'ok' : 'missing'; }
                finally { Doc.free(); }
            } catch (Error) { window.hcRoundtrip = 'failed:' + String(Error); }
        })())JS").arg(Encoded));
        const auto Roundtrip = std::make_shared<QString>();
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            if (!Roundtrip->isEmpty()) return true;
            EditorView->page()->runJavaScript("window.hcRoundtrip || ''", [Roundtrip](const QVariant& Value) { *Roundtrip = Value.toString(); });
            return false;
        })(), 10000);
        QCOMPARE(*Roundtrip, QString("ok"));
        Input.seek(0); QCOMPARE(Input.readAll(), InputBytes);

        const DocumentOperationTicket PrintTicket{"embedded-document",2,6,5,DocumentOperation::Printing,
            {4,Model["sequence"].toString().toULongLong(),Model["savepoint"].toString().toStdString()},{}};
        HwpPrintTransfer Printed(PrintTicket,{},30000,5);
        auto PrintRequest=Command("DocumentPrint");PrintRequest["printIdentity"]=HwpPrintIdentity(PrintTicket,5);PrintRequest["pages"]=QJsonArray{};
        QVERIFY(Document.Handle(PrintRequest)["ok"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(([&]{auto Reply=Document.Handle(Command("DocumentEditorPoll"));
            if(Reply["printMessage"].isObject())Printed.Handle(Reply["printMessage"].toObject());
            return Printed.Ready()||!Printed.Error().isEmpty();})(),30000);
        QVERIFY2(Printed.Ready(),qPrintable(Printed.Error()));
        const auto Pdf=BuildHwpPdf(PrintTicket,Printed.Pages(),Printed.Frames());QVERIFY(Pdf.HasValue());
        const auto Evidence=qEnvironmentVariable("HC_RHWP_TEST_PRINT_EVIDENCE");
        if(!Evidence.isEmpty()) {
            QVERIFY(Write(Evidence+"/unsaved-"+Format+".pdf",Pdf.Value()));
            EditorView->grab().save(Evidence+"/unsaved-"+Format+".png");
        }
        const auto AfterPrint=Document.Handle(Command("DocumentStatus"))["editor"].toObject();
        QCOMPARE(AfterPrint["sequence"],Model["sequence"]);QCOMPARE(AfterPrint["savepoint"],Model["savepoint"]);
        Input.seek(0);QCOMPARE(Input.readAll(),InputBytes);
        Pins["index.html"] = QByteArray(64, '0');
        QVERIFY(!HwpBundle::LoadPinned(Root, Pins, Error));
        QCOMPARE(Error, QStringLiteral("rhwp-bundle-hash"));
    }
    void workerFirstFrame_data()
    {
        QTest::addColumn<QByteArray>("Mode"); QTest::addColumn<bool>("Cancel");
        QTest::newRow("ready-and-close") << QByteArray("ready") << false;
        QTest::newRow("reentrant-ready-stop") << QByteArray("reentrant-stop") << false;
        QTest::newRow("whole-load-deadline") << QByteArray("hang-start") << false;
        QTest::newRow("cancel-during-helper-stall") << QByteArray("hang-start") << true;
        QTest::newRow("helper-crash-after-ready") << QByteArray("crash-after-ready") << false;
        QTest::newRow("deferred-native-watchdog") << QByteArray("view-hang") << false;
        QTest::newRow("close-during-deferred-render") << QByteArray("view-hang-close") << false;
        QTest::newRow("deferred-render-failed") << QByteArray("view-failed") << false;
        QTest::newRow("stale-status") << QByteArray("stale-status") << false;
    }
    void workerFirstFrame()
    {
        QFETCH(QByteArray, Mode); QFETCH(bool, Cancel);
        QTemporaryDir Dir; QVERIFY(Dir.isValid());
        const QString SourceRoot = QString::fromUtf8(HC_LOAD_FIXTURE_DIR);
        QDirIterator Files(SourceRoot, QDir::Files, QDirIterator::Subdirectories);
        WebProcessRuntime Runtime; Runtime.root_ = Dir.path();
        while(Files.hasNext()) {
            const auto SourcePath = Files.next();
            auto Relative = QDir(SourceRoot).relativeFilePath(SourcePath);
            if(Relative.endsWith(".pdb") || Relative.endsWith(".lib") || Relative.endsWith(".exp")) continue;
            if(Relative == "hc_hwp_load_fixture.exe") Relative = "HyperEditorWeb.exe";
            const auto Destination = Dir.filePath(Relative);
            QVERIFY(QDir().mkpath(QFileInfo(Destination).path()));
            QVERIFY(QFile::copy(SourcePath, Destination));
        }
        QVERIFY(Write(Dir.filePath("index.html"), fixture::Page));
        QVERIFY(Write(Dir.filePath("app.js"), Mode.startsWith("view-")?fixture::DeferredScript(Mode.startsWith("view-hang")):fixture::Script));
        QVERIFY(Write(Dir.filePath("mode.txt"), Mode));
        QDirIterator Pins(Dir.path(), QDir::Files, QDirIterator::Subdirectories);
        while(Pins.hasNext()) {
            const auto Path = Pins.next(); QFile File(Path); QVERIFY(File.open(QIODevice::ReadOnly));
            Runtime.sha256_[QDir(Dir.path()).relativeFilePath(Path)] = Hash(File.readAll());
        }
        const auto Path = Dir.filePath("input.hwpx"); const QByteArray Bytes = ValidInput();
        QVERIFY(!Bytes.isEmpty());
        QVERIFY(Write(Path, Bytes));
        SourceIdentity Source; Source.localPath_ = Path.toUtf8().toStdString(); Source.inputSha256_ = Hash(Bytes).toStdString();
        auto Lease = std::make_shared<InputLease>(Source.localPath_); std::weak_ptr<InputLease> Weak = Lease;
        Source.lease_ = Lease;
        DocumentOperationTicket Ticket; Ticket.documentId_ = "worker-alpha"; Ticket.generation_ = 2;
        Ticket.token_ = 3; Ticket.operation_ = DocumentOperation::Loading; Ticket.lease_ = Lease;
        auto Backend = std::make_unique<HwpProcessBackend>(Runtime, WebProcessLimits{8, 2048ULL*1024*1024, 10000},
            Mode == "hang-start" && !Cancel ? 4500 : 30000);
        Backend->resize(900, 700); Backend->show();
        QSignalSpy Loaded(Backend.get(), &HwpSdiBackend::Loaded), Stopped(Backend.get(), &HwpSdiBackend::Stopped);
        QSignalSpy Cancelled(Backend.get(), &HwpSdiBackend::Cancelled);
        int Ticks = 0; QTimer Heartbeat; Heartbeat.setInterval(10);
        connect(&Heartbeat, &QTimer::timeout, this, [&] { ++Ticks; }); Heartbeat.start();
        bool NestedStopped = false;
        if(Mode == "reentrant-stop") connect(Backend.get(), &HwpSdiBackend::Loaded, this,
            [&](quint64, const DocumentOperationTicket&, const HwpLoadResult& Result) {
                if(!Result.HasValue()) return;
                QEventLoop Loop;
                // Qt suppresses recursion of the same timer event. Explicitly
                // dispatch its public signal from a second timer to exercise the
                // observer-driven reentry contract rather than waiting for it.
                QTimer Reenter; Reenter.setInterval(10);
                connect(&Reenter, &QTimer::timeout, &Loop, [&] {
                    QMetaObject::invokeMethod(Backend->findChild<QTimer*>(), "timeout", Qt::DirectConnection);
                });
                Reenter.start();
                connect(Backend.get(), &HwpSdiBackend::Stopped, &Loop, &QEventLoop::quit);
                Backend->Stop(99); QTimer::singleShot(5000, &Loop, &QEventLoop::quit); Loop.exec();
                NestedStopped = Stopped.size() == 1;
            });
        Backend->Load(99, Ticket, Source);
        const auto Native = Backend->NativeBinding();
        Source.lease_.reset(); Lease.reset();
        const auto IsLocked = [&] { QFile File(Path); return !File.open(QIODevice::ReadWrite); };
        if(Cancel) {
            QTRY_VERIFY_WITH_TIMEOUT(IsLocked(), 15000);
            Backend->Cancel(99, Ticket); QVERIFY(!Weak.expired()); QCOMPARE(Stopped.size(), 0);
        }
        Ticket.lease_.reset();
        if(Mode == "ready" || Mode == "crash-after-ready" || Mode.startsWith("view-")) {
            QTRY_COMPARE_WITH_TIMEOUT(Loaded.size(), 1, 20000);
            const auto Result = *static_cast<const HwpLoadResult*>(Loaded.first()[2].constData());
            QVERIFY2(Result.HasValue(), Result.HasValue() ? "" : Result.Failure().code_.c_str());
            QCOMPARE(Result.Value().epoch_, Native.owner_); QCOMPARE(Result.Value().savepoint_, Source.inputSha256_);
            QVERIFY(IsLocked()); QVERIFY(!Weak.expired()); QCOMPARE(Stopped.size(), 0);
            Backend->resize(640, 480);
            if(Mode=="view-hang-close") {
                QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(Dir.filePath("deferred-running.txt")),5000);
                QVERIFY(IsLocked());QCOMPARE(Stopped.size(),0);Backend->Stop(99);
            }
            if(Mode == "ready") Backend->Stop(99);
        }
        QTRY_COMPARE_WITH_TIMEOUT(Stopped.size(), 1, 15000);
        QVERIFY(HwpProcessBackend::IsDrained()); QVERIFY(!IsLocked()); QVERIFY(Ticks > 5);
        if(Cancel) { QCOMPARE(Cancelled.size(), 1); QCOMPARE(Loaded.size(), 0); }
        else {
            QCOMPARE(Loaded.size(), 1);
            const auto Result = *static_cast<const HwpLoadResult*>(Loaded.first()[2].constData());
            QCOMPARE(Result.HasValue(), Mode == "ready" || Mode == "crash-after-ready" || Mode.startsWith("view-") || Mode == "reentrant-stop");
            if(Mode == "reentrant-stop") QVERIFY(NestedStopped);
        }
        Loaded.clear(); Cancelled.clear(); // Spies deliberately retain full ticket leases.
        Backend.reset(); QVERIFY(Weak.expired());
    }
    void nativeSourcePreparation_data()
    {
        QTest::addColumn<QString>("Mode");
        for (const auto* Mode : {"missing", "cancelled", "deadline", "unsupported", "oversized", "stale", "protected-copy"})
            QTest::newRow(Mode) << QString::fromLatin1(Mode);
    }
    void nativeSourcePreparation()
    {
        QFETCH(QString, Mode);
        using namespace hc::storage;
        QTemporaryDir Dir; QVERIFY(Dir.isValid());
        const auto Path = Dir.filePath(Mode == "unsupported" ? "document.txt" : "document.hwpx");
        const auto Bytes = ValidInput(); QVERIFY(Write(Path, Bytes));
        auto Provider = MakeNativeReadProvider(LocatorKind::Local);
        auto Entry = Provider->Resolve({LocatorKind::Local, Path.toUtf8().toStdString()}, {}).get();
        QVERIFY(Entry.HasValue());
        auto Store = MakeNativeMaterializer(Dir.path().toUtf8().toStdString()); QVERIFY(Store.HasValue());
        if (Mode == "oversized") Entry.Value().size_ = 64ULL * 1024 * 1024 + 1;
        MaterializationRequest Request;
        if (Mode != "missing") Request.source_ = std::make_shared<ProviderContentSource>(Provider, Entry.Value());
        OperationContext Context; std::stop_source Stop;
        if (Mode == "cancelled") { Context.stopToken_ = Stop.get_token(); Stop.request_stop(); }
        if (Mode == "deadline") Context.deadline_ = std::chrono::steady_clock::now();
        if (Mode == "stale") QVERIFY(Write(Path, "changed before acquisition"));
        auto Prepared = PrepareHwpSource(*Store.Value(), std::move(Request), Context);
        if (Mode == "protected-copy") {
            QVERIFY(Prepared.HasValue()); auto Source = std::move(Prepared.Value());
            // The native lease itself prevents replacement for its whole lifetime.
            QVERIFY(!Write(QString::fromUtf8(Source.localPath_), "replaced after acquisition"));
            Source.inputSha256_ = Hash("wrong expected bytes").toStdString();
            DocumentOperationTicket Ticket{"protected-copy", 1, 2, 0, DocumentOperation::Loading, {}, Source.lease_};
            QString Error; QVERIFY(!HwpAdmittedInput::Read(Ticket, Source, 1, 1, 1, "hwpx", Error));
            QCOMPARE(Error, QString("rhwp-input-integrity"));
            QFile Original(Path); QVERIFY(Original.open(QIODevice::ReadOnly)); QCOMPARE(Original.readAll(), Bytes);
        } else {
            QVERIFY(!Prepared.HasValue());
            if (Mode == "missing") QCOMPARE(Prepared.Failure().code_, std::string("rhwp-source-required"));
            if (Mode == "cancelled") QCOMPARE(Prepared.Failure().category_, ErrorCategory::Cancelled);
            if (Mode == "deadline") QCOMPARE(Prepared.Failure().code_, std::string("deadline-exceeded"));
            if (Mode == "unsupported") QCOMPARE(Prepared.Failure().code_, std::string("rhwp-source-format"));
            if (Mode == "oversized") QCOMPARE(Prepared.Failure().code_, std::string("rhwp-input-budget"));
            if (Mode == "stale") QCOMPARE(Prepared.Failure().category_, ErrorCategory::StaleRevision);
        }
        const auto Cleanup = Store.Value()->DrainCleanup(); QVERIFY(Cleanup.errors_.empty());
        QCOMPARE(Cleanup.busy_, std::size_t{0});
    }
    void isolatedEngineDocument_data()
    {
        QTest::addColumn<QString>("Fixture"); QTest::addColumn<QString>("Format"); QTest::addColumn<bool>("Success");
        QTest::newRow("hwp-body") << "c02.hwp" << "hwp" << true;
        QTest::newRow("async-product-open") << "c02.hwp" << "hwp" << true;
        QTest::newRow("host-session-open") << "c02.hwp" << "hwp" << true;
        QTest::newRow("hwpx-table") << "c05.hwpx" << "hwpx" << true;
        QTest::newRow("five-page-hwp") << "c06.hwp" << "hwp" << true;
        QTest::newRow("five-page-hwpx") << "c07.hwpx" << "hwpx" << true;
        QTest::newRow("format-mismatch") << "c02.hwp" << "hwpx" << false;
        QTest::newRow("damaged-input") << "damaged" << "hwp" << false;
        QTest::newRow("password-before-renderer") << "c14.hwp" << "hwp" << false;
    }
    void isolatedEngineDocument()
    {
        QFETCH(QString, Fixture); QFETCH(QString, Format); QFETCH(bool, Success);
        const auto Site = qEnvironmentVariable("HC_RHWP_TEST_ENGINE"), Corpus = qEnvironmentVariable("HC_RHWP_TEST_CORPUS");
        if(Site.isEmpty() || Corpus.isEmpty()) QSKIP("Local pinned diagnostic engine/corpus not requested");
        const auto Read = [](const QString& Path) {
            QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{};
        };
        QTemporaryDir Dir; QVERIFY(Dir.isValid());
        const QString SourceRoot = QString::fromUtf8(HC_LOAD_FIXTURE_DIR);
        QDirIterator Files(SourceRoot, QDir::Files, QDirIterator::Subdirectories);
        while(Files.hasNext()) {
            const auto SourcePath = Files.next();
            auto Relative = QDir(SourceRoot).relativeFilePath(SourcePath);
            if(Relative.endsWith(".pdb") || Relative.endsWith(".lib") || Relative.endsWith(".exp")) continue;
            if(Relative == "hc_hwp_load_fixture.exe") Relative = "HyperEditorWeb.exe";
            const auto Destination = Dir.filePath(Relative);
            QVERIFY(QDir().mkpath(QFileInfo(Destination).path()));
            QVERIFY(QFile::copy(SourcePath, Destination));
        }
        const auto Js = Read(Site + "/rhwp.js"), Wasm = Read(Site + "/rhwp_bg.wasm");
        const auto Adapter = Read(QFINDTESTDATA("../../UniqueLibs/document/rhwp/readOnlyAdapter.js"));
        QCOMPARE(Hash(Js), fixture::EngineJsSha); QCOMPARE(Hash(Wasm), fixture::EngineWasmSha);
        QCOMPARE(Hash(Adapter), fixture::AdapterSha);
        QVERIFY(Write(Dir.filePath("rhwp.js"), Js)); QVERIFY(Write(Dir.filePath("rhwp_bg.wasm"), Wasm));
        QVERIFY(Write(Dir.filePath("adapter.js"), Adapter));
        QVERIFY(Write(Dir.filePath("index.html"), fixture::EnginePage));
        QVERIFY(Write(Dir.filePath("app.js"), fixture::EngineScript)); QVERIFY(Write(Dir.filePath("mode.txt"), "engine"));
        WebProcessRuntime Runtime; Runtime.root_ = Dir.path();
        QDirIterator Pins(Dir.path(), QDir::Files, QDirIterator::Subdirectories);
        while(Pins.hasNext()) {
            const auto Path = Pins.next(); Runtime.sha256_[QDir(Dir.path()).relativeFilePath(Path)] = Hash(Read(Path));
        }
        const auto Bytes = Fixture == "damaged" ? QByteArray("damaged document") : Read(Corpus + '/' + Fixture);
        QVERIFY(!Bytes.isEmpty());
        if (Fixture == "c14.hwp") QCOMPARE(Hash(Bytes), QByteArray("59d4bed335b9552fe78fa68d2a56f7cfa3d586bcdeaaba839af80df13f3e08dc"));
        if (Fixture == "c06.hwp") QCOMPARE(Hash(Bytes), QByteArray("a854b822c3150d9731b8ba0687160b2120be1078fc5c91221566a15176de3e20"));
        if (Fixture == "c07.hwpx") QCOMPARE(Hash(Bytes), QByteArray("4405f89db156da688d1a10ae0c0829f0159ad24ad4ad5e083902577a8304338a"));
        if(Fixture == "c02.hwp") QCOMPARE(Hash(Bytes), QByteArray("86e607233a2218928fabe5969155116a46d4ab4400db1402fe45b0082149496c"));
        if(Fixture == "c05.hwpx") QCOMPARE(Hash(Bytes), QByteArray("8d9df52bb09803fa1754317360e27709621555b3e1163a0d7eb4ad9abab5b26b"));
        if (QByteArray(QTest::currentDataTag()) == "async-product-open" || QByteArray(QTest::currentDataTag()) == "host-session-open") {
            const auto Original = Dir.filePath("source.hwp"); QVERIFY(Write(Original, Bytes));
            QVERIFY(QDir(Dir.path()).mkpath("Libs/bundle"));
            // Native test policy approves this fixed engine fixture only. This
            // inventory is not a distributable product package or release pin.
            const QByteArray BundleManifest("fixture bundle admission");
            QVERIFY(Write(Dir.filePath("Libs/bundle/asset-manifest.json"), BundleManifest));
            Runtime.sha256_["Libs/bundle/asset-manifest.json"] = Hash(BundleManifest);
            QVERIFY(Runtime.sha256_.contains("QtWebEngineProcess.exe"));
            QJsonArray Entries;
            for (auto At = Runtime.sha256_.cbegin(); At != Runtime.sha256_.cend(); ++At)
                Entries.append(QJsonObject{{"path", At.key()}, {"sha256", QString::fromLatin1(At.value())}});
            const QJsonObject Inventory{{"schemaVersion", 1}, {"profile", "native-read-only"},
                {"sourceCommit", "1a76570e833917d15817415a53c09ad61ab3203f"}, {"releaseEligible", true},
                {"assetManifestSha256", QString::fromLatin1(Hash(BundleManifest))}, {"files", Entries}};
            const auto InventoryBytes = QJsonDocument(Inventory).toJson(QJsonDocument::Compact);
            QVERIFY(Write(Dir.filePath("runtime-inventory.json"), InventoryBytes));
            Runtime.approvedInventorySha256_ = Hash(InventoryBytes); Runtime.sha256_.clear();
            if (QByteArray(QTest::currentDataTag()) == "host-session-open") {
                HwpHostSession Host(Dir.filePath("cache"), Runtime);
                int NativeCalls = 0;
                const auto Native = [&](const QJsonObject& Request) -> QJsonObject {
                    ++NativeCalls;
                    if (Request.contains("protocolVersion")) return {{"error", "document-protocol-version"}};
                    return {{"accepted", true}};
                };
                const auto Opened = Host.Dispatch({{"action", "openHost"}, {"path", Original}}, Native);
                QCOMPARE(NativeCalls, 0);
                const auto Id = Opened["documentId"].toString(); QVERIFY(!Id.isEmpty());
                const QJsonObject State{{"action", "rhwpState"}, {"version", 1}, {"documentId", Id}};
                QJsonObject Reply;
                QTRY_VERIFY_WITH_TIMEOUT(([&] { Reply = Host.Handle(State); return Reply["state"] == "ready" || Reply["state"] == "failed"; })(), 30000);
                QCOMPARE(Reply["state"].toString(), QString("ready"));
                QVERIFY(Reply["capabilities"].toObject()["view"].toBool());
                QVERIFY(!Reply["capabilities"].toObject()["save"].toBool());
                QVERIFY(!Reply.contains("path"));
                for (const auto Action : {"cancelHost", "openHost"}) {
                    const auto Rejected = Host.Dispatch({{"action", Action}, {"path", Dir.filePath("other.txt")}, {"protocolVersion", 99}}, Native);
                    QCOMPARE(Rejected["error"].toString(), QString("document-protocol-version"));
                    QCOMPARE(Host.Handle(State)["state"].toString(), QString("ready"));
                }
                QCOMPARE(NativeCalls, 2);
                QVERIFY(Host.Dispatch({{"action", "cancelHost"}}, Native)["accepted"].toBool());
                QTRY_VERIFY_WITH_TIMEOUT(Host.IsIdle(), 10000);
                QVERIFY(HwpProcessBackend::IsDrained()); QCOMPARE(Read(Original), Bytes);
                QDirIterator Copies(Dir.filePath("cache"), QDir::Files, QDirIterator::Subdirectories);
                while (Copies.hasNext()) QVERIFY(!Copies.next().endsWith(".hwp"));
                return;
            }
            HwpOpenWindow Window(Original, Dir.filePath("cache"), Runtime);
            Window.show(); Window.Open(); QTRY_VERIFY_WITH_TIMEOUT(Window.Document(), 10000);
            const auto Copy = QString::fromStdString(Window.Document()->Snapshot().source_.localPath_);
            QVERIFY(Copy != Original); QVERIFY(Write(Original, "changed origin"));
            QTRY_COMPARE_WITH_TIMEOUT(Window.Document()->Snapshot().lifecycle_, DocumentLifecycle::Ready, 30000);
            QCOMPARE(Window.Document()->Snapshot().model_.savepoint_, Hash(Bytes).toStdString());
            QVERIFY(Window.Document()->Capabilities().view_); QVERIFY(!Window.Document()->Capabilities().edit_);
            QVERIFY(QFile::exists(Copy)); Window.RequestClose();
            QTRY_VERIFY_WITH_TIMEOUT(Window.IsClosed(), 10000);
            QVERIFY(!QFile::exists(Copy)); QVERIFY(HwpProcessBackend::IsDrained());
            QCOMPARE(Read(Original), QByteArray("changed origin")); return;
        }
        // Exercise native provider -> verified copy -> hash -> SDI -> real engine.
        const auto OriginalPath = Dir.filePath("document." + Format); QVERIFY(Write(OriginalPath, Bytes));
        auto Provider = hc::storage::MakeNativeReadProvider(hc::storage::LocatorKind::Local);
        auto Entry = Provider->Resolve({hc::storage::LocatorKind::Local, OriginalPath.toUtf8().toStdString()}, {}).get();
        QVERIFY(Entry.HasValue());
        auto Store = hc::storage::MakeNativeMaterializer(Dir.path().toUtf8().toStdString());
        QVERIFY(Store.HasValue());
        hc::storage::MaterializationRequest Request;
        Request.source_ = std::make_shared<hc::storage::ProviderContentSource>(Provider, Entry.Value());
        auto Prepared = PrepareHwpSource(*Store.Value(), std::move(Request), {});
        QVERIFY2(Prepared.HasValue(), Prepared.HasValue() ? "" : Prepared.Failure().code_.c_str());
        SourceIdentity Source = std::move(Prepared.Value());
        const auto InputPath = QString::fromUtf8(Source.localPath_);
        QVERIFY(InputPath != OriginalPath); QCOMPARE(Read(InputPath), Bytes);
        QCOMPARE(Source.inputSha256_, Hash(Bytes).toStdString());
        QVERIFY(!Source.originalWritable_); QVERIFY(Source.sourceToken_.empty());
        auto Lease = Source.lease_; std::weak_ptr<hc::storage::IMaterializedFileLease> Weak = Lease;
        // The session must use its acquired copy even if the origin later changes.
        QVERIFY(Write(OriginalPath, "changed after acquisition"));
        auto Backend = std::make_unique<HwpProcessBackend>(Runtime, WebProcessLimits{8, 2048ULL*1024*1024, 10000});
        auto* Port = Backend.get();
        auto Window = std::make_unique<HwpSdiWindow>("isolated-engine-document", Source, std::move(Backend));
        const auto Drain = qScopeGuard([&] {
            Window.reset();
            QElapsedTimer Wait; Wait.start();
            while(!HwpProcessBackend::IsDrained() && Wait.elapsed() < 10000) QTest::qWait(10);
        });
        QSignalSpy Loaded(Port, &HwpSdiBackend::Loaded), Stopped(Port, &HwpSdiBackend::Stopped);
        Source.lease_.reset(); Lease.reset();
        Window->show(); QVERIFY(Window->Open());
        QTRY_COMPARE_WITH_TIMEOUT(Loaded.size(), 1, 30000);
        const auto Result = *static_cast<const HwpLoadResult*>(Loaded.first()[2].constData());
        QVERIFY2(Result.HasValue() == Success, Result.HasValue() ? "unexpected Ready" : Result.Failure().code_.c_str());
        const auto IsLocked = [&] { QFile File(InputPath); return !File.open(QIODevice::ReadWrite); };
        std::vector<std::shared_ptr<void>> Processes;
        if(Success) {
            QVERIFY(QFile::exists(Dir.filePath("input-observed.txt")));
            QTRY_COMPARE(Window->Snapshot().lifecycle_, DocumentLifecycle::Ready);
            QVERIFY(Window->Capabilities().view_); QVERIFY(!Window->Capabilities().edit_);
            QVERIFY(!Window->Capabilities().save_); QVERIFY(!Window->Capabilities().saveAs_);
            QCOMPARE(Window->Snapshot().model_.savepoint_, Source.inputSha256_);
            QVERIFY(IsLocked()); QCOMPARE(Stopped.size(), 0);
            DWORD Helper = 0;
            EnumChildWindows(reinterpret_cast<HWND>(Port->winId()), [](HWND Child, LPARAM Value) -> BOOL {
                DWORD Pid = 0; GetWindowThreadProcessId(Child, &Pid);
                if(Pid && Pid != GetCurrentProcessId()) { *reinterpret_cast<DWORD*>(Value) = Pid; return FALSE; }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&Helper));
            QVERIFY(Helper);
            std::vector<DWORD> Pids{Helper};
            const auto Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            QVERIFY(Snapshot != INVALID_HANDLE_VALUE);
            std::vector<PROCESSENTRY32W> Entries; PROCESSENTRY32W Entry{}; Entry.dwSize = sizeof(Entry);
            if(Process32FirstW(Snapshot, &Entry)) do { Entries.push_back(Entry); } while(Process32NextW(Snapshot, &Entry));
            CloseHandle(Snapshot);
            for(std::size_t At = 0; At < Pids.size(); ++At)
                for(const auto& Child : Entries)
                    if(Child.th32ParentProcessID == Pids[At] && std::find(Pids.begin(), Pids.end(), Child.th32ProcessID) == Pids.end())
                        Pids.push_back(Child.th32ProcessID);
            // A real isolated engine frame requires at least helper + renderer.
            QVERIFY(Pids.size() >= 2);
            for(const auto Pid : Pids) {
                const auto Handle = OpenProcess(SYNCHRONIZE, FALSE, Pid); QVERIFY(Handle);
                Processes.emplace_back(Handle, [](void* Value) { CloseHandle(Value); });
                QCOMPARE(WaitForSingleObject(Handle, 0), DWORD(WAIT_TIMEOUT));
            }
            Window->resize(820, 620); Window->RequestClose();
        } else {
            QVERIFY(!QFile::exists(Dir.filePath("input-observed.txt")));
            QCOMPARE(Result.Failure().code_, std::string(Fixture == "c14.hwp" ? "rhwp-password-required" : "rhwp-input-container"));
            QTRY_COMPARE_WITH_TIMEOUT(Stopped.size(), 1, 10000);
            QTRY_COMPARE(Window->Snapshot().lifecycle_, DocumentLifecycle::Failed);
            QVERIFY(!Window->Capabilities().view_); Window->RequestClose();
        }
        QTRY_COMPARE_WITH_TIMEOUT(Window->Snapshot().lifecycle_, DocumentLifecycle::Closed, 10000);
        QCOMPARE(Stopped.size(), 1); QVERIFY(HwpProcessBackend::IsDrained());
        // Native materialization keeps its own read lock until the final lease,
        // including the window source and captured signal tickets, is released.
        QVERIFY(IsLocked());
        for(const auto& Process : Processes) QCOMPARE(WaitForSingleObject(Process.get(), 0), DWORD(WAIT_OBJECT_0));
        Window.reset(); Loaded.clear(); QVERIFY(Weak.expired());
        const auto Cleanup = Store.Value()->DrainCleanup(); QVERIFY(Cleanup.errors_.empty());
        QVERIFY(!QFile::exists(InputPath)); QCOMPARE(Read(OriginalPath), QByteArray("changed after acquisition"));
        QCOMPARE(Hash(Read(Site + "/rhwp.js")), fixture::EngineJsSha);
        QCOMPARE(Hash(Read(Site + "/rhwp_bg.wasm")), fixture::EngineWasmSha);
    }
    void helperDestroyedWhileLoading()
    {
        QTemporaryDir Dir; QString Error;
        const QByteArray Page("<!doctype html><canvas></canvas>");
        QVERIFY(Write(Dir.filePath("index.html"), Page));
        auto Bundle = HwpBundle::LoadPinned(Dir.path(), {{"index.html", Hash(Page)}}, Error); QVERIFY(Bundle);
        const QJsonObject Identity{{"documentId", "destroy-loading"}, {"generation", "1"}, {"token", "2"},
            {"bindingOwner", "3"}, {"bindingGeneration", "4"}, {"epoch", "5"}};
        auto Command = [&](const char* Type) { return QJsonObject{{"version", 1}, {"type", Type}, {"identity", Identity}}; };
        auto Controller = std::make_unique<HwpHelperDocument>(Bundle);
        auto Begin = Command("InputBegin"); Begin["format"] = "hwp"; Begin["readOnly"] = true;
        Begin["byteLength"] = 1; Begin["sha256"] = QString::fromLatin1(Hash("x"));
        QCOMPARE(Controller->Handle(Begin)["type"].toString(), QString("InputAccepted"));
        auto Chunk = Command("InputChunk"); Chunk["offset"] = 0; Chunk["data"] = "eA==";
        QCOMPARE(Controller->Handle(Chunk)["type"].toString(), QString("InputChunkAccepted"));
        QCOMPARE(Controller->Handle(Command("InputCommit"))["type"].toString(), QString("InputVerified"));
        QCOMPARE(Controller->Handle(Command("DocumentStart"))["state"].toString(), QString("loading"));
        Controller.reset(); // Endpoint destruction emits Failed before QObject base teardown.
        QCoreApplication::processEvents();
    }
    void admittedInputRoundTrip()
    {
        QTemporaryDir Dir; const auto Path = Dir.filePath("input.hwpx");
        const auto Bytes = ValidInput(); QVERIFY(!Bytes.isEmpty()); QVERIFY(Write(Path, Bytes));
        SourceIdentity Source; Source.localPath_ = Path.toUtf8().toStdString();
        Source.lease_ = std::make_shared<InputLease>(Source.localPath_);
        Source.inputSha256_ = Hash(Bytes).toStdString();
        DocumentOperationTicket Ticket; Ticket.documentId_ = "doc-alpha";
        Ticket.generation_ = 2; Ticket.token_ = 3; Ticket.operation_ = DocumentOperation::Loading;
        Ticket.lease_ = Source.lease_;
        QString Error;
        auto Input = HwpAdmittedInput::Read(Ticket, Source, 4, 5, 6, "hwpx", Error);
        QVERIFY2(Input != nullptr, qPrintable(Error));
        // Held native read handle prevents modification until owner release.
        QVERIFY(!Write(Path, "replacement"));
        HwpInputTransfer Receiver(true); int Calls = 0;
        const HwpAdmittedInput::Exchange Transport = [&](const QByteArray& Request, QByteArray& Reply, quint32 Timeout, WebProcessError&) {
            ++Calls; if(!Timeout || Timeout > 30000 || Request.contains(Path.toUtf8())) return false;
            Reply = QJsonDocument(Receiver.Handle(QJsonDocument::fromJson(Request).object())).toJson(QJsonDocument::Compact);
            return true;
        };
        QVERIFY2(Input->Send(Transport, [] { return false; }, Error, 120000), qPrintable(Error));
        QCOMPARE(Calls, 5);
        const auto Taken = Receiver.Take(); QVERIFY(Taken);
        QCOMPARE(Taken->bytes_, Bytes); QCOMPARE(Taken->identity_["documentId"].toString(), QString("doc-alpha"));
        QCOMPARE(Taken->identity_["epoch"].toString(), QString("6"));
        Ticket.lease_.reset(); Source.lease_.reset();
        QVERIFY(!Write(Path, "replacement"));
        Input.reset(); QVERIFY(Write(Path, "replacement"));
    }
    void admittedInputContainerPolicy_data()
    {
        QTest::addColumn<int>("Mode");
        QTest::newRow("ordinary-read-only") << 0;
        QTest::newRow("signed-read-only") << 1;
        QTest::newRow("mismatched-format") << 2;
        QTest::newRow("damaged-container") << 3;
    }
    void admittedInputContainerPolicy()
    {
        QFETCH(int, Mode); QTemporaryDir Dir; const auto Path = Dir.filePath("materialized.bin");
        const auto Bytes = Mode == 3 ? QByteArray("damaged") : ValidInput(Mode == 1);
        QVERIFY(!Bytes.isEmpty()); QVERIFY(Write(Path, Bytes));
        SourceIdentity Source; Source.localPath_ = Path.toUtf8().toStdString();
        Source.lease_ = std::make_shared<InputLease>(Source.localPath_); Source.inputSha256_ = Hash(Bytes).toStdString();
        DocumentOperationTicket Ticket{"policy", 1, 2, 0, DocumentOperation::Loading, {}, Source.lease_};
        QString Error; auto Input = HwpAdmittedInput::Read(Ticket, Source, 1, 1, 1, Mode == 2 ? "hwp" : "hwpx", Error);
        if (Mode < 2) { QVERIFY2(Input, qPrintable(Error)); QCOMPARE(Input->IsRestricted(), Mode == 1); }
        else { QVERIFY(!Input); QCOMPARE(Error, QString("rhwp-input-container")); QVERIFY(Write(Path, "unlocked")); }
    }
    void admittedInputAuthority()
    {
        QTemporaryDir Dir; const auto Path = Dir.filePath("input.hwp"); QVERIFY(Write(Path, "bytes"));
        SourceIdentity Source; Source.localPath_ = Path.toUtf8().toStdString();
        Source.lease_ = std::make_shared<InputLease>(Source.localPath_); Source.inputSha256_ = Hash("bytes").toStdString();
        DocumentOperationTicket Ticket; Ticket.documentId_ = "doc"; Ticket.generation_ = 1; Ticket.token_ = 1;
        Ticket.operation_ = DocumentOperation::Loading; Ticket.lease_ = Source.lease_; QString Error;
        auto Wrong = Ticket; Wrong.lease_ = std::make_shared<InputLease>(Source.localPath_);
        QVERIFY(!HwpAdmittedInput::Read(Wrong, Source, 1, 1, 1, "hwp", Error));
        Wrong = Ticket; Wrong.operation_ = DocumentOperation::Idle;
        QVERIFY(!HwpAdmittedInput::Read(Wrong, Source, 1, 1, 1, "hwp", Error));
        QVERIFY(!HwpAdmittedInput::Read(Ticket, Source, 0, 1, 1, "hwp", Error));
        Source.inputSha256_ = Hash("changed").toStdString();
        QVERIFY(!HwpAdmittedInput::Read(Ticket, Source, 1, 1, 1, "hwp", Error));
        QCOMPARE(Error, QString("rhwp-input-integrity"));
        Source.inputSha256_ = Hash("bytes").toStdString(); Source.localPath_ += ":stream";
        QVERIFY(!HwpAdmittedInput::Read(Ticket, Source, 1, 1, 1, "hwp", Error));
    }
    void inputSenderRejectsReplies()
    {
        QTemporaryDir Dir; const auto Path = Dir.filePath("input.hwpx");
        const auto Bytes = ValidInput(); QVERIFY(!Bytes.isEmpty()); QVERIFY(Write(Path, Bytes));
        SourceIdentity Source; Source.localPath_ = Path.toUtf8().toStdString();
        Source.lease_ = std::make_shared<InputLease>(Source.localPath_); Source.inputSha256_ = Hash(Bytes).toStdString();
        DocumentOperationTicket Ticket; Ticket.documentId_ = "doc"; Ticket.generation_ = 1; Ticket.token_ = 1;
        Ticket.operation_ = DocumentOperation::Loading; Ticket.lease_ = Source.lease_; QString Error;
        auto Input = HwpAdmittedInput::Read(Ticket, Source, 1, 1, 1, "hwpx", Error); QVERIFY(Input);
        for(int Mode = 0; Mode < 5; ++Mode) {
            HwpInputTransfer Receiver(true); int Calls = 0;
            auto Transport = [&](const QByteArray& Request, QByteArray& Reply, quint32, WebProcessError&) {
                ++Calls; auto Value = Receiver.Handle(QJsonDocument::fromJson(Request).object());
                if(Mode == 0) Value["received"] = 99;
                if(Mode == 1) Value["identity"] = QJsonObject{};
                if(Mode == 2) Value["extra"] = true;
                if(Mode == 3) QTest::qSleep(20);
                Reply = QJsonDocument(Value).toJson(QJsonDocument::Compact); return true;
            };
            QVERIFY(!Input->Send(Transport, [&] { return Mode == 4; }, Error, Mode == 3 ? 5 : 30000));
            QCOMPARE(Calls, Mode == 4 ? 0 : 1);
            QCOMPARE(Error, QString(Mode == 3 ? "rhwp-input-timeout" : Mode == 4 ? "rhwp-input-cancelled" : "rhwp-input-reply"));
        }
    }
    void immutableSnapshot()
    {
        QTemporaryDir Dir; QVERIFY(Dir.isValid()); QVERIFY(Write(Dir.filePath("index.html"), Html));
        QString Error;
        auto Bundle = HwpBundle::LoadPinned(Dir.path(), {{"index.html", Hash(Html)}}, Error);
        QVERIFY2(Bundle != nullptr, qPrintable(Error));
        QVERIFY(Write(Dir.filePath("index.html"), "changed"));
        QCOMPARE(Bundle->Bytes(QUrl("rhwp://bundle/index.html")), Html);
        QVERIFY(!HwpBundle::LoadPinned(Dir.path(), {{"index.html", Hash(Html)}}, Error));
        QCOMPARE(Error, QString("rhwp-bundle-hash"));
    }
    void malformedAndMissing()
    {
        QTemporaryDir Dir; QString Error; QVERIFY(Write(Dir.filePath("index.html"), Html));
        QVERIFY(!HwpBundle::LoadPinned(Dir.path(), {}, Error));
        QVERIFY(!HwpBundle::LoadPinned("relative", {{"index.html", Hash(Html)}}, Error));
        for (const auto& Name : {"../escape", "/absolute", "a//b", "a/./b", "a:b", "a\\b", "INDEX.HTML"}) {
            QMap<QString, QByteArray> Pins{{"index.html", Hash(Html)}, {Name, Hash(Html)}};
            QVERIFY(!HwpBundle::LoadPinned(Dir.path(), Pins, Error));
        }
        QVERIFY(!HwpBundle::LoadPinned(Dir.path(), {{"index.html", "wrong"}}, Error));
        QVERIFY(!HwpBundle::LoadPinned(Dir.path(), {{"index.html", Hash(Html)}, {"missing.js", Hash(Html)}}, Error));
    }
    void urlBoundary()
    {
        QTemporaryDir Dir; QString Error; QVERIFY(Write(Dir.filePath("index.html"), Html));
        const auto Bundle = HwpBundle::LoadPinned(Dir.path(), {{"index.html", Hash(Html)}}, Error); QVERIFY(Bundle);
        for (const auto* Url : {"https://bundle/index.html", "file:///index.html", "qrc:/index.html",
            "rhwp://other/index.html", "rhwp://user@bundle/index.html", "rhwp://bundle:80/index.html",
            "rhwp://bundle/index.html?q=1", "rhwp://bundle/index.html#x", "rhwp://bundle/missing.js",
            "rhwp://bundle/../index.html", "rhwp://bundle/%2findex.html", "data:text/html,hello", "blob:rhwp://bundle/id"})
            QVERIFY2(!Bundle->Allows(QUrl(Url)), Url);
        QVERIFY(Bundle->Allows(QUrl("rhwp://bundle/index.html")));
    }
    void byteBudget()
    {
        QTemporaryDir Dir; QString Error; QFile File(Dir.filePath("index.html"));
        QVERIFY(File.open(QIODevice::WriteOnly)); QVERIFY(File.resize(64LL * 1024 * 1024 + 1)); File.close();
        QVERIFY(!HwpBundle::LoadPinned(Dir.path(), {{"index.html", Hash(Html)}}, Error));
        QCOMPARE(Error, QString("rhwp-bundle-size"));
    }
    void initializationBoundary()
    {
        QVERIFY(!RegisterHwpScheme());
        QString Error; QVERIFY(!HwpBundleView::Create({}, Error));
        for (const auto& Flag : {"--webEngineArgs", "--WEBENGINEARGS", "--WeBeNgInEaRgS",
            "--webEngineArgs=--single-process", "--single-process", "--NO-SANDBOX",
            "--disable-web-security", "--remote-debugging-port=9222"})
            QVERIFY(!HwpWebArgumentsAllowed({"helper", Flag}));
        QVERIFY(HwpWebArgumentsAllowed({"helper", "--web-owner", "--parent", "123"}));
        QTemporaryDir Dir; QVERIFY(Write(Dir.filePath("index.html"), Html));
        auto Bundle = HwpBundle::LoadPinned(Dir.path(), {{"index.html", Hash(Html)}}, Error);
        QVERIFY(Bundle);
        qputenv("QTWEBENGINE_DISABLE_SANDBOX", "1");
        QVERIFY(!HwpBundleView::Create(Bundle, Error));
        QCOMPARE(Error, QString("rhwp-web-environment"));
        QVERIFY(!HwpWebEnvironmentAllowed()); qunsetenv("QTWEBENGINE_DISABLE_SANDBOX");
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--no-sandbox");
        QVERIFY(!HwpWebEnvironmentAllowed()); qunsetenv("QTWEBENGINE_CHROMIUM_FLAGS");
        qputenv("QTWEBENGINE_REMOTE_DEBUGGING", "9222");
        QVERIFY(!HwpWebEnvironmentAllowed()); qunsetenv("QTWEBENGINE_REMOTE_DEBUGGING");
    }
    void releasePackageAdmission()
    {
        QTemporaryDir Dir; QString Error; QVERIFY(Write(Dir.filePath("index.html"), Html));
        QJsonObject Manifest{{"schemaVersion", 1}, {"releaseEligible", false},
            {"sourceCommit", "1a76570e833917d15817415a53c09ad61ab3203f"},
            {"files", QJsonArray{QJsonObject{{"path", "index.html"}, {"sha256", QString::fromLatin1(Hash(Html))}}}}};
        auto Bytes = QJsonDocument(Manifest).toJson(QJsonDocument::Compact);
        QVERIFY(Write(Dir.filePath("asset-manifest.json"), Bytes));
        QVERIFY(!LoadApprovedHwpPackage(Dir.path(), {}, Error));
        QCOMPARE(Error, QString("rhwp-package-policy-unavailable"));
        QVERIFY(!LoadApprovedHwpPackage(Dir.path(), Hash(Bytes), Error));
        QCOMPARE(Error, QString("rhwp-package-not-release-eligible"));
        const auto OldPin = Hash(Bytes);
        Manifest["releaseEligible"] = true; Bytes = QJsonDocument(Manifest).toJson(QJsonDocument::Compact);
        QVERIFY(Write(Dir.filePath("asset-manifest.json"), Bytes));
        QVERIFY(!LoadApprovedHwpPackage(Dir.path(), OldPin, Error));
        QCOMPARE(Error, QString("rhwp-package-manifest-hash"));
        QVERIFY(LoadApprovedHwpPackage(Dir.path(), Hash(Bytes), Error));
        QVERIFY(Write(Dir.filePath("index.html"), "tampered"));
        QVERIFY(!LoadApprovedHwpPackage(Dir.path(), Hash(Bytes), Error));
        QCOMPARE(Error, QString("rhwp-bundle-hash"));
        QVERIFY(Write(Dir.filePath("index.html"), Html));
        Manifest["sourceCommit"] = "unknown"; Bytes = QJsonDocument(Manifest).toJson(QJsonDocument::Compact);
        QVERIFY(Write(Dir.filePath("asset-manifest.json"), Bytes));
        QVERIFY(!LoadApprovedHwpPackage(Dir.path(), Hash(Bytes), Error));
        QCOMPARE(Error, QString("rhwp-package-source"));
    }
    void remoteMetaobjectBoundary()
    {
        HwpLoadBridge Bridge; QString Error; const QByteArray Bytes("input");
        QVERIFY(Bridge.Begin({"1", 2, 3, 4, 5, "hwp", Hash(Bytes)}, Bytes, Error));
        auto* Endpoint = new HwpLoadEndpoint(&Bridge, nullptr);
        for (const auto* Name : {"Ready()", "Cancelled()", "Retire()", "FailLoad()", "RequestCancel()", "RetirementRequested()"})
            QCOMPARE(Endpoint->metaObject()->indexOfMethod(Name), -1);
        QSignalSpy Failed(&Bridge, &HwpLoadBridge::Failed);
        delete Endpoint;
        QCOMPARE(Failed.size(), 1); QCOMPARE(Bridge.CurrentState(), HwpLoadBridge::State::Failed);
    }
    void inputTransferBoundary()
    {
        const QByteArray Bytes(192 * 1024 + 17, 'x');
        const QJsonObject Identity{{"documentId", "document-A"}, {"generation", "9007199254740993"},
            {"token", "2"}, {"bindingOwner", "3"}, {"bindingGeneration", "4"}, {"epoch", "5"}};
        const QJsonObject Begin{{"type", "InputBegin"}, {"version", 1}, {"identity", Identity},
            {"format", "hwp"}, {"readOnly", true}, {"byteLength", Bytes.size()}, {"sha256", QString::fromLatin1(Hash(Bytes))}};
        auto Command = [&](const char* Type) { return QJsonObject{{"type", Type}, {"version", 1}, {"identity", Identity}}; };
        auto Chunk = [&](int Offset, const QByteArray& Part) {
            auto Value = Command("InputChunk"); Value["offset"] = Offset;
            Value["data"] = QString::fromLatin1(Part.toBase64()); return Value;
        };
        HwpInputTransfer Denied(false);
        QCOMPARE(Denied.Handle(Begin)["error"].toString(), QString("rhwp-input-package-not-approved"));
        HwpInputTransfer Input(true);
        auto Bad = Begin; Bad["path"] = "file.hwp"; QVERIFY(Input.Handle(Bad).contains("error"));
        Bad = Begin; Bad["readOnly"] = false; QVERIFY(Input.Handle(Bad).contains("error"));
        auto BadIdentity = Identity; BadIdentity["generation"] = 2;
        Bad = Begin; Bad["identity"] = BadIdentity; QVERIFY(Input.Handle(Bad).contains("error"));
        QCOMPARE(Input.Handle(Begin)["type"].toString(), QString("InputAccepted"));
        QVERIFY(Input.Handle(Begin).contains("error")); QVERIFY(!Input.Take());
        auto Stale = Chunk(0, Bytes.left(192 * 1024)); Stale["identity"] = BadIdentity;
        QCOMPARE(Input.Handle(Stale)["error"].toString(), QString("rhwp-input-stale"));
        QCOMPARE(Input.Handle(Chunk(0, Bytes.left(192 * 1024)))["received"].toInt(), 192 * 1024);
        QCOMPARE(Input.Handle(Chunk(192 * 1024, Bytes.mid(192 * 1024)))["received"].toInt(), Bytes.size());
        QCOMPARE(Input.Handle(Command("InputCommit"))["type"].toString(), QString("InputVerified"));
        QVERIFY(Input.Handle(Command("InputCommit")).contains("error"));
        const auto Payload = Input.Take(); QVERIFY(Payload); QCOMPARE(Payload->bytes_, Bytes);
        QCOMPARE(Payload->identity_, Identity); QVERIFY(!Input.Take());

        HwpInputTransfer WrongOrder(true); WrongOrder.Handle(Begin);
        QVERIFY(WrongOrder.Handle(Chunk(1, "x")).contains("error")); QVERIFY(!WrongOrder.Take());
        HwpInputTransfer WrongHash(true); Bad = Begin; Bad["byteLength"] = 1;
        WrongHash.Handle(Bad); WrongHash.Handle(Chunk(0, "x"));
        QCOMPARE(WrongHash.Handle(Command("InputCommit"))["error"].toString(), QString("rhwp-input-integrity"));
        HwpInputTransfer Cancelled(true); Cancelled.Handle(Begin);
        QCOMPARE(Cancelled.Handle(Command("InputCancel"))["type"].toString(), QString("InputDiscarded"));
        QVERIFY(!Cancelled.Take()); QVERIFY(Cancelled.Handle(Begin).contains("error"));
        HwpInputTransfer Expired(true, 10); Expired.Handle(Begin); QTest::qSleep(30);
        QVERIFY(Expired.Handle(Chunk(0, "x")).contains("error")); QVERIFY(!Expired.Take());
        HwpInputTransfer SizeGate(true);
        Bad = Begin; Bad["byteLength"] = 0; QVERIFY(SizeGate.Handle(Bad).contains("error"));
        Bad["byteLength"] = 64 * 1024 * 1024 + 1; QVERIFY(SizeGate.Handle(Bad).contains("error"));
        HwpInputTransfer Missing(true); Missing.Handle(Begin);
        QCOMPARE(Missing.Handle(Command("InputCommit"))["error"].toString(), QString("rhwp-input-integrity"));
        HwpInputTransfer VerifiedExpiry(true, 10);
        Bad = Begin; Bad["byteLength"] = 1; Bad["sha256"] = QString::fromLatin1(Hash("x"));
        VerifiedExpiry.Handle(Bad); VerifiedExpiry.Handle(Chunk(0, "x"));
        QCOMPARE(VerifiedExpiry.Handle(Command("InputCommit"))["type"].toString(), QString("InputVerified"));
        QTest::qSleep(30); QVERIFY(!VerifiedExpiry.Take());
        HwpInputTransfer VerifiedCancel(true); VerifiedCancel.Handle(Bad); VerifiedCancel.Handle(Chunk(0, "x"));
        VerifiedCancel.Handle(Command("InputCommit"));
        QCOMPARE(VerifiedCancel.Handle(Command("InputCancel"))["type"].toString(), QString("InputDiscarded"));
        QVERIFY(!VerifiedCancel.Take());
        HwpInputTransfer InvalidBase64(true); InvalidBase64.Handle(Begin);
        Bad = Chunk(0, "x"); Bad["data"] = "!!!!";
        QVERIFY(InvalidBase64.Handle(Bad).contains("error")); QVERIFY(!InvalidBase64.Take());
    }
    void wholeJobRetirement_data()
    {
        QTest::addColumn<bool>("Cancel");
        QTest::newRow("deadline") << false;
        QTest::newRow("cancel") << true;
    }
    void wholeJobRetirement()
    {
        QFETCH(bool, Cancel);
        QTemporaryDir Dir; WebProcessRuntime Runtime; Runtime.root_ = Dir.path();
        QVERIFY(QFile::copy(QString::fromUtf8(HC_RETIREMENT_FIXTURE), Dir.filePath("HyperEditorWeb.exe")));
        QVERIFY(Write(Dir.filePath("mode.txt"), "hang-ready-tree"));
        for(const auto* Name : {"HyperEditorWeb.exe", "mode.txt"}) {
            QFile File(Dir.filePath(Name)); QVERIFY(File.open(QIODevice::ReadOnly));
            Runtime.sha256_[Name] = Hash(File.readAll());
        }
        HwpProcessBackend Backend(Runtime, {4, 512ULL * 1024 * 1024, 1500});
        DocumentOperationTicket Ticket; Ticket.documentId_ = "retirement";
        auto Lease = std::make_shared<RetirementLease>(); std::weak_ptr<RetirementLease> Weak = Lease;
        Ticket.lease_ = Lease; SourceIdentity Source; Source.lease_ = Lease;
        Backend.Load(87, Ticket, Source);
        HwpLoadBridge Bridge; QString Error;
        auto Stale = Ticket; Stale.documentId_ = "stale";
        QVERIFY(!ConnectHwpRetirement(Bridge, Backend, 87, Stale));
        QVERIFY(ConnectHwpRetirement(Bridge, Backend, 87, Ticket));
        QVERIFY(!ConnectHwpRetirement(Bridge, Backend, 87, Ticket));
        Stale.lease_.reset(); Ticket.lease_.reset(); Source.lease_.reset(); Lease.reset();
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(Dir.filePath("tree.txt")), 1000);
        QFile Marker(Dir.filePath("tree.txt")); QVERIFY(Marker.open(QIODevice::ReadOnly));
        const auto Pids = Marker.readAll().split(' '); QCOMPARE(Pids.size(), 2);
        auto Close = [](void* Value) { CloseHandle(Value); };
        std::unique_ptr<void, decltype(Close)> Parent(OpenProcess(SYNCHRONIZE, FALSE, Pids[0].toUInt()), Close);
        std::unique_ptr<void, decltype(Close)> Child(OpenProcess(SYNCHRONIZE, FALSE, Pids[1].toUInt()), Close);
        QVERIFY(Parent); QVERIFY(Child);
        QSignalSpy Stopped(&Backend, &HwpSdiBackend::Stopped), Cancelled(&Bridge, &HwpLoadBridge::Cancelled);
        QString LoadError;
        connect(&Backend, &HwpSdiBackend::Loaded, this, [&](quint64, DocumentOperationTicket, HwpLoadResult Result) {
            if(!Result.HasValue()) LoadError = QString::fromStdString(Result.Failure().code_);
        });
        bool TreeExitedAtStopped = false;
        connect(&Backend, &HwpSdiBackend::Stopped, this, [&](quint64) {
            TreeExitedAtStopped = WaitForSingleObject(Parent.get(), 0) == WAIT_OBJECT_0
                && WaitForSingleObject(Child.get(), 0) == WAIT_OBJECT_0;
        });
        const QByteArray Bytes("input");
        QVERIFY(Bridge.Begin({"1", 2, 3, 4, 5, "hwp", Hash(Bytes)}, Bytes, Error, Cancel ? 30000 : 20));
        if(Cancel) Bridge.RequestCancel();
        QTRY_VERIFY_WITH_TIMEOUT(Bridge.CurrentState() != HwpLoadBridge::State::Loading, 1000);
        QCOMPARE(Stopped.size(), 0); QCOMPARE(Cancelled.size(), 0);
        QVERIFY(Bridge.CurrentState() != HwpLoadBridge::State::Retired); QVERIFY(!Weak.expired());
        QTRY_COMPARE_WITH_TIMEOUT(Stopped.size(), 1, 10000);
        QVERIFY(TreeExitedAtStopped); QVERIFY(HwpProcessBackend::IsDrained());
        QTRY_VERIFY(Weak.expired());
        QCOMPARE(Bridge.CurrentState(), HwpLoadBridge::State::Retired);
        QCOMPARE(Cancelled.size(), Cancel ? 1 : 0);
        QCOMPARE(LoadError, Cancel ? QString{} : QString("rhwp-load-timeout"));
        QFile Released(Dir.filePath("mode.txt")); QVERIFY(Released.open(QIODevice::ReadWrite));
    }
    void deadlineRejectsQueuedReply()
    {
        HwpLoadBridge Bridge; QString Error; const QByteArray Bytes("input");
        QVERIFY(!Bridge.Begin({"1", 2, 3, 4, 5, "hwp", Hash(Bytes)}, Bytes, Error, 0));
        QVERIFY(Bridge.Begin({"1", 2, 3, 4, 5, "hwp", Hash(Bytes)}, Bytes, Error, 20));
        auto Report = QJsonObject::fromVariantMap(Bridge.PendingOpen());
        const auto Operation = Report["operation"].toString();
        for (const auto* Key : {"readOnly", "byteLength", "chunkSize", "sha256"}) Report.remove(Key);
        Report["type"] = "Parsed"; Report["pageCount"] = 1;
        QSignalSpy Failed(&Bridge, &HwpLoadBridge::Failed), Retirement(&Bridge, &HwpLoadBridge::RetirementRequested);
        // Deliberately do not dispatch the QTimer event before the late reply.
        QTest::qSleep(40);
        QVERIFY(Bridge.PendingOpen().isEmpty()); QVERIFY(Bridge.ReadChunk(Operation, 0).isEmpty());
        QVERIFY(!Bridge.Report(QString::fromUtf8(QJsonDocument(Report).toJson())));
        QCOMPARE(Failed.size(), 1); QCOMPARE(Failed.first().first().toString(), QString("rhwp-load-timeout"));
        QCOMPARE(Retirement.size(), 1); QCOMPARE(Bridge.CurrentState(), HwpLoadBridge::State::Failed);
        Bridge.FailLoad(); Bridge.RequestCancel(); QTest::qWait(30);
        QCOMPARE(Retirement.size(), 1); QCOMPARE(Failed.size(), 1);
        Bridge.Retire(); QCOMPARE(Bridge.CurrentState(), HwpLoadBridge::State::Retired);
    }
    void readyDisarmsDeadline()
    {
        HwpLoadBridge Bridge; QString Error; const QByteArray Bytes("input");
        QVERIFY(Bridge.Begin({"1", 2, 3, 4, 5, "hwp", Hash(Bytes)}, Bytes, Error, 100));
        auto Report = QJsonObject::fromVariantMap(Bridge.PendingOpen());
        for (const auto* Key : {"readOnly", "byteLength", "chunkSize", "sha256"}) Report.remove(Key);
        auto Send = [&](QJsonObject Value) { return Bridge.Report(QString::fromUtf8(QJsonDocument(Value).toJson())); };
        auto Parsed = Report; Parsed["type"] = "Parsed"; Parsed["pageCount"] = 1; QVERIFY(Send(Parsed));
        Report["type"] = "FirstFrame"; Report["page"] = 0; Report["width"] = 10; Report["height"] = 10;
        QVERIFY(Send(Report));
        QSignalSpy Retirement(&Bridge, &HwpLoadBridge::RetirementRequested), Failed(&Bridge, &HwpLoadBridge::Failed);
        QTest::qWait(150); QCOMPARE(Bridge.CurrentState(), HwpLoadBridge::State::Ready);
        QCOMPARE(Retirement.size(), 0); QCOMPARE(Failed.size(), 0);
    }
    void retirementObserverMayDeleteBridge()
    {
        auto* Bridge = new HwpLoadBridge; QString Error; const QByteArray Bytes("input");
        QVERIFY(Bridge->Begin({"1", 2, 3, 4, 5, "hwp", Hash(Bytes)}, Bytes, Error, 10));
        QPointer<HwpLoadBridge> Guard(Bridge);
        connect(Bridge, &HwpLoadBridge::RetirementRequested, Bridge, [Bridge] { delete Bridge; });
        QTRY_VERIFY_WITH_TIMEOUT(Guard.isNull(), 1000);
    }
    void nativeDeadlineWithBusyRenderer()
    {
        QTemporaryDir Dir; QString Error;
        const QByteArray Page("<!doctype html><script src='app.js'></script>");
        const QByteArray Script(R"JS(
new QWebChannel(qt.webChannelTransport, channel => {
    channel.objects.loadBridge.PendingOpen(meta => {
        for (const key of ['readOnly','byteLength','chunkSize','sha256']) delete meta[key];
        channel.objects.loadBridge.Report(JSON.stringify({...meta,type:'Parsed',pageCount:1}), () => {
            document.title = 'busy';
            setTimeout(() => { while (true) {} }, 0);
        });
    });
});
)JS");
        QVERIFY(Write(Dir.filePath("index.html"), Page)); QVERIFY(Write(Dir.filePath("app.js"), Script));
        auto Surface = HwpBundleView::Create(HwpBundle::LoadPinned(Dir.path(),
            {{"index.html", Hash(Page)}, {"app.js", Hash(Script)}}, Error), Error);
        QVERIFY2(Surface, qPrintable(Error));
        auto* Bridge = Surface->LoadBridge(); const QByteArray Bytes("input");
        QVERIFY(Bridge->Begin({"1", 2, 3, 4, 5, "hwp", Hash(Bytes)}, Bytes, Error, 3000));
        QSignalSpy Retirement(Bridge, &HwpLoadBridge::RetirementRequested), Failed(Bridge, &HwpLoadBridge::Failed);
        QSignalSpy Cancelled(Bridge, &HwpLoadBridge::Cancelled);
        Surface->resize(640, 480); Surface->show();
        QTRY_COMPARE_WITH_TIMEOUT(Surface->View()->title(), QString("busy"), 2500);
        const auto Pid = Surface->View()->page()->renderProcessPid();
        QVERIFY(Pid > 0 && Pid != GetCurrentProcessId());
        const auto Handle = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(Pid));
        QVERIFY(Handle);
        const auto Close = [](void* Value) {
            // Also clean up the owned busy renderer when an assertion returns early.
            if (WaitForSingleObject(Value, 0) == WAIT_TIMEOUT) TerminateProcess(Value, 1);
            CloseHandle(Value);
        };
        std::unique_ptr<void, decltype(Close)> Process(Handle, Close);
        QTRY_COMPARE_WITH_TIMEOUT(Retirement.size(), 1, 5000);
        QCOMPARE(Failed.first().first().toString(), QString("rhwp-load-timeout"));
        QCOMPARE(WaitForSingleObject(Handle, 0), DWORD(WAIT_TIMEOUT));
        QCOMPARE(Cancelled.size(), 0); QCOMPARE(Bridge->CurrentState(), HwpLoadBridge::State::Failed);
        // Only the test's own renderer handle is terminated. This is not a
        // production whole-Job retirement receipt and must not call Retire().
        QVERIFY(TerminateProcess(Handle, 1));
        QTRY_COMPARE_WITH_TIMEOUT(WaitForSingleObject(Handle, 0), DWORD(WAIT_OBJECT_0), 5000);
        QCOMPARE(Cancelled.size(), 0); QCOMPARE(Retirement.size(), 1);
        QCOMPARE(Bridge->CurrentState(), HwpLoadBridge::State::Failed);
    }
    void typedLoadBoundary()
    {
        HwpLoadBridge Bridge; QString Error;
        const QByteArray Bytes("native input");
        HwpNativeLoad Request{"9007199254740993", 2, 3, 4, 5, "hwp", Hash(Bytes)};
        auto Bad = Request; Bad.sha256_ = Hash("wrong");
        QVERIFY(!Bridge.Begin(Bad, Bytes, Error));
        QVERIFY(Bridge.Begin(Request, Bytes, Error));
        QVERIFY(!Bridge.Begin(Request, Bytes, Error));
        auto Identity = QJsonObject::fromVariantMap(Bridge.PendingOpen());
        QCOMPARE(Identity.take("documentId").toString(), QString("9007199254740993"));
        Identity["documentId"] = "9007199254740993";
        for (const auto* Key : {"readOnly", "byteLength", "chunkSize", "sha256"}) Identity.remove(Key);
        const auto Operation = Identity["operation"].toString();
        QCOMPARE(QByteArray::fromBase64(Bridge.ReadChunk(Operation, 0).toLatin1()), Bytes);
        QVERIFY(Bridge.ReadChunk("stale", 0).isEmpty());
        QVERIFY(Bridge.ReadChunk(Operation, -1).isEmpty());
        QVERIFY(Bridge.ReadChunk(Operation, 1).isEmpty());
        auto Send = [&](QJsonObject Value) {
            return Bridge.Report(QString::fromUtf8(QJsonDocument(Value).toJson(QJsonDocument::Compact)));
        };
        auto Frame = Identity; Frame["type"] = "FirstFrame"; Frame["page"] = 0; Frame["width"] = 100; Frame["height"] = 80;
        QVERIFY(!Send(Frame));
        auto Parsed = Identity; Parsed["type"] = "Parsed"; Parsed["pageCount"] = 1;
        auto Stale = Parsed; Stale["bindingGeneration"] = "3"; QVERIFY(!Send(Stale));
        Stale = Parsed; Stale["epoch"] = 5; QVERIFY(!Send(Stale));
        Stale = Parsed; Stale["unexpected"] = true; QVERIFY(!Send(Stale));
        QVERIFY(!Bridge.Report(QString(4097, 'x')));
        QVERIFY(Send(Parsed)); QVERIFY(!Send(Parsed));
        QSignalSpy Ready(&Bridge, &HwpLoadBridge::Ready);
        QVERIFY(Send(Frame)); QCOMPARE(Ready.size(), 1); QVERIFY(!Send(Frame));
        QVERIFY(Bridge.ReadChunk(Operation, 0).isEmpty());
        Bridge.Retire(); QCOMPARE(Ready.size(), 1);
    }
    void startupTraceBoundaries()
    {
        startup::Recorder Off(false);QCOMPARE(Off.Begin("ignored"),-1);QVERIFY(Off.Snapshot().isEmpty());
        startup::Recorder On(true);On.SetIdentity(QString(32,'a'),"test");
        const auto Index=On.Begin("test.span");On.End(Index,10,1);On.End(Index,99,99);
        const auto Unfinished=On.Begin("test.unfinished");Q_UNUSED(Unfinished);
        auto Pending=On.Snapshot()["events"].toArray().last().toObject();QVERIFY(Pending["open"].toBool());QVERIFY(!Pending.contains("durationMs"));
        auto Snap=On.Snapshot();const auto Entry=Snap["events"].toArray().first().toObject();
        QCOMPARE(Entry["bytes"].toInteger(),qint64(10));QVERIFY(Entry["durationMs"].toDouble(-1)>=0);
        QJsonArray Renderer{QJsonObject{{"name","js.inputHash"},{"kind","span"},{"startMs",0},{"durationMs",1},{"count",1}}};
        auto Bad=Renderer;auto B=Bad[0].toObject();B["name"]="private-source-path";Bad[0]=B;QVERIFY(!On.AcceptRenderer(Bad));
        B=Renderer[0].toObject();B["durationMs"]=-1;Bad[0]=B;QVERIFY(!On.AcceptRenderer(Bad));
        B=Renderer[0].toObject();B["extra"]=true;Bad[0]=B;QVERIFY(!On.AcceptRenderer(Bad));
        QVERIFY(On.AcceptRenderer(Renderer));QVERIFY(!On.AcceptRenderer(Renderer));
        for(int I=0;I<500;++I){const auto At=On.Begin("bounded");On.End(At);}
        Snap=On.Snapshot();QCOMPARE(Snap["events"].toArray().size(),startup::Recorder::Capacity);QVERIFY(Snap["dropped"].toInt()>0);
        QVERIFY(QJsonDocument(Snap).toJson().size()<64*1024);
    }
    void deferredRenderBoundary()
    {
        HwpLoadBridge Bridge; QString Error;const QByteArray Bytes("input");
        QVERIFY(Bridge.Begin({"1",2,3,4,5,"hwp",Hash(Bytes)},Bytes,Error,120,true));
        auto Identity=QJsonObject::fromVariantMap(Bridge.PendingOpen());
        QCOMPARE(Identity.take("renderMode").toString(),QString("viewport-v1"));
        for(const auto* Key:{"readOnly","byteLength","chunkSize","sha256"})Identity.remove(Key);
        const auto Send=[&](const char* Type,QJsonObject Fields=QJsonObject{}){auto V=Identity;V["type"]=Type;
            for(auto I=Fields.begin();I!=Fields.end();++I)V[I.key()]=I.value();return Bridge.Report(QString::fromUtf8(QJsonDocument(V).toJson()));};
        QSignalSpy Ready(&Bridge,&HwpLoadBridge::Ready),Failed(&Bridge,&HwpLoadBridge::Failed),Late(&Bridge,&HwpLoadBridge::ViewFailed),Retirement(&Bridge,&HwpLoadBridge::RetirementRequested);
        QVERIFY(Send("Parsed",{{"pageCount",3}}));QVERIFY(Send("FirstFrame",{{"page",0},{"width",100},{"height",80}}));
        QJsonObject Begin{{"page",1},{"sequence","1"}},End=Begin;End["width"]=100;End["height"]=80;
        QVERIFY(!Send("ViewRenderEnd",End));QVERIFY(Send("ViewRenderBegin",Begin));QVERIFY(!Send("ViewRenderBegin",Begin));
        auto Wrong=End;Wrong["bindingGeneration"]="99";QVERIFY(!Send("ViewRenderEnd",Wrong));
        Wrong=End;Wrong["page"]=2;QVERIFY(!Send("ViewRenderEnd",Wrong));
        Wrong=End;Wrong["sequence"]="2";QVERIFY(!Send("ViewRenderEnd",Wrong));
        QVERIFY(Send("ViewRenderEnd",End));QVERIFY(!Send("ViewRenderEnd",End));
        auto Next=Begin;Next["page"]=2;Next["sequence"]="2";QVERIFY(Send("ViewRenderBegin",Next));
        QVERIFY(!Send("ViewRenderEnd",End)); // Duplicate old End must not disarm page 2.
        QTest::qWait(150);QCOMPARE(Late.size(),1);QCOMPARE(Failed.size(),0);QCOMPARE(Ready.size(),1);QCOMPARE(Retirement.size(),1);
        QCOMPARE(Bridge.CurrentState(),HwpLoadBridge::State::Failed);QVERIFY(!Send("ViewFailed"));
        End=Next;End["width"]=100;End["height"]=80;QVERIFY(!Send("ViewRenderEnd",End));Bridge.Retire();Bridge.Retire();QCOMPARE(Retirement.size(),1);
    }
    void chunkAndFailureBoundary()
    {
        const QByteArray Bytes(192 * 1024 + 17, 'a');
        HwpLoadBridge Bridge; QString Error;
        QVERIFY(Bridge.Begin({"1", 2, 3, 4, 5, "hwp", Hash(Bytes)}, Bytes, Error));
        const auto Operation = Bridge.PendingOpen()["operation"].toString();
        const auto First = QByteArray::fromBase64(Bridge.ReadChunk(Operation, 0).toLatin1());
        const auto Last = QByteArray::fromBase64(Bridge.ReadChunk(Operation, 192 * 1024).toLatin1());
        QCOMPARE(First.size(), 192 * 1024); QCOMPARE(Last.size(), 17); QCOMPARE(First + Last, Bytes);
        QVERIFY(Bridge.ReadChunk(Operation, 384 * 1024).isEmpty());
        QSignalSpy Failed(&Bridge, &HwpLoadBridge::Failed);
        Bridge.FailLoad(); Bridge.FailLoad(); Bridge.Retire();
        QCOMPARE(Failed.size(), 1); QVERIFY(Bridge.PendingOpen().isEmpty());
    }
    void cancellationNeedsNativeRetirement()
    {
        HwpLoadBridge Bridge; QString Error; const QByteArray Bytes("input");
        QVERIFY(Bridge.Begin({"1", 2, 3, 4, 5, "hwpx", Hash(Bytes)}, Bytes, Error));
        auto Report = QJsonObject::fromVariantMap(Bridge.PendingOpen());
        for (const auto* Key : {"readOnly", "byteLength", "chunkSize", "sha256"}) Report.remove(Key);
        QSignalSpy Retirement(&Bridge, &HwpLoadBridge::RetirementRequested);
        Bridge.RequestCancel();
        QCOMPARE(Retirement.size(), 1);
        QSignalSpy Cancelled(&Bridge, &HwpLoadBridge::Cancelled);
        Report["type"] = "CancelObserved";
        QVERIFY(Bridge.Report(QString::fromUtf8(QJsonDocument(Report).toJson())));
        QCOMPARE(Cancelled.size(), 0);
        QCOMPARE(Bridge.CurrentState(), HwpLoadBridge::State::CancelPending);
        QVERIFY(Bridge.PendingOpen().isEmpty());
        Bridge.FailLoad(); QCOMPARE(Bridge.CurrentState(), HwpLoadBridge::State::CancelPending);
        Bridge.Retire(); Bridge.Retire(); QCOMPARE(Cancelled.size(), 1);
    }
    void typedWebChannelFrame()
    {
        QTemporaryDir Dir; QString Error;
        const QByteArray Page("<!doctype html><canvas width='100' height='80'></canvas><script src='app.js'></script>");
        const QByteArray Script(R"JS(
new QWebChannel(qt.webChannelTransport, channel => {
    const bridge = channel.objects.loadBridge;
    bridge.PendingOpen(meta => {
        bridge.ReadChunk(meta.operation, 0, data => {
            if (atob(data) !== 'native fixture') { document.title = 'bad-bytes'; return; }
            const identity = {...meta};
            for (const key of ['readOnly','byteLength','chunkSize','sha256']) delete identity[key];
            bridge.Report(JSON.stringify({...identity,type:'Parsed',pageCount:1}), accepted => {
                if (!accepted) { document.title = 'bad-parse'; return; }
                const canvas = document.querySelector('canvas');
                canvas.getContext('2d').fillRect(0,0,100,80);
                requestAnimationFrame(() => requestAnimationFrame(() => {
                    bridge.Report(JSON.stringify({...identity,type:'FirstFrame',page:0,width:100,height:80}));
                }));
            });
        });
    });
});
)JS");
        QVERIFY(Write(Dir.filePath("index.html"), Page)); QVERIFY(Write(Dir.filePath("app.js"), Script));
        auto Bundle = HwpBundle::LoadPinned(Dir.path(), {{"index.html", Hash(Page)}, {"app.js", Hash(Script)}}, Error);
        auto Surface = HwpBundleView::Create(Bundle, Error); QVERIFY2(Surface, qPrintable(Error));
        auto* Bridge = Surface->LoadBridge(); QVERIFY(Bridge);
        const QByteArray Bytes("native fixture");
        QVERIFY(Bridge->Begin({"1", 2, 3, 4, 5, "hwp", Hash(Bytes)}, Bytes, Error));
        QSignalSpy Ready(Bridge, &HwpLoadBridge::Ready);
        Surface->resize(640, 480); Surface->show();
        QTRY_COMPARE_WITH_TIMEOUT(Ready.size(), 1, 20000);
        QCOMPARE(Bridge->CurrentState(), HwpLoadBridge::State::Ready);
    }
    void actualEngineFirstFrame_data()
    {
        QTest::addColumn<QString>("Fixture");
        QTest::addColumn<QString>("Format");
        QTest::addColumn<bool>("Success");
        QTest::newRow("hwp-body") << "c02.hwp" << "hwp" << true;
        QTest::newRow("hwpx-table") << "c05.hwpx" << "hwpx" << true;
        QTest::newRow("five-page-hwp") << "c06.hwp" << "hwp" << true;
        QTest::newRow("five-page-hwpx") << "c07.hwpx" << "hwpx" << true;
        QTest::newRow("format-mismatch") << "c02.hwp" << "hwpx" << false;
        QTest::newRow("damaged-input") << "damaged" << "hwp" << false;
    }
    void actualEngineFirstFrame()
    {
        QFETCH(QString, Fixture); QFETCH(QString, Format); QFETCH(bool, Success);
        const auto Site = qEnvironmentVariable("HC_RHWP_TEST_ENGINE");
        const auto Corpus = qEnvironmentVariable("HC_RHWP_TEST_CORPUS");
        if (Site.isEmpty() || Corpus.isEmpty()) QSKIP("Local pinned RHWP diagnostic engine/corpus not requested");
        auto Read = [](const QString& Path) {
            QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{};
        };
        QTemporaryDir Dir; QMap<QString, QByteArray> Pins; QString Error;
        for (const auto* Name : {"rhwp.js", "rhwp_bg.wasm"}) {
            const auto Bytes = Read(Site + '/' + Name); QVERIFY(!Bytes.isEmpty());
            const auto Expected = QByteArray(Name) == "rhwp.js" ? fixture::EngineJsSha : fixture::EngineWasmSha;
            QCOMPARE(Hash(Bytes), Expected);
            QVERIFY(Write(Dir.filePath(Name), Bytes)); Pins[Name] = Expected;
        }
        const auto Adapter = Read(QFINDTESTDATA("../../UniqueLibs/document/rhwp/readOnlyAdapter.js"));
        QVERIFY(!Adapter.isEmpty());
        const QByteArray Page("<!doctype html><title>loading</title><canvas></canvas><script type='module' src='app.js'></script>");
        const QByteArray Script(R"JS(
import * as Engine from './rhwp.js';
import {OpenReadOnly} from './adapter.js';
new QWebChannel(qt.webChannelTransport, channel => {
    OpenReadOnly(channel.objects.loadBridge, Engine, document.querySelector('canvas'))
        .then(() => { document.title = 'rendered'; })
        .catch(error => { document.title = 'failed:' + String(error); });
});
)JS");
        QVERIFY(Write(Dir.filePath("index.html"), Page)); Pins["index.html"] = Hash(Page);
        QVERIFY(Write(Dir.filePath("app.js"), Script)); Pins["app.js"] = Hash(Script);
        QVERIFY(Write(Dir.filePath("adapter.js"), Adapter)); Pins["adapter.js"] = Hash(Adapter);
        const auto Bytes = Fixture == "damaged" ? QByteArray("damaged document") : Read(Corpus + '/' + Fixture);
        QVERIFY(!Bytes.isEmpty());
        if (Fixture == "c06.hwp") QCOMPARE(Hash(Bytes), QByteArray("a854b822c3150d9731b8ba0687160b2120be1078fc5c91221566a15176de3e20"));
        if (Fixture == "c07.hwpx") QCOMPARE(Hash(Bytes), QByteArray("4405f89db156da688d1a10ae0c0829f0159ad24ad4ad5e083902577a8304338a"));
        if (Fixture == "c02.hwp") QCOMPARE(Hash(Bytes), QByteArray("86e607233a2218928fabe5969155116a46d4ab4400db1402fe45b0082149496c"));
        if (Fixture == "c05.hwpx") QCOMPARE(Hash(Bytes), QByteArray("8d9df52bb09803fa1754317360e27709621555b3e1163a0d7eb4ad9abab5b26b"));
        auto Bundle = HwpBundle::LoadPinned(Dir.path(), Pins, Error); QVERIFY2(Bundle, qPrintable(Error));
        HwpHelperDocument Document(Bundle);
        const QJsonObject Identity{{"documentId", "engine-document-alpha"}, {"generation", "2"},
            {"token", "3"}, {"bindingOwner", "4"}, {"bindingGeneration", "5"}, {"epoch", "6"}};
        auto Command = [&](const char* Type) {
            return QJsonObject{{"version", 1}, {"type", Type}, {"identity", Identity}};
        };
        auto Begin = Command("InputBegin"); Begin["format"] = Format; Begin["readOnly"] = true;
        Begin["byteLength"] = Bytes.size(); Begin["sha256"] = QString::fromLatin1(Hash(Bytes));
        QCOMPARE(Document.Handle(Begin)["type"].toString(), QString("InputAccepted"));
        for(qsizetype Offset = 0; Offset < Bytes.size(); Offset += 192 * 1024) {
            auto Chunk = Command("InputChunk"); Chunk["offset"] = Offset;
            Chunk["data"] = QString::fromLatin1(Bytes.mid(Offset, 192 * 1024).toBase64());
            QCOMPARE(Document.Handle(Chunk)["type"].toString(), QString("InputChunkAccepted"));
        }
        QCOMPARE(Document.Handle(Command("InputCommit"))["type"].toString(), QString("InputVerified"));
        QVERIFY(!Document.RendererCreated());
        auto Stale = Command("DocumentStart"); Stale["identity"] = QJsonObject{};
        QVERIFY(Document.Handle(Stale).contains("error")); QVERIFY(!Document.RendererCreated());
        QCOMPARE(Document.Handle(Command("DocumentStart"))["state"].toString(), QString("loading"));
        QVERIFY(Document.RendererCreated()); QVERIFY(Document.Handle(Command("DocumentStart")).contains("error"));
        QJsonObject Status;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            Status = Document.Handle(Command("DocumentStatus"));
            return Status["state"] == QJsonValue("ready") || Status["state"] == QJsonValue("failed");
        })(), 20000);
        QCOMPARE(Status["identity"].toObject(), Identity);
        QCOMPARE(Status["state"].toString(), QString(Success ? "ready" : "failed"));

    }
    void actualEnginePrintSnapshot() {
        const auto Site=qEnvironmentVariable("HC_RHWP_TEST_ENGINE"),Corpus=qEnvironmentVariable("HC_RHWP_TEST_CORPUS");
        if(Site.isEmpty()||Corpus.isEmpty())QSKIP("Pinned engine/corpus required");
        auto Read=[](const QString& Path){QFile File(Path);return File.open(QIODevice::ReadOnly)?File.readAll():QByteArray{};};
        QTemporaryDir Dir;QMap<QString,QByteArray> Pins;QString Error;
        for(const auto* Name:{"rhwp.js","rhwp_bg.wasm"}){
            const auto Bytes=Read(Site+'/'+Name);const auto Expected=QByteArray(Name)=="rhwp.js"?fixture::EngineJsSha:fixture::EngineWasmSha;
            QCOMPARE(Hash(Bytes),Expected);QVERIFY(Write(Dir.filePath(Name),Bytes));Pins[Name]=Expected;
        }
        const auto Input=Read(Corpus+"/c07.hwpx");QCOMPARE(Hash(Input),QByteArray("4405f89db156da688d1a10ae0c0829f0159ad24ad4ad5e083902577a8304338a"));
        const auto Adapter=Read(QFINDTESTDATA("../../UniqueLibs/document/rhwp/printSnapshot.js"));QVERIFY(!Adapter.isEmpty());
        const QByteArray Page("<!doctype html><title>loading</title><script type='module' src='app.js'></script>");
        const QByteArray Script(R"JS(
import * as Engine from './rhwp.js';
import {RenderPrintSnapshot} from './print.js';
try {
    const Bytes=new Uint8Array(await (await fetch('./input.bin')).arrayBuffer());
    window.printFrames=await RenderPrintSnapshot(Engine,Bytes,undefined,()=>document.createElement('canvas'));
    document.title='printed';
} catch(Error) {document.title='failed:'+String(Error);}
)JS");
        const QMap<QString,QByteArray> Assets{{"index.html",Page},{"app.js",Script},{"input.bin",Input},{"print.js",Adapter}};
        for(auto At=Assets.cbegin();At!=Assets.cend();++At){QVERIFY(Write(Dir.filePath(At.key()),At.value()));Pins[At.key()]=Hash(At.value());}
        auto Bundle=HwpBundle::LoadPinned(Dir.path(),Pins,Error);QVERIFY2(Bundle,qPrintable(Error));
        auto Surface=HwpBundleView::Create(Bundle,Error);QVERIFY2(Surface,qPrintable(Error));Surface->resize(500,400);Surface->show();
        QTRY_VERIFY_WITH_TIMEOUT(Surface->View()->title()=="printed" || Surface->View()->title().startsWith("failed:"),30000);
        QCOMPARE(Surface->View()->title(),QString("printed"));
        bool Received=false;QString Json;
        Surface->View()->page()->runJavaScript("JSON.stringify(window.printFrames)",[&](const QVariant& Value){Json=Value.toString();Received=true;});
        QTRY_VERIFY_WITH_TIMEOUT(Received,10000);
        const auto Object=QJsonDocument::fromJson(Json.toUtf8()).object();QCOMPARE(Object["totalPages"].toInt(),5);
        const DocumentOperationTicket Ticket{"print-real",2,3,4,DocumentOperation::Printing,{5,0,"snapshot"},{}};
        HwpPrintFrames Frames{Ticket,5,{}};
        for(const auto Value:Object["pages"].toArray()){
            const auto Item=Value.toObject();Frames.pages_.push_back({Item["page"].toInt(),{Item["width"].toDouble(),Item["height"].toDouble()},QByteArray::fromBase64(Item["png"].toString().toLatin1())});
        }
        QCOMPARE(Frames.pages_.size(),size_t(5));auto Pdf=BuildHwpPdf(Ticket,{0,1,2,3,4},Frames);QVERIFY(Pdf.HasValue());
        const auto Output=qEnvironmentVariable("HC_RHWP_TEST_PRINT_OUTPUT");
        if(!Output.isEmpty())QVERIFY(Write(Output,Pdf.Value()));
    }
    void explicitRender()
    {
        QTemporaryDir Dir; QString Error; QVERIFY(Write(Dir.filePath("index.html"), Html));
        const QByteArray Script("document.title = window.inlineRan ? 'unsafe' : 'executed';");
        const QByteArray Page("<!doctype html><title>pinned</title><script>window.inlineRan=true;</script><script src='app.js'></script>");
        QVERIFY(Write(Dir.filePath("index.html"), Page));
        QVERIFY(Write(Dir.filePath("app.js"), Script));
        auto Bundle = HwpBundle::LoadPinned(Dir.path(), {{"index.html", Hash(Page)}, {"app.js", Hash(Script)}}, Error);
        auto Surface = HwpBundleView::Create(Bundle, Error); QVERIFY2(Surface != nullptr, qPrintable(Error));
        QVERIFY(Surface->IsOffTheRecord()); Surface->resize(640, 480); Surface->show();
        QTRY_COMPARE_WITH_TIMEOUT(Surface->View()->title(), QString("executed"), 20000);
        QSignalSpy Navigation(Surface->View(), &QWebEngineView::loadFinished);
        Surface->View()->load(QUrl("file:///unapproved.html"));
        QTRY_VERIFY_WITH_TIMEOUT(!Navigation.isEmpty(), 5000);
        QVERIFY(!Navigation.last().first().toBool());
    }
};
int main(int Argc, char** Argv)
{
    if (!HwpWebEnvironmentAllowed() || !RegisterHwpScheme()) return 2;
    QApplication App(Argc, Argv);
    BundleTests Tests;
    return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstHwpBundle.moc"
