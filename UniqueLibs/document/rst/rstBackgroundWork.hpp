#pragma once

class QThreadPool;

namespace hc::rst {

/// 앱이 내려가는 중인가. **워커 스레드가 물어보는 곳**이다.
///
/// 왜 프로세스 전역인가: WorkspaceController::shuttingDown_ 는 GUI 스레드 전용
/// 평범한 bool 이고, UpdateService / UpdateInstaller / QBaseEditor /
/// PythonEnvManager / ProjectScanner 는 그 컨트롤러를 알지 못한다.
///
/// QCoreApplication::closingDown() 으로는 안 된다. 그것은 ~QCoreApplication 이
/// 시작된 뒤에야 true 인데, 우리가 필요한 시점(closeEvent 진입)보다 한참 늦다.
///
/// 기존 generation_ + QPointer guard 관용구로도 안 된다. 그것은 **결과를 버리는**
/// 것이지 **일을 멈추는** 것이 아니다 — 워커는 문서 수천 개를 전부 읽고 나서야
/// 결과가 버려지는 것을 알게 되고, 그 시간만큼 프로세스가 살아 있다.
[[nodiscard]] bool isShuttingDown();

/// 종료 의사를 워커들에게 알린다. MainWindow::closeEvent 진입부에서 부른다.
void requestShutdown();

/// 사용자가 저장 확인에서 취소를 눌러 종료가 되돌아갔을 때 표시를 지운다.
/// 지우지 않으면 남은 세션 동안 개요·용어집·스캔이 전부 즉시 포기한다.
void cancelShutdownRequest();

/// "반드시 끝나야 하는" 디스크 쓰기 전용 풀.
///
/// 전역 풀과 나누는 이유: 종료 시 전역 풀은 clear() 로 큐를 버리고 협조적
/// 취소로 끊는다. 그런데 텍스트 저장(QTextView::saveFile)과 hot-exit 스냅샷은
/// 버리면 **데이터가 사라진다.** QThreadPool 은 "어떤 runnable 을 버릴지" 를
/// 고를 수 없으므로, 버리면 안 되는 것만 이쪽으로 옮겨 둘을 분리한다.
///
/// 지금까지 이 저장은 ~QCoreApplication 의 **무제한** waitForDone() 덕에만
/// 살아 있었다(MainWindow::saveView 는 저장이 "시작" 되었다는 뜻으로 true 를
/// 돌려주고, 그 직후 shutdownUi() 가 뷰를 지운다). 그 우연을 명시적인 규칙으로
/// 바꾸는 것이 이 풀의 목적이다.
[[nodiscard]] QThreadPool& persistencePool();

}  // namespace hc::rst
