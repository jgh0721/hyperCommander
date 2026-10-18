#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <QtTest>
#include <QLocalServer>
#include <QUuid>
#include <QProcess>
#include "UniqueLibs/document/documentWebPeer.hpp"
using namespace hc::document;
class DocumentWebPeerTests final : public QObject
{
    Q_OBJECT
    const QByteArray nonce_ = QByteArray(32,'a');
    HostFrame frame(std::uint64_t Id=1)
    {
        HostFrame Frame; Frame.requestId_=Id;
        const auto Bytes=QByteArray::fromHex(nonce_); std::memcpy(Frame.connection_.data(),Bytes.constData(),16); return Frame;
    }
    QByteArray bytes(const HostFrame& Frame) { return QByteArray(reinterpret_cast<const char*>(&Frame),sizeof(Frame)); }
    QString name() { return "hc-web-peer-test-"+QUuid::createUuid().toString(QUuid::Id128); }
private slots:
    void fragmentedRequestsAndMonotonicReplies()
    {
        QLocalServer Server; const auto Name=name(); QVERIFY(Server.listen(Name));
        DocumentWebPeer Peer; QVERIFY(Peer.Connect(Name,nonce_,GetCurrentProcessId(),0));
        QTRY_VERIFY(Server.hasPendingConnections()); auto* Socket=Server.nextPendingConnection(); QVERIFY(Socket);
        int Calls=0; bool Closed=false;
        QVERIFY(Peer.Ready(0,[&](const QByteArray& Request) { ++Calls; return Request.toUpper(); },[&] { Closed=true; }));
        QTRY_VERIFY(Socket->bytesAvailable()>=qint64(sizeof(HostFrame)));
        HostFrame Ready; const auto ReadyBytes=Socket->read(sizeof(Ready)); std::memcpy(&Ready,ReadyBytes.constData(),sizeof(Ready));
        QCOMPARE(Ready.type_,0U); QCOMPARE(Ready.requestId_,0ULL); QVERIFY(ValidFrame(Ready,frame().connection_));
        auto Frame=frame(); Frame.length_=5; const auto Header=bytes(Frame);
        Socket->write(Header.left(7)); Socket->flush(); QTest::qWait(20); QCOMPARE(Calls,0);
        Socket->write(Header.mid(7)+"he"); Socket->flush(); QTest::qWait(20); QCOMPARE(Calls,0);
        Socket->write("llo"); Socket->flush();
        QTRY_VERIFY(Socket->bytesAvailable()>=qint64(sizeof(Frame)+5));
        auto Reply=Socket->read(sizeof(Frame)); std::memcpy(&Frame,Reply.constData(),sizeof(Frame));
        QCOMPARE(Frame.type_,2U); QCOMPARE(Frame.requestId_,1ULL); QCOMPARE(Socket->read(5),QByteArray("HELLO"));
        Socket->write(bytes(frame(2))); Socket->flush();
        QTRY_VERIFY(Socket->bytesAvailable()>=qint64(sizeof(Frame)));
        Reply=Socket->read(sizeof(Frame)); std::memcpy(&Frame,Reply.constData(),sizeof(Frame));
        QCOMPARE(Frame.requestId_,2ULL); QCOMPARE(Frame.length_,0U); QCOMPARE(Calls,2); QVERIFY(!Closed);
        Socket->abort(); QTRY_VERIFY(Closed);
    }
    void invalidFrameDisconnects_data()
    {
        QTest::addColumn<QString>("Kind");
        for(auto Kind:{"nonce","sequence","type","version","window","oversize","pipelined","reply-budget"})
            QTest::newRow(Kind)<<QString(Kind);
    }
    void invalidFrameDisconnects()
    {
        QFETCH(QString,Kind); QLocalServer Server; const auto Name=name(); QVERIFY(Server.listen(Name));
        DocumentWebPeer Peer; QVERIFY(Peer.Connect(Name,nonce_,GetCurrentProcessId(),0));
        QTRY_VERIFY(Server.hasPendingConnections()); auto* Socket=Server.nextPendingConnection(); QVERIFY(Socket);
        int Calls=0; bool Closed=false;
        QVERIFY(Peer.Ready(0,[&](const QByteArray&) { ++Calls; return Kind=="reply-budget"?QByteArray(HostPayloadLimit+1,'x'):QByteArray(); },[&] { Closed=true; }));
        QTRY_VERIFY(Socket->bytesAvailable()>=qint64(sizeof(HostFrame))); Socket->readAll();
        auto Frame=frame();
        if(Kind=="nonce") Frame.connection_[0]^=1;
        if(Kind=="sequence") Frame.requestId_=2;
        if(Kind=="type") Frame.type_=2;
        if(Kind=="version") ++Frame.version_;
        if(Kind=="window") Frame.window_=1;
        if(Kind=="oversize") Frame.length_=HostPayloadLimit+1;
        auto Data=bytes(Frame); if(Kind=="pipelined") Data+=bytes(frame(2));
        Socket->write(Data); Socket->flush(); QTRY_VERIFY(Closed);
        QCOMPARE(Calls,Kind=="reply-budget"?1:0);
    }
    void serverPidMismatchRejected()
    {
        QLocalServer Server; const auto Name=name(); QVERIFY(Server.listen(Name));
        QProcess Other; Other.start(QCoreApplication::applicationDirPath()+"/hc_web_process_fixture.exe",{"--child"});
        QVERIFY(Other.waitForStarted(5000));
        DocumentWebPeer Peer; QVERIFY(!Peer.Connect(Name,nonce_,std::uint32_t(Other.processId()),0));
        Other.kill(); QVERIFY(Other.waitForFinished(5000));
    }
};
QTEST_GUILESS_MAIN(DocumentWebPeerTests)
#include "tstDocumentWebPeer.moc"
