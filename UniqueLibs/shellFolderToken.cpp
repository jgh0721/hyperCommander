#include "stdafx.h"
#include "shellFolderToken.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <ShlObj.h>
#include <QThread>

DECLARE_CMNLIBSV2_NAMESPACE

namespace hc::shell
{
    QString ResolveKnownFolderPath(const QUuid& Id)
    {
        Q_ASSERT(QThread::isMainThread());
        const GUID Native = Id;
        PWSTR Buffer = nullptr;
        const auto Status = SHGetKnownFolderPath(Native, KF_FLAG_DONT_VERIFY, nullptr, &Buffer);
        const auto Path = SUCCEEDED(Status) && Buffer ? QString::fromWCharArray(Buffer) : QString{};
        CoTaskMemFree(Buffer);
        return Path;
    }
    ResolvedFolder ResolveFolderToken(const FolderToken& Token)
    {
        Q_ASSERT(QThread::isMainThread());
        ResolvedFolder Result;
        auto Id = Token.id_;
        if (Token.kind_ == FolderToken::Kind::KnownFolderName) {
            IKnownFolderManager* Manager = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_KnownFolderManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Manager)))) {
                IKnownFolder* Folder = nullptr;
                if (SUCCEEDED(Manager->GetFolderByName(reinterpret_cast<LPCWSTR>(Token.name_.utf16()), &Folder))) {
                    KNOWNFOLDERID Native{};
                    if (SUCCEEDED(Folder->GetId(&Native))) Id = QUuid(Native);
                    Folder->Release();
                }
                Manager->Release();
            }
            if (Id.isNull()) { Result.error_ = QObject::tr("셸 폴더 이름을 찾을 수 없습니다."); return Result; }
        }
        PIDLIST_ABSOLUTE Pidl = nullptr;
        if (Token.kind_ == FolderToken::Kind::Namespace) {
            if (SUCCEEDED(SHParseDisplayName(reinterpret_cast<LPCWSTR>(Token.token_.utf16()), nullptr, &Pidl, 0, nullptr))) {
                wchar_t Buffer[32768]{};
                if (SHGetPathFromIDListEx(Pidl, Buffer, DWORD(std::size(Buffer)), GPFIDL_DEFAULT)) Result.path_ = QString::fromWCharArray(Buffer);
            }
        } else if (Token.kind_ == FolderToken::Kind::KnownFolder || Token.kind_ == FolderToken::Kind::KnownFolderName) {
            Result.path_ = ResolveKnownFolderPath(Id);
            const GUID Native = Id;
            SHGetKnownFolderIDList(Native, KF_FLAG_DONT_VERIFY, nullptr, &Pidl);
        }
        if (Pidl) {
            PWSTR Label = nullptr;
            if (SUCCEEDED(SHGetNameFromIDList(Pidl, SIGDN_NORMALDISPLAY, &Label)) && Label) Result.label_ = QString::fromWCharArray(Label);
            CoTaskMemFree(Label);
            CoTaskMemFree(Pidl);
        }
        if (Result.path_.isEmpty()) Result.error_ = QObject::tr("파일 시스템 경로 없음");
        return Result;
    }
    QVector<KnownFolderChoice> KnownFolderChoices()
    {
        return {{"downloads", QObject::tr("다운로드"), QUuid(FOLDERID_Downloads)},
            {"documents", QObject::tr("문서"), QUuid(FOLDERID_Documents)},
            {"desktop", QObject::tr("바탕 화면"), QUuid(FOLDERID_Desktop)},
            {"pictures", QObject::tr("사진"), QUuid(FOLDERID_Pictures)},
            {"music", QObject::tr("음악"), QUuid(FOLDERID_Music)},
            {"videos", QObject::tr("동영상"), QUuid(FOLDERID_Videos)},
            {"profile", QObject::tr("사용자 폴더"), QUuid(FOLDERID_Profile)},
            {"public", QObject::tr("공용 폴더"), QUuid(FOLDERID_Public)}};
    }
}
