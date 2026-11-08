#include "stdafx.h"
#include "UniqueLibs/archivePreviewWriteback.hpp"
#include "UniqueLibs/wcxStorage.hpp"
#include "UniqueLibs/document/documentHostBroker.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "HyperCommanderBuiltInPlugin/SevenZip/builtinSevenZip.hpp"
#include <QtTest>
#include <QCryptographicHash>
#include <QMessageBox>
#include <QAbstractButton>
#include <QTimer>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QScopeGuard>
#include <future>

using namespace hc::storage;
using namespace hc::document;
namespace {
bool Put(const QString& Path, const QByteArray& Bytes) { QFile File(Path); return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size(); }
QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
template<class F> auto Worker(F&& Fn) {
    auto Future = std::async(std::launch::async, std::forward<F>(Fn));
    while (Future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) QTest::qWait(5);
    return Future.get();
}
QJsonObject Document(const DocumentWorkspaceWidget& Window) { return Window.Snapshot()["documents"].toArray().first().toObject(); }
}
class PreviewArchiveTests final : public QObject {
    Q_OBJECT
private slots:
    void closeTransaction_data() {
        QTest::addColumn<bool>("Nested"); QTest::addColumn<int>("Decision");
        QTest::newRow("zip-update") << false << int(QMessageBox::Yes);
        QTest::newRow("nested-zip-update") << true << int(QMessageBox::Yes);
        QTest::newRow("discard-saved-draft") << false << int(QMessageBox::No);
    }
    void closeTransaction() {
        QFETCH(bool, Nested); QFETCH(int, Decision);
        QTemporaryDir Root; QVERIFY(Root.isValid());
        QVERIFY(Put(Root.filePath("inside.txt"), "original")); QVERIFY(Put(Root.filePath("keep.txt"), "untouched"));
        auto Backend = hc::plugins::MakeApprovedWcxBackend(hc::plugins::BuiltinSevenZipOptions());
        const auto Shutdown = qScopeGuard([&] { hc::plugins::CloseApprovedWcxBackend(Backend); });
        const auto Inner = Root.filePath("inner.zip"), Outer = Root.filePath("outer.zip");
        QVERIFY(Worker([&] { return Backend->Update({Inner.toStdString(), Root.path().toStdString(), {"inside.txt", "keep.txt"}}, {}); }).HasValue());
        if (Nested) QVERIFY(Worker([&] { return Backend->Update({Outer.toStdString(), Root.path().toStdString(), {"inner.zip", "keep.txt"}}, {}); }).HasValue());
        const auto ArchivePath = Nested ? Outer : Inner;
        Locator Archive{LocatorKind::Archive, ArchivePath.toStdString(), {{"wcx", "/"}}};
        if (Nested) { Archive.nesting_.back().entry_ = "/inner.zip"; Archive.nesting_.push_back({"wcx", "/"}); }
        auto Reader = MakeWcxReadProvider({}, Archive, Root.path().toStdString(), Backend);
        auto Location = Archive; Location.nesting_.back().entry_ = "/inside.txt";
        auto Entry = Worker([&] { return Reader->Resolve(Location, {}).get(); }); QVERIFY2(Entry.HasValue(), Entry.HasValue() ? "" : Entry.Failure().code_.c_str());
        // The same provider/source used by the UI retains the probe decoder.
        auto Source = std::make_shared<ProviderContentSource>(Reader, Entry.Value());
        auto ProbeStream = Worker([&] { return Source->OpenRead({}).get(); }); QVERIFY(ProbeStream.HasValue());
        auto Prefix = Worker([&] { return ProbeStream.Value()->Read(3, {}).get(); }); QVERIFY(Prefix.HasValue());
        Source->RememberPrefix(Prefix.Value().bytes_, ProbeStream.Value());
        auto Replayed = Worker([&] { return Source->OpenRead({}).get(); }); QVERIFY(Replayed.HasValue());
        QVERIFY(!Replayed.Value()->CanSeek());
        auto First = Worker([&] { return Replayed.Value()->Read(1024, {}).get(); }); QVERIFY(First.HasValue());
        QCOMPARE(First.Value().bytes_.size(), size_t(3));
        auto Rest = Worker([&] { return Replayed.Value()->Read(1024, {}).get(); }); QVERIFY(Rest.HasValue()); QVERIFY(Rest.Value().eof_);
        auto Combined = First.Value().bytes_; Combined.insert(Combined.end(), Rest.Value().bytes_.begin(), Rest.Value().bytes_.end());
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(Combined.data()), qsizetype(Combined.size())), QByteArray("original"));
        Replayed.Value().reset(); ProbeStream.Value().reset(); Source.reset();
        auto Events = std::make_shared<hc::commands::EventHub>(); hc::operations::JobScheduler Scheduler(Events);
        auto Callback = MakeArchivePreviewWriteback(Archive, Entry.Value(), Reader, Backend, {}, Scheduler);
        const auto Before = Read(ArchivePath); int Updates = 0;
        DocumentHostBroker Broker;
        const auto Token = Broker.Bind(Entry.Value(), {}, {}, QCryptographicHash::hash("original", QCryptographicHash::Sha256),
            [&](const std::string& Path, OperationContext Context) { ++Updates; return Callback(Path, Context); });
        DocumentWorkspaceWidget Window(Root.filePath("recovery"), true); Window.show();
        const auto Open = Window.Handle({{"action", "open"}, {"path", Root.filePath("inside.txt")}, {"sourceToken", Token}, {"broker", true}});
        const auto Id = Open["documentId"].toString(); QVERIFY(!Id.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!Document(Window)["loading"].toBool(), 10000);
        QVERIFY(!Window.Handle({{"action", "replaceText"}, {"documentId", Id}, {"text", "edited"}}).contains("error"));
        auto Prepared = Window.Handle({{"action", "prepareSave"}, {"documentId", Id}}); Prepared["documentId"] = Id;
        auto Saved = Worker([&] { return Broker.Commit(Prepared); }); QVERIFY(Saved["committed"].toBool());
        QVERIFY(!Window.Handle(Saved).contains("error")); QVERIFY(Document(Window)["archivePending"].toBool());
        const auto Draft = Saved["draftPath"].toString(); QVERIFY(QFileInfo::exists(Draft)); QCOMPARE(Updates, 0); QCOMPARE(Read(ArchivePath), Before);
        // Closing cancellation must keep both the tab and its saved draft.
        QTimer Answer; Answer.setInterval(10); int Choice = QMessageBox::Cancel; int Prompts = 0;
        connect(&Answer, &QTimer::timeout, &Window, [&] {
            if (auto* Box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                if (auto* Button = Box->button(QMessageBox::StandardButton(Choice))) { ++Prompts; Button->click(); }
            }
        }); Answer.start();
        QVERIFY(!Window.RequestViewerClose()); QVERIFY(Prompts > 0); QVERIFY(!Document(Window)["archiveUpdating"].toBool()); QVERIFY(QFileInfo::exists(Draft));
        Choice = Decision; QVERIFY(!Window.RequestViewerClose()); QVERIFY(Document(Window)["archiveUpdating"].toBool());
        QCOMPARE(Document(Window)["archiveDiscard"].toBool(), Decision == QMessageBox::No);
        auto Result = Worker([&] { return Broker.CommitArchive(Token, Decision == QMessageBox::No); });
        QVERIFY2(Result["committed"].toBool(), qPrintable(Result["error"].toString()));
        Result["action"] = "archiveResult"; Result["documentId"] = Id;
        QVERIFY(!Window.Handle(Result).contains("error")); QVERIFY(Window.Snapshot()["documents"].toArray().isEmpty());
        Broker.Close(); QVERIFY(!QFileInfo::exists(Draft));
        auto Fresh = MakeWcxReadProvider({}, Archive, Root.path().toStdString(), Backend);
        auto Data = Worker([&]() -> hc::storage::Result<ReadChunk> {
            auto Current = Fresh->Resolve(Location, {}).get(); if (!Current.HasValue()) return Current.Failure();
            auto Stream = Fresh->OpenRead(Current.Value().id_, {}).get(); if (!Stream.HasValue()) return Stream.Failure();
            return Stream.Value()->Read(4096, {}).get();
        }); QVERIFY(Data.HasValue());
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(Data.Value().bytes_.data()), qsizetype(Data.Value().bytes_.size())), Decision == QMessageBox::Yes ? QByteArray("edited") : QByteArray("original"));
        if (Decision == QMessageBox::Yes) {
            // A second editor with the captured old snapshot must not overwrite the commit.
            QVERIFY(Put(Root.filePath("inside.txt"), "second editor"));
            auto Conflict = Worker([&] { return Callback(Root.filePath("inside.txt").toStdString(), {}); }); QVERIFY(!Conflict.HasValue());
        } else QCOMPARE(Read(ArchivePath), Before);
        auto Keep = Archive; Keep.nesting_.back().entry_ = "/keep.txt";
        auto Kept = Worker([&] { return Fresh->Resolve(Keep, {}).get(); }); QVERIFY(Kept.HasValue()); QCOMPARE(Kept.Value().size_, uint64_t(9));
    }
};
QTEST_MAIN(PreviewArchiveTests)
#include "tstPreviewArchive.moc"