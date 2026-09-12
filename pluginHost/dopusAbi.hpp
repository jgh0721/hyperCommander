#pragma once
#include <Windows.h>
#include <objidl.h>
#include <cstddef>

namespace hc::plugins::dopus
{
    // Private host-side declarations for the supplied Opus 9 SDK: Viewer v4,
    // VFS v2. No SDK implementation is linked into the application. The SDK
    // archive and original headers remain external; tstDopusSdkAbi.cpp checks
    // these layouts against those headers when HC_DOPUS_SDK_DIRECTORY is set.
    // x64 uses the Windows x64 calling convention; no packed structures here.
    struct InitData
    {
        UINT size_;
        HWND messageWindow_;
        DWORD opusMajor_, opusMinor_;
        wchar_t* language_;
    };
    template<class Char> struct ViewerInfo
    {
        UINT size_;
        DWORD flags_, versionHigh_, versionLow_;
        Char *extensions_, *name_, *description_, *copyright_, *url_;
        UINT extensionsCapacity_, nameCapacity_, descriptionCapacity_, copyrightCapacity_, urlCapacity_;
        DWORDLONG minFile_, maxFile_, minPreview_, maxPreview_;
        UINT majorType_;
        GUID id_;
        DWORD opusMajor_, opusMinor_, initFlags_;
        HICON smallIcon_, largeIcon_;
    };
    template<class Char> struct ViewerFileInfo
    {
        UINT size_;
        DWORD flags_;
        WORD majorType_, minorType_;
        SIZE imageSize_;
        int bits_;
        Char* description_;
        UINT descriptionCapacity_;
        DWORD privateData_[8];
        SIZE resolution_;
        int typeHint_;
        COLORREF transparent_;
        WORD thumbnailQuality_;
        DWORDLONG fileSize_;
        int colorSpace_;
    };
    template<class Char> struct LoadTextData
    {
        UINT size_;
        DWORD flags_;
        HWND parent_;
        Char* file_;
        IStream* input_;
        DWORD streamFlags_;
        IStream* output_;
        int textType_;
        Char preferredViewer_[40];
        HANDLE abort_;
    };
    template<class Char> struct VfsInfo
    {
        UINT size_;
        GUID id_;
        DWORD versionHigh_, versionLow_, flags_, capabilities_;
        Char *prefix_, *extensions_, *name_, *description_, *copyright_, *url_;
        UINT prefixCapacity_, extensionsCapacity_, nameCapacity_, descriptionCapacity_, copyrightCapacity_, urlCapacity_;
        DWORD opusMajor_, opusMinor_, initFlags_;
        HICON smallIcon_, largeIcon_;
    };
    struct VfsFileHeader
    {
        UINT size_;
        VfsFileHeader* next_;
        int count_;
        UINT entrySize_;
    };
    struct VfsFileColumn { int id_; wchar_t* value_; };
    struct VfsFileData
    {
        WIN32_FIND_DATAW file_;
        DWORD flags_;
        wchar_t* comment_;
        int columnCount_;
        VfsFileColumn* columns_;
    };
    struct VfsReadDirectoryData
    {
        UINT size_;
        HWND parent_;
        wchar_t* path_;
        int operation_;
        HANDLE abort_, heap_;
        VfsFileHeader* files_;
    };

    inline constexpr DWORD VfsReadOnly = 1u << 29;
    inline constexpr DWORD VfsRandomSeek = 1u << 11;
    inline constexpr DWORD VfsMoveByRename = 1u << 0;
    inline constexpr DWORD ViewerBitmap = 1u << 0;
    inline constexpr DWORD ViewerWindow = 1u << 1;
    inline constexpr DWORD ViewerText = 1u << 7;
    inline constexpr DWORD ViewerAlpha = 1u << 8;
    inline constexpr DWORD ViewerJpegStream = 1u << 17;
    inline constexpr DWORD ViewerPngStream = 1u << 18;
    inline constexpr UINT ViewerLoadA = WM_APP + 0xf01;
    inline constexpr UINT ViewerLoadW = WM_APP + 0xf02;
    inline constexpr UINT ViewerResize = WM_APP + 0xf08;

    using Init = BOOL (*)(InitData*);
    using Uninit = void (*)();
    using VfsIdentify = BOOL (*)(VfsInfo<wchar_t>*);
    using VfsCreate = HANDLE (*)(GUID*, HWND);
    using VfsDestroy = void (*)(HANDLE);
    using VfsError = long (*)(HANDLE);
    using VfsCapabilities = DWORD (*)(HANDLE);
    using VfsFindFirst = HANDLE (*)(HANDLE, void*, wchar_t*, WIN32_FIND_DATAW*, HANDLE);
    using VfsFindNext = BOOL (*)(HANDLE, void*, HANDLE, WIN32_FIND_DATAW*);
    using VfsFindClose = void (*)(HANDLE, HANDLE);
    using VfsOpenFile = HANDLE (*)(HANDLE, void*, wchar_t*, DWORD, DWORD, DWORD, FILETIME*);
    using VfsReadFile = BOOL (*)(HANDLE, void*, HANDLE, void*, DWORD, DWORD*);
    using VfsWriteFile = BOOL (*)(HANDLE, void*, HANDLE, void*, DWORD, BOOL, DWORD*);
    using VfsCloseFile = void (*)(HANDLE, void*, HANDLE);
    using VfsMoveFile = BOOL (*)(HANDLE, void*, wchar_t*, wchar_t*);
    using VfsDeleteFile = BOOL (*)(HANDLE, void*, wchar_t*, DWORD, int);
    using VfsDirectory = BOOL (*)(HANDLE, void*, wchar_t*, DWORD);
    using VfsSetAttributes = BOOL (*)(HANDLE, void*, wchar_t*, DWORD, BOOL);
    using VfsSetTime = BOOL (*)(HANDLE, void*, wchar_t*, HANDLE, FILETIME*, FILETIME*, FILETIME*);
    template<class Char> using IdentifyViewer = BOOL (*)(ViewerInfo<Char>*);
    template<class Char> using IdentifyFile = BOOL (*)(HWND, Char*, ViewerFileInfo<Char>*, HANDLE);
    template<class Char> using LoadBitmap = HBITMAP (*)(HWND, Char*, ViewerFileInfo<Char>*, SIZE*, HANDLE);
    template<class Char> using LoadText = BOOL (*)(LoadTextData<Char>*);
    using CreateViewer = HWND (*)(HWND, RECT*, DWORD);

    static_assert(sizeof(void*) == 8, "DOpus adapters require an x64 host");
}
