#include "rstTestRunner.hpp"

#include "esbonioLspClient.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace hc::rst;

namespace {

QJsonObject parse( const char* json )
{
    return QJsonDocument::fromJson( QByteArray( json ) ).object();
}

}  // namespace

/// 이 분류가 틀리면 서버가 보낸 요청을 우리 요청의 응답으로 오인해 응답하지
/// 않게 되고, Esbonio 는 초기화 중 workspace/configuration 을 기다리며 멈춘다.
/// 증상은 "진단이 하나도 안 뜬다" 뿐이라 원인을 찾기 매우 어렵다.
class TestLspMessageRouting : public QObject
{
    Q_OBJECT

private slots:
    void classifies_data();
    void classifies();
    void serverRequestIdMayCollideWithOurs();
    void stringIdIsPreservedVerbatimInResponse();
    void jsonRpcParserHandlesSplitPackets();
    void jsonRpcParserHandlesMultipleMessagesInOneRead();
    void workspaceUriMatchesEsbonioScope_data();
    void workspaceUriMatchesEsbonioScope();
};

void TestLspMessageRouting::classifies_data()
{
    QTest::addColumn< QByteArray >( "json" );
    QTest::addColumn< int >( "expected" );

    QTest::newRow( "서버 요청 (id+method)" )
        << QByteArray( R"({"jsonrpc":"2.0","id":1,"method":"workspace/configuration","params":{}})" )
        << int( LspMessageKind::Request );
    QTest::newRow( "우리 요청의 응답 (id만)" )
        << QByteArray( R"({"jsonrpc":"2.0","id":1,"result":[]})" )
        << int( LspMessageKind::Response );
    QTest::newRow( "오류 응답도 응답이다" )
        << QByteArray( R"({"jsonrpc":"2.0","id":7,"error":{"code":-32601,"message":"x"}})" )
        << int( LspMessageKind::Response );
    QTest::newRow( "알림 (method만)" )
        << QByteArray( R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{}})" )
        << int( LspMessageKind::Notification );
    QTest::newRow( "id 가 null 이면 요청이 아니다" )
        << QByteArray( R"({"jsonrpc":"2.0","id":null,"method":"$/progress","params":{}})" )
        << int( LspMessageKind::Notification );
    QTest::newRow( "둘 다 없으면 무효" )
        << QByteArray( R"({"jsonrpc":"2.0"})" )
        << int( LspMessageKind::Invalid );
    QTest::newRow( "method 가 빈 문자열이면 method 없음" )
        << QByteArray( R"({"jsonrpc":"2.0","id":3,"method":""})" )
        << int( LspMessageKind::Response );
}

void TestLspMessageRouting::classifies()
{
    QFETCH( QByteArray, json );
    QFETCH( int, expected );

    QCOMPARE( int( classifyLspMessage( QJsonDocument::fromJson( json ).object() ) ), expected );
}

void TestLspMessageRouting::serverRequestIdMayCollideWithOurs()
{
    // 서버의 id 번호 공간은 우리 것과 별개다. 둘 다 1부터 시작하므로,
    // "id 가 내 요청 목록에 있는가" 로 판단하면 서버 요청을 응답으로 착각한다.
    const QJsonObject serverRequest = parse( R"({"id":1,"method":"workspace/configuration","params":{"items":[]}})" );
    const QJsonObject ourResponse = parse( R"({"id":1,"result":{"capabilities":{}}})" );

    QCOMPARE( int( classifyLspMessage( serverRequest ) ), int( LspMessageKind::Request ) );
    QCOMPARE( int( classifyLspMessage( ourResponse ) ), int( LspMessageKind::Response ) );
}

void TestLspMessageRouting::stringIdIsPreservedVerbatimInResponse()
{
    // Esbonio 는 요청 id 로 문자열 UUID 를 쓴다. 숫자로 바꿔 응답하면 서버가
    // 자기 요청과 짝지을 수 없어 영원히 기다린다 (진단이 하나도 안 뜬다).
    const QJsonObject request = parse(
        R"({"id":"2bc7344c-f5dc-4ec0-a096-c0d543d58ec2","jsonrpc":"2.0","method":"workspace/configuration"})" );
    QCOMPARE( int( classifyLspMessage( request ) ), int( LspMessageKind::Request ) );

    JsonRpcWriter writer;
    const QByteArray frame = writer.response( request.value( QStringLiteral( "id" ) ), QJsonArray{} );
    const qsizetype bodyStart = frame.indexOf( "\r\n\r\n" ) + 4;
    const QJsonObject sent = QJsonDocument::fromJson( frame.mid( bodyStart ) ).object();

    QVERIFY( sent.value( QStringLiteral( "id" ) ).isString() );
    QCOMPARE( sent.value( QStringLiteral( "id" ) ).toString(),
             QStringLiteral( "2bc7344c-f5dc-4ec0-a096-c0d543d58ec2" ) );

    // 숫자 id 도 숫자로 그대로 돌아가야 한다.
    const QByteArray numeric = writer.response( QJsonValue( 42 ), QJsonValue() );
    const qsizetype numericBody = numeric.indexOf( "\r\n\r\n" ) + 4;
    QCOMPARE( QJsonDocument::fromJson( numeric.mid( numericBody ) ).object()
                  .value( QStringLiteral( "id" ) ).toInt(), 42 );
}

void TestLspMessageRouting::jsonRpcParserHandlesSplitPackets()
{
    JsonRpcParser parser;
    const QByteArray body = R"({"jsonrpc":"2.0","method":"ping","params":{}})";
    const QByteArray framed = "Content-Length: " + QByteArray::number( body.size() ) + "\r\n\r\n" + body;

    // 겹치지 않게 3조각으로 자른다 (헤더 도중 / 본문 도중 / 나머지).
    const qsizetype firstCut = 8;
    const qsizetype secondCut = framed.size() - 12;

    parser.append( framed.mid( 0, firstCut ) );
    QVERIFY( parser.takeMessages().isEmpty() );

    parser.append( framed.mid( firstCut, secondCut - firstCut ) );
    QVERIFY( parser.takeMessages().isEmpty() );

    parser.append( framed.mid( secondCut ) );
    const QList< QJsonObject > messages = parser.takeMessages();
    QCOMPARE( messages.size(), 1 );
    QCOMPARE( messages.first().value( QStringLiteral( "method" ) ).toString(), QStringLiteral( "ping" ) );
}

void TestLspMessageRouting::jsonRpcParserHandlesMultipleMessagesInOneRead()
{
    JsonRpcParser parser;
    const QByteArray a = R"({"method":"a"})";
    const QByteArray b = R"({"method":"b"})";
    parser.append( "Content-Length: " + QByteArray::number( a.size() ) + "\r\n\r\n" + a
                  + "Content-Length: " + QByteArray::number( b.size() ) + "\r\n\r\n" + b );

    const QList< QJsonObject > messages = parser.takeMessages();
    QCOMPARE( messages.size(), 2 );
    QCOMPARE( messages.at( 0 ).value( QStringLiteral( "method" ) ).toString(), QStringLiteral( "a" ) );
    QCOMPARE( messages.at( 1 ).value( QStringLiteral( "method" ) ).toString(), QStringLiteral( "b" ) );
}

// Esbonio 는 workspace/configuration 의 답을 우리가 보낸 작업공간 URI 그대로 키로 두고, 찾을 때는
// Uri.for_file 모양으로 찾는다. 모양이 어긋나면 우리 pythonCommand 대신 프로젝트 pyproject.toml 의
// [tool.esbonio] 설정이 쓰인다. 기대값은 esbonio 2.1.0 의 Uri.for_file 출력이다.
void TestLspMessageRouting::workspaceUriMatchesEsbonioScope_data()
{
    QTest::addColumn< QString >( "path" );
    QTest::addColumn< QString >( "expected" );
    QTest::newRow( "드라이브" ) << QStringLiteral( "D:\\Dev\\iMonAGR\\docs\\source" ) << QStringLiteral( "file:///d%3A/Dev/iMonAGR/docs/source" );
    QTest::newRow( "예약 문자 · 한글" ) << QString::fromUtf8( "C:\\Users\\x (1)\\한글+a,b;c=d&e~f_g.h-i" )
        << QStringLiteral( "file:///c%3A/Users/x%20%281%29/%ED%95%9C%EA%B8%80%2Ba%2Cb%3Bc%3Dd%26e~f_g.h-i" );
    QTest::newRow( "앞쪽 슬래시" ) << QStringLiteral( "C:/Docs/" ) << QStringLiteral( "file:///c%3A/Docs" );
}

void TestLspMessageRouting::workspaceUriMatchesEsbonioScope()
{
    QFETCH( QString, path );
    QFETCH( QString, expected );
    QCOMPARE( hc::rst::esbonioScopeUri( path ), expected );
}

HC_RST_REGISTER_TEST( TestLspMessageRouting );

#include "tstLspMessageRouting.moc"
