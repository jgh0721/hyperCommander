#pragma once

// 관리자 도우미(HyperCommanderAdmin)와 본체 사이의 인증된 파이프 채널.
// 본체가 파이프 서버이고 도우미가 클라이언트다. 양쪽이 서로의 PID·생성 시각·SID·이미지를 확인한다.

#include <QJsonObject>
#include <QString>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>

namespace hc::admin {

struct ChannelSpec
{
    QString  protocol;          // 프레임의 protocol 값
    int      version    = 1;
    QString  pipePrefix;        // \\.\pipe\<prefix><nonce>
    QString  mode;              // 도우미 명령줄 모드(--file-lock-session 등)
    quint32  frameLimit = 65536;
};

QString     NewId();
bool        SameImage( const QString& First, const QString& Second );
QJsonObject TransportError( bool Action, unsigned long Code );
bool        PipeIo( void* Pipe, bool Write, QByteArray& Bytes, unsigned long Timeout, const std::atomic_bool* Cancel = nullptr );
bool        SendFrame( void* Pipe, QJsonObject Message, const ChannelSpec& Spec, const QString& Nonce, const std::atomic_bool* Cancel = nullptr );
std::optional<QJsonObject> ReceiveFrame( void* Pipe, const ChannelSpec& Spec, const QString& Nonce, unsigned long Timeout, const std::atomic_bool* Cancel = nullptr );

// 본체 쪽 연결. 첫 교환 때 도우미를 실행하고 연결한다. 교환은 요청-응답 한 쌍씩 직렬로 처리한다.
class ChannelClient
{
public:
    using Launcher = std::function<void*( const QString& Helper, const QString& Arguments, bool Elevated )>;
    ChannelClient( ChannelSpec Spec, bool Elevated, QString Helper = {}, Launcher Launch = {} );
    ~ChannelClient();
    ChannelClient( const ChannelClient& ) = delete;
    ChannelClient& operator=( const ChannelClient& ) = delete;

    // 부트스트랩에 덧붙일 값(예: 대상 경로). 연결 전에만 의미가 있다.
    void        SetBootstrap( QJsonObject Extra ) { extra_ = std::move( Extra ); }
    // Message의 requestId와 같은 응답만 받는다. 실패하면 TransportError 형태를 돌려주고 채널을 닫힌 것으로 본다.
    QJsonObject Exchange( QJsonObject Message, unsigned long Timeout, bool Action );
    void        Cancel() { cancelled_ = true; }
    bool        Cancelled() const { return cancelled_; }
    bool        Connected() const { return pipe_ != nullptr && process_ != nullptr; }
    // 도우미 프로세스가 아직 살아 있는가
    bool        HelperAlive() const;
    unsigned long LastError() const { return lastError_; }

private:
    bool start();
    ChannelSpec       spec_;
    bool              elevated_ = false;
    QString           helper_, nonce_;
    Launcher          launcher_;
    QJsonObject       extra_;
    void*             pipe_    = nullptr;
    void*             process_ = nullptr;
    std::atomic_bool  cancelled_{ false };
    unsigned long     lastError_ = 0;
    std::mutex        mutex_;
};

// 도우미 쪽 연결 결과. 소멸자가 핸들을 닫는다.
struct AcceptedChannel
{
    void*   pipe      = nullptr;
    void*   parent    = nullptr;
    quint32 parentPid = 0;
    QString nonce;
    AcceptedChannel() = default;
    AcceptedChannel( const AcceptedChannel& ) = delete;
    AcceptedChannel& operator=( const AcceptedChannel& ) = delete;
    ~AcceptedChannel();
    bool ParentAlive() const;
};

// 부트스트랩의 신선도·부모 식별·권한·파이프 서버 PID를 확인하고 연결한다. 성공하면 0(ERROR_SUCCESS).
// ExpectedCaller는 제품 진입점이 정한다. 시험은 자기 시험 실행 파일을 넘긴다.
unsigned long AcceptChannel( const QJsonObject& Bootstrap, const ChannelSpec& Spec, const QString& ExpectedCaller, AcceptedChannel& Out );

} // namespace hc::admin
