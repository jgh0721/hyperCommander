#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <bcrypt.h>
#include <QByteArray>
#include <functional>
#include <memory>

namespace hc::document
{
    // Native-only dependency seam for deterministic failure tests. Product callers
    // always use System(); no environment/IPC input can select these functions.
    struct RuntimeSha256Api
    {
        decltype(&BCryptOpenAlgorithmProvider) open_ = BCryptOpenAlgorithmProvider;
        decltype(&BCryptGetProperty) property_ = BCryptGetProperty;
        decltype(&BCryptCreateHash) create_ = BCryptCreateHash;
        decltype(&BCryptHashData) add_ = BCryptHashData;
        decltype(&BCryptFinishHash) finish_ = BCryptFinishHash;
        decltype(&BCryptDestroyHash) destroy_ = BCryptDestroyHash;
        decltype(&BCryptCloseAlgorithmProvider) close_ = BCryptCloseAlgorithmProvider;
        decltype(&ReadFile) read_ = ReadFile;
        static const RuntimeSha256Api& System();
    };
    enum class RuntimeHashResult { Success, ReadFailed, LengthMismatch, HashFailed, Cancelled };
    enum class RuntimeHashBackend { PreferCng, QtOnly };
    struct RuntimeHashMetrics
    {
        double providerInitMs_ = 0, providerCleanupMs_ = 0, fileInitMs_ = 0;
        double readMs_ = 0, updateMs_ = 0, finishMs_ = 0, fileCleanupMs_ = 0;
        qint64 bytes_ = 0;
    };
    // One provider per verification pass, one hash state per file. No byte/hash
    // result cache. The existing owner retains file leases through Job retirement.
    class HwpRuntimeSha256 final
    {
    public:
        explicit HwpRuntimeSha256(RuntimeHashBackend Backend = RuntimeHashBackend::PreferCng,
            const RuntimeSha256Api& Api = RuntimeSha256Api::System(), bool Measure = false);
        ~HwpRuntimeSha256();
        HwpRuntimeSha256(const HwpRuntimeSha256&) = delete;
        HwpRuntimeSha256& operator=(const HwpRuntimeSha256&) = delete;
        RuntimeHashResult Read(HANDLE File, qint64 ExpectedBytes, QByteArray& Hex,
            const std::function<bool()>& Cancelled = {});
        bool UsesCng() const;
        bool Close();
        RuntimeHashMetrics Metrics() const;
    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
