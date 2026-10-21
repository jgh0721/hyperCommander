#include "stdafx.h"
#include "passwordStore.hpp"

/// Platform libraries
#include <bcrypt.h>
#include <wincrypt.h>

namespace hc::passwords
{
    namespace
    {
        constexpr int MaxBytes = 4 * 1024 * 1024;
        constexpr int MaxEntries = 100;
        constexpr ULONGLONG Iterations = 600000;
        void erase(QByteArray& Bytes) { if (!Bytes.isEmpty()) SecureZeroMemory(Bytes.data(), size_t(Bytes.size())); Bytes.clear(); }
        QByteArray randomBytes(int Size)
        {
            QByteArray Bytes(Size, Qt::Uninitialized);
            if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(Bytes.data()), ULONG(Size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
            return Bytes;
        }
        QByteArray derive(const QString& Password, const QByteArray& Salt)
        {
            BCRYPT_ALG_HANDLE Algorithm = nullptr;
            if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return {};
            auto Close = qScopeGuard([&] { BCryptCloseAlgorithmProvider(Algorithm, 0); });
            auto Bytes = Password.toUtf8(); auto Clear = qScopeGuard([&] { erase(Bytes); });
            QByteArray Key(32, Qt::Uninitialized);
            const auto Status = BCryptDeriveKeyPBKDF2(Algorithm, reinterpret_cast<PUCHAR>(Bytes.data()), ULONG(Bytes.size()),
                reinterpret_cast<PUCHAR>(const_cast<char*>(Salt.constData())), ULONG(Salt.size()), Iterations,
                reinterpret_cast<PUCHAR>(Key.data()), ULONG(Key.size()), 0);
            if (Status < 0) { erase(Key); return {}; }
            return Key;
        }
        QByteArray crypt(bool Encrypt, const QByteArray& Input, const QByteArray& Key, QByteArray& Nonce, QByteArray& Tag)
        {
            if (Key.size() != 32 || Nonce.size() != 12 || Tag.size() != 16 || Input.isEmpty() || Input.size() > MaxBytes) return {};
            BCRYPT_ALG_HANDLE Algorithm = nullptr;
            if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) return {};
            auto Close = qScopeGuard([&] { BCryptCloseAlgorithmProvider(Algorithm, 0); });
            if (BCryptSetProperty(Algorithm, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)), sizeof(BCRYPT_CHAIN_MODE_GCM), 0) < 0) return {};
            BCRYPT_KEY_HANDLE Handle = nullptr;
            if (BCryptGenerateSymmetricKey(Algorithm, &Handle, nullptr, 0,
                reinterpret_cast<PUCHAR>(const_cast<char*>(Key.constData())), ULONG(Key.size()), 0) < 0) return {};
            auto Destroy = qScopeGuard([&] { BCryptDestroyKey(Handle); });
            BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO Info;
            BCRYPT_INIT_AUTH_MODE_INFO(Info);
            Info.pbNonce = reinterpret_cast<PUCHAR>(Nonce.data()); Info.cbNonce = ULONG(Nonce.size());
            Info.pbTag = reinterpret_cast<PUCHAR>(Tag.data()); Info.cbTag = ULONG(Tag.size());
            QByteArray Output(Input.size(), Qt::Uninitialized); ULONG Written = 0;
            auto Function = Encrypt ? BCryptEncrypt : BCryptDecrypt;
            const auto Status = Function(Handle, reinterpret_cast<PUCHAR>(const_cast<char*>(Input.constData())), ULONG(Input.size()),
                &Info, nullptr, 0, reinterpret_cast<PUCHAR>(Output.data()), ULONG(Output.size()), &Written, 0);
            if (Status < 0) { erase(Output); return {}; }
            Output.resize(Written); return Output;
        }
        QByteArray digest(const QByteArray& Data) { return QCryptographicHash::hash(Data, QCryptographicHash::Sha256); }
    }

    Store::Store(QString Path) : path_(std::move(Path)) {}
    bool Store::fail(const QString& Message) { error_ = Message; return false; }
    QByteArray Store::Protect(const QByteArray& Plain)
    {
        if (Plain.size() > MaxBytes) return {};
        DATA_BLOB Input{DWORD(Plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(Plain.constData()))}, Output{};
        if (!CryptProtectData(&Input, L"HyperCommander passwords", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Output)) return {};
        QByteArray Result(reinterpret_cast<char*>(Output.pbData), Output.cbData);
        SecureZeroMemory(Output.pbData, Output.cbData); LocalFree(Output.pbData); return Result;
    }
    QByteArray Store::Reveal(const QByteArray& Protected)
    {
        if (Protected.isEmpty() || Protected.size() > MaxBytes) return {};
        DATA_BLOB Input{DWORD(Protected.size()), reinterpret_cast<BYTE*>(const_cast<char*>(Protected.constData()))}, Output{};
        if (!CryptUnprotectData(&Input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Output)) return {};
        QByteArray Result(reinterpret_cast<char*>(Output.pbData), Output.cbData);
        SecureZeroMemory(Output.pbData, Output.cbData); LocalFree(Output.pbData); return Result;
    }
    bool Store::Load()
    {
        error_.clear(); entries_.clear(); protectedKey_.clear(); salt_.clear(); envelope_.clear(); master_ = locked_ = false; automatic_ = true;
        QFile File(path_);
        if (!File.exists()) { revision_ = digest({}); return true; }
        if (!File.open(QIODevice::ReadOnly) || File.size() > MaxBytes) { locked_ = true; return fail(QObject::tr("비밀번호 저장소를 읽을 수 없습니다.")); }
        envelope_ = File.readAll(); revision_ = digest(envelope_);
        const auto Object = QJsonDocument::fromJson(envelope_).object();
        if (Object["version"].toInt() != 1 || !Object["master"].isBool() || !Object["data"].isString()) {
            locked_ = true; return fail(QObject::tr("비밀번호 저장소 형식이 올바르지 않습니다."));
        }
        master_ = Object["master"].toBool(); locked_ = true;
        if (master_) {
            salt_ = QByteArray::fromBase64(Object["salt"].toString().toLatin1());
            if (salt_.size() != 32 || Object["iterations"].toInt() != Iterations) return fail(QObject::tr("비밀번호 저장소 보호 정보가 올바르지 않습니다."));
            return true;
        }
        if (!decode(Reveal(QByteArray::fromBase64(Object["data"].toString().toLatin1())))) return false;
        locked_ = false; return true;
    }
    bool Store::ReloadIfChanged()
    {
        QFile File(path_); QByteArray Bytes;
        if (File.exists()) {
            if (!File.open(QIODevice::ReadOnly) || File.size() > MaxBytes) return Load();
            Bytes = File.readAll(); File.close();
        }
        return digest(Bytes) == revision_ ? true : Load();
    }
    bool Store::decode(QByteArray Plain)
    {
        auto Clear = qScopeGuard([&] { erase(Plain); });
        const auto Object = QJsonDocument::fromJson(Plain).object();
        if (Object["version"].toInt() != 1 || !Object["entries"].isArray() || !Object["automatic"].isBool()
            || Object["entries"].toArray().size() > MaxEntries) return fail(QObject::tr("비밀번호 저장소를 열 수 없습니다. Windows 계정 또는 파일을 확인하세요."));
        QVector<Entry> Entries; QSet<QString> Ids;
        for (const auto& Item : Object["entries"].toArray()) {
            const auto Row = Item.toObject();
            Entry Value{Row["id"].toString(), Row["name"].toString(), QByteArray::fromBase64(Row["value"].toString().toLatin1()), Row["automatic"].toBool()};
            if (Value.id_.isEmpty() || Ids.contains(Value.id_) || Value.name_.isEmpty() || Value.name_.size() > 100 || Value.protectedValue_.isEmpty())
                return fail(QObject::tr("저장된 비밀번호 항목이 올바르지 않습니다."));
            Ids.insert(Value.id_); Entries.push_back(std::move(Value));
        }
        entries_ = std::move(Entries); automatic_ = Object["automatic"].toBool(); error_.clear(); return true;
    }
    QByteArray Store::encode() const
    {
        QJsonArray Items;
        for (const auto& Entry : entries_) Items.append(QJsonObject{{"id", Entry.id_}, {"name", Entry.name_},
            {"value", QString::fromLatin1(Entry.protectedValue_.toBase64())}, {"automatic", Entry.automatic_}});
        return QJsonDocument(QJsonObject{{"version", 1}, {"automatic", automatic_}, {"entries", Items}}).toJson(QJsonDocument::Compact);
    }
    bool Store::Unlock(const QString& Master)
    {
        if (!locked_) return true;
        if (!master_ || Master.isEmpty() || Master.size() > 4096) return fail(QObject::tr("마스터 비밀번호를 확인하세요."));
        const auto Object = QJsonDocument::fromJson(envelope_).object();
        auto Key = derive(Master, salt_); auto Clear = qScopeGuard([&] { erase(Key); });
        auto Nonce = QByteArray::fromBase64(Object["nonce"].toString().toLatin1());
        auto Tag = QByteArray::fromBase64(Object["tag"].toString().toLatin1());
        auto Cipher = Reveal(QByteArray::fromBase64(Object["data"].toString().toLatin1()));
        auto Plain = crypt(false, Cipher, Key, Nonce, Tag);
        if (Plain.isEmpty()) return fail(QObject::tr("마스터 비밀번호가 맞지 않거나 저장소가 손상되었습니다."));
        const auto Protected = Protect(Key);
        if (Protected.isEmpty() || !decode(std::move(Plain))) return fail(QObject::tr("비밀번호 저장소를 열 수 없습니다."));
        protectedKey_ = Protected; locked_ = false; error_.clear(); return true;
    }
    void Store::Lock()
    {
        if (!master_) return;
        entries_.clear(); protectedKey_.clear(); locked_ = true;
    }
    bool Store::SetMaster(const QString& Master)
    {
        if (locked_) return fail(QObject::tr("먼저 비밀번호 저장소의 잠금을 해제하세요."));
        if (Master.size() > 4096 || Master.contains(QChar::Null)) return fail(QObject::tr("마스터 비밀번호가 너무 길거나 사용할 수 없는 문자가 있습니다."));
        if (Master.isEmpty()) { master_ = false; protectedKey_.clear(); salt_.clear(); error_.clear(); return true; }
        auto Salt = randomBytes(32); if (Salt.isEmpty()) return fail(QObject::tr("보호 정보를 생성할 수 없습니다."));
        auto Key = derive(Master, Salt); auto Clear = qScopeGuard([&] { erase(Key); });
        auto Protected = Protect(Key); if (Protected.isEmpty()) return fail(QObject::tr("마스터 비밀번호 보호를 설정할 수 없습니다."));
        salt_ = Salt; protectedKey_ = Protected; master_ = true; error_.clear(); return true;
    }
    bool Store::Save()
    {
        if (locked_) return fail(QObject::tr("잠긴 저장소는 변경할 수 없습니다."));
        QLockFile LockFile(path_ + ".lock");
        if (!LockFile.tryLock(0)) return fail(QObject::tr("다른 창에서 비밀번호 저장소를 사용 중입니다. 다시 시도하세요."));
        QFile Current(path_); QByteArray Before;
        if (Current.exists()) {
            if (!Current.open(QIODevice::ReadOnly) || Current.size() > MaxBytes) return fail(QObject::tr("기존 비밀번호 저장소를 확인할 수 없습니다."));
            Before = Current.readAll();
            Current.close();
        }
        if (digest(Before) != revision_) return fail(QObject::tr("다른 창에서 저장소를 변경했습니다. 다시 읽은 후 수정하세요."));
        auto Plain = encode(); auto Clear = qScopeGuard([&] { erase(Plain); });
        QJsonObject Object{{"version", 1}, {"master", master_}}; QByteArray Payload;
        if (master_) {
            auto Key = Reveal(protectedKey_); auto ClearKey = qScopeGuard([&] { erase(Key); });
            auto Nonce = randomBytes(12); QByteArray Tag(16, Qt::Uninitialized);
            auto Cipher = crypt(true, Plain, Key, Nonce, Tag);
            if (Cipher.isEmpty()) return fail(QObject::tr("비밀번호 저장소를 암호화할 수 없습니다."));
            Payload = Protect(Cipher);
            Object["salt"] = QString::fromLatin1(salt_.toBase64()); Object["nonce"] = QString::fromLatin1(Nonce.toBase64());
            Object["tag"] = QString::fromLatin1(Tag.toBase64()); Object["iterations"] = int(Iterations);
        } else Payload = Protect(Plain);
        if (Payload.isEmpty()) return fail(QObject::tr("Windows 계정으로 비밀번호를 보호할 수 없습니다."));
        Object["data"] = QString::fromLatin1(Payload.toBase64());
        const auto Bytes = QJsonDocument(Object).toJson(QJsonDocument::Compact);
        QSaveFile File(path_); File.setDirectWriteFallback(false);
        if (!File.open(QIODevice::WriteOnly) || File.write(Bytes) != Bytes.size() || !File.commit()) return fail(QObject::tr("비밀번호 저장소를 저장할 수 없습니다. 기존 파일은 유지됩니다."));
        envelope_ = Bytes; revision_ = digest(Bytes); error_.clear(); return true;
    }
    bool Store::Add(const QString& Name, const QString& Value, bool Automatic)
    {
        if (entries_.size() >= MaxEntries) return fail(QObject::tr("비밀번호는 최대 100개까지 저장할 수 있습니다."));
        entries_.append({QUuid::createUuid().toString(QUuid::WithoutBraces), {}, {}, Automatic});
        if (Edit(entries_.size() - 1, Name, Value, Automatic)) return true;
        entries_.removeLast(); return false;
    }
    bool Store::Edit(int Row, const QString& Name, const QString& Value, bool Automatic)
    {
        if (locked_ || Row < 0 || Row >= entries_.size()) return fail(QObject::tr("비밀번호 항목을 선택하세요."));
        if (Name.trimmed().isEmpty() || Name.size() > 100 || Value.isEmpty() || Value.size() > 4096 || Value.contains(QChar::Null))
            return fail(QObject::tr("이름과 비밀번호를 입력하세요. 이름은 100자, 비밀번호는 4096자까지 사용할 수 있습니다."));
        for (int Index = 0; Index < entries_.size(); ++Index) if (Index != Row && entries_[Index].name_.compare(Name.trimmed(), Qt::CaseInsensitive) == 0)
            return fail(QObject::tr("같은 이름의 항목이 있습니다. 다른 이름을 입력하세요."));
        auto Bytes = Value.toUtf8(); auto Clear = qScopeGuard([&] { erase(Bytes); });
        auto Protected = Protect(Bytes); if (Protected.isEmpty()) return fail(QObject::tr("비밀번호를 보호할 수 없습니다."));
        auto& Entry = entries_[Row]; Entry.name_ = Name.trimmed(); Entry.protectedValue_ = Protected; Entry.automatic_ = Automatic; error_.clear(); return true;
    }
    bool Store::Remove(int Row)
    {
        if (locked_ || Row < 0 || Row >= entries_.size()) return false;
        entries_.removeAt(Row); return true;
    }
    QString Store::Value(int Row) const
    {
        if (locked_ || Row < 0 || Row >= entries_.size()) return {};
        auto Bytes = Reveal(entries_[Row].protectedValue_); auto Clear = qScopeGuard([&] { erase(Bytes); });
        return QString::fromUtf8(Bytes);
    }
}
