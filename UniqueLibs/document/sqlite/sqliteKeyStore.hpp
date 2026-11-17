#pragma once

/// 고유 라이브러리
#include "sqliteConnection.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <optional>
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

namespace hc::sqlite
{
    struct ProtectedKey
    {
        QByteArray bytes_;
        KeyFormat format_ = KeyFormat::Text;
    };

    // 메모리의 키를 지운다(SecureZeroMemory, 설계 8.3).
    void WipeKey(QByteArray& Key);
    // DPAPI(CryptProtectData, 현재 사용자)로 감싸고 푼다. 실패하면 빈 값.
    QByteArray ProtectKey(const QByteArray& Key);
    QByteArray RevealKey(const QByteArray& Protected);
    // 볼륨 일련번호 + NTFS 파일 ID(같은 볼륨 안에서 옮기거나 이름을 바꿔도 같다). 읽지 못하면 빈 글.
    QString FileIdentity(const QString& Path);
    QString KeyFormatName(KeyFormat Format);       // textkey · key · hexkey
    QString KeyFormatLabel(KeyFormat Format);      // 암호문 (textkey) · 문자열 (key) · 16진 (hexkey)
    // DPAPI 로 감싼 키를 풀어 연결에 넣고 곧바로 지운다. 키가 맞는지는 Probe 로 본다(틀리면 SQLITE_NOTADB).
    bool ApplyProtectedKey(Connection& Db, const QByteArray& Protected, KeyFormat Format, QString* Error);

    // SQLite 전용 키 저장소(설계 8.3, 결정 2026-11-13). 문서 창 설정 폴더의 sqlite-keys.dat 하나에 경로 · 파일 ID ·
    // 키 형식 · 저장 · 마지막 사용 시각 · DPAPI 로 감싼 키를 둔다. 설정 파일 · ‘기존 설정 가져오기’ 에는 들어가지 않는다.
    // 빠른 보기(HyperCommander) · HyperViewer · HyperEditor 가 함께 쓰므로 QLockFile 로 쓰기를 막고 쓸 때마다 다시 읽는다.
    class KeyStore
    {
    public:
        struct Entry
        {
            QString path_, fileId_;
            KeyFormat format_ = KeyFormat::Text;
            QDateTime saved_, used_;
            QByteArray protected_;      // DPAPI 로 감싼 키
        };
        enum class State { Ok, Missing, Unreachable };

        explicit KeyStore(const QString& Directory);
        bool IsValid() const { return !file_.isEmpty(); }
        QString FileName() const { return file_; }
        QList<Entry> Entries() const;
        // 경로가 같은 항목, 없으면 파일 ID 가 같은 항목(경로를 고쳐 둔다).
        std::optional<Entry> Find(const QString& Path);
        bool Save(const QString& Path, const QByteArray& Key, KeyFormat Format);
        bool SaveProtected(const QString& Path, const ProtectedKey& Key);
        bool Touch(const QString& Path);
        bool Remove(const QString& Path);
        bool ChangePath(const QString& From, const QString& To);
        // 없는 파일 항목을 지운다(네트워크 경로에 닿지 못한 항목은 둔다). 지운 개수.
        int RemoveMissing();
        bool RemoveAll();
        static State StateOf(const Entry& Item);
    private:
        QList<Entry> read() const;
        bool write(const QList<Entry>& Items);
        template <class Fn> bool update(Fn&& Change);

        QString file_;
    };
}
