#include "stdafx.h"
#include "pluginHost/viewerUiDispatcher.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QApplication>
#include <QDialog>
#include <QtTest>
#include <future>
#include <thread>
#include <wct.h>
#include <DbgHelp.h>

using hc::plugins::ViewerUiDispatcher;
namespace
{
    constexpr UINT ModalMessage = WM_APP + 0x22;
    LONG CALLBACK SqliteException(EXCEPTION_POINTERS* Exception)
    {
        if (Exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
        const auto Path=qEnvironmentVariable("HC_SQLITE_TRACE")+".dmp";
        HANDLE File=CreateFileW(reinterpret_cast<LPCWSTR>(Path.utf16()),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (File!=INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION Information{GetCurrentThreadId(),Exception,FALSE};
            MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),File,MiniDumpNormal,&Information,nullptr,nullptr);
            CloseHandle(File);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    struct ModalState
    {
        ViewerUiDispatcher* dispatcher_;
        int kind_;
        int timeoutMs_ = 2000;
        bool modal_ = false, finished_ = false, boundaryInside_ = false;
        std::atomic<HWND> modalWindow_{nullptr};
        std::atomic<bool> entered_{false}, release_{false};
        std::atomic<bool> pending_{false};
        std::atomic<unsigned> probes_{0};
        DWORD thread_ = GetCurrentThreadId();
    };
    LRESULT CALLBACK ModalProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
    {
        auto* State = reinterpret_cast<ModalState*>(GetWindowLongPtrW(Window, GWLP_USERDATA));
        if (Message == WM_NCCREATE) {
            State = static_cast<ModalState*>(reinterpret_cast<CREATESTRUCTW*>(LParam)->lpCreateParams);
            SetWindowLongPtrW(Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(State));
        }
        if (Message == ModalMessage) {
            ViewerUiDispatcher::NativeCallback Callback;
            State->modal_ = true;
            State->boundaryInside_ = State->dispatcher_->AtBoundary();
            if (State->kind_ == 0) {
                QDialog Dialog; QTimer Poll;
                QObject::connect(&Poll, &QTimer::timeout, &Dialog, [&] { if (State->release_) Dialog.accept(); });
                Poll.start(20); QTimer::singleShot(State->timeoutMs_, &Dialog, &QDialog::reject);
                State->modalWindow_ = reinterpret_cast<HWND>(Dialog.winId());
                QTimer::singleShot(0, &Dialog, [&] { State->entered_ = true; }); Dialog.exec();
            } else if (State->kind_ == 1) {
                auto Menu = CreatePopupMenu(); AppendMenuW(Menu, MF_STRING, 1, L"test");
                SetTimer(Window, 1, 20, nullptr); State->entered_ = true;
                SetTimer(Window, 2, 2000, nullptr);
                TrackPopupMenu(Menu, TPM_RETURNCMD, 30, 30, 0, Window, nullptr);
                KillTimer(Window, 1); KillTimer(Window, 2); DestroyMenu(Menu);
            } else if (State->kind_ == 2) {
                struct { DLGTEMPLATE dialog_; WORD menu_, class_, title_; } Template{};
                Template.dialog_.style = WS_POPUP | WS_CAPTION | DS_MODALFRAME;
                Template.dialog_.cx = 100; Template.dialog_.cy = 50;
                DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &Template.dialog_, Window,
                    [](HWND Dialog, UINT Message, WPARAM WParam, LPARAM LParam) -> INT_PTR {
                        auto* Current = reinterpret_cast<ModalState*>(GetWindowLongPtrW(Dialog, DWLP_USER));
                        if (Message == WM_INITDIALOG) {
                            Current = reinterpret_cast<ModalState*>(LParam); SetWindowLongPtrW(Dialog, DWLP_USER, LParam);
                            SetTimer(Dialog, 1, 20, nullptr); SetTimer(Dialog, 2, 2000, nullptr); Current->entered_ = true; return TRUE;
                        }
                        if (Message == WM_TIMER && (WParam == 2 || Current->release_)) { EndDialog(Dialog, 0); return TRUE; } return FALSE;
                    }, reinterpret_cast<LPARAM>(State));
            } else {
                const auto Until = GetTickCount64() + 2000; State->entered_ = true;
                // Deliberately filter to this HWND. Sent messages to the probe
                // are still dispatched by Win32; no Qt slot is needed.
                while (!State->release_ && GetTickCount64() < Until) {
                    MSG Next{};
                    if (PeekMessageW(&Next, Window, 0, 0, PM_REMOVE)) { TranslateMessage(&Next); DispatchMessageW(&Next); }
                    else MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                }
            }
            State->modal_ = false; State->finished_ = true;
            return 0;
        }
        if (Message == WM_TIMER && (WParam == 2 || (WParam == 1 && State->release_))) { EndMenu(); return 0; }
        return DefWindowProcW(Window, Message, WParam, LParam);
    }
}
class ViewerUiDispatcherTests final : public QObject
{
    Q_OBJECT
private slots:
    void sqliteDirect()
    {
        const auto Binary = qEnvironmentVariable("HC_SQLITE_DLL"), File = qEnvironmentVariable("HC_SQLITE_DB");
        if (Binary.isEmpty()) QSKIP("Explicit VM-only SQLite diagnostic inputs required");
        ViewerUiDispatcher Dispatcher; QVERIFY(Dispatcher.Initialize());
        const auto Handler=AddVectoredExceptionHandler(1,SqliteException);
        const auto Remove=qScopeGuard([&] { if (Handler) RemoveVectoredExceptionHandler(Handler); });
        using TraceCall=std::function<void(const QString&)>;
        const TraceCall Trace = [](const QString& Message) {
            const auto Path=qEnvironmentVariable("HC_SQLITE_TRACE");
            if (!Path.isEmpty()) { QFile File(Path); if (File.open(QIODevice::Append)) { File.write((QString::number(GetTickCount64())+" "+Message+"\r\n").toUtf8()); File.flush(); } }
            qInfo().noquote() << Message;
        };
        Trace("SQLite diagnostic LoadLibrary begin");
        const auto Module = LoadLibraryW(reinterpret_cast<LPCWSTR>(Binary.utf16())); QVERIFY(Module);
        Trace("SQLite diagnostic LoadLibrary returned");
        const auto Load = reinterpret_cast<HWND(__stdcall*)(HWND,wchar_t*,int)>(GetProcAddress(Module,"ListLoadW"));
        const auto Close = reinterpret_cast<void(__stdcall*)(HWND)>(GetProcAddress(Module,"ListCloseWindow"));
        QVERIFY(Load && Close); QVERIFY(QFileInfo::exists(File));
        QTemporaryDir Config;
        if (qEnvironmentVariableIntValue("HC_SQLITE_DISABLE_PREVIEW")) {
            QFile Ini(Config.filePath("diagnostic.ini")); QVERIFY(Ini.open(QIODevice::WriteOnly));
            QVERIFY(Ini.write("[SQLiteViewer]\r\nShowDataPreview=0\r\nShowDataPreviewQView=0\r\n") > 0);
        }
        struct Defaults { int size_; DWORD low_,high_; char ini_[MAX_PATH]; } Params{};
        Params.size_=sizeof(Params); Params.low_=10; Params.high_=2;
        const auto Ini=Config.filePath("diagnostic.ini").toLocal8Bit();
        std::copy(Ini.begin(),Ini.end(),Params.ini_);
        if (const auto Set=reinterpret_cast<void(__stdcall*)(Defaults*)>(GetProcAddress(Module,"ListSetDefaultParams"))) Set(&Params);
        Trace("SQLite diagnostic default params returned");
        HWND Window = CreateWindowExW(0,L"STATIC",L"SQLite diagnostic",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
            0,0,900,600,nullptr,nullptr,nullptr,nullptr); QVERIFY(Window);
        std::atomic<bool> Done=false;
        const auto UiThread=GetCurrentThreadId();
        auto Observer = std::async(std::launch::async,[&] {
            HANDLE Thread=OpenThread(THREAD_QUERY_INFORMATION,FALSE,UiThread);
            const auto Chain=OpenThreadWaitChainSession(0,nullptr);
            for (unsigned Index=1; !Done && Index<100; ++Index) {
                FILETIME Start{},Exit{},Kernel{},User{}; GetThreadTimes(Thread,&Start,&Exit,&Kernel,&User);
                Trace(QString("SQLite UI probe %1 ack=%2 cpu=%3").arg(Index).arg(Dispatcher.Probe(Index).acknowledged_)
                    .arg((quint64(User.dwHighDateTime)<<32)|User.dwLowDateTime));
                if (Index==10) EnumWindows([](HWND Window,LPARAM Context)->BOOL {
                    DWORD Pid=0; GetWindowThreadProcessId(Window,&Pid);
                    if(Pid==GetCurrentProcessId()) {
                        wchar_t Title[1024]{},Class[256]{}; GetWindowTextW(Window,Title,1024); GetClassNameW(Window,Class,256);
                        auto& Logger=*reinterpret_cast<const TraceCall*>(Context);
                        Logger(QString("window hwnd=%1 visible=%2 class=%3 title=%4").arg(quintptr(Window)).arg(IsWindowVisible(Window))
                            .arg(QString::fromWCharArray(Class)).arg(QString::fromWCharArray(Title)));
                        EnumChildWindows(Window,[](HWND Child,LPARAM Context)->BOOL {
                            wchar_t Text[1024]{}; GetWindowTextW(Child,Text,1024);
                            if(*Text) (*reinterpret_cast<const TraceCall*>(Context))(QString("child=%1").arg(QString::fromWCharArray(Text)));
                            return TRUE;
                        },Context);
                    } return TRUE;
                },reinterpret_cast<LPARAM>(&Trace));
                WAITCHAIN_NODE_INFO Nodes[WCT_MAX_NODE_COUNT]{}; DWORD Count=WCT_MAX_NODE_COUNT; BOOL Cycle=FALSE;
                if (Chain && GetThreadWaitChain(Chain,0,WCTP_GETINFO_ALL_FLAGS,UiThread,&Count,Nodes,&Cycle)) {
                    for (DWORD At=0;At<Count;++At) Trace(QString("wait node type=%1 status=%2 pid=%3 tid=%4 cycle=%5")
                        .arg(Nodes[At].ObjectType).arg(Nodes[At].ObjectStatus).arg(Nodes[At].ThreadObject.ProcessId)
                        .arg(Nodes[At].ThreadObject.ThreadId).arg(Cycle));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (Chain) CloseThreadWaitChainSession(Chain); CloseHandle(Thread);
        });
        Trace(QString("SQLite diagnostic ListLoadW begin thread=%1").arg(GetCurrentThreadId()));
        auto Name=File.toStdWString();
        const auto Viewer=Load(Window,Name.data(),0);
        Trace(QString("SQLite diagnostic ListLoadW returned viewer=%1").arg(quintptr(Viewer)));
        Done=true; Observer.get(); QVERIFY(Viewer);
        for (unsigned Index=0;Index<20;++Index) Dispatcher.Cycle(true);
        Close(Viewer); DestroyWindow(Window); FreeLibrary(Module);
    }
    void nativeAndQtLoops_data()
    {
        QTest::addColumn<int>("Kind"); QTest::addColumn<bool>("Sdk");
        for (int Kind = 0; Kind != 4; ++Kind) for (bool Sdk : {false, true})
            QTest::newRow(qPrintable(QString::number(Kind) + (Sdk ? "-sdk" : "-toolbar"))) << Kind << Sdk;
    }
    void nativeAndQtLoops()
    {
        QFETCH(int, Kind); QFETCH(bool, Sdk);
        ViewerUiDispatcher Dispatcher; QVERIFY(Dispatcher.Initialize());
        ModalState State{&Dispatcher, Kind};
        WNDCLASSW Class{}; Class.lpfnWndProc = ModalProc; Class.hInstance = GetModuleHandleW(nullptr);
        Class.lpszClassName = L"HC.DispatcherTest"; RegisterClassW(&Class);
        auto Window = CreateWindowExW(0, Class.lpszClassName, L"test", WS_OVERLAPPEDWINDOW,
            0, 0, 200, 100, nullptr, nullptr, Class.hInstance, &State);
        QVERIFY(Window); ShowWindow(Window, SW_SHOWNOACTIVATE);
        auto Observer = std::async(std::launch::async, [&] {
            const auto Until = GetTickCount64() + 2000;
            while (!State.entered_ && GetTickCount64() < Until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            State.pending_ = true;
            Dispatcher.Wake();
            for (unsigned Index = 1; Index <= 3; ++Index) {
                if (Dispatcher.Probe(Index).acknowledged_) ++State.probes_;
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            State.release_ = true;
        });
        if (Sdk) { ViewerUiDispatcher::Request Request(Dispatcher); SendMessageW(Window, ModalMessage, 0, 0); }
        else {
            PostMessageW(Window, ModalMessage, 0, 0);
            QElapsedTimer Wait; Wait.start();
            while (!State.finished_ && Wait.elapsed() < 2000) Dispatcher.Cycle(true);
        }
        Observer.get();
        QVERIFY(State.finished_); QVERIFY(State.pending_); QVERIFY(State.probes_ >= 2);
        QVERIFY(!State.modal_); QVERIFY(!State.boundaryInside_); QVERIFY(Dispatcher.AtBoundary());
        // The pending SDK is taken here, after the modal call returned.
        { ViewerUiDispatcher::Request Request(Dispatcher); State.pending_ = false; }
        DestroyWindow(Window);
    }
    void deferredDeleteAndLostWake()
    {
        ViewerUiDispatcher Dispatcher; QVERIFY(Dispatcher.Initialize());
        QPointer<QObject> Object = new QObject;
        { ViewerUiDispatcher::Request Request(Dispatcher); Object->deleteLater(); QVERIFY(Object); }
        QElapsedTimer Timer; Timer.start();
        Dispatcher.Cycle(true); // No explicit Wake; periodic public Qt timer.
        QVERIFY(Object.isNull()); QVERIFY(Timer.elapsed() < 250);
    }
    void qtModalDoesNotFloodNativeQueue()
    {
        ViewerUiDispatcher Dispatcher; QVERIFY(Dispatcher.Initialize()); ModalState State{&Dispatcher, 0}; State.timeoutMs_ = 5000;
        WNDCLASSW Class{}; Class.lpfnWndProc = ModalProc; Class.hInstance = GetModuleHandleW(nullptr); Class.lpszClassName = L"HC.DispatcherTest"; RegisterClassW(&Class);
        auto Window = CreateWindowExW(0, Class.lpszClassName, L"modal queue test", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, nullptr, nullptr, Class.hInstance, &State); QVERIFY(Window);
        const auto Destroy = qScopeGuard([&] { DestroyWindow(Window); });
        unsigned QuotaFailures = 0, OtherFailures = 0;
        auto Observer = std::async(std::launch::async, [&] {
            const auto Until = GetTickCount64() + 2000;
            while (!State.entered_ && GetTickCount64() < Until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            State.pending_ = true; Dispatcher.Wake();
            // Well below USERPostMessageLimit. A full queue here is generated
            // by the host's feedback, not by this bounded external traffic.
            for (unsigned Round = 0; Round < 25; ++Round) {
                for (unsigned Index = 0; Index < 32; ++Index) {
                    SetLastError(0);
                    if (!PostMessageW(Window, WM_NULL, 0, 0)) {
                        if (GetLastError() == ERROR_NOT_ENOUGH_QUOTA) ++QuotaFailures; else ++OtherFailures;
                    }
                }
                if (Dispatcher.Probe(Round + 1).acknowledged_) ++State.probes_;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            State.release_ = true;
            // Sent delivery also provides bounded teardown when a regressing
            // posted queue is full and its Qt safety timer cannot be delivered.
            DWORD_PTR Reply = 0;
            SendMessageTimeoutW(State.modalWindow_, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &Reply);
        });
        QVERIFY(PostMessageW(Window, ModalMessage, 0, 0));
        QElapsedTimer Timer; Timer.start(); while (!State.finished_ && Timer.elapsed() < 7000) Dispatcher.Cycle(true);
        Observer.get(); QVERIFY(State.finished_); QVERIFY(State.pending_); QVERIFY(!State.boundaryInside_); QVERIFY(Dispatcher.AtBoundary());
        QCOMPARE(QuotaFailures, 0u); QCOMPARE(OtherFailures, 0u); QVERIFY(State.probes_ >= 20);
        DWORD_PTR Reply = 0; QVERIFY(SendMessageTimeoutW(Window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &Reply));
    }
    void floodAndQuit()
    {
        ViewerUiDispatcher Dispatcher; QVERIFY(Dispatcher.Initialize());
        auto Window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        for (int Index = 0; Index < 1000; ++Index) PostMessageW(Window, WM_NULL, 0, 0);
        Dispatcher.Cycle(false); QVERIFY(Dispatcher.AtBoundary());
        MSG Remaining{}; QVERIFY(PeekMessageW(&Remaining, Window, 0, 0, PM_NOREMOVE));
        while (PeekMessageW(&Remaining, Window, 0, 0, PM_REMOVE)) {}
        const auto Serial = Dispatcher.BoundarySerial();
        PostQuitMessage(0);
        for (unsigned Index = 0; Index < 100 && !Dispatcher.QuitRequested(); ++Index) Dispatcher.Cycle(false);
        QVERIFY(Dispatcher.QuitRequested()); QVERIFY(Dispatcher.BoundarySerial() > Serial);
        DestroyWindow(Window);
    }
};
int main(int Argc, char** Argv)
{
    QApplication App(Argc, Argv); App.setQuitOnLastWindowClosed(false);
    ViewerUiDispatcherTests Tests; return QTest::qExec(&Tests, Argc, Argv);
}
#include "tstViewerUiDispatcher.moc"
