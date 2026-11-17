#include "stdafx.h"
#include "sqliteKeyStore.hpp"

/// 모듈

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리
#include "sqliteApi.hpp"

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <Windows.h>
#include <dpapi.h>

namespace hc::sqlite
{
    namespace
    {
        constexpr auto FormatId = "hc-sqlite-keys";
        constexpr auto Protection = "dpapi-user";

        KeyFormat FormatFromName(const QString& Name)
        {
            if (Name == QLatin1String("key")) return KeyFormat::Raw;
            if (Name == QLatin1String("hexkey")) return KeyFormat::Hex;
            return KeyFormat::Text;
        }

        bool SamePath(const QString& Left, const QString& Right)
        {
            return QDir::cleanPath(QFileInfo(Left).absoluteFilePath()).compare(QDir::cleanPath(QFileInfo(Right).absoluteFilePath()), Qt::CaseInsensitive) == 0;
        }

        // 네트워크 경로(\\server\share)는 닿지 못해도 ‘없는 파일’ 로 보지 않는다.
        bool NetworkPath(const QString& Path)
        {
            const auto Native = QDir::toNativeSeparators(Path);
            return Native.startsWith(QLatin1String("\\\\"));
        }
    }

    void WipeKey(QByteArray& Key)
    {
        if (Key.isEmpty()) return;
        SecureZeroMemory(Key.data(), size_t(Key.size()));
        Key.clear();
    }

    QByteArray ProtectKey(const QByteArray& Key)
    {
        DATA_BLOB In{DWORD(Key.size()), reinterpret_cast<BYTE*>(const_cast<char*>(Key.constData()))}, Out{};
        if (!CryptProtectData(&In, L"HyperCommander SQLite key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Out)) return {};
        QByteArray Result(reinterpret_cast<const char*>(Out.pbData), int(Out.cbData));
        LocalFree(Out.pbData);
        return Result;
    }

    QByteArray RevealKey(const QByteArray& Protected)
    {
        DATA_BLOB In{DWORD(Protected.size()), reinterpret_cast<BYTE*>(const_cast<char*>(Protected.constData()))}, Out{};
        if (Protected.isEmpty() || !CryptUnprotectData(&In, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Out)) return {};
        QByteArray Result(reinterpret_cast<const char*>(Out.pbData), int(Out.cbData));
        SecureZeroMemory(Out.pbData, Out.cbData);
        LocalFree(Out.pbData);
        return Result;
    }

    QString FileIdentity(const QString& Path)
    {
        const auto Native = QDir::toNativeSeparators(QFileInfo(Path).absoluteFilePath()).toStdWString();
        HANDLE File = CreateFileW(Native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (File == INVALID_HANDLE_VALUE) return {};
        BY_HANDLE_FILE_INFORMATION Info{};
        const bool Ok = GetFileInformationByHandle(File, &Info) != 0;
        CloseHandle(File);
        if (!Ok) return {};
        return QStringLiteral("%1:%2%3").arg(Info.dwVolumeSerialNumber, 8, 16, QLatin1Char('0'))
            .arg(Info.nFileIndexHigh, 8, 16, QLatin1Char('0')).arg(Info.nFileIndexLow, 8, 16, QLatin1Char('0'));
    }

    QString KeyFormatName(KeyFormat Format)
    {
        return Format == KeyFormat::Raw ? QStringLiteral("key") : Format == KeyFormat::Hex ? QStringLiteral("hexkey") : QStringLiteral("textkey");
    }

    QString KeyFormatLabel(KeyFormat Format)
    {
        return Format == KeyFormat::Raw ? QObject::tr("문자열 (key)") : Format == KeyFormat::Hex ? QObject::tr("16진 (hexkey)") : QObject::tr("암호문 (textkey)");
    }

    bool ApplyProtectedKey(Connection& Db, const QByteArray& Protected, KeyFormat Format, QString* Error)
    {
        auto Key = RevealKey(Protected);
        if (Key.isEmpty()) { if (Error) *Error = QObject::tr("키를 풀지 못했습니다(DPAPI)."); return false; }
        const int Rc = Db.ApplyKey(Key, Format);
        WipeKey(Key);
        if (Rc != SQLITE_OK) { if (Error) *Error = Db.ErrorMessage(); return false; }
        return true;
    }

    KeyStore::KeyStore(const QString& Directory)
    {
        if (!Directory.isEmpty()) file_ = QDir(Directory).filePath(QStringLiteral("sqlite-keys.dat"));
    }

    QList<KeyStore::Entry> KeyStore::read() const
    {
        QList<Entry> Items;
        QFile Input(file_);
        if (file_.isEmpty() || !Input.open(QIODevice::ReadOnly)) return Items;
        const auto Root = QJsonDocument::fromJson(Input.readAll()).object();
        // 형식 · 보호 방식을 모르는 파일은 읽지 않는다(나중에 다른 보호 방식을 더할 자리).
        if (Root[QLatin1String("format")].toString() != QLatin1String(FormatId) || Root[QLatin1String("protection")].toString() != QLatin1String(Protection)) return Items;
        for (const auto& Value : Root[QLatin1String("entries")].toArray()) {
            const auto Object = Value.toObject();
            Entry Item;
            Item.path_ = Object[QLatin1String("path")].toString();
            Item.fileId_ = Object[QLatin1String("fileId")].toString();
            Item.format_ = FormatFromName(Object[QLatin1String("keyFormat")].toString());
            Item.saved_ = QDateTime::fromString(Object[QLatin1String("saved")].toString(), Qt::ISODate);
            Item.used_ = QDateTime::fromString(Object[QLatin1String("used")].toString(), Qt::ISODate);
            Item.protected_ = QByteArray::fromBase64(Object[QLatin1String("key")].toString().toLatin1());
            if (!Item.path_.isEmpty() && !Item.protected_.isEmpty()) Items.append(Item);
        }
        return Items;
    }

    bool KeyStore::write(const QList<Entry>& Items)
    {
        QJsonArray Array;
        for (const auto& Item : Items)
            Array.append(QJsonObject{{"path", QDir::toNativeSeparators(Item.path_)}, {"fileId", Item.fileId_}, {"keyFormat", KeyFormatName(Item.format_)},
                                     {"saved", Item.saved_.toUTC().toString(Qt::ISODate)}, {"used", Item.used_.toUTC().toString(Qt::ISODate)},
                                     {"key", QString::fromLatin1(Item.protected_.toBase64())}});
        QDir().mkpath(QFileInfo(file_).absolutePath());
        QSaveFile Output(file_);
        return Output.open(QIODevice::WriteOnly)
            && Output.write(QJsonDocument(QJsonObject{{"format", FormatId}, {"version", 1}, {"protection", Protection}, {"entries", Array}}).toJson(QJsonDocument::Indented)) >= 0
            && Output.commit();
    }

    template <class Fn> bool KeyStore::update(Fn&& Change)
    {
        if (file_.isEmpty()) return false;
        QDir().mkpath(QFileInfo(file_).absolutePath());
        QLockFile Lock(file_ + QStringLiteral(".lock"));
        Lock.setStaleLockTime(10000);
        if (!Lock.tryLock(3000)) return false;
        auto Items = read();
        if (!Change(Items)) return true;
        return write(Items);
    }

    QList<KeyStore::Entry> KeyStore::Entries() const { return read(); }

    std::optional<KeyStore::Entry> KeyStore::Find(const QString& Path)
    {
        const auto Items = read();
        for (const auto& Item : Items) if (SamePath(Item.path_, Path)) return Item;
        // 옮기거나 이름을 바꾼 파일: 파일 ID 로 찾고 경로를 고쳐 둔다.
        const auto Identity = FileIdentity(Path);
        if (Identity.isEmpty()) return std::nullopt;
        for (const auto& Item : Items)
            if (Item.fileId_ == Identity) {
                auto Moved = Item;
                Moved.path_ = QFileInfo(Path).absoluteFilePath();
                ChangePath(Item.path_, Moved.path_);
                return Moved;
            }
        return std::nullopt;
    }

    bool KeyStore::Save(const QString& Path, const QByteArray& Key, KeyFormat Format)
    {
        return SaveProtected(Path, {ProtectKey(Key), Format});
    }

    bool KeyStore::SaveProtected(const QString& Path, const ProtectedKey& Key)
    {
        if (Key.bytes_.isEmpty()) return false;
        const auto Now = QDateTime::currentDateTime();
        return update([&](QList<Entry>& Items) {
            Items.erase(std::remove_if(Items.begin(), Items.end(), [&](const Entry& Item) { return SamePath(Item.path_, Path); }), Items.end());
            Items.append(Entry{QFileInfo(Path).absoluteFilePath(), FileIdentity(Path), Key.format_, Now, Now, Key.bytes_});
            return true;
        });
    }

    bool KeyStore::Touch(const QString& Path)
    {
        return update([&](QList<Entry>& Items) {
            for (auto& Item : Items) if (SamePath(Item.path_, Path)) { Item.used_ = QDateTime::currentDateTime(); return true; }
            return false;
        });
    }

    bool KeyStore::Remove(const QString& Path)
    {
        return update([&](QList<Entry>& Items) {
            const auto Before = Items.size();
            Items.erase(std::remove_if(Items.begin(), Items.end(), [&](const Entry& Item) { return SamePath(Item.path_, Path); }), Items.end());
            return Items.size() != Before;
        });
    }

    bool KeyStore::ChangePath(const QString& From, const QString& To)
    {
        return update([&](QList<Entry>& Items) {
            for (auto& Item : Items)
                if (SamePath(Item.path_, From)) { Item.path_ = QFileInfo(To).absoluteFilePath(); Item.fileId_ = FileIdentity(To); return true; }
            return false;
        });
    }

    int KeyStore::RemoveMissing()
    {
        int Removed = 0;
        update([&](QList<Entry>& Items) {
            const auto Before = Items.size();
            Items.erase(std::remove_if(Items.begin(), Items.end(), [](const Entry& Item) { return StateOf(Item) == State::Missing; }), Items.end());
            Removed = int(Before - Items.size());
            return Removed > 0;
        });
        return Removed;
    }

    bool KeyStore::RemoveAll()
    {
        return update([](QList<Entry>& Items) { Items.clear(); return true; });
    }

    KeyStore::State KeyStore::StateOf(const Entry& Item)
    {
        if (QFileInfo::exists(Item.path_)) return State::Ok;
        if (NetworkPath(Item.path_)) {
            // 공유 자체에 닿지 못하면 ‘연결 안 됨’(정리 대상이 아니다).
            const auto Native = QDir::toNativeSeparators(Item.path_);
            const auto Parts = Native.mid(2).split(QLatin1Char('\\'));
            if (Parts.size() >= 2 && !QFileInfo::exists(QStringLiteral("\\\\%1\\%2").arg(Parts[0], Parts[1]))) return State::Unreachable;
        }
        return State::Missing;
    }
}
