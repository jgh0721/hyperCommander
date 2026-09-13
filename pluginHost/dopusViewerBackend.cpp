#include "dopusViewerBackend.hpp"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QScopeGuard>
#include <array>
#include <algorithm>
#include <shlwapi.h>

namespace hc::plugins
{
    using namespace storage;
    namespace
    {
        Error Invalid(const char* Code) { return {ErrorCategory::InvalidRequest, Code, {"dopus-viewer"}}; }
        Error Unsupported(const char* Code) { return {ErrorCategory::Unsupported, Code, {"dopus-viewer"}}; }
        template<class T> T Export(HMODULE Module, const char* Name) { return reinterpret_cast<T>(GetProcAddress(Module, Name)); }
        template<class Char> QString Text(const Char* Value)
        { if constexpr (std::is_same_v<Char, wchar_t>) return QString::fromWCharArray(Value); else return QString::fromLocal8Bit(Value); }
        template<class Char> constexpr const char* Pick(const char* Wide, const char* Ansi)
        { if constexpr (std::is_same_v<Char, wchar_t>) return Wide; else return Ansi; }
        constexpr qsizetype TextBudget = 512 * 1024; // Base64 reply fits the 1 MiB wire budget.
    }
    DopusViewerBackend::DopusViewerBackend(HMODULE Module) : module_(Module), abort_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    { com_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    DopusViewerBackend::~DopusViewerBackend()
    {
        Close();
        if (initialized_) if (auto Uninit = Export<dopus::Uninit>(module_, "DVP_Uninit")) Uninit();
        if (com_) CoUninitialize();
    }
    Result<Unit> DopusViewerBackend::Initialize()
    {
        if (initialized_) return Invalid("dopus-viewer-already-initialized");
        if (!abort_.Get() || !com_) return HostError("dopus-viewer-initialize");
        if (auto Init = Export<dopus::Init>(module_, "DVP_InitEx"))
        {
            wchar_t Language[] = L"english"; dopus::InitData Data{sizeof(Data), nullptr, 9, 0, Language};
            if (!Init(&Data)) return Unsupported("dopus-viewer-init-rejected");
        }
        else if (auto Init = Export<BOOL (*)()>(module_, "DVP_Init"))
            if (!Init()) return Unsupported("dopus-viewer-init-rejected");
        initialized_ = true;
        unicode_ = GetProcAddress(module_, "DVP_IdentifyW") != nullptr;
        auto Result = unicode_ ? identifyPlugin<wchar_t>() : identifyPlugin<char>();
        if (!Result.HasValue()) return Result.Failure(); probe_ = Result.Value(); return Unit{};
    }
    template<class Char> Result<QJsonObject> DopusViewerBackend::identifyPlugin()
    {
        auto Identify = Export<dopus::IdentifyViewer<Char>>(module_, Pick<Char>("DVP_IdentifyW", "DVP_IdentifyA"));
        if (!Identify) return Unsupported("dopus-viewer-identify-export");
        std::array<Char, 2048> Extensions{}, Name{}, Description{}, Copyright{}, Url{};
        dopus::ViewerInfo<Char> Info{}; Info.size_ = sizeof(Info); Info.opusMajor_ = 9;
        Info.extensions_ = Extensions.data(); Info.extensionsCapacity_ = Extensions.size(); Info.name_ = Name.data(); Info.nameCapacity_ = Name.size();
        Info.description_ = Description.data(); Info.descriptionCapacity_ = Description.size(); Info.copyright_ = Copyright.data(); Info.copyrightCapacity_ = Copyright.size();
        Info.url_ = Url.data(); Info.urlCapacity_ = Url.size();
        const auto Identified = Identify(&Info);
        if (Info.smallIcon_) DestroyIcon(Info.smallIcon_);
        if (Info.largeIcon_ && Info.largeIcon_ != Info.smallIcon_) DestroyIcon(Info.largeIcon_);
        if (!Identified) return Unsupported("dopus-viewer-identify-rejected");
        for (const auto* Buffer : {&Extensions, &Name, &Description, &Copyright, &Url})
            if (std::find(Buffer->begin(), Buffer->end(), Char{}) == Buffer->end()) return Invalid("dopus-viewer-info-unterminated");
        QJsonArray Exports;
        for (const auto* Entry : {"DVP_IdentifyFileW", "DVP_IdentifyFileA", "DVP_IdentifyFileStreamW", "DVP_IdentifyFileStreamA",
            "DVP_LoadBitmapW", "DVP_LoadBitmapA", "DVP_LoadBitmapStreamW", "DVP_LoadBitmapStreamA", "DVP_CreateViewer", "DVP_LoadTextW", "DVP_LoadTextA"})
            if (GetProcAddress(module_, Entry)) Exports.append(Entry);
        return QJsonObject{{"format", "dopus-viewer"}, {"sdkVersion", 4}, {"unicode", unicode_}, {"name", Text(Name.data())},
            {"extensions", Text(Extensions.data())}, {"flags", qint64(Info.flags_)}, {"exports", Exports},
            {"commands", false}, {"search", false}, {"processId", qint64(GetCurrentProcessId())}};
    }
    Result<QJsonObject> DopusViewerBackend::Probe()
    { if (!initialized_) return Invalid("dopus-viewer-not-initialized"); return probe_; }
    void DopusViewerBackend::Close()
    {
        SetEvent(abort_.Get());
        if (window_ && IsWindow(window_)) DestroyWindow(window_);
        window_ = nullptr; native_ = false; pixels_.clear();
        if (input_) std::exchange(input_, nullptr)->Release();
    }
    void DopusViewerBackend::Resize(int Width, int Height)
    {
        if (!window_) return;
        MoveWindow(window_, 0, 0, Width, Height, TRUE);
        if (native_) SendMessageW(window_, dopus::ViewerResize, 0, MAKELPARAM(Width, Height));
        else InvalidateRect(window_, nullptr, FALSE);
    }
    Result<HWND> DopusViewerBackend::Open(HWND Parent, const QString& File, int Width, int Height, bool Preview)
    {
        Close();
        if (!ResetEvent(abort_.Get())) return HostError("dopus-viewer-reset-abort", GetLastError());
        preview_ = Preview;
        auto Result = unicode_ ? process<wchar_t>(Parent, File, Width, Height, true, false) : process<char>(Parent, File, Width, Height, true, false);
        if (!Result.HasValue()) { Close(); return Result.Failure(); } return window_;
    }
    template<class Char> Result<QJsonObject> DopusViewerBackend::process(HWND Parent, const QString& File, int Width, int Height, bool View, bool Convert)
    {
        if (File.contains(QChar::Null) || !QFileInfo(File).isAbsolute() || !QFileInfo(File).isFile()) return Invalid("dopus-viewer-file");
        const auto Native = QDir::toNativeSeparators(File); auto Wide = Native.toStdWString(); auto Ansi = Native.toLocal8Bit();
        std::basic_string<Char> Name;
        bool FromStream = false;
        if constexpr (std::is_same_v<Char, wchar_t>) Name = Wide;
        else
        {
            FromStream = QString::fromLocal8Bit(Ansi) != Native;
            // A display name preserves the extension for ANSI stream-only SDKs.
            Name = FromStream ? (QString("content.") + QFileInfo(File).suffix()).toLatin1().toStdString() : Ansi.toStdString();
        }
        using IdentifyStream = BOOL (*)(HWND, IStream*, Char*, dopus::ViewerFileInfo<Char>*, DWORD);
        using BitmapStream = HBITMAP (*)(HWND, IStream*, Char*, dopus::ViewerFileInfo<Char>*, SIZE*, DWORD);
        auto Identify = Export<dopus::IdentifyFile<Char>>(module_, Pick<Char>("DVP_IdentifyFileW", "DVP_IdentifyFileA"));
        auto IdentifyFromStream = Export<IdentifyStream>(module_, Pick<Char>("DVP_IdentifyFileStreamW", "DVP_IdentifyFileStreamA"));
        if (!Identify) FromStream = true;
        IStream* Stream = nullptr;
        const auto Release = qScopeGuard([&] { if (Stream) Stream->Release(); });
        if (FromStream)
        {
            if (!(probe_["flags"].toInteger() & 1) || !IdentifyFromStream) return Unsupported("dopus-viewer-stream-unavailable");
            if (FAILED(SHCreateStreamOnFileEx(Wide.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &Stream))) return HostError("dopus-viewer-input-stream");
        }
        std::array<Char, 4096> Description{};
        dopus::ViewerFileInfo<Char> Info{}; Info.size_ = sizeof(Info); Info.fileSize_ = QFileInfo(File).size();
        Info.description_ = Description.data(); Info.descriptionCapacity_ = Description.size(); if (!View && !Convert) Info.flags_ = 1u << 6;
        const bool Identified = FromStream ? IdentifyFromStream(Parent, Stream, Name.data(), &Info, 0) : Identify(Parent, Name.data(), &Info, abort_.Get());
        if (!Identified) return Unsupported("host-viewer-load-rejected");
        if (std::find(Description.begin(), Description.end(), Char{}) == Description.end()) return Invalid("dopus-viewer-description-unterminated");
        if (Stream && FAILED(Stream->Seek({}, STREAM_SEEK_SET, nullptr))) return HostError("dopus-viewer-stream-seek");
        if (Convert || (View && (Info.flags_ & dopus::ViewerText)))
        {
            auto Loaded = loadText(Parent, Name.data(), Stream, FromStream); if (!Loaded.HasValue()) return Loaded.Failure();
            if (!View) return Loaded;
            // Keep markup inert. Rich text/HTML are returned as typed conversion
            // results, and are not injected into an active browser surface.
            if (Loaded.Value()["textType"].toInt() != 0) return Unsupported("dopus-viewer-rich-text-surface");
            const auto Bytes = QByteArray::fromBase64(Loaded.Value()["bytes"].toString().toLatin1());
            QString TextValue;
            if (Bytes.startsWith("\xff\xfe"))
            {
                if ((Bytes.size() & 1) != 0) return Invalid("dopus-viewer-text-encoding");
                TextValue = QString::fromUtf16(reinterpret_cast<const char16_t*>(Bytes.constData() + 2), (Bytes.size() - 2) / 2);
            }
            else TextValue = Bytes.startsWith("\xef\xbb\xbf") ? QString::fromUtf8(Bytes.sliced(3)) : QString::fromLocal8Bit(Bytes);
            window_ = CreateWindowExW(0, L"EDIT", reinterpret_cast<LPCWSTR>(TextValue.utf16()),
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, Width, Height, Parent, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!window_) return HostError("dopus-viewer-text-window"); return QJsonObject{};
        }
        if (!View) return QJsonObject{{"description", Text(Description.data())}, {"width", int(Info.imageSize_.cx)}, {"height", int(Info.imageSize_.cy)},
            {"bits", Info.bits_}, {"majorType", int(Info.majorType_)}, {"minorType", int(Info.minorType_)}};
        if ((Info.flags_ & dopus::ViewerWindow) && GetProcAddress(module_, "DVP_CreateViewer"))
        {
            auto Create = Export<dopus::CreateViewer>(module_, "DVP_CreateViewer"); if (!Create) return Unsupported("dopus-viewer-create-export");
            RECT Rect{0, 0, Width, Height}; auto Window = Create(Parent, &Rect, preview_ ? dopus::ViewerPreview : 0); DWORD Process = 0;
            if (!Window || !GetWindowThreadProcessId(Window, &Process) || Process != GetCurrentProcessId() || !IsChild(Parent, Window)) return Invalid("dopus-viewer-window-parent");
            window_ = Window; native_ = true;
            if (Stream) { Stream->AddRef(); input_ = Stream; }
            SendMessageW(Window, dopus::ViewerSetAbort, 0, reinterpret_cast<LPARAM>(abort_.Get()));
            const auto Loaded = FromStream ? SendMessageW(Window, std::is_same_v<Char, wchar_t> ? WM_APP + 0xf04 : WM_APP + 0xf03, reinterpret_cast<WPARAM>(Name.data()), reinterpret_cast<LPARAM>(Stream))
                : SendMessageW(Window, std::is_same_v<Char, wchar_t> ? dopus::ViewerLoadW : dopus::ViewerLoadA, 0, reinterpret_cast<LPARAM>(Name.data()));
            if (!Loaded) return Unsupported("host-viewer-load-rejected");
            return QJsonObject{};
        }
        if (!(Info.flags_ & dopus::ViewerBitmap)) return Unsupported("host-viewer-load-rejected");
        HBITMAP Bitmap = nullptr; SIZE Desired{Width, Height};
        if (FromStream)
        {
            auto Load = Export<BitmapStream>(module_, Pick<Char>("DVP_LoadBitmapStreamW", "DVP_LoadBitmapStreamA"));
            if (Load) Bitmap = Load(Parent, Stream, Name.data(), &Info, &Desired, 0);
        }
        else if (auto Load = Export<dopus::LoadBitmap<Char>>(module_, Pick<Char>("DVP_LoadBitmapW", "DVP_LoadBitmapA"))) Bitmap = Load(Parent, Name.data(), &Info, &Desired, abort_.Get());
        if (!Bitmap) return Unsupported("host-viewer-load-rejected");
        if (Info.flags_ & (dopus::ViewerJpegStream | dopus::ViewerPngStream))
        { LocalFree(reinterpret_cast<HLOCAL>(Bitmap)); return Unsupported("dopus-viewer-encoded-bitmap"); }
        auto Shown = showBitmap(Parent, Bitmap, Info.flags_, Info.transparent_); if (!Shown.HasValue()) return Shown.Failure();
        window_ = Shown.Value(); return QJsonObject{};
    }
    template<class Char> Result<QJsonObject> DopusViewerBackend::loadText(HWND Parent, Char* Name, IStream* Input, bool FromStream)
    {
        auto Load = Export<dopus::LoadText<Char>>(module_, Pick<Char>("DVP_LoadTextW", "DVP_LoadTextA"));
        if (!Load) return Unsupported("dopus-viewer-convert-export");
        dopus::LoadTextData<Char> Data{}; Data.size_ = sizeof(Data); Data.parent_ = Parent; Data.file_ = Name; Data.input_ = Input;
        Data.flags_ = FromStream ? 1 : 0; Data.abort_ = abort_.Get();
        const auto Loaded = Load(&Data); const auto Release = qScopeGuard([&] { if (Data.output_) Data.output_->Release(); });
        if (!Loaded || !Data.output_) return Unsupported("dopus-viewer-convert-rejected");
        if (Data.textType_ < 0 || Data.textType_ > 2) return Invalid("dopus-viewer-convert-type");
        if (FAILED(Data.output_->Seek({}, STREAM_SEEK_SET, nullptr))) return HostError("dopus-viewer-convert-seek");
        QByteArray Bytes; std::array<char, 32768> Buffer{};
        for (;;)
        {
            ULONG Read = 0; const auto Status = Data.output_->Read(Buffer.data(), Buffer.size(), &Read);
            if (FAILED(Status) || Read > Buffer.size()) return HostError("dopus-viewer-convert-read");
            if (Bytes.size() + Read > TextBudget) return Error{ErrorCategory::BudgetExceeded, "dopus-viewer-convert-budget", {"dopus-viewer"}};
            Bytes.append(Buffer.data(), Read); if (!Read || Status == S_FALSE) break;
        }
        return QJsonObject{{"textType", Data.textType_}, {"bytes", QString::fromLatin1(Bytes.toBase64())}};
    }
    Result<QJsonObject> DopusViewerBackend::Data(WireKind Kind, const QJsonObject& Request)
    {
        if (Kind != WireKind::MetadataQuery && Kind != WireKind::ContentConvert) return Unsupported("dopus-viewer-data-command");
        const auto Action = Request["action"].toString();
        if (Kind == WireKind::MetadataQuery && Action == "fields")
        {
            if (Request.size() != 1) return Invalid("dopus-viewer-metadata-fields");
            QJsonArray Fields; const QStringList Names{"description", "width", "height", "bits", "majorType", "minorType"};
            for (int Index = 0; Index < Names.size(); ++Index) Fields.append(QJsonObject{{"index", Index}, {"name", Names[Index]}, {"type", Index ? 1 : 8}, {"units", ""}});
            return QJsonObject{{"fields", Fields}};
        }
        const bool Convert = Kind == WireKind::ContentConvert;
        if ((!Convert && (Request.size() != 5 || Action != "value" || Request["unit"].toInt(-1) != 0 || !Request["defer"].isBool()))
            || (Convert && Request.size() != 1) || !Request["path"].isString()) return Invalid("dopus-viewer-data-fields");
        // Close signals the event before native destruction. Each subsequent
        // synchronous data operation starts with a fresh cancellation state.
        if (!ResetEvent(abort_.Get())) return HostError("dopus-viewer-reset-abort", GetLastError());
        auto Info = unicode_ ? process<wchar_t>(nullptr, Request["path"].toString(), 0, 0, false, Convert) : process<char>(nullptr, Request["path"].toString(), 0, 0, false, Convert);
        if (!Info.HasValue()) return Info.Failure(); if (Convert) return Info;
        const auto Field = Request["field"].toInt(-1); const QStringList Names{"description", "width", "height", "bits", "majorType", "minorType"};
        if (Field < 0 || Field >= Names.size() || Request["field"].toDouble() != Field) return Invalid("dopus-viewer-metadata-index");
        return QJsonObject{{"type", Field ? 1 : 8}, {"value", Field ? QString::number(Info.Value()[Names[Field]].toInt()) : Info.Value()[Names[Field]].toString()}};
    }
    Result<HWND> DopusViewerBackend::showBitmap(HWND Parent, HBITMAP Bitmap, DWORD Flags, COLORREF Transparent)
    {
        const auto Delete = qScopeGuard([&] { DeleteObject(Bitmap); }); BITMAP Object{};
        if (!GetObjectW(Bitmap, sizeof(Object), &Object) || Object.bmWidth <= 0 || Object.bmHeight <= 0
            || Object.bmWidth > 16384 || Object.bmHeight > 16384 || std::uint64_t(Object.bmWidth) * Object.bmHeight > 16 * 1024 * 1024)
            return Invalid("dopus-viewer-bitmap-dimensions");
        bitmapInfo_ = {}; auto& Header = bitmapInfo_.bmiHeader; Header.biSize = sizeof(Header); Header.biWidth = Object.bmWidth;
        Header.biHeight = -Object.bmHeight; Header.biPlanes = 1; Header.biBitCount = 32; Header.biCompression = BI_RGB;
        pixels_.resize(std::size_t(Object.bmWidth) * Object.bmHeight);
        const auto Dc = GetDC(nullptr); const auto Read = GetDIBits(Dc, Bitmap, 0, Object.bmHeight, pixels_.data(), &bitmapInfo_, DIB_RGB_COLORS); ReleaseDC(nullptr, Dc);
        if (Read != Object.bmHeight) return HostError("dopus-viewer-bitmap-read");
        for (auto& Pixel : pixels_)
        {
            const auto Red = (Pixel >> 16) & 255, Green = (Pixel >> 8) & 255, Blue = Pixel & 255;
            const auto Alpha = (Flags & (1u << 9)) && RGB(Red, Green, Blue) == Transparent ? 0 : (Flags & dopus::ViewerAlpha) ? (Pixel >> 24) : 255;
            Pixel = (((Red * Alpha + 32 * (255 - Alpha)) / 255) << 16) | (((Green * Alpha + 32 * (255 - Alpha)) / 255) << 8) | ((Blue * Alpha + 32 * (255 - Alpha)) / 255);
        }
        WNDCLASSW Class{}; Class.lpfnWndProc = bitmapProc; Class.hInstance = GetModuleHandleW(nullptr); Class.lpszClassName = L"HyperCommander.DopusBitmap.1";
        if (!RegisterClassW(&Class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return HostError("dopus-viewer-bitmap-class");
        auto Window = CreateWindowExW(0, Class.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, Parent, nullptr, Class.hInstance, this);
        if (!Window) return HostError("dopus-viewer-bitmap-window"); return Window;
    }
    LRESULT CALLBACK DopusViewerBackend::bitmapProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
    {
        if (Message == WM_NCCREATE) SetWindowLongPtrW(Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(LParam)->lpCreateParams));
        auto* Self = reinterpret_cast<DopusViewerBackend*>(GetWindowLongPtrW(Window, GWLP_USERDATA));
        if (Message == WM_PAINT && Self)
        {
            PAINTSTRUCT Paint{}; const auto Dc = BeginPaint(Window, &Paint); RECT Rect{}; GetClientRect(Window, &Rect);
            const auto Brush = CreateSolidBrush(RGB(32, 32, 32)); FillRect(Dc, &Rect, Brush); DeleteObject(Brush);
            const auto Width = Self->bitmapInfo_.bmiHeader.biWidth, Height = -Self->bitmapInfo_.bmiHeader.biHeight;
            if (Width > 0 && Height > 0 && !Self->pixels_.empty())
            {
                const double Scale = std::min(double(Rect.right) / Width, double(Rect.bottom) / Height);
                const int DrawWidth = int(Width * Scale), DrawHeight = int(Height * Scale);
                SetStretchBltMode(Dc, HALFTONE); SetBrushOrgEx(Dc, 0, 0, nullptr);
                StretchDIBits(Dc, (Rect.right - DrawWidth) / 2, (Rect.bottom - DrawHeight) / 2, DrawWidth, DrawHeight,
                    0, 0, Width, Height, Self->pixels_.data(), &Self->bitmapInfo_, DIB_RGB_COLORS, SRCCOPY);
            }
            EndPaint(Window, &Paint); return 0;
        }
        if (Message == WM_ERASEBKGND) return 1;
        return DefWindowProcW(Window, Message, WParam, LParam);
    }
}
