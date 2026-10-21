#include "stdafx.h"
#include "nativeIconExtractor.hpp"

/// Platform/framework headers
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QScopeGuard>
#include <ShlObj.h>
#include <shellapi.h>
#include <commoncontrols.h>

namespace hc::icons {
namespace {
QImage DirectIcon(const QString& Path, int Index, int Pixels, State& Status)
{
    HICON Icon = nullptr;
    const auto Code = SHDefExtractIconW(reinterpret_cast<LPCWSTR>(Path.utf16()), Index, 0, &Icon, nullptr, UINT(Pixels));
    const auto Free = qScopeGuard([&] { if (Icon) DestroyIcon(Icon); });
    if (Code == S_OK && Icon) { Status = State::Ready; return QImage::fromHICON(Icon); }
    Status = Code == S_FALSE ? State::NoEmbeddedIcon : State::TransientFailure;
    return {};
}
QString Expand(QString Path)
{
    std::wstring Buffer(32768, L'\0');
    const auto Count = ExpandEnvironmentStringsW(reinterpret_cast<LPCWSTR>(Path.utf16()), Buffer.data(), DWORD(Buffer.size()));
    return Count && Count <= Buffer.size() ? QString::fromWCharArray(Buffer.data()) : Path;
}
QString IniValue(const QString& Path, const wchar_t* Key)
{
    std::wstring Buffer(32768, L'\0');
    GetPrivateProfileStringW(L".ShellClassInfo", Key, L"", Buffer.data(), DWORD(Buffer.size()), reinterpret_cast<LPCWSTR>(Path.utf16()));
    return QString::fromWCharArray(Buffer.data());
}
QImage ResourceIcon(QString Path, int Pixels, Result& Output, int Depth = 0)
{
    if (Depth > 3) { Output.state_ = State::TransientFailure; return {}; }
    const QFileInfo Info(Path);
    Output.dependencies_.append(Path);
    if (Info.isDir()) {
        const auto Ini = QDir(Path).filePath("desktop.ini");
        Output.dependencies_.append(Ini);
        // Windows only interprets customized folders with the system/read-only marker.
        const auto Attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(Path.utf16()));
        if (Attributes == INVALID_FILE_ATTRIBUTES || !(Attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM))) return {};
        auto Source = IniValue(Ini, L"IconResource"); int Index = 0;
        if (Source.isEmpty()) { Source = IniValue(Ini, L"IconFile"); Index = IniValue(Ini, L"IconIndex").toInt(); }
        else { const auto Comma = Source.lastIndexOf(','); if (Comma >= 0) { Index = Source.mid(Comma + 1).toInt(); Source.truncate(Comma); } }
        Source = Expand(Source.trimmed().remove('"'));
        if (Source.isEmpty()) return {};
        Output.customFolder_ = true;
        if (QDir::isRelativePath(Source)) Source = QDir(Path).filePath(Source);
        Output.dependencies_.append(Source);
        return DirectIcon(Source, Index, Pixels, Output.state_);
    }
    const auto Ext = Info.suffix().toLower();
    if (Ext == "lnk") {
        IShellLinkW* Link = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Link)))) { Output.state_ = State::TransientFailure; return {}; }
        const auto Free = qScopeGuard([&] { Link->Release(); });
        IPersistFile* Persist = nullptr;
        if (FAILED(Link->QueryInterface(IID_PPV_ARGS(&Persist)))) { Output.state_ = State::TransientFailure; return {}; }
        const auto FreePersist = qScopeGuard([&] { Persist->Release(); });
        if (FAILED(Persist->Load(reinterpret_cast<LPCWSTR>(Path.utf16()), STGM_READ))) { Output.state_ = State::TransientFailure; return {}; }
        wchar_t Target[32768]{}; int Index = 0;
        if (SUCCEEDED(Link->GetIconLocation(Target, 32768, &Index)) && *Target) {
            auto Source = Expand(QString::fromWCharArray(Target));
            if (QDir::isRelativePath(Source)) Source = Info.dir().filePath(Source);
            Output.dependencies_.append(Source);
            return DirectIcon(Source, Index, Pixels, Output.state_);
        }
        if (SUCCEEDED(Link->GetPath(Target, 32768, nullptr, SLGP_RAWPATH)) && *Target)
            return ResourceIcon(Expand(QString::fromWCharArray(Target)), Pixels, Output, Depth + 1);
        return {};
    }
    if (Ext == "exe" || Ext == "dll" || Ext == "ico") return DirectIcon(Path, 0, Pixels, Output.state_);
    return {};
}
}

Result ExtractNativeIcon(const Request& Query)
{
    // The dedicated pool reuses its threads. Keep one apartment per worker,
    // rather than initializing/uninitializing COM for every file in a cold list.
    struct Apartment {
        HRESULT result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~Apartment() { if (SUCCEEDED(result_)) CoUninitialize(); }
    };
    thread_local Apartment Com;
    Result Output;
    // Direct resources are re-read on each cache miss, including expiry/force refresh.
    if (Query.kind_ == Kind::Item && !Query.virtual_) {
        const QFileInfo Before(Query.path_);
        const auto Modified = Before.lastModified(); const auto Size = Before.size();
        Output.image_ = ResourceIcon(Query.path_, Query.pixels_, Output);
        const QFileInfo After(Query.path_);
        if (Modified != After.lastModified() || Size != After.size()) { Output.image_ = {}; Output.state_ = State::TransientFailure; return Output; }
        if (Query.directory_ && Query.knownCustomFolder_ && !Output.customFolder_ && Output.state_ == State::Ready) {
            Output.state_ = State::NoCustomIcon; return Output;
        }
        if (!Output.image_.isNull() || Output.state_ != State::Ready) return Output;
    }
    SHFILEINFOW Info{}; PIDLIST_ABSOLUTE Pidl = nullptr;
    const auto Free = qScopeGuard([&] { CoTaskMemFree(Pidl); if (Info.hIcon) DestroyIcon(Info.hIcon); });
    const bool Generic = Query.kind_ == Kind::File || Query.kind_ == Kind::Folder || Query.kind_ == Kind::Associated;
    QString Name = Generic ? Query.kind_ == Kind::Associated ? "hc-icon." + Query.extension_ : QStringLiteral("hc-icon") : Query.path_;
    if (Query.virtual_ && FAILED(SHParseDisplayName(reinterpret_cast<LPCWSTR>(Name.utf16()), nullptr, &Pidl, 0, nullptr))) {
        Output.state_ = State::TransientFailure; return Output;
    }
    UINT Flags = SHGFI_SYSICONINDEX | (Query.virtual_ ? SHGFI_PIDL : Generic ? SHGFI_USEFILEATTRIBUTES : 0);
    if (Query.kind_ == Kind::Overlay) Flags |= SHGFI_ICON | SHGFI_OVERLAYINDEX | SHGFI_ADDOVERLAYS;
    if (!SHGetFileInfoW(Query.virtual_ ? reinterpret_cast<LPCWSTR>(Pidl) : reinterpret_cast<LPCWSTR>(Name.utf16()),
        Query.kind_ == Kind::Folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &Info, sizeof(Info), Flags)) {
        Output.state_ = State::TransientFailure; return Output;
    }
    const auto Overlay = Query.kind_ == Kind::Overlay ? UINT(Info.iIcon) >> 24 : 0;
    if (Query.kind_ == Kind::Overlay && !Overlay) { Output.state_ = State::NoOverlay; return Output; }
    const auto Read = [&](int List) {
        QImage Image; IImageList* Images = nullptr;
        if (FAILED(SHGetImageList(List, IID_PPV_ARGS(&Images))) || !Images) return Image;
        const auto Release = qScopeGuard([&] { Images->Release(); });
        int Index = Info.iIcon & 0x00ffffff;
        if (Overlay && FAILED(Images->GetOverlayImage(int(Overlay), &Index))) return Image;
        HICON Icon = nullptr;
        if (SUCCEEDED(Images->GetIcon(Index, ILD_TRANSPARENT, &Icon)) && Icon) { Image = QImage::fromHICON(Icon); DestroyIcon(Icon); }
        return Image;
    };
    const int Preferred = Query.pixels_ <= 16 ? SHIL_SMALL : Query.pixels_ <= 32 ? SHIL_LARGE : Query.pixels_ <= 48 ? SHIL_EXTRALARGE : SHIL_JUMBO;
    Output.image_ = Read(Preferred);
    // Some jumbo entries contain a 48-pixel image in an otherwise empty 256-pixel canvas.
    if (Preferred == SHIL_JUMBO && !Output.image_.isNull()) {
        const auto Argb = Output.image_.convertToFormat(QImage::Format_ARGB32); bool Padded = true;
        for (int Y = 0; Y < Argb.height() && Padded; ++Y) for (int X = 0; X < Argb.width(); ++X)
            if ((X >= 48 || Y >= 48) && qAlpha(Argb.pixel(X, Y))) { Padded = false; break; }
        if (Padded) Output.image_ = Read(SHIL_EXTRALARGE);
    }
    if (Output.image_.isNull()) Output.image_ = Read(SHIL_LARGE);
    if (Output.image_.isNull()) Output.state_ = State::TransientFailure;
    else if (Output.image_.width() != Query.pixels_) Output.image_ = Output.image_.scaled(Query.pixels_, Query.pixels_, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return Output;
}
}
