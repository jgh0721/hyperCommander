// One decoder pass, at most two 256 KiB packets, no materialized contents.
// Producer and consumer own separate streams. Consumer cancellation stops the
// decoder; a bad CRC never publishes EOF and therefore never commits a target.
class StreamingRead final : public IArchiveReadSession {
    struct Packet { UInt32 ordinal_; std::vector<std::byte> bytes_; bool eof_; std::size_t offset_ = 0; };
    struct Prompt { std::string subject_; unsigned attempt_; std::promise<Result<storage::Password>> answer_; };
    class Callback final : public IArchiveExtractCallback, public ICryptoGetTextPassword {
    public:
        Callback(StreamingRead& Owner, Archive& Value) : owner_(Owner), archive_(Value) {}
        ~Callback() { if (output_) output_->Release(); }
        HC_COM_REFS;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID Id, void** Value) noexcept override {
            *Value=nullptr;
            if (Id==IID_IUnknown || Id==IID_IArchiveExtractCallback || Id==IID_IProgress) *Value=static_cast<IArchiveExtractCallback*>(this);
            else if (Id==IID_ICryptoGetTextPassword) *Value=static_cast<ICryptoGetTextPassword*>(this);
            if (!*Value) return E_NOINTERFACE; AddRef(); return S_OK;
        }
        HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept override { return archive_.state_->Check(); }
        HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* Done) noexcept override { return archive_.state_->Check(Done?*Done:0); }
        HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* Value) noexcept override { return archive_.state_->Password(Value); }
        HRESULT STDMETHODCALLTYPE GetStream(UInt32 Index,ISequentialOutStream** Stream,Int32 Mode) noexcept override {
            *Stream=nullptr;
            try {
                if (output_) { output_->Release(); output_=nullptr; }
                if (Mode!=NArchive::NExtract::NAskMode::kExtract) return archive_.state_->Check();
                auto Header=archive_.Header(Index); if (!Header.HasValue()) return archive_.state_->Fail(Header.Failure());
                if (Header.Value().directory_ || (Header.Value().attributes_&FILE_ATTRIBUTE_REPARSE_POINT)) return archive_.state_->Fail(Failure("7zip-stream-special-entry",ErrorCategory::Unsupported));
                ordinal_=Index; size_=Header.Value().size_;
                output_=new Output(nullptr,size_,archive_.state_,false,[this](std::span<const std::byte> Bytes) { return owner_.push(ordinal_,Bytes,false); });
                output_->AddRef(); *Stream=output_; return archive_.state_->Check();
            } catch (...) { return archive_.state_->Fail(Failure("7zip-stream-callback")); }
        }
        HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) noexcept override { return archive_.state_->Check(); }
        HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 Code) noexcept override {
            if (Code) return archive_.state_->Fail(Failure("7zip-extract-result:"+std::to_string(Code),Code==NArchive::NExtract::NOperationResult::kWrongPassword?ErrorCategory::AccessDenied:ErrorCategory::Unavailable));
            if (!output_) return archive_.state_->Check();
            if (output_->written_!=size_) return archive_.state_->Fail(Failure("7zip-stream-output-size"));
            try { auto Done=owner_.push(ordinal_,{},true); return Done.HasValue()?archive_.state_->Check():archive_.state_->Fail(Done.Failure()); }
            catch (...) { return archive_.state_->Fail(Failure("7zip-stream-callback")); }
        }
    private:
        StreamingRead& owner_; Archive& archive_; Output* output_=nullptr; UInt32 ordinal_=0; std::uint64_t size_=0;
    };
public:
    StreamingRead(HMODULE Module,std::string Path,std::vector<UInt32> Ordinals,OperationContext Context)
        : module_(Module),path_(std::move(Path)),ordinals_(std::move(Ordinals)),context_(std::move(Context)) {
        HMODULE Pinned=nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(Module),&Pinned)) module_=Pinned;
        else module_=nullptr;
        context_.passwordPrompt_={}; context_.interactionPrompt_={}; context_.interactionStart_={};
        context_.progress_=[this](std::uint64_t Done) { auto Before=progress_.load(); while (Done>Before && !progress_.compare_exchange_weak(Before,Done)) {} };
        context_.stopToken_=stop_.get_token();
    }
    ~StreamingRead() { stop_.request_stop(); changed_.notify_all(); if (worker_.joinable()) worker_.join(); if (module_) FreeLibrary(module_); }
    Result<ReadChunk> Read(UInt32 Ordinal,std::size_t Count,OperationContext Context) override {
        if (!Count || !Context.maxReadBytes_ || !std::binary_search(ordinals_.begin(),ordinals_.end(),Ordinal) || (last_ && Ordinal<*last_)) return Failure("7zip-stream-read-order",ErrorCategory::InvalidRequest);
        last_=Ordinal;
        std::stop_callback Cancel(Context.stopToken_,[this]{stop_.request_stop();changed_.notify_all();});
        std::unique_lock Lock(mutex_);
        if (!worker_.joinable()) worker_=std::thread([this]{decode();});
        ReadChunk Collected; const auto Target=std::min(Count,Context.maxReadBytes_);
        for (;;) {
            if (const auto Stop=Context.Check({"7zip"})) { stop_.request_stop(); changed_.notify_all(); return *Stop; }
            if (error_) return *error_;
            if (prompt_) {
                auto Query=std::move(prompt_); Lock.unlock();
                Result<storage::Password> Answer=Failure("archive-password-required",ErrorCategory::AccessDenied);
                try { if (Context.passwordPrompt_) Answer=Context.passwordPrompt_(Query->subject_,Query->attempt_,Context); } catch (...) { Answer=Failure("archive-password-callback"); }
                Query->answer_.set_value(std::move(Answer)); Lock.lock(); continue;
            }
            if (!packets_.empty()) {
                auto& Packet=packets_.front();
                // A conflict skip is a forward jump, not a reason to spool bytes.
                if (Packet.ordinal_<Ordinal) { packets_.pop_front(); changed_.notify_all(); continue; }
                if (Packet.ordinal_!=Ordinal) return Failure("7zip-stream-read-order",ErrorCategory::InvalidRequest);
                if (Packet.eof_ && Ordinal==ordinals_.back() && !done_) { changed_.wait_for(Lock,std::chrono::milliseconds(20)); continue; }
                const auto Wanted=std::min(Target-Collected.bytes_.size(),Packet.bytes_.size()-Packet.offset_);
                Collected.bytes_.insert(Collected.bytes_.end(),Packet.bytes_.begin()+Packet.offset_,Packet.bytes_.begin()+Packet.offset_+Wanted); Packet.offset_+=Wanted;
                Collected.eof_=Packet.eof_;
                if (Packet.offset_==Packet.bytes_.size()) { packets_.pop_front(); changed_.notify_all(); }
                if (Collected.eof_ || Collected.bytes_.size()==Target) return Collected;
                continue;
            }
            if (done_) return Failure("7zip-stream-incomplete",ErrorCategory::StaleRevision);
            if (Context.progress_) { Lock.unlock(); Context.progress_(progress_.load()); Lock.lock(); }
            changed_.wait_for(Lock,std::chrono::milliseconds(20));
        }
    }
private:
    Result<Unit> push(UInt32 Ordinal,std::span<const std::byte> Bytes,bool Eof) {
        do {
            const auto Count=std::min<std::size_t>(Bytes.size(),256*1024);
            std::unique_lock Lock(mutex_);
            while (packets_.size()>=2) { if (const auto Stop=context_.Check({"7zip"})) return *Stop; changed_.wait_for(Lock,std::chrono::milliseconds(20)); }
            if (const auto Stop=context_.Check({"7zip"})) return *Stop;
            packets_.push_back({Ordinal,{Bytes.begin(),Bytes.begin()+Count},Eof}); changed_.notify_all(); Bytes=Bytes.subspan(Count);
        } while (!Bytes.empty());
        return Unit{};
    }
    Result<storage::Password> password(const std::string& Subject,unsigned Attempt,const OperationContext& Context) {
        auto Query=std::make_unique<Prompt>(); Query->subject_=Subject; Query->attempt_=Attempt; auto Future=Query->answer_.get_future();
        { std::lock_guard Lock(mutex_); prompt_=std::move(Query); changed_.notify_all(); }
        while (Future.wait_for(std::chrono::milliseconds(20))!=std::future_status::ready) if (const auto Stop=Context.Check({"7zip"})) return *Stop;
        return Future.get();
    }
    void decode() noexcept {
        Result<Unit> Result=Failure("7zip-stream-exception");
        try {
            context_.passwordPrompt_=[this](const std::string& Subject,unsigned Attempt,const OperationContext& Context){return password(Subject,Attempt,Context);};
            auto Opened=module_?Open(module_,path_,context_):hc::storage::Result<std::unique_ptr<Archive>>(Failure("7zip-stream-module"));
            if (!Opened.HasValue()) Result=Opened.Failure();
            else {
                auto& Value=*Opened.Value();
                if (ordinals_.back()>=Value.count_) Result=Failure("7zip-stream-indices",ErrorCategory::InvalidRequest);
                else {
                    Com<Callback> Output{new Callback(*this,Value)};
                    const auto Code=Value.archive_->Extract(ordinals_.data(),UInt32(ordinals_.size()),0,Output.value_);
                    Result=Code==S_OK && !Value.state_->error_?hc::storage::Result<Unit>(Unit{}):hc::storage::Result<Unit>(Value.state_->Result(Code));
                }
            }
        } catch (...) {}
        std::lock_guard Lock(mutex_); if (!Result.HasValue()) error_=Result.Failure(); done_=true; changed_.notify_all();
    }
    HMODULE module_; std::string path_; std::vector<UInt32> ordinals_; OperationContext context_; std::stop_source stop_;
    std::mutex mutex_; std::condition_variable changed_; std::deque<Packet> packets_; std::unique_ptr<Prompt> prompt_;
    std::thread worker_; std::optional<Error> error_; std::optional<UInt32> last_; bool done_=false; std::atomic<std::uint64_t> progress_{0};
};
