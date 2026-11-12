#pragma once

#include <QtGlobal>

#include <memory>

class QProcess;

namespace hc::rst {

/// 자식 프로세스를 "앱이 죽으면 같이 죽는" 그룹에 넣는다.
///
/// 왜 필요한가: Esbonio 서버는 다시 sphinx_agent 를 자식으로 띄운다. Windows
/// 에는 프로세스 트리 개념이 없어서 우리가 직접 자식만 terminate/kill 해도
/// 손자는 그대로 살아남는다. 실제로 앱을 한 번 띄웠다 닫을 때마다 python
/// 프로세스 4개가 고아로 남았다.
///
/// Job Object 에 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 를 걸어 두면, 우리 프로세스가
/// 정상 종료하든 크래시하든 핸들이 닫히면서 그룹 전체가 정리된다.
///
/// Windows 외 플랫폼에서는 아무 것도 하지 않는다.
void assignToKillOnExitJob( qint64 processId );

/// 그 PID 의 프로세스가 아직 살아 있는가.
/// 크래시로 남은 임시 디렉터리를 정리할 때, 다른 인스턴스가 쓰는 것을 지우지
/// 않으려고 쓴다. (PID 재사용으로 오탐할 수 있지만 방향이 안전한 쪽이다 —
/// 살아 있다고 잘못 보면 지우지 않고 넘어갈 뿐이다.)
[[nodiscard]] bool isProcessRunning( qint64 processId );

/// 프로세스를 죽이고, **끝나기를 기다리지 않고** 소유권을 가져간다.
///
/// 왜 필요한가: `~QProcess` 는 프로세스가 아직 돌고 있으면 `kill()` 뒤에
/// `waitForFinished()` 를 부르는데 그 기본 인자가 **30초**다
/// (qtbase 의 qprocess.cpp / qprocess.h). 그래서 종료 경로에서 대기만 지우고
/// `unique_ptr` 을 그냥 `reset()` 하면 1.5초 대기를 30초 대기로 바꿔 놓는 셈이다.
///
/// 이미 끝난 객체는 즉시 파괴하고, 실행 중인 객체는 kill() 뒤 NotRunning
/// 신호에서 지운다. 정상 실행 중 반복 호출해도 객체가 누적되지 않는다.
/// 종료 후 이벤트 루프가 더 돌지 않는 경우에만 OS 종료 시까지 남는다.
///
/// 시그널은 먼저 끊는다. 호출한 쪽(LspClient / UvTask)이 곧 delete 되기 때문이다.
void abandonProcess( std::unique_ptr< QProcess > process );

/// 프로세스가 스스로 끝날 시간을 주되 **기다리지는 않는다.**
///
/// 소유권을 가져가서, 프로세스가 끝나면 그때 QProcess 를 지운다. 유예 시간이
/// 지나도 살아 있으면 `kill()` 을 보내고, 그 뒤 도착하는 finished 가 정리를
/// 마무리한다. 어느 경로로도 GUI 스레드는 멈추지 않고, 객체는 프로세스가
/// 끝난 뒤에만 파괴되므로 `~QProcess` 의 30초 대기에 걸리지 않는다.
///
/// 이벤트 루프가 도는 동안에만 의미가 있다. 앱 종료 경로에서는
/// abandonProcess() 를 쓸 것.
void reapProcessLater( std::unique_ptr< QProcess > process, int graceMs );

}  // namespace hc::rst
