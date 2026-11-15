#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <Windows.h>
#include <QByteArray>
#include <QString>
#include <QList>
#include <atomic>
#include <optional>
#include <memory>

namespace hc::registry {
enum class Hive { ClassesRoot, CurrentUser, LocalMachine, Users, CurrentConfig };
enum class View { Native64, Wow32 };
enum class Origin { Direct, Merged, UserClasses, MachineClasses };
struct RegistryLocation
{
    Hive hive = Hive::CurrentUser;
    QString key;
    View view = View::Native64;
    QString sid; // CurrentUser is opened through HKEY_USERS when a caller SID is supplied.
    Origin origin = Origin::Direct;
    QString Path() const;
    QString PhysicalPath() const;
    QString Id() const;
    RegistryLocation Child(const QString& Name) const;
    std::optional<RegistryLocation> Parent() const;
    static std::optional<RegistryLocation> Parse(const QString& Path, View SelectedView = View::Native64);
    bool operator==(const RegistryLocation&) const = default;
};
struct RegistryValue
{
    QString name; // Empty is the default value, never a translated display label.
    quint32 type = REG_NONE;
    QByteArray bytes;
    bool operator==(const RegistryValue&) const = default;
    QString Warning() const;
};
struct KeyInfo
{
    QString name;
    quint64 lastWrite = 0;
    quint32 subkeyCount = 0;
    quint32 valueCount = 0;
    bool link = false;
};
template<class T> struct RegistryResult
{
    T value{};
    quint32 error = ERROR_SUCCESS;
    bool complete = true;
    explicit operator bool() const { return error == ERROR_SUCCESS && complete; }
};
struct KeySnapshot { KeyInfo info; QList<KeyInfo> keys; QList<RegistryValue> values; };
class KeyHandle
{
public:
    explicit KeyHandle(HKEY Handle = nullptr) : handle_(Handle) {}
    ~KeyHandle() { if (handle_) RegCloseKey(handle_); }
    KeyHandle(KeyHandle&& Other) noexcept : handle_(Other.handle_) { Other.handle_ = nullptr; }
    KeyHandle& operator=(KeyHandle&& Other) noexcept;
    KeyHandle(const KeyHandle&) = delete;
    KeyHandle& operator=(const KeyHandle&) = delete;
    HKEY Get() const { return handle_; }
private:
    HKEY handle_ = nullptr;
};
class IRegistryBackend
{
public:
    virtual ~IRegistryBackend() = default;
    virtual RegistryResult<KeySnapshot> Enumerate(const RegistryLocation&, const std::atomic_bool* Cancel = nullptr) = 0;
    virtual RegistryResult<KeyInfo> QueryInfo(const RegistryLocation& Location)
    {
        const auto Result = Enumerate(Location); return {Result.value.info, Result.error, Result.complete};
    }
    virtual RegistryResult<RegistryValue> Read(const RegistryLocation&, const QString& Name) = 0;
    virtual quint32 Create(const RegistryLocation&) = 0;
    virtual quint32 CreateLink(const RegistryLocation&, const QByteArray& Target) = 0;
    virtual quint32 Write(const RegistryLocation&, const RegistryValue&) = 0;
    virtual quint32 DeleteValue(const RegistryLocation&, const QString& Name) = 0;
    virtual quint32 DeleteKey(const RegistryLocation&) = 0; // Empty key only; recursive deletion is an explicit change plan.
    virtual quint32 RenameKey(const RegistryLocation&, const QString& Name) = 0;
};
class WinRegistryBackend final : public IRegistryBackend
{
public:
    static constexpr qsizetype MaxValueBytes = 64 * 1024 * 1024;
    RegistryResult<KeySnapshot> Enumerate(const RegistryLocation&, const std::atomic_bool* Cancel = nullptr) override;
    RegistryResult<KeyInfo> QueryInfo(const RegistryLocation&) override;
    RegistryResult<RegistryValue> Read(const RegistryLocation&, const QString& Name) override;
    quint32 Create(const RegistryLocation&) override;
    quint32 CreateLink(const RegistryLocation&, const QByteArray& Target) override;
    quint32 Write(const RegistryLocation&, const RegistryValue&) override;
    quint32 DeleteValue(const RegistryLocation&, const QString& Name) override;
    quint32 DeleteKey(const RegistryLocation&) override;
    quint32 RenameKey(const RegistryLocation&, const QString& Name) override;
    static RegistryResult<KeyHandle> Open(const RegistryLocation&, REGSAM Access, bool FollowLink = false);
    static bool ValidName(const QString& Name, bool Key);
};
} // namespace hc::registry
