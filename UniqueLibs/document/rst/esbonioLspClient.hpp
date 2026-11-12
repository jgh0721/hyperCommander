#pragma once

#include "sphinxScanner.hpp"
#include "sphinxDiagnostics.hpp"

#include <QObject>
#include <QProcess>
#include <QHash>
#include <QJsonValue>
#include <QList>
#include <QSet>

#include <memory>

namespace hc::rst {

struct LspCompletionItem {
    QString label;
    QString insertText;
    QString detail;
    int kind = 0;
    QString filterText;
};

struct LspDocumentSymbol {
    QString name;
    QString detail;
    int kind = 0;
    int line = 1;
    int column = 1;
    int endLine = 1;
    int endColumn = 1;
    QList<LspDocumentSymbol> children;
};

[[nodiscard]] QList<LspDocumentSymbol> parseDocumentSymbols(const QJsonValue& result);

/// JSON-RPC 메시지 종류.
///
/// 이걸 잘못 나누면 서버가 보낸 **요청** 을 우리 요청의 **응답** 으로 오인해
/// 답을 하지 않게 되고, 서버는 그 자리에서 멈춘다. Esbonio 는 초기화 중
/// workspace/configuration 을 요청하므로 곧바로 치명적이다.
enum class LspMessageKind
{
    Invalid,
    Request,        ///< id 와 method 가 둘 다 있다 -> 반드시 응답해야 한다
    Response,       ///< id 만 있다 -> 우리가 보낸 요청의 결과
    Notification,   ///< method 만 있다 -> 응답 불필요
};

/// 서버가 보낸 id 와 우리가 발급한 id 는 서로 다른 번호 공간이다.
/// 따라서 id 를 우리 요청 목록에서 찾아보는 것으로 종류를 판단하면 안 된다.
[[nodiscard]] LspMessageKind classifyLspMessage(const QJsonObject& message);
/// 작업공간 폴더의 URI 를 Esbonio 가 설정 범위 키로 쓰는 모양( esbonio.server.Uri.for_file )으로 만든다.
[[nodiscard]] QString esbonioScopeUri(const QString& path);

class JsonRpcWriter final
{
public:
    [[nodiscard]] int nextId() const;
    [[nodiscard]] QByteArray request( const QString& method, const QJsonObject& params, int* idOut = nullptr );
    [[nodiscard]] QByteArray notify( const QString& method, const QJsonObject& params );
    /// 응답의 id 는 요청에서 받은 값을 **그대로** 돌려줘야 한다.
    /// LSP/JSON-RPC 의 id 는 숫자일 수도 문자열일 수도 있는데, Esbonio 는
    /// 문자열 UUID 를 쓴다. 숫자로 변환해 보내면 서버가 자기 요청과 짝지을 수
    /// 없어 영원히 기다린다 (증상은 "아무 진단도 안 뜸").
    [[nodiscard]] QByteArray response( const QJsonValue& id, const QJsonValue& result );

private:
    int nextId_ = 1;
    [[nodiscard]] static QByteArray frame( const QJsonObject& payload );
};

class JsonRpcParser final
{
public:
    void append( const QByteArray& bytes );
    [[nodiscard]] QList<QJsonObject> takeMessages();

private:
    QByteArray buffer_;
};

class LspClient final : public QObject {
    Q_OBJECT

public:
    explicit LspClient(QObject* parent = nullptr);
    ~LspClient() override;

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] QString activeProjectId() const;

    /// conf.py 에 html_style = '' 이 있으면 Sphinx 8 에서 _static checksum
    /// 오류가 난다. configOverrides 로 None 을 강제하기 위해 미리 확인한다.
    void setHtmlStyleOverride(bool overrideToNull);

    /// conf.py / 확장 / 테마를 해석할 인터프리터. 비우면 esbonio 가 자기 것을 쓴다.
    ///
    /// esbonio 서버 자체는 항상 번들에서 돌리고, sphinx_agent 만 이 인터프리터로
    /// 띄운다. 그래서 사용자 venv 에 esbonio 를 설치할 필요가 없다.
    void setSphinxPythonCommand(const QString& pythonExe);

    void start(const SphinxProject& project, const QString& pythonExe, const QString& sphinxBuildExe);

    /// 서버를 내리는 방식.
    ///
    /// 예전에는 하나뿐이었고 `terminate()` + `waitForFinished(1500)` 이었다.
    /// 그런데 Windows 의 `QProcess::terminate()` 는 top-level 창에 WM_CLOSE 를
    /// 보내는 방식이라 **창 없는 `python -m esbonio.server` 에는 무효**다
    /// (같은 사실이 solUvTaskRunner.hpp 에도 적혀 있다). 그래서 그 대기는
    /// **항상 1500ms 를 꽉 채우고 타임아웃**했다. 실측으로 종료 시간의 84% 였고,
    /// 프로젝트를 전환할 때마다(LRU 축출) GUI 가 그만큼 얼어붙었다.
    enum class StopMode
    {
        Graceful,    ///< 런타임 축출/프로젝트 전환. 이벤트 루프가 살아 있다.
        Immediate,   ///< 앱 종료. 아무것도 기다리지 않는다.
    };

    /// 기본값이 Immediate 인 이유: ~LspClient / ~LspServerPool 이 부르는 경로는
    /// 이벤트 루프가 이미 없을 수 있고, 거기서 Graceful 을 쓰면 응답을 영영
    /// 기다리게 된다. 기다려도 되는 쪽이 명시적으로 골라야 안전하다.
    void stop(StopMode mode = StopMode::Immediate);
    void didOpen(const QString& path, const QString& text, const QString& languageId = QStringLiteral("rst"));
    void didChange(const QString& path, const QString& text);
    /// 편집 한 건만 보낸다. 좌표는 협상된 위치 인코딩 기준이며, 우리는 utf-8 을
    /// 선호로 선언하므로 성사되면 Scintilla 의 바이트 오프셋이 그대로 들어간다.
    ///
    /// supportsIncrementalSync() 가 참일 때만 부를 것. 거짓이면 didChange(전문)
    /// 로 되돌아가야 한다 — 하나라도 빠뜨리면 서버 사본이 어긋난다.
    void didChangeIncremental(const QString& path, int startLine, int startColumn,
                              int endLine, int endColumn, const QByteArray& newTextUtf8);
    /// 서버가 증분 동기화를 광고했고 위치 인코딩이 utf-8 로 협상되었는가.
    [[nodiscard]] bool supportsIncrementalSync() const;
    void didSave(const QString& path, const QString& text);
    void didClose(const QString& path);
    /// triggerCharacter 를 주면 triggerKind=2 로 보낸다. Esbonio 가 등록한
    /// 문자(: / < > 공백 백틱)일 때만 의미가 있다.
    int completion(const QString& path, int line, int column, const QString& triggerCharacter = {});
    int documentSymbols(const QString& path);

signals:
    void logMessage(const QString& text);
    void jsonMessage(const QJsonObject& message);
    /// sphinx/clientCreated, sphinx/appCreated, sphinx/clientErrored, $/progress 등.
    /// 상태 표시와 "빌드 끝난 뒤 자동완성 재시도" 에 쓴다.
    void serverNotification(const QString& method, const QJsonObject& params);
    /// path 를 따로 실어 보낸다. publishDiagnostics 는 "이 파일은 이제 깨끗하다"
    /// 를 **빈 배열**로 알리는데, entries 만 봐서는 어느 파일인지 알 수 없어
    /// 옛 진단을 지울 수가 없다.
    void diagnosticsReady(const QString& source, const QString& path,
                          const QVector<DiagnosticEntry>& entries);
    void completionsReady(int requestId, const QList<LspCompletionItem>& items);
    void documentSymbolsReady(const QString& path, const QList<LspDocumentSymbol>& symbols);

private slots:
    void readStdout();
    void readStderr();

private:
    void initialize();
    void handleMessage(const QJsonObject& message);
    void handleServerRequest(const QJsonObject& message);
    void handleResponse(const QJsonObject& message);
    void handleNotification(const QString& method, const QJsonObject& params);
    void write(const QByteArray& frame);
    [[nodiscard]] QString pathToUri(const QString& path) const;
    [[nodiscard]] QJsonObject textDocumentIdentifier(const QString& path) const;
    [[nodiscard]] QJsonObject sphinxConfiguration() const;
    [[nodiscard]] QJsonValue configurationForSection(const QString& section) const;

    std::unique_ptr<QProcess> process_;
    JsonRpcWriter writer_;
    JsonRpcParser parser_;
    SphinxProject project_;
    QString sphinxBuildExe_;
    QString activeProjectId_;
    bool htmlStyleOverride_ = false;
    QString sphinxPythonCommand_;
    QHash<QString, int> documentVersions_;
    /// initialize 응답에서 읽는다. 협상 전에는 둘 다 보수적인 기본값이므로
    /// 그 사이의 편집은 전문 전송 경로로 흘러간다.
    int  serverSyncKind_ = 1;          ///< LSP TextDocumentSyncKind. 1 = Full
    bool serverUsesUtf8Positions_ = false;
    /// 우리가 보낸 요청: id -> method. 예전에는 completion/documentSymbol 을
    /// 따로 들고 있었는데, 그러면 "이 id 가 내 것인가" 를 추측하게 된다.
    QHash<int, QString> pendingRequests_;
    QHash<int, QString> documentSymbolRequestPaths_;
};

}  // namespace hc::rst


