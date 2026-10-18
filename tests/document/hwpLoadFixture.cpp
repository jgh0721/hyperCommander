#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include "UniqueLibs/document/documentWebPeer.hpp"
#include "UniqueLibs/document/hwpHelperDocument.hpp"
#include "hwpLoadFixtureData.hpp"
#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QTimer>
#include <QtWebEngineWidgets/QWebEngineView>
using namespace hc::document;
int main(int Argc, char** Argv)
{
    if(!RegisterHwpScheme()) return 2;
    QApplication App(Argc, Argv); App.setQuitOnLastWindowClosed(false);
    const auto Args = App.arguments();
    if(Args.size() != 10 || Args[1] != "--web-owner") return 2;
    const auto Parent = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(Args[9].toULongLong()));
    const auto Surface = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_CLIPCHILDREN,
        0, 0, 640, 480, Parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if(!Surface) return 3;
    DocumentWebPeer Peer;
    if(!Peer.Connect(Args[3], Args[5].toLatin1(), Args[7].toUInt(), reinterpret_cast<std::uintptr_t>(Parent))) return 4;
    QFile ModeFile(QCoreApplication::applicationDirPath() + "/mode.txt");
    if(!ModeFile.open(QIODevice::ReadOnly)) return 5;
    const auto Mode = ModeFile.readAll(); ModeFile.close();
    QString Error;
    const auto Hash = [](const QByteArray& Bytes) { return QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex(); };
    QMap<QString, QByteArray> Pins{{"index.html", Hash(fixture::Page)}, {"app.js", Hash(fixture::Script)}};
    if(Mode.startsWith("view-")) Pins["app.js"]=Hash(fixture::DeferredScript(Mode.startsWith("view-hang")));
    if(Mode == "engine") {
        // Separate diagnostic executable only; production package policy is unchanged.
        Pins = {{"index.html", Hash(fixture::EnginePage)}, {"app.js", Hash(fixture::EngineScript)},
            {"rhwp.js", fixture::EngineJsSha}, {"rhwp_bg.wasm", fixture::EngineWasmSha}, {"adapter.js", fixture::AdapterSha}};
    }
    auto Bundle = HwpBundle::LoadPinned(QCoreApplication::applicationDirPath(), Pins, Error);
    if(!Bundle) return 6;
    HwpHelperDocument Document(Bundle, reinterpret_cast<std::uintptr_t>(Surface), Mode=="view-hang-close"?10000:Mode.startsWith("view-")?1800:30000);
    QTimer Timer; Timer.setInterval(50);
    QObject::connect(&Timer, &QTimer::timeout, &App, [&] {
        RECT Rect{}; if(!GetClientRect(Parent, &Rect)) { App.quit(); return; }
        SetWindowPos(Surface, nullptr, 0, 0, Rect.right, Rect.bottom, SWP_NOACTIVATE | SWP_NOZORDER);
        Document.Refresh();
        if(Mode=="view-hang-close")for(auto* Widget:QApplication::allWidgets()) {
            auto* View=qobject_cast<QWebEngineView*>(Widget);
            if(View && View->title()=="deferred-running") {
                QFile Marker(QCoreApplication::applicationDirPath()+"/deferred-running.txt");
                if(Marker.open(QIODevice::WriteOnly))Marker.write("Begin ACK accepted; renderer entered synchronous deferred work");
            }
        }
    }); Timer.start();
    bool CrashScheduled = false;
    if(!Peer.Ready(reinterpret_cast<std::uintptr_t>(Surface), [&](const QByteArray& Bytes) {
        const auto Request = QJsonDocument::fromJson(Bytes).object();
        const auto Type = Request["type"].toString();
        // Native test observer: proves rejected input never crossed the pipe.
        if (Type.startsWith("Input")) {
            QFile Observed(QCoreApplication::applicationDirPath() + "/input-observed.txt");
            if (!Observed.open(QIODevice::WriteOnly | QIODevice::Append)
                || Observed.write(Type.toUtf8() + '\n') < 0) return QByteArray("{}");
        }
        QJsonObject Reply;
        if(Type == "RuntimeStatus")
            Reply = {{"type", Type}, {"version", 1}, {"transportReady", true}, {"packageApproved", true},
                {"packageError", ""}, {"rendererCreated", false}, {"documentCommands", true}, {"inputTransfer", true}};
        else if(Type == "DocumentStart" && Mode == "hang-start") { Sleep(60000); }
        else {
            Reply = Document.Handle(Request);
            if(Type == "DocumentStatus" && Mode == "stale-status") Reply["identity"] = QJsonObject{};
            if(Type == "DocumentStatus" && Mode == "crash-after-ready" && Reply["state"] == QJsonValue("ready") && !CrashScheduled) {
                CrashScheduled = true; QTimer::singleShot(600, &App, [] { ExitProcess(17); });
            }
        }
        return QJsonDocument(Reply).toJson(QJsonDocument::Compact);
    }, [&] { App.quit(); })) return 7;
    const auto Result = App.exec();
    DestroyWindow(Surface); return Result;
}
