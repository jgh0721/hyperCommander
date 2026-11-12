#pragma once

#include <QString>

namespace hc::rst {

/// HC_RST_PHASE_TRACE 가 지정되면 기동/종료 각 구간을 한 줄씩 파일에 남긴다.
///
/// 형식: <startPhaseClock() 이후 경과 ms>\t<태그>\t<세부>
///
/// 왜 별도 파일인가: 같은 모양의 트레이스가 이미 셋 있지만(MainWindow.cpp 의
/// HC_RST_LOG_FILE, ScintillaQtDirectBackend.cpp 의 MV_TEXT_LEXER_TRACE_FILE,
/// solEsbonioLspClient.cpp 의 HC_RST_LSP_TRACE) 전부 각 .cpp 의 익명 네임스페이스
/// 안이라 여러 파일이 **하나의 타임라인**에 찍을 수가 없다. 기동/종료 구간은
/// main.cpp / MainWindow.cpp / 컨트롤러 / 에디터가 함께 만들기 때문에 공유
/// 심볼이 필요하다.
///
/// 그리고 appendLog() 로는 이 구간을 못 잰다. 그것은 Ui.logView 가 살아 있어야
/// 하고(MainWindow.cpp 의 조기 반환), 우리가 재려는 구간의 절반은 위젯도
/// QApplication 도 없는 곳이다(exec() 반환 이후). 그래서 QFile/tr() 을 쓰지 않고
/// std::fopen 만 쓰며, 매 호출 open/close 한다 — 강제 종료돼도 내용이 남는다
/// (traceLsp 와 같은 판단).
///
/// 환경 변수가 없으면 첫 호출에서 즉시 반환하므로 배포 빌드에 그대로 둬도 된다.
void traceP( const char* tag, const QString& detail = {} );

/// 이 트레이스가 켜져 있는가. **켜져 있을 때만 만들 물건**을 위한 것이다.
///
/// traceP() 는 스스로 게이트를 보므로 호출자는 이것을 물어볼 필요가 없다. 필요한
/// 것은 정체 감시자(MainWindow 의 50 ms 타이머)처럼 **존재 자체가 비용인** 경우다 —
/// 배포 빌드에서 쓸데없이 이벤트 루프를 깨우지 않으려면 만들지 않아야 하고,
/// 그 판단을 하려면 게이트를 밖에서도 볼 수 있어야 한다.
[[nodiscard]] bool phaseTraceEnabled();

/// 기준 시각을 세운다. main() 첫 줄에서 한 번 부른다.
/// 부르지 않아도 traceP() 의 첫 호출이 대신 세운다(경과 시간의 원점만 늦어진다).
void startPhaseClock();

/// 한 구간을 재는 RAII 도우미. 생성에서 "<tag>.begin", 파괴에서 "<tag>.end" 를
/// 남기고 end 쪽 세부에 그 구간의 소요 ms 를 붙인다.
class PhaseSpan final
{
public:
    explicit PhaseSpan( const char* tag, QString detail = {} );
    ~PhaseSpan();

    PhaseSpan( const PhaseSpan& ) = delete;
    PhaseSpan& operator=( const PhaseSpan& ) = delete;

private:
    const char*                         tag_ = nullptr;
    QString                             detail_;
    qint64                              startedAtNs_ = 0;
};

}  // namespace hc::rst
