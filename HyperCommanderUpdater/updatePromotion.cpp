#include "stdafx.h"
#include "updatePromotion.hpp"

/// 기타 헤더파일
#include "updateOwnershipPolicy.hpp"
#include "updateTransaction.hpp"

/// 고유 라이브러리
#include "commonLibs/layout/productLayout.hpp"

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <algorithm>

namespace hc::update {
namespace {
struct Failure : std::runtime_error {
    Failure(const char* Code, const Path& File, DWORD Error)
        : std::runtime_error(std::string(Code) + ": " + Utf8(File.wstring()) + " (win32=" + std::to_string(Error) + ")"), win32_(Error) {}
    DWORD win32_;
};
[[noreturn]] void Fail(const char* Code, const Path& File) { throw Failure(Code, File, GetLastError()); }
struct Handle {
    HANDLE value_ = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
};
struct Carried { std::string name_, relative_, hash_; std::uint64_t size_ = 0; Path carrier_, target_; };
// Debug builds are unsigned; their inventory hash alone ties a copy to the build.
void VerifyCarried(const Path& File, const Carried& Item, std::string_view Relative) {
#ifdef _DEBUG
    CheckRelative(Relative);
    if (std::filesystem::file_size(File) != Item.size_ || HashFile(File) != Item.hash_) throw std::runtime_error("update-file-hash: " + std::string(Relative));
#else
    VerifyProductFile(File, Relative, Item.hash_, Item.size_);
#endif
}
bool ProductSigned(const Path& File, std::string_view Name) {
#ifdef _DEBUG
    // A differing copy in a Debug build folder is a fresh build of the program: keep it.
    return false;
#else
    try { VerifyProductSignature(File, Name); return true; } catch (const std::exception&) { return false; }
#endif
}
bool Elevated() {
    Handle Token; TOKEN_ELEVATION Elevation{}; DWORD Size = 0;
    return OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token.value_)
        && GetTokenInformation(Token.value_, TokenElevation, &Elevation, sizeof(Elevation), &Size) && Elevation.TokenIsElevated;
}
// Instances of one installation take turns; other installations do not wait.
std::wstring MutexName(const Path& Root) {
    auto Text = Root.lexically_normal().wstring(); CharLowerBuffW(Text.data(), DWORD(Text.size()));
    return L"Local\\HyperCommander.RootPromotion." + Wide(HashBytes(Utf8(Text)).substr(0, 32));
}
// The copy is verified again where it lands, so a carrier changed after the first
// check never reaches ROOT. The rename is atomic on NTFS and ReFS.
void Place(const Carried& Item) {
    const auto Temp = Path(Item.target_.wstring() + L".hc-promote"); CheckPlainPath(Temp);
    if (std::filesystem::exists(Temp) && !Retried([&] { return DeleteFileW(Temp.c_str()) != FALSE; })) Fail("update-promote-temp", Temp);
    // A read-only folder fails at once instead of after the transient-lock retries.
    { Handle Probe; Probe.value_ = CreateFileW(Temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (Probe.value_ == INVALID_HANDLE_VALUE) Fail("update-promote-access", Temp); }
    try {
        if (!Retried([&] { return CopyFileW(Item.carrier_.c_str(), Temp.c_str(), FALSE) != FALSE; })) Fail("update-promote-copy", Temp);
        VerifyCarried(Temp, Item, Item.name_);
        { Handle Flush; Flush.value_ = CreateFileW(Temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
          if (Flush.value_ == INVALID_HANDLE_VALUE || !FlushFileBuffers(Flush.value_)) Fail("update-promote-flush", Temp); }
        if (!Retried([&] { return MoveFileExW(Temp.c_str(), Item.target_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE; }))
            Fail("update-promote-move", Item.target_);
    } catch (...) { DeleteFileW(Temp.c_str()); throw; }
}
}
const char* PromotionStatusName(PromotionStatus Status) {
    switch (Status) {
    case PromotionStatus::Done: return "done";
    case PromotionStatus::Pending: return "pending";
    case PromotionStatus::Busy: return "busy";
    case PromotionStatus::Foreign: return "foreign";
    case PromotionStatus::Failed: return "failed";
    }
    return "unknown";
}
PromotionResult PromoteRootFiles(const Path& InputRoot) {
    PromotionResult Result;
    const auto Stop = [&](PromotionStatus Status) { Result.status_ = Status; return Result; };
    try {
        // No install-root check: manual ZIP installations on FAT, exFAT, network or
        // elevated runs also need HyperCompare. Links are still refused.
        const auto Root = InstallRoot(InputRoot);
        if (!Root.is_absolute()) throw std::runtime_error("update-install-root");
        CheckPlainPath(Root);
        Handle Mutex; Mutex.value_ = CreateMutexW(nullptr, FALSE, MutexName(Root).c_str());
        // An elevated instance's mutex refuses this token; it is promoting right now.
        if (!Mutex.value_) return Stop(PromotionStatus::Busy);
        const auto Waited = WaitForSingleObject(Mutex.value_, 15000);
        if (Waited != WAIT_OBJECT_0 && Waited != WAIT_ABANDONED) return Stop(PromotionStatus::Busy);
        struct Release { HANDLE value_; ~Release() { ReleaseMutex(value_); } } Unlock{Mutex.value_};

        const auto Inventory = ParseJson(ReadBounded(Root / L"product-inventory.json", 1048576));
        const auto& Files = Inventory.At("files", Json::Kind::Array).array_;
        std::vector<Carried> Bundle;
        for (const auto Known : policy::CarriedRootFiles) {
            const std::string Name(Known), Relative = std::string(layout::RootCarrier) + '/' + Name;
            const auto Listed = std::ranges::find_if(Files, [&](const Json& Entry) { return Entry.At("path", Json::Kind::String).text_ == Relative; });
            if (Listed == Files.end()) continue;
            Bundle.push_back({Name, Relative, Listed->At("sha256", Json::Kind::String).text_, Listed->At("size", Json::Kind::Number).number_,
                Root / Wide(Relative), Root / Wide(Name)});
        }
        // The contract lists each program before its worker. Workers change first, so a
        // program that starts meanwhile never finds a worker older than itself.
        std::ranges::reverse(Bundle);
        std::vector<const Carried*> Stale;
        for (const auto& Item : Bundle) {
            CheckPlainPath(Item.target_);
            if (!std::filesystem::exists(Item.target_) || HashFile(Item.target_) != Item.hash_) Stale.push_back(&Item);
        }
        if (Stale.empty()) return Result;
        for (const auto* Item : Stale)
            if (std::filesystem::exists(Item->target_) && !ProductSigned(Item->target_, Item->name_)) return Stop(PromotionStatus::Foreign);
        for (const auto& Item : Bundle)
            if (ImageRunning(Item.target_)) return Stop(PromotionStatus::Busy);
        for (const auto* Item : Stale) VerifyCarried(Item->carrier_, *Item, Item->relative_);
        const auto PlaceAll = [&] { for (const auto* Item : Stale) Place(*Item); };
        if (std::filesystem::exists(Root / L".update/active.json")) {
            // An elevated process never changes a transaction (update-contract.json elevatedInstall).
            if (Elevated()) return Stop(PromotionStatus::Pending);
            std::vector<RootCopy> Copies;
            for (const auto* Item : Stale) Copies.push_back({Item->name_, Item->hash_});
            try { if (!PlaceWithinTransaction(Root, Copies, PlaceAll)) PlaceAll(); }
            catch (const Failure&) { throw; }
            catch (const std::runtime_error& Error) {
                const std::string_view Code(Error.what());
                if (Code.starts_with("update-promotion-pending") || Code.starts_with("update-root-locked")) return Stop(PromotionStatus::Pending);
                throw;
            }
        } else PlaceAll();
        for (const auto* Item : Stale) Result.promoted_.push_back(Item->name_);
        return Result;
    } catch (const Failure& Error) {
        Result.status_ = PromotionStatus::Failed; Result.error_ = Error.what(); Result.win32_ = Error.win32_;
    } catch (const std::exception& Error) {
        Result.status_ = PromotionStatus::Failed; Result.error_ = Error.what();
    } catch (...) {
        Result.status_ = PromotionStatus::Failed; Result.error_ = "update-promote-failed";
    }
    return Result;
}
RetireResult RetireCarriers(const Path& InputRoot) {
    RetireResult Result;
    try {
        const auto Root = InstallRoot(InputRoot);
        if (!Root.is_absolute()) throw std::runtime_error("update-install-root");
        CheckPlainPath(Root);
        // The same lock as promotion: another instance may be copying from these carriers.
        Handle Mutex; Mutex.value_ = CreateMutexW(nullptr, FALSE, MutexName(Root).c_str());
        if (!Mutex.value_) return Result;
        const auto Waited = WaitForSingleObject(Mutex.value_, 15000);
        if (Waited != WAIT_OBJECT_0 && Waited != WAIT_ABANDONED) return Result;
        struct Release { HANDLE value_; ~Release() { ReleaseMutex(value_); } } Unlock{Mutex.value_};
        // The health check of an applied update verifies every inventory file, carriers included.
        if (std::filesystem::exists(Root / L".update/active.json")) return Result;
        const auto Inventory = ParseJson(ReadBounded(Root / L"product-inventory.json", 1048576));
        const auto& Files = Inventory.At("files", Json::Kind::Array).array_;
        struct Listed { std::string relative_, hash_; };
        std::vector<Listed> Carriers;
        for (const auto Known : policy::CarriedRootFiles) {
            const std::string Name(Known), Relative = std::string(layout::RootCarrier) + '/' + Name;
            const auto Entry = std::ranges::find_if(Files, [&](const Json& Item) { return Item.At("path", Json::Kind::String).text_ == Relative; });
            if (Entry == Files.end()) continue;
            const auto Hash = Entry->At("sha256", Json::Kind::String).text_;
            const auto Target = Root / Wide(Name); CheckPlainPath(Target);
            // A carrier is the only source of its ROOT copy until that copy is current.
            if (!std::filesystem::exists(Target) || HashFile(Target) != Hash) return Result;
            Carriers.push_back({Relative, Hash});
        }
        for (const auto& Item : Carriers) {
            const auto Carrier = Root / Wide(Item.relative_); CheckPlainPath(Carrier);
            if (!std::filesystem::exists(Carrier) || HashFile(Carrier) != Item.hash_) continue;
            // A scanner holding the file briefly is waited for; a lasting lock leaves it for the next start.
            if (Retried([&] { return DeleteFileW(Carrier.c_str()) != FALSE; })) Result.removed_.push_back(Item.relative_);
        }
        std::error_code Ignored;
        const auto Folder = Root / Wide(std::string(layout::RootCarrier));
        if (std::filesystem::is_directory(Folder, Ignored) && std::filesystem::is_empty(Folder, Ignored)) std::filesystem::remove(Folder, Ignored);
        Result.retired_ = true;
    } catch (const std::exception& Error) {
        Result.error_ = Error.what();
    } catch (...) {
        Result.error_ = "update-retire-failed";
    }
    return Result;
}
}
