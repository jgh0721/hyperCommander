#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QString>

namespace hc::document::explorer
{
    // 탐색기의 파일 조작. 모두 동기 호출이며 실패하면 false 와 사용자에게 보일 이유(Error)를 돌려준다.
    // 휴지통은 ‘휴지통으로만’ 옮긴다 — 휴지통이 없는 위치(네트워크 공유 · 이동식 드라이브)에서 영구 삭제로
    // 바꾸지 않는다(설계 5.5). 영구 삭제는 PermanentlyRemove 를 따로 부른다.
    struct FileOpResult
    {
        bool ok_ = false;
        QString error_;
        explicit operator bool() const { return ok_; }
    };

    // Folder 안에 빈 파일 Name 을 만든다. 이미 있으면 실패.
    FileOpResult CreateEmptyFile(const QString& Folder, const QString& Name, QString* Created = nullptr);
    FileOpResult CreateFolder(const QString& Parent, const QString& Name, QString* Created = nullptr);
    // 같은 폴더 안에서 이름만 바꾼다. 대소문자만 다른 이름도 허용한다.
    FileOpResult RenameEntry(const QString& Path, const QString& NewName, QString* Renamed = nullptr);
    // 이 위치에 휴지통이 있는가(로컬 고정 디스크). 없으면 Recycle 은 하지 않는다.
    bool CanRecycle(const QString& Path);
    FileOpResult Recycle(const QString& Path);
    FileOpResult PermanentlyRemove(const QString& Path);
    // Windows 탐색기에서 항목을 선택해 보인다(폴더면 그 폴더를 연다).
    void RevealInShell(const QString& Path);
}
