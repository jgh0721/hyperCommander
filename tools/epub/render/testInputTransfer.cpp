#include "stdafx.h"
#include "commonLibs/document/epub/epubInputSender.hpp"
#include "inputTransfer.hpp"

/// Platform/framework headers
#include <QtTest>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

using hc::epub::poc::InputTransfer;
namespace
{
    QJsonObject request(const char* Kind)
    { return {{"protocolVersion", 1}, {"kind", Kind}, {"documentId", QString(64, 'a')}, {"generation", 7}}; }
    QJsonObject begin(const QByteArray& Data)
    {
        auto Value = request("input-begin"); Value["byteLength"] = Data.size();
        Value["sha256"] = QString::fromLatin1(QCryptographicHash::hash(Data, QCryptographicHash::Sha256).toHex());
        Value["engine"] = "readium"; Value["readOnly"] = true; return Value;
    }
    QJsonObject chunk(const QByteArray& Data, qsizetype Offset = 0)
    { auto Value = request("input-chunk"); Value["data"] = QString::fromLatin1(Data.toBase64()); Value["offset"] = Offset; return Value; }
}
class InputTransferTests final : public QObject
{
    Q_OBJECT
private slots:
    void mappedArchive() {
        const QByteArray Bytes("PK immutable archive snapshot");
        const auto Mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,DWORD(Bytes.size()),nullptr);
        QVERIFY(Mapping);
        auto* View=MapViewOfFile(Mapping,FILE_MAP_WRITE,0,0,Bytes.size());QVERIFY(View);
        memcpy(View,Bytes.constData(),Bytes.size());UnmapViewOfFile(View);
        HANDLE ReadOnly=nullptr;
        QVERIFY(DuplicateHandle(GetCurrentProcess(),Mapping,GetCurrentProcess(),&ReadOnly,FILE_MAP_READ,FALSE,0));CloseHandle(Mapping);
        QVERIFY(!MapViewOfFile(ReadOnly,FILE_MAP_WRITE,0,0,Bytes.size()));
        const auto Hash=QString::fromLatin1(QCryptographicHash::hash(Bytes,QCryptographicHash::Sha256).toHex());
        QJsonObject Request{{"protocolVersion",1},{"kind","input-map"},{"documentId",Hash},{"generation",7},
            {"epubInputVersion",2},{"engine","epubjs"},{"byteLength",Bytes.size()},{"sha256",Hash},{"handle",QString::number(quintptr(ReadOnly))}};
        InputTransfer Transfer;QVERIFY(Transfer.Handle(Request)["accepted"].toBool());
        const auto Input=Transfer.Take();QVERIFY(Input);QCOMPARE(Input->bytes,Bytes);QCOMPARE(Input->inputVersion,2);
        QVERIFY(Transfer.Handle(Request).contains("error"));QVERIFY(!Transfer.Take());
        InputTransfer Rejected;Request["byteLength"]=65*1024*1024;
        QCOMPARE(Rejected.Handle(Request)["error"],QJsonValue("epub-mapping-admission"));
        Request["byteLength"]=Bytes.size();Request["epubInputVersion"]=1;
        QCOMPARE(Rejected.Handle(Request)["error"],QJsonValue("epub-mapping-admission"));
    }
    void senderNegotiatesChunkBudget_data() {
        QTest::addColumn<int>("Limit");QTest::addColumn<bool>("Accepted");
        QTest::newRow("legacy")<<0<<true;QTest::newRow("bounded")<<512*1024<<true;
        QTest::newRow("too-large")<<512*1024+1<<false;QTest::newRow("too-small")<<1024<<false;
    }
    void senderNegotiatesChunkBudget() {
        QFETCH(int,Limit);QFETCH(bool,Accepted);InputTransfer Transfer;int Chunks=0;bool Bounded=true;
        const QByteArray Data(1024*1024+17,'a');
        const auto Result=hc::document::epub::SendInput(Data,QString(64,'a'),7,"epubjs",[&](const QByteArray& Bytes,QByteArray& Reply,int){
            const auto Request=QJsonDocument::fromJson(Bytes).object();if(Request["kind"]=="input-chunk"){++Chunks;Bounded&=Bytes.size()<768*1024;}
            auto Response=Transfer.Handle(Request);if(Request["kind"]=="input-begin"){if(Limit)Response["chunkBytes"]=Limit;else Response.remove("chunkBytes");}
            Reply=QJsonDocument(Response).toJson(QJsonDocument::Compact);return true;
        });
        QCOMPARE(Result.accepted_,Accepted);QVERIFY(Bounded);if(Accepted){QCOMPARE(Chunks,Limit?3:6);const auto Input=Transfer.Take();QVERIFY(Input);QCOMPARE(Input->bytes,Data);}
        else {QCOMPARE(Chunks,0);QCOMPARE(Result.error_,QString("epub-send-chunk-budget"));}
    }
    void senderLostCommitAcknowledgement()
    {
        InputTransfer Transfer;
        const auto Result = hc::document::epub::SendInput("payload", QString(64, 'a'), 7, "epubjs",
            [&](const QByteArray& Request, QByteArray& Reply, int) {
                const auto Value = QJsonDocument::fromJson(Request).object();
                Reply = QJsonDocument(Transfer.Handle(Value)).toJson();
                return Value["kind"] != "input-commit";
            });
        QVERIFY(!Result.accepted_); QVERIFY(Result.retireRequired_);
        QCOMPARE(Result.error_, QString("epub-send-transport"));
        // The helper accepted the bytes, but the owner cannot claim success without its acknowledgement.
        QCOMPARE(Transfer.Status()["status"], QJsonValue("verified"));
    }
    void senderRoundTrip_data()
    { QTest::addColumn<bool>("Preparing"); QTest::newRow("verified") << false; QTest::newRow("preparing") << true; }
    void senderRoundTrip()
    {
        InputTransfer Transfer;
        QFETCH(bool, Preparing);
        const QByteArray Data(InputTransfer::ChunkBytes * 2 + 17, 'a');
        int Calls = 0;
        const auto Result = hc::document::epub::SendInput(Data, QString(64, 'a'), 7, "epubjs",
            [&](const QByteArray& Request, QByteArray& Reply, int Timeout) {
                if(Timeout < 1 || Timeout > 5000) return false;
                ++Calls; const auto Value = QJsonDocument::fromJson(Request).object();
                auto Response = Transfer.Handle(Value);
                if(Preparing && Value["kind"] == "input-commit") Response["status"] = "preparing";
                Reply = QJsonDocument(Response).toJson(); return true;
            });
        QVERIFY(Result.accepted_); QVERIFY(!Result.retireRequired_); QCOMPARE(Calls, 5);
        const auto Input = Transfer.Take(); QVERIFY(Input); QCOMPARE(Input->bytes, Data);
    }
    void senderRejectsReplies_data()
    {
        QTest::addColumn<QString>("Mode");
        for(const auto* Mode : {"transport", "json", "budget", "document", "generation", "version", "accepted", "status", "received"})
            QTest::newRow(Mode) << QString(Mode);
    }
    void senderRejectsReplies()
    {
        QFETCH(QString, Mode); InputTransfer Transfer; int Calls = 0;
        const auto Result = hc::document::epub::SendInput("payload", QString(64, 'a'), 7, "epubjs",
            [&](const QByteArray& Request, QByteArray& Reply, int) {
                ++Calls; auto Response = Transfer.Handle(QJsonDocument::fromJson(Request).object());
                if(Mode == "transport") return false;
                if(Mode == "document") Response["documentId"] = QString(64, 'b');
                if(Mode == "generation") Response["generation"] = 8;
                if(Mode == "version") Response["protocolVersion"] = 2;
                if(Mode == "accepted") Response["accepted"] = false;
                if(Mode == "status") Response["status"] = "verified";
                if(Mode == "received") Response["received"] = 1;
                Reply = Mode == "json" ? QByteArray("{") : Mode == "budget" ? QByteArray(4097, ' ') : QJsonDocument(Response).toJson();
                return true;
            });
        QVERIFY(!Result.accepted_); QVERIFY(Result.retireRequired_); QVERIFY(!Result.error_.isEmpty()); QCOMPARE(Calls, 1);
        QVERIFY(!Transfer.Take());
    }
    void senderCancellationAndTimeout()
    {
        InputTransfer Transfer; bool Cancelled = false; int Calls = 0;
        const auto Exchange = [&](const QByteArray& Request, QByteArray& Reply, int) {
            ++Calls; const auto Value = QJsonDocument::fromJson(Request).object();
            Reply = QJsonDocument(Transfer.Handle(Value)).toJson();
            if(Value["kind"] == "input-chunk") Cancelled = true;
            return true;
        };
        auto Result = hc::document::epub::SendInput("data", QString(64, 'a'), 7, "epubjs", Exchange, [] { return true; });
        QVERIFY(!Result.accepted_); QVERIFY(!Result.retireRequired_); QCOMPARE(Calls, 0);
        Result = hc::document::epub::SendInput("data", QString(64, 'a'), 7, "epubjs", Exchange, [&] { return Cancelled; });
        QVERIFY(!Result.accepted_); QVERIFY(Result.retireRequired_); QCOMPARE(Result.error_, QString("epub-send-cancelled"));
        QCOMPARE(Transfer.Status()["status"], QJsonValue("cancelled")); QVERIFY(!Transfer.Take());
        Result = hc::document::epub::SendInput("data", QString(64, 'a'), 7, "epubjs",
            [](const QByteArray&, QByteArray&, int) { QTest::qSleep(30); return true; }, {}, 20);
        QVERIFY(!Result.accepted_); QVERIFY(Result.retireRequired_); QCOMPARE(Result.error_, QString("epub-send-timeout"));
        Result = hc::document::epub::SendInput("data", "invalid", 7, "epubjs", Exchange);
        QVERIFY(!Result.accepted_); QVERIFY(!Result.retireRequired_); QCOMPARE(Calls, 3);
    }

    void multiChunkAndSingleConsumption()
    {
        const QByteArray Data(InputTransfer::ChunkBytes + 17, 'x'); InputTransfer Transfer;
        QVERIFY(Transfer.Handle(begin(Data))["accepted"].toBool());
        QVERIFY(Transfer.Handle(chunk(Data.first(InputTransfer::ChunkBytes)))["accepted"].toBool());
        auto Stale = chunk(Data.last(17), InputTransfer::ChunkBytes); Stale["generation"] = 8;
        QCOMPARE(Transfer.Handle(Stale)["error"], QJsonValue("epub-input-stale"));
        QVERIFY(Transfer.Handle(chunk(Data.last(17), InputTransfer::ChunkBytes))["accepted"].toBool());
        QVERIFY(Transfer.Handle(request("input-commit"))["accepted"].toBool());
        const auto Input = Transfer.Take(); QVERIFY(Input); QCOMPARE(Input->bytes, Data); QCOMPARE(Input->generation, 7);
        QVERIFY(!Transfer.Take()); QVERIFY(Transfer.Matches(request("snapshot")));
        QCOMPARE(Transfer.Handle(begin(Data))["error"], QJsonValue("epub-input-state"));
        QCOMPARE(Transfer.Handle(request("input-cancel"))["error"], QJsonValue("epub-input-state"));
    }
    void admission_data()
    {
        QTest::addColumn<QString>("Field"); QTest::addColumn<QJsonValue>("Value");
        QTest::newRow("budget") << QString("byteLength") << QJsonValue(InputTransfer::MaxBytes + 1);
        QTest::newRow("empty") << QString("byteLength") << QJsonValue(0);
        QTest::newRow("fraction") << QString("byteLength") << QJsonValue(1.5);
        QTest::newRow("string-size") << QString("byteLength") << QJsonValue("4");
        QTest::newRow("zero-generation") << QString("generation") << QJsonValue(0);
        QTest::newRow("fraction-generation") << QString("generation") << QJsonValue(1.5);
        QTest::newRow("unsafe-generation") << QString("generation") << QJsonValue(9007199254740992.0);
        QTest::newRow("document") << QString("documentId") << QJsonValue("bad");
        QTest::newRow("hash") << QString("sha256") << QJsonValue(QString(64, 'G'));
        QTest::newRow("engine") << QString("engine") << QJsonValue("other");
        QTest::newRow("editing") << QString("readOnly") << QJsonValue(false);
        QTest::newRow("path") << QString("path") << QJsonValue("C:/outside.epub");
    }
    void admission()
    {
        QFETCH(QString, Field); QFETCH(QJsonValue, Value);
        InputTransfer Transfer; auto ValueRequest = begin("data"); ValueRequest[Field] = Value;
        QCOMPARE(Transfer.Handle(ValueRequest)["error"], QJsonValue("epub-input-admission")); QVERIFY(!Transfer.Take());
        QCOMPARE(Transfer.Status()["received"].toInteger(), 0);
    }
    void corruptTransfer_data()
    {
        QTest::addColumn<QString>("Mode");
        for(const auto* Mode : {"duplicate", "gap", "overrun", "base64", "oversized-chunk", "truncated", "hash"})
            QTest::newRow(Mode) << QString::fromLatin1(Mode);
    }
    void corruptTransfer()
    {
        QFETCH(QString, Mode); InputTransfer Transfer;
        QVERIFY(Transfer.Handle(begin("data"))["accepted"].toBool());
        auto Chunk = chunk(Mode == "hash" ? "fail" : "data");
        if(Mode == "duplicate") QVERIFY(Transfer.Handle(Chunk)["accepted"].toBool());
        if(Mode == "gap") Chunk["offset"] = 1;
        if(Mode == "overrun") Chunk = chunk("longer");
        if(Mode == "base64") Chunk["data"] = "!!!!";
        if(Mode == "oversized-chunk") Chunk = chunk(QByteArray(InputTransfer::ChunkBytes + 1, 'x'));
        if(Mode == "truncated") Chunk = chunk("da");
        const auto Reply = Transfer.Handle(Chunk);
        if(Mode == "truncated" || Mode == "hash") {
            QVERIFY(Reply["accepted"].toBool());
            QCOMPARE(Transfer.Handle(request("input-commit"))["error"], QJsonValue("epub-input-integrity"));
        } else QVERIFY(Reply.contains("error"));
        QVERIFY(!Transfer.Take()); QCOMPARE(Transfer.Status()["status"], QJsonValue("failed"));
        QVERIFY(Transfer.Handle(begin("data")).contains("error"));
    }
    void cancellationAndExpiry()
    {
        for(const bool Verified : {false, true}) {
            InputTransfer Transfer; QVERIFY(Transfer.Handle(begin("data"))["accepted"].toBool());
            QVERIFY(Transfer.Handle(chunk("data"))["accepted"].toBool());
            if(Verified) QVERIFY(Transfer.Handle(request("input-commit"))["accepted"].toBool());
            QCOMPARE(Transfer.Handle(request("input-cancel"))["status"], QJsonValue("cancelled"));
            QVERIFY(!Transfer.Take()); QVERIFY(Transfer.Handle(request("input-commit")).contains("error"));
        }
        InputTransfer Transfer(1); QVERIFY(Transfer.Handle(begin("data"))["accepted"].toBool());
        QTest::qWait(5); QCOMPARE(Transfer.Status()["error"], QJsonValue("epub-input-timeout")); QVERIFY(!Transfer.Take());
    }
};
QTEST_MAIN(InputTransferTests)
#include "testInputTransfer.moc"
