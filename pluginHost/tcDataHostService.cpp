#include "tcDataHostService.hpp"
#include <QDir>
#include <QJsonArray>
#include <QFileInfo>
#include <QDate>
#include <QTime>
#include <array>
#include <cstring>
#include <cmath>

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        // ABI declarations derived from the official WFX 2.1 / WDX 2.12
        // contracts. These types are private to the process boundary.
        struct Defaults { int size_; DWORD low_, high_; char ini_[MAX_PATH]; };
        struct RemoteInfo { DWORD low_, high_; FILETIME modified_; int attributes_; };
        template<class T> T Export(HMODULE Module, const char* Name)
        { return reinterpret_cast<T>(GetProcAddress(Module, Name)); }
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"tc-data"}}; }
        Error Unsupported(const char* Code) { return {ErrorCategory::Unsupported, Code, {"tc-data"}}; }
        bool Integer(QJsonValue Value, int Minimum, int Maximum)
        { return Value.isDouble() && Value.toDouble() == Value.toInt(Minimum - 1) && Value.toInt() >= Minimum && Value.toInt() <= Maximum; }
        bool Path(QString Value, bool Remote)
        {
            if (Value.isEmpty() || Value.size() > 1023 || Value.contains(QChar::Null)) return false;
            if (!Remote) return QDir::isAbsolutePath(Value);
            if (!Value.startsWith('\\') || Value.startsWith("\\\\") || Value.contains('/')) return false;
            for (const auto& Part : Value.split('\\')) if (Part == "." || Part == "..") return false;
            return true;
        }
        std::array<wchar_t, 1024> Buffer(const QString& Text)
        { std::array<wchar_t, 1024> Out{}; Text.toWCharArray(Out.data()); return Out; }
        QString Ansi(const char* Bytes, int Length)
        {
            const auto Size = MultiByteToWideChar(CP_ACP, 0, Bytes, Length, nullptr, 0);
            std::wstring Text(Size, L'\0'); MultiByteToWideChar(CP_ACP, 0, Bytes, Length, Text.data(), Size);
            return QString::fromStdWString(Text);
        }
        std::optional<QByteArray> ToAnsi(QString Text)
        {
            BOOL Substituted = FALSE; const bool Utf8 = GetACP() == CP_UTF8;
            const auto Wide = Text.toStdWString();
            const auto Size = WideCharToMultiByte(CP_ACP, Utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS,
                Wide.c_str(), -1, nullptr, 0, nullptr, Utf8 ? nullptr : &Substituted);
            if (!Size || Substituted) return {};
            QByteArray Bytes(Size, '\0');
            if (!WideCharToMultiByte(CP_ACP, Utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS,
                Wide.c_str(), -1, Bytes.data(), Size, nullptr, Utf8 ? nullptr : &Substituted) || Substituted) return {};
            return Bytes;
        }
        int __stdcall ProgressA(int, char*, char*, int) { return 0; }
        int __stdcall ProgressW(int, wchar_t*, wchar_t*, int) { return 0; }
        void __stdcall LogA(int, int, char*) {}
        void __stdcall LogW(int, int, wchar_t*) {}
        // A request for credentials or a dialog is cancelled explicitly. No
        // modal dialog can block the application's UI or expose saved secrets.
        BOOL __stdcall RequestA(int, int, char*, char*, char*, int) { return FALSE; }
        BOOL __stdcall RequestW(int, int, wchar_t*, wchar_t*, wchar_t*, int) { return FALSE; }
        std::uint64_t Ticks(FILETIME Value) { return (std::uint64_t(Value.dwHighDateTime) << 32) | Value.dwLowDateTime; }
        QJsonObject Entry(const WIN32_FIND_DATAW& Data)
        {
            const auto End = std::find(std::begin(Data.cFileName), std::end(Data.cFileName), L'\0');
            const auto Name = QString::fromWCharArray(Data.cFileName, End - std::begin(Data.cFileName));
            return {{"name", Name}, {"attributes", qint64(Data.dwFileAttributes)},
                {"size", QString::number((quint64(Data.nFileSizeHigh) << 32) | Data.nFileSizeLow)},
                {"created", QString::number(Ticks(Data.ftCreationTime))}, {"modified", QString::number(Ticks(Data.ftLastWriteTime))}};
        }
    }
    TcDataHostService::TcDataHostService(HMODULE Module, PluginManifest Manifest) : module_(Module), manifest_(std::move(Manifest)) {}
    TcDataHostService::~TcDataHostService()
    {
        if (const auto Close = Export<int(__stdcall*)(HANDLE)>(module_, "FsFindClose"))
            for (const auto& [Id, Cursor] : cursors_) Close(Cursor.handle_);
        if (manifest_.format_ == PluginFormat::Wdx)
            if (const auto Unload = Export<void(__stdcall*)()>(module_, "ContentPluginUnloading")) Unload();
    }
    Result<Unit> TcDataHostService::Initialize(const QString& Ini)
    {
        const bool Wfx = manifest_.format_ == PluginFormat::Wfx;
        if (const auto Set = Export<void(__stdcall*)(Defaults*)>(module_, Wfx ? "FsSetDefaultParams" : "ContentSetDefaultParams"))
        {
            auto Encoded = ToAnsi(QDir::toNativeSeparators(Ini));
            if (!Path(Ini, false) || !Encoded || Encoded->size() > MAX_PATH) return Unsupported("host-data-ini-encoding");
            if (!QDir().mkpath(QFileInfo(Ini).absolutePath())) return HostError("host-data-ini-directory");
            Defaults Parameters{}; Parameters.size_ = sizeof(Parameters); Parameters.high_ = 2; Parameters.low_ = Wfx ? 10 : 12;
            std::memcpy(Parameters.ini_, Encoded->constData(), Encoded->size()); Set(&Parameters);
        }
        if (Wfx)
        {
            const auto InitA = Export<int(__stdcall*)(int, decltype(&ProgressA), decltype(&LogA), decltype(&RequestA))>(module_, "FsInit");
            const auto InitW = Export<int(__stdcall*)(int, decltype(&ProgressW), decltype(&LogW), decltype(&RequestW))>(module_, "FsInitW");
            if ((!InitA && !InitW) || (InitA && InitA(1, ProgressA, LogA, RequestA) != 0)
                || (InitW && InitW(1, ProgressW, LogW, RequestW) != 0)) return Unsupported("host-wfx-init");
        }
        auto Probed = Probe(); if (!Probed.HasValue()) return Probed.Failure(); return Unit{};
    }
    Result<QJsonObject> TcDataHostService::Probe()
    {
        const bool Wfx = manifest_.format_ == PluginFormat::Wfx;
        for (auto Name : Wfx ? std::vector<const char*>{"FsFindFirstW", "FsFindNextW", "FsFindClose"}
                            : std::vector<const char*>{"ContentGetSupportedField"})
            if (!GetProcAddress(module_, Name)) return Unsupported("host-data-required-exports");
        if (!Wfx && !GetProcAddress(module_, "ContentGetValueW") && !GetProcAddress(module_, "ContentGetValue"))
            return Unsupported("host-wdx-value-export");
        QJsonArray Exports;
        for (auto Name : {"FsGetFileW", "FsPutFileW", "FsMkDirW", "FsDeleteFileW", "FsRemoveDirW", "FsRenMovFileW", "FsSetAttrW", "FsSetTimeW",
            "ContentGetValueW", "ContentGetValue", "ContentStopGetValueW", "ContentGetDefaultSortOrder", "ContentGetDetectString"})
            if (GetProcAddress(module_, Name)) Exports.append(Name);
        return QJsonObject{{"format", Wfx ? "wfx" : "wdx"}, {"exports", Exports}, {"unicode", Wfx || bool(GetProcAddress(module_, "ContentGetValueW"))}};
    }
    Result<QJsonObject> TcDataHostService::enumerate(const QJsonObject& Request)
    {
        const auto Action = Request["action"].toString();
        if (Action == "open")
        {
            if (Request.size() != 2 || !Path(Request["path"].toString(), true)) return Invalid("host-wfx-path");
            if (cursors_.size() >= 64 || nextCursor_ == INT_MAX) return Error{ErrorCategory::BudgetExceeded, "host-wfx-cursors", {"wfx"}};
            auto Name = Buffer(Request["path"].toString()); Cursor Value{};
            SetLastError(ERROR_SUCCESS);
            Value.handle_ = Export<HANDLE(__stdcall*)(wchar_t*, WIN32_FIND_DATAW*)>(module_, "FsFindFirstW")(Name.data(), &Value.next_);
            if (Value.handle_ == INVALID_HANDLE_VALUE || !Value.handle_)
            {
                const auto Code = GetLastError();
                if (Code != ERROR_SUCCESS && Code != ERROR_NO_MORE_FILES && Code != ERROR_FILE_NOT_FOUND) return HostError("host-wfx-find-first", Code);
                return QJsonObject{{"cursor", 0}, {"end", true}};
            }
            const int Id = nextCursor_++; cursors_.emplace(Id, Value); return QJsonObject{{"cursor", Id}, {"end", false}};
        }
        if (!Integer(Request["cursor"], 1, INT_MAX)) return Invalid("host-wfx-cursor");
        const auto At = cursors_.find(Request["cursor"].toInt()); if (At == cursors_.end()) return Invalid("host-wfx-cursor");
        auto& Cursor = At->second;
        if (Action == "close" && Request.size() == 2)
        { Export<int(__stdcall*)(HANDLE)>(module_, "FsFindClose")(Cursor.handle_); cursors_.erase(At); return QJsonObject{}; }
        if (Action != "next" || Request.size() != 3 || !Integer(Request["count"], 1, 256)) return Invalid("host-wfx-page");
        QJsonArray Entries; int Examined = 0;
        while (!Cursor.end_ && Entries.size() < Request["count"].toInt() && ++Examined <= 1024)
        {
            auto Item = Entry(Cursor.next_); const auto Name = Item["name"].toString();
            if (Name.size() >= MAX_PATH || Name.isEmpty() || Name.contains('\\') || Name.contains('/')) return Invalid("host-wfx-entry-name");
            if (Name != "." && Name != "..") Entries.append(Item);
            SetLastError(ERROR_SUCCESS);
            if (!Export<BOOL(__stdcall*)(HANDLE, WIN32_FIND_DATAW*)>(module_, "FsFindNextW")(Cursor.handle_, &Cursor.next_))
            {
                const auto Code = GetLastError(); Cursor.end_ = true;
                if (Code != ERROR_SUCCESS && Code != ERROR_NO_MORE_FILES) return HostError("host-wfx-find-next", Code);
            }
        }
        return QJsonObject{{"entries", Entries}, {"end", Cursor.end_}};
    }
    Result<QJsonObject> TcDataHostService::metadata(const QJsonObject& Request)
    {
        if (Request["action"] == "fields" && Request.size() == 1)
        {
            QJsonArray Fields;
            const auto Field = Export<int(__stdcall*)(int, char*, char*, int)>(module_, "ContentGetSupportedField");
            const auto Sort = Export<int(__stdcall*)(int)>(module_, "ContentGetDefaultSortOrder");
            for (int Index = 0; Index <= 256; ++Index)
            {
                std::array<char, 1024> Name{}, Units{};
                const auto Type = Field(Index, Name.data(), Units.data(), int(Name.size()));
                if (Type == 0) return QJsonObject{{"fields", Fields}};
                if (Index == 256) return Error{ErrorCategory::BudgetExceeded, "host-wdx-field-budget", {"wdx"}};
                if (!memchr(Name.data(), 0, Name.size()) || !memchr(Units.data(), 0, Units.size())) return Invalid("host-wdx-field-termination");
                // Comparison pseudo-fields do not describe a column value.
                if (Type == 100) continue;
                if (Type < 1 || Type > 12) return Unsupported("host-wdx-field-type");
                Fields.append(QJsonObject{{"index", Index}, {"name", Ansi(Name.data(), int(strlen(Name.data())))},
                    {"units", Ansi(Units.data(), int(strlen(Units.data())))}, {"type", Type}, {"descending", Sort && Sort(Index) < 0}});
            }
        }
        if (Request.size() != 5 || Request["action"] != "value" || !Path(Request["path"].toString(), false)
            || !Integer(Request["field"], 0, 255) || !Integer(Request["unit"], 0, INT_MAX) || !Request["defer"].isBool()) return Invalid("host-wdx-request");
        alignas(16) std::array<std::byte, 65536> Bytes{};
        auto Name = Buffer(QDir::toNativeSeparators(Request["path"].toString()));
        const auto Wide = Export<int(__stdcall*)(wchar_t*, int, int, void*, int, int)>(module_, "ContentGetValueW");
        int Type;
        if (Wide) Type = Wide(Name.data(), Request["field"].toInt(), Request["unit"].toInt(), Bytes.data(), int(Bytes.size()), Request["defer"].toBool() ? 1 : 0);
        else
        {
            auto Narrow = ToAnsi(QString::fromWCharArray(Name.data())); if (!Narrow) return Unsupported("host-wdx-path-encoding");
            Type = Export<int(__stdcall*)(char*, int, int, void*, int, int)>(module_, "ContentGetValue")(Narrow->data(), Request["field"].toInt(), Request["unit"].toInt(), Bytes.data(), int(Bytes.size()), Request["defer"].toBool() ? 1 : 0);
        }
        if (Type == -3 || Type == -4 || Type == 0) return QJsonObject{{"type", Type}};
        if (Type < 0) return Error{Type == -2 ? ErrorCategory::Unavailable : ErrorCategory::Unsupported, "host-wdx-value-" + std::to_string(Type), {"wdx"}};
        QJsonValue Value;
        if (Type == 1 || Type == 6) { int Number; std::memcpy(&Number, Bytes.data(), 4); Value = Type == 6 ? QJsonValue(Number != 0) : QJsonValue(QString::number(Number)); }
        else if (Type == 2 || Type == 10) { qint64 Number; std::memcpy(&Number, Bytes.data(), 8); Value = QString::number(Number); }
        else if (Type == 3) { double Number; std::memcpy(&Number, Bytes.data(), 8); if (!std::isfinite(Number)) return Invalid("host-wdx-nonfinite"); Value = Number; }
        else if (Type == 4 || Type == 5)
        {
            WORD Parts[3]; std::memcpy(Parts, Bytes.data(), 6);
            const auto Text = Type == 4 ? QDate(Parts[0], Parts[1], Parts[2]).toString(Qt::ISODate) : QTime(Parts[0], Parts[1], Parts[2]).toString(Qt::ISODate);
            if (Text.isEmpty()) return Invalid("host-wdx-calendar"); Value = Text;
        }
        else if (Type == 11 || Type == 12)
        {
            const auto* Begin = reinterpret_cast<const wchar_t*>(Bytes.data()); const auto* End = std::find(Begin, Begin + Bytes.size() / 2, L'\0');
            if (End == Begin + Bytes.size() / 2) return Invalid("host-wdx-value-termination"); Value = QString::fromWCharArray(Begin, End - Begin);
        }
        else if (Type >= 7 && Type <= 9)
        {
            const auto* Begin = reinterpret_cast<const char*>(Bytes.data()); const auto* End = static_cast<const char*>(memchr(Begin, 0, Bytes.size()));
            if (!End) return Invalid("host-wdx-value-termination"); Value = Ansi(Begin, int(End - Begin));
        }
        else return Unsupported("host-wdx-value-type");
        return QJsonObject{{"type", Type}, {"value", Value}};
    }
    Result<QJsonObject> TcDataHostService::Handle(WireKind Kind, const QJsonObject& Request)
    {
        if (!manifest_.permissions_.contains(Permission::ReadContent)) return Error{ErrorCategory::AccessDenied, "host-data-read-denied", {"tc-data"}};
        if (manifest_.format_ == PluginFormat::Wdx && Kind == WireKind::MetadataQuery) return metadata(Request);
        if (manifest_.format_ != PluginFormat::Wfx) return Invalid("host-data-format");
        if (Kind == WireKind::StorageEnumerate) return enumerate(Request);
        const auto Remote = Request["path"].toString(); if (!Path(Remote, true)) return Invalid("host-wfx-path");
        auto Name = Buffer(Remote);
        if (Kind == WireKind::StorageGet && Request.size() == 3 && Path(Request["local"].toString(), false) && Request["info"].isObject())
        {
            const auto Get = Export<int(__stdcall*)(wchar_t*, wchar_t*, int, RemoteInfo*)>(module_, "FsGetFileW");
            if (!Get) return Unsupported("host-wfx-get");
            auto Local = Buffer(QDir::toNativeSeparators(Request["local"].toString())); RemoteInfo Info{};
            const auto Data = Request["info"].toObject(); bool Parsed = false; auto Size = Data["size"].toString().toULongLong(&Parsed);
            if (!Parsed) return Invalid("host-wfx-remote-info"); Info.low_ = DWORD(Size); Info.high_ = DWORD(Size >> 32);
            const auto Time = Data["modified"].toString().toULongLong(&Parsed); if (!Parsed) return Invalid("host-wfx-remote-info");
            Info.modified_ = {DWORD(Time), DWORD(Time >> 32)}; Info.attributes_ = Data["attributes"].toInt();
            const int Code = Get(Name.data(), Local.data(), 1, &Info);
            if (Code != 0) return Error{Code == 5 ? ErrorCategory::Cancelled : ErrorCategory::Unavailable, "host-wfx-get-" + std::to_string(Code), {"wfx"}};
            return QJsonObject{};
        }
        if (Kind != WireKind::StorageMutate) return Invalid("host-data-command");
        if (!manifest_.extensions_.contains(Extension::StorageWrite) || !manifest_.permissions_.contains(Permission::WriteContent))
            return Error{ErrorCategory::AccessDenied, "host-wfx-write-denied", {"wfx"}};
        const auto Action = Request["action"].toString();
        if (Action == "attributes" && Request.size() == 3 && Integer(Request["value"], 0, 65535))
        {
            const auto Set = Export<BOOL(__stdcall*)(wchar_t*, int)>(module_, "FsSetAttrW"); if (!Set) return Unsupported("host-wfx-attributes");
            if (!Set(Name.data(), Request["value"].toInt())) return HostError("host-wfx-attributes-failed"); return QJsonObject{};
        }
        if (Action == "modified" && Request.size() == 3 && Request["value"].isString())
        {
            bool Parsed; const auto Ticks = Request["value"].toString().toULongLong(&Parsed); if (!Parsed) return Invalid("host-wfx-time");
            const auto Set = Export<BOOL(__stdcall*)(wchar_t*, FILETIME*, FILETIME*, FILETIME*)>(module_, "FsSetTimeW"); if (!Set) return Unsupported("host-wfx-time");
            FILETIME Time{DWORD(Ticks), DWORD(Ticks >> 32)};
            if (!Set(Name.data(), nullptr, nullptr, &Time)) return HostError("host-wfx-time-failed"); return QJsonObject{};
        }
        if (Action == "put" && Request.size() == 4 && Path(Request["local"].toString(), false) && Request["replace"].isBool())
        {
            const auto Put = Export<int(__stdcall*)(wchar_t*, wchar_t*, int)>(module_, "FsPutFileW"); if (!Put) return Unsupported("host-wfx-put");
            auto Local = Buffer(QDir::toNativeSeparators(Request["local"].toString()));
            const int Code = Put(Local.data(), Name.data(), Request["replace"].toBool() ? 1 : 0);
            if (Code != 0) return Error{Code == 5 ? ErrorCategory::Cancelled : ErrorCategory::Unavailable, "host-wfx-put-" + std::to_string(Code), {"wfx"}};
            return QJsonObject{};
        }
        const char* Function = Action == "mkdir" ? "FsMkDirW" : Action == "delete" ? "FsDeleteFileW" : Action == "rmdir" ? "FsRemoveDirW" : nullptr;
        if (Function && Request.size() == 2)
        {
            const auto Call = Export<BOOL(__stdcall*)(wchar_t*)>(module_, Function); if (!Call) return Unsupported("host-wfx-mutation");
            if (!Call(Name.data())) return HostError("host-wfx-mutation-failed"); return QJsonObject{};
        }
        if (Action == "rename" && Request.size() == 3 && Path(Request["to"].toString(), true))
        {
            const auto Rename = Export<int(__stdcall*)(wchar_t*, wchar_t*, BOOL, BOOL, RemoteInfo*)>(module_, "FsRenMovFileW");
            if (!Rename) return Unsupported("host-wfx-rename"); auto To = Buffer(Request["to"].toString()); RemoteInfo Info{};
            const int Code = Rename(Name.data(), To.data(), TRUE, FALSE, &Info);
            if (Code != 0) return HostError("host-wfx-rename-failed", Code); return QJsonObject{};
        }
        return Invalid("host-wfx-mutation-request");
    }
}
