#pragma once

// 관리자 도우미 시스템 세션(--system-session). 한 번의 UAC 확인으로 세션을 열고,
// 관리자 권한이 필요한 프로세스·서비스 동작을 요청마다 처리한다.
//
// 요청 종류
//   ping
//   process.terminate   { targets:[{pid,created,image}], gracefulMs, allowWarned }
//   process.suspend     { target, resume:bool, allowWarned }
//   process.priority    { target, priorityClass, allowWarned }
//   process.efficiency  { target, enable, allowWarned }
//   process.details     { pid, created }
//   service.query       { name }  — 표준 권한으로 구성을 읽지 못한 서비스
//   service.control     { name, control: start|stop|pause|continue|restart }
//   service.config      { name, changes:{ ... } }

#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>
#include <mutex>

namespace hc::admin {

class ChannelClient;

class SystemSessionClient
{
public:
    using Launcher = std::function<void*( const QString&, const QString&, bool )>;
    explicit SystemSessionClient( QString Helper = {}, Launcher Launch = {}, bool Elevated = true );
    ~SystemSessionClient();
    // 블로킹 호출. 작업자 스레드에서만 부른다. 연결이 끊기면 다음 요청에서 새 세션(새 UAC 확인)을 연다.
    QJsonObject Request( const QString& Kind, QJsonObject Payload, unsigned long Timeout = 60000 );
    // 이미 연결된 세션에만 보낸다. 연결이 없으면 도우미를 띄우지(UAC 확인) 않고 outcome = "Unavailable".
    QJsonObject RequestIfConnected( const QString& Kind, QJsonObject Payload, unsigned long Timeout = 15000 );
    bool        Connected() const;
    void        Close();
    // 세션 유휴 제한(밀리초). 다음 연결부터 적용한다.
    void        SetIdleTimeout( int Milliseconds ) { idleMs_ = Milliseconds; }

private:
    QString                        helper_;
    Launcher                       launcher_;
    bool                           elevated_ = true;
    int                            idleMs_   = 600000;
    std::unique_ptr<ChannelClient> channel_;
    mutable std::mutex             mutex_;
};

// RequireElevation은 제품 진입점에서 항상 true다. 시험만 표준 권한 채널로 프로토콜을 검증한다.
unsigned long ExecuteSystemSession( const QJsonObject& Bootstrap, const QString& ExpectedCaller = {}, bool RequireElevation = true );

} // namespace hc::admin
