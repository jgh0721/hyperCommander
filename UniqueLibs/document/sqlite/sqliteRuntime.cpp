#include "stdafx.h"
#include "sqliteRuntime.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteApi.hpp"
#if __has_include("UniqueLibs/sqlite_see_activate.hpp")
#include "UniqueLibs/sqlite_see_activate.hpp"
#define HC_SQLITE_SEE_PHRASE 1
#endif

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <Windows.h>

namespace hc::sqlite
{
    namespace
    {
        constexpr auto DllName =
#ifdef QT_DEBUG
            "sqlite3d.dll";
#else
            "sqlite3.dll";
#endif

        // 이 코드가 든 모듈(HyperEditor.exe · HyperViewer.dll · 시험 exe)의 폴더. HyperViewer 가 다른 프로그램에
        // 실려 있으면 applicationDirPath() 는 그 프로그램의 폴더라서 따로 구한다.
        QString ModuleDirectory()
        {
            HMODULE Module = nullptr;
            if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&ModuleDirectory), &Module)) return {};
            wchar_t Buffer[MAX_PATH * 2] = {};
            const auto Length = ::GetModuleFileNameW(Module, Buffer, DWORD(std::size(Buffer)));
            if (Length == 0 || Length >= std::size(Buffer)) return {};
            return QFileInfo(QString::fromWCharArray(Buffer, int(Length))).absolutePath();
        }

        struct Loaded
        {
            bool ok_ = false;
            QString path_;
        };

        const Loaded& LoadOnce()
        {
            static const Loaded Result = [] {
                QStringList Candidates;
                const auto AppDir = QCoreApplication::instance() ? QCoreApplication::applicationDirPath() : QString();
                const auto ModuleDir = ModuleDirectory();
                for (const auto& Base : {AppDir, ModuleDir, ModuleDir.isEmpty() ? QString() : QDir(ModuleDir).absoluteFilePath(QStringLiteral(".."))}) {
                    if (Base.isEmpty()) continue;
                    Candidates << QDir(Base).absoluteFilePath(QStringLiteral("Libs/sqldrivers/") + QLatin1String(DllName));
                }
                for (const auto& Base : {ModuleDir, AppDir}) if (!Base.isEmpty()) Candidates << QDir(Base).absoluteFilePath(QLatin1String(DllName));
                Candidates.removeDuplicates();
                for (const auto& Path : Candidates) {
                    if (!QFileInfo::exists(Path)) continue;
                    // 프로세스가 끝날 때까지 내리지 않는다. 지연 로드 도우미와 QSQLITE 플러그인이 이름으로 같은 모듈을 찾는다.
                    auto* Library = new QLibrary(Path);
                    Library->setLoadHints(QLibrary::PreventUnloadHint);
                    if (Library->load()) return Loaded{true, QDir::toNativeSeparators(Path)};
                    delete Library;
                }
                return Loaded{};
            }();
            return Result;
        }

        SeeState ActivateOnce()
        {
            if (!Runtime::Load()) return SeeState::LibraryMissing;
#ifndef HC_SQLITE_SEE_PHRASE
            return SeeState::NotInBuild;
#else
            // SEE 함수는 이름으로 찾는다. SEE 가 없는 DLL 이면 지연 로드 호출이 예외로 끝나므로 직접 부르지 않는다.
            const auto Module = ::GetModuleHandleW(reinterpret_cast<LPCWSTR>(QString::fromLatin1(DllName).utf16()));
            if (!Module) return SeeState::LibraryMissing;
            using Activate = void(__cdecl*)(const char*);
            using Key = int(__cdecl*)(sqlite3*, const char*, const void*, int);
            const auto ActivateSee = reinterpret_cast<Activate>(::GetProcAddress(Module, "sqlite3_activate_see"));
            const auto KeyV2 = reinterpret_cast<Key>(::GetProcAddress(Module, "sqlite3_key_v2"));
            if (!ActivateSee || !KeyV2) return SeeState::NotInLibrary;
            ActivateSee(SeeActivationPhrase);
            // 활성화가 먹었는지는 메모리 데이터베이스에 키를 넣어 본다. 꺼져 있으면 SQLITE_MISUSE 다.
            sqlite3* Db = nullptr;
            if (sqlite3_open_v2(":memory:", &Db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
                if (Db) sqlite3_close_v2(Db);
                return SeeState::ActivationFailed;
            }
            static constexpr char Probe[] = "hc-see-probe";
            const int Rc = KeyV2(Db, "main", Probe, int(sizeof(Probe) - 1));
            sqlite3_close_v2(Db);
            return Rc == SQLITE_MISUSE ? SeeState::ActivationFailed : SeeState::Available;
#endif
        }
    }

    bool Runtime::Load() { return LoadOnce().ok_; }

    SeeState Runtime::See()
    {
        static const SeeState State = ActivateOnce();
        return State;
    }

    QString Runtime::SeeText(SeeState State)
    {
        switch (State) {
        case SeeState::Available: return QObject::tr("SEE 포함 — AES-256 (Windows CryptoAPI)");
        case SeeState::LibraryMissing: return QObject::tr("SQLite 라이브러리(%1)를 불러오지 못했습니다.").arg(QLatin1String(DllName));
        case SeeState::NotInBuild: return QObject::tr("이 빌드에는 SEE 암호화가 들어 있지 않습니다. 암호화된 데이터베이스는 열 수 없습니다.");
        case SeeState::NotInLibrary: return QObject::tr("불러온 SQLite 라이브러리에 SEE 가 없습니다. Libs\\sqldrivers\\%1 이 다른 파일로 바뀌었는지 확인하세요.").arg(QLatin1String(DllName));
        case SeeState::ActivationFailed: return QObject::tr("SQLite 라이브러리의 SEE 를 활성화하지 못했습니다. 제품과 라이브러리 버전이 맞는지 확인하세요.");
        }
        return {};
    }

    QString Runtime::LibraryPath() { return LoadOnce().path_; }

    QString Runtime::Version() { return Load() ? QString::fromLatin1(sqlite3_libversion()) : QString(); }
}
