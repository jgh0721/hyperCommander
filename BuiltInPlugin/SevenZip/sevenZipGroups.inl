// The ZIP SDK has one encryption setting per UpdateItems call. Each pass adds
// one cipher group and copies every preceding entry unchanged. All passes stay
// inside the same private transaction directory and share cancellation/budgets.
Result<Unit> WriteZipGroups(HMODULE Module, IOutArchive* Initial, Archive* Original,
    std::vector<UpdateItem>& Items, const QString& Path, const QString& Method,
    unsigned Level, uint64_t Limit, std::shared_ptr<State> Shared) {
    std::map<QString, std::vector<UpdateItem>> Groups;
    for (const auto& Item : Items) if (Item.old_ == UINT32_MAX) Groups[Item.zipEncryption_].push_back(Item);
    std::unique_ptr<Archive> Current;
    QString Previous; unsigned Number = 0;
    auto Context = Shared->context_;
    if (Shared->password_) Context.passwordPrompt_ = [Shared](const std::string&, unsigned, const OperationContext&) -> Result<storage::Password> { return storage::Password(Shared->password_->Value()); };
    for (auto& [Cipher, Added] : Groups) {
        if (const auto Stop = Context.Check({"7zip"})) return *Stop;
        std::vector<UpdateItem> Pass;
        if (Current) {
            for (UInt32 I = 0; I < Current->count_; ++I) {
                auto H = Current->Header(I); if (!H.HasValue()) return H.Failure();
                Pass.push_back({I, EntryKey(QString::fromStdString(H.Value().name_)), {}, H.Value().directory_, H.Value().size_});
            }
        } else for (const auto& Item : Items) if (Item.old_ != UINT32_MAX) Pass.push_back(Item);
        Pass.insert(Pass.end(), Added.begin(), Added.end());
        const auto OutputPath = Path + QString(".cipher-%1.zip").arg(Number++);
        {
            Com<IOutArchive> Writer;
            if (Current) { if (Current->archive_->QueryInterface(IID_IOutArchive, reinterpret_cast<void**>(&Writer.value_)) != S_OK) return Failure("zip-group-writer"); }
            else { Initial->AddRef(); Writer.value_ = Initial; }
            Com<ISetProperties> Properties; if (Writer->QueryInterface(IID_ISetProperties, reinterpret_cast<void**>(&Properties.value_)) != S_OK) return Failure("zip-group-properties");
            std::vector<std::wstring> Names{L"x", L"cu", L"tc", L"ta"}, Strings(4);
            if (!Method.isEmpty()) { Names.push_back(L"m"); Strings.push_back(Method.toStdWString()); }
            if (!Cipher.isEmpty()) { Names.push_back(L"em"); Strings.push_back(Cipher.toStdWString()); }
            std::vector<const wchar_t*> Keys; std::vector<PROPVARIANT> Values(Names.size());
            for (size_t I = 0; I < Names.size(); ++I) {
                Keys.push_back(Names[I].c_str()); PropVariantInit(&Values[I]);
                if (!I) { Values[I].vt = VT_UI4; Values[I].ulVal = Level; }
                else if (I < 4) { Values[I].vt = VT_BOOL; Values[I].boolVal = VARIANT_TRUE; }
                else { Values[I].vt = VT_BSTR; Values[I].bstrVal = SysAllocString(Strings[I].c_str()); }
            }
            const auto Set = Properties->SetProperties(Keys.data(), Values.data(), UInt32(Values.size())); for (auto& V : Values) PropVariantClear(&V); if (Set != S_OK) return Failure("zip-group-settings");
            QFile Output(OutputPath); if (!Output.open(QIODevice::ReadWrite | QIODevice::NewOnly)) return Failure("zip-group-open");
            Com<UpdateOutput> Stream{new UpdateOutput(&Output, Limit, Shared)};
            Com<UpdateCallback> Callback{new UpdateCallback(Pass, Shared, !Cipher.isEmpty(), true, Original ? Original->archive_.value_ : nullptr)};
            const auto Code = Writer->UpdateItems(Stream.value_, UInt32(Pass.size()), Callback.value_);
            if (Code != S_OK || Shared->error_) return Shared->Result(Code);
            if (!Output.flush()) return Failure("zip-group-flush");
        }
        Current.reset(); Previous = OutputPath;
        if (Number < Groups.size()) {
            auto Opened = Open(Module, Previous.toStdString(), Context); if (!Opened.HasValue()) return Opened.Failure(); Current = std::move(Opened.Value());
        }
    }
    if (!MoveFileExW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Previous).utf16()), reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(Path).utf16()), MOVEFILE_WRITE_THROUGH)) return Failure("zip-group-staging-publish");
    return Unit{};
}
