#include "stdafx.h"
#include "explorerFileOps.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <ShlObj.h>
#include <shellapi.h>
#include <wrl/client.h>

namespace hc::document::explorer
{
    namespace
    {
        FileOpResult Fail(const QString& Error) { return {false, Error}; }
        FileOpResult Done() { return {true, {}}; }
        QString Native(const QString& Path) { return QDir::toNativeSeparators(QDir::cleanPath(Path)); }
        // 같은 이름이 있는가 — Windows 는 대소문자를 가리지 않으므로 이름만 대소문자가 다른 자기 자신은 뺀다.
        bool Occupied(const QString& Target, const QString& Self = {})
        {
            if (!QFileInfo::exists(Target)) return false;
            return Self.isEmpty() || QFileInfo(Target).canonicalFilePath().compare(QFileInfo(Self).canonicalFilePath(), Qt::CaseInsensitive) != 0;
        }
    }

    FileOpResult CreateEmptyFile(const QString& Folder, const QString& Name, QString* Created)
    {
        const auto Target = QDir(Folder).filePath(Name);
        if (Occupied(Target)) return Fail(QObject::tr("같은 이름이 이미 있습니다: %1").arg(Name));
        QFile File(Target);
        // NewOnly: 확인과 만들기 사이에 다른 프로그램이 같은 이름을 만들어도 덮어쓰지 않는다.
        if (!File.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return Fail(QObject::tr("파일을 만들지 못했습니다: %1").arg(File.errorString()));
        File.close();
        if (Created) *Created = QDir::cleanPath(Target);
        return Done();
    }

    FileOpResult CreateFolder(const QString& Parent, const QString& Name, QString* Created)
    {
        const auto Target = QDir(Parent).filePath(Name);
        if (Occupied(Target)) return Fail(QObject::tr("같은 이름이 이미 있습니다: %1").arg(Name));
        if (!QDir(Parent).mkdir(Name)) return Fail(QObject::tr("폴더를 만들지 못했습니다: %1").arg(Native(Target)));
        if (Created) *Created = QDir::cleanPath(Target);
        return Done();
    }

    FileOpResult RenameEntry(const QString& Path, const QString& NewName, QString* Renamed)
    {
        const QFileInfo Info(Path);
        if (!Info.exists()) return Fail(QObject::tr("항목을 찾을 수 없습니다: %1").arg(Native(Path)));
        const auto Target = Info.dir().filePath(NewName);
        if (Occupied(Target, Path)) return Fail(QObject::tr("같은 이름이 이미 있습니다: %1").arg(NewName));
        // MoveFileExW 는 대소문자만 바꾸는 이름 바꾸기도 처리한다(QFile::rename 은 같은 파일로 보고 거절한다).
        if (!MoveFileExW(reinterpret_cast<LPCWSTR>(Native(Path).utf16()), reinterpret_cast<LPCWSTR>(Native(Target).utf16()), 0)) {
            const auto Code = GetLastError();
            if (Code == ERROR_SHARING_VIOLATION || Code == ERROR_ACCESS_DENIED)
                return Fail(QObject::tr("다른 프로그램이 쓰고 있어 이름을 바꾸지 못했습니다: %1").arg(Info.fileName()));
            return Fail(QObject::tr("이름을 바꾸지 못했습니다(오류 %1): %2").arg(Code).arg(Info.fileName()));
        }
        if (Renamed) *Renamed = QDir::cleanPath(Target);
        return Done();
    }

    bool CanRecycle(const QString& Path)
    {
        const auto Clean = Native(QFileInfo(Path).absoluteFilePath());
        if (Clean.startsWith(QStringLiteral("\\\\"))) return false;
        const auto Root = Clean.left(3);
        if (GetDriveTypeW(reinterpret_cast<LPCWSTR>(Root.utf16())) != DRIVE_FIXED) return false;
        SHQUERYRBINFO Info{}; Info.cbSize = sizeof(Info);
        return SUCCEEDED(SHQueryRecycleBinW(reinterpret_cast<LPCWSTR>(Root.utf16()), &Info));
    }

    FileOpResult Recycle(const QString& Path)
    {
        using Microsoft::WRL::ComPtr;
        if (!QFileInfo::exists(Path)) return Fail(QObject::tr("항목을 찾을 수 없습니다: %1").arg(Native(Path)));
        if (!CanRecycle(Path)) return Fail(QObject::tr("이 위치에는 휴지통이 없습니다. 영구 삭제하려면 Shift+Del 을 누르십시오."));
        const auto Initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(Initialized) && Initialized != RPC_E_CHANGED_MODE) return Fail(QObject::tr("휴지통을 쓸 수 없습니다."));
        struct Apartment { bool owned_; ~Apartment() { if (owned_) CoUninitialize(); } } Scope{SUCCEEDED(Initialized)};
        // nativeWritableStorage 의 Recycle 과 같은 길: RecycleItem 은 ‘휴지통으로만’ 옮기고 실패해도 지우지 않는다.
        ComPtr<IShellItem> Item, Parent, Bin, Recycled;
        ComPtr<ITransferSource> Transfer;
        const auto Shell = Native(QFileInfo(Path).absoluteFilePath());
        auto Hr = SHCreateItemFromParsingName(reinterpret_cast<LPCWSTR>(Shell.utf16()), nullptr, IID_PPV_ARGS(&Item));
        if (SUCCEEDED(Hr)) Hr = Item->GetParent(&Parent);
        if (SUCCEEDED(Hr)) Hr = Parent->BindToHandler(nullptr, BHID_Transfer, IID_PPV_ARGS(&Transfer));
        if (SUCCEEDED(Hr)) Hr = SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&Bin));
        if (SUCCEEDED(Hr)) Hr = Transfer->RecycleItem(Item.Get(), Bin.Get(), TSF_NORMAL, &Recycled);
        if (FAILED(Hr) || !Recycled) {
            if (HRESULT_FACILITY(Hr) == FACILITY_WIN32 && (HRESULT_CODE(Hr) == ERROR_SHARING_VIOLATION || HRESULT_CODE(Hr) == ERROR_LOCK_VIOLATION))
                return Fail(QObject::tr("다른 프로그램이 쓰고 있어 휴지통으로 옮기지 못했습니다: %1").arg(QFileInfo(Path).fileName()));
            return Fail(QObject::tr("휴지통으로 옮기지 못했습니다(0x%1): %2").arg(quint32(Hr), 8, 16, QLatin1Char('0')).arg(QFileInfo(Path).fileName()));
        }
        return Done();
    }

    FileOpResult PermanentlyRemove(const QString& Path)
    {
        const QFileInfo Info(Path);
        if (!Info.exists() && !Info.isSymLink()) return Fail(QObject::tr("항목을 찾을 수 없습니다: %1").arg(Native(Path)));
        // 정션 · 바로 가기 폴더는 링크만 지운다(가리키는 곳의 내용은 건드리지 않는다).
        const bool Removed = Info.isDir() && !Info.isSymLink() && !Info.isJunction() ? QDir(Path).removeRecursively() : Info.isDir() ? QDir().rmdir(Path) : QFile::remove(Path);
        if (!Removed) return Fail(QObject::tr("지우지 못했습니다: %1").arg(Native(Path)));
        return Done();
    }

    void RevealInShell(const QString& Path)
    {
        const QFileInfo Info(Path);
        if (Info.isDir()) QProcess::startDetached(QStringLiteral("explorer.exe"), {Native(Path)});
        else QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,") + Native(Path)});
    }
}
