#include "hwpRuntimeSha256.hpp"
#include <QCryptographicHash>
#include <array>
#include <chrono>
#include <vector>
#include <new>
#include <optional>

namespace hc::document
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        double elapsed(Clock::time_point Start)
        { return std::chrono::duration<double, std::milli>(Clock::now()-Start).count(); }
        struct FileHash final
        {
            const RuntimeSha256Api& api_;
            BCRYPT_HASH_HANDLE handle_ = nullptr;
            std::vector<unsigned char> object_;
            RuntimeHashMetrics* metrics_ = nullptr;
            bool Close()
            {
                if(!handle_ && object_.empty()) return true;
                const auto Start = metrics_ ? Clock::now() : Clock::time_point{};
                bool Ok = true;
                if(handle_) { const auto Value=handle_;handle_=nullptr;Ok=api_.destroy_(Value)==0; }
                // Include backing storage release in the diagnostic cleanup cost.
                std::vector<unsigned char>().swap(object_);
                if(metrics_) metrics_->fileCleanupMs_ += elapsed(Start);
                return Ok;
            }
            ~FileHash() { Close(); }
        };
    }
    const RuntimeSha256Api& RuntimeSha256Api::System()
    { static const RuntimeSha256Api Api; return Api; }
    struct HwpRuntimeSha256::State
    {
        const RuntimeSha256Api& api_;
        BCRYPT_ALG_HANDLE provider_ = nullptr;
        DWORD objectBytes_ = 0;
        bool cng_ = false, measure_ = false, cleanupOk_ = true;
        RuntimeHashMetrics metrics_;
        explicit State(const RuntimeSha256Api& Api, bool Measure) : api_(Api), measure_(Measure) {}
        bool close()
        {
            const auto Start = measure_ ? Clock::now() : Clock::time_point{};
            if(provider_) { cleanupOk_ = api_.close_(provider_,0)==0 && cleanupOk_; provider_ = nullptr; }
            cng_ = false; objectBytes_ = 0;
            if(measure_) metrics_.providerCleanupMs_ += elapsed(Start);
            return cleanupOk_;
        }
        ~State() { if(provider_) close(); }
    };
    HwpRuntimeSha256::HwpRuntimeSha256(RuntimeHashBackend Backend, const RuntimeSha256Api& Api, bool Measure)
        : state_(std::make_unique<State>(Api,Measure))
    {
        auto& S = *state_;
        const auto Start = Measure ? Clock::now() : Clock::time_point{};
        if(Backend == RuntimeHashBackend::PreferCng) {
            // A named Microsoft provider avoids loading a configurable third-party
            // implementation. An unavailable/invalid provider falls back before
            // ANY file bytes have been consumed, so Qt still hashes from offset 0.
            if(Api.open_(&S.provider_,BCRYPT_SHA256_ALGORITHM,MS_PRIMITIVE_PROVIDER,0)==0) {
                DWORD Copied=0,HashBytes=0;
                if(Api.property_(S.provider_,BCRYPT_HASH_LENGTH,reinterpret_cast<PUCHAR>(&HashBytes),sizeof(HashBytes),&Copied,0)==0
                    && Copied==sizeof(HashBytes) && HashBytes==32
                    && Api.property_(S.provider_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&S.objectBytes_),sizeof(S.objectBytes_),&Copied,0)==0
                    && Copied==sizeof(S.objectBytes_) && S.objectBytes_>0 && S.objectBytes_<=1024*1024)
                    S.cng_ = true;
            }
            if(!S.cng_ && S.provider_) S.close();
        }
        if(Measure) S.metrics_.providerInitMs_ += elapsed(Start);
    }
    HwpRuntimeSha256::~HwpRuntimeSha256() = default;
    bool HwpRuntimeSha256::UsesCng() const { return state_->cng_; }
    bool HwpRuntimeSha256::Close() { return state_->close(); }
    RuntimeHashMetrics HwpRuntimeSha256::Metrics() const { return state_->metrics_; }
    RuntimeHashResult HwpRuntimeSha256::Read(HANDLE File, qint64 ExpectedBytes, QByteArray& Hex,
        const std::function<bool()>& Cancelled)
    {
        Hex.clear(); auto& S = *state_;
        if(ExpectedBytes<0 || ExpectedBytes>512LL*1024*1024) return RuntimeHashResult::LengthMismatch;
        if(Cancelled && Cancelled()) return RuntimeHashResult::Cancelled;
        FileHash Native{S.api_,nullptr,{},S.measure_?&S.metrics_:nullptr};
        std::optional<QCryptographicHash> Legacy;
        auto Start = S.measure_?Clock::now():Clock::time_point{};
        if(S.cng_) {
            try { Native.object_.resize(S.objectBytes_); }
            catch(const std::bad_alloc&) { return RuntimeHashResult::HashFailed; }
            if(S.api_.create_(S.provider_,&Native.handle_,Native.object_.data(),S.objectBytes_,nullptr,0,0)!=0)
                return RuntimeHashResult::HashFailed;
        } else {
            try { Legacy.emplace(QCryptographicHash::Sha256); }
            catch(const std::bad_alloc&) { return RuntimeHashResult::HashFailed; }
        }
        if(S.measure_) S.metrics_.fileInitMs_ += elapsed(Start);
        char Buffer[65536]; qint64 Total=0; DWORD Count=0;
        for(;;) {
            if(Cancelled && Cancelled()) return RuntimeHashResult::Cancelled;
            Start = S.measure_?Clock::now():Clock::time_point{};
            const bool Ok = S.api_.read_(File,Buffer,sizeof(Buffer),&Count,nullptr)!=FALSE;
            if(S.measure_) S.metrics_.readMs_ += elapsed(Start);
            if(!Ok) return RuntimeHashResult::ReadFailed;
            if(Count>sizeof(Buffer) || Total+Count>ExpectedBytes) return RuntimeHashResult::LengthMismatch;
            if(!Count) break;
            Total += Count;
            Start = S.measure_?Clock::now():Clock::time_point{};
            if(S.cng_) {
                if(S.api_.add_(Native.handle_,reinterpret_cast<PUCHAR>(Buffer),Count,0)!=0)
                    return RuntimeHashResult::HashFailed;
            } else Legacy->addData(QByteArrayView(Buffer,Count));
            if(S.measure_) { S.metrics_.updateMs_ += elapsed(Start); S.metrics_.bytes_ += Count; }
        }
        if(Total!=ExpectedBytes) return RuntimeHashResult::LengthMismatch;
        if(Cancelled && Cancelled()) return RuntimeHashResult::Cancelled;
        Start = S.measure_?Clock::now():Clock::time_point{};
        if(S.cng_) {
            std::array<unsigned char,32> Digest{};
            if(S.api_.finish_(Native.handle_,Digest.data(),static_cast<ULONG>(Digest.size()),0)!=0)
                return RuntimeHashResult::HashFailed;
            Hex = QByteArray(reinterpret_cast<const char*>(Digest.data()),32).toHex();
        } else Hex = Legacy->result().toHex();
        if(S.measure_) S.metrics_.finishMs_ += elapsed(Start);
        // The unique Qt state is also destroyed before returning to the pass.
        Start = S.measure_?Clock::now():Clock::time_point{};
        Legacy.reset();
        if(S.measure_) S.metrics_.fileCleanupMs_ += elapsed(Start);
        if(!Native.Close()) { Hex.clear();return RuntimeHashResult::HashFailed; }
        return RuntimeHashResult::Success;
    }
}
