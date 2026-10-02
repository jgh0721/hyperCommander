#include "pluginHost/dopusAbi.hpp"
#include <commctrl.h>
#include "plugin support.h"
#include "viewer plugins.h"
#include "vfs plugins.h"
#include <type_traits>

using namespace hc::plugins::dopus;
#define HC_SIZE(Own, Sdk) static_assert(sizeof(Own) == sizeof(Sdk)); static_assert(alignof(Own) == alignof(Sdk))
#define HC_OFFSET(Own, Member, Sdk, Field) static_assert(offsetof(Own, Member) == offsetof(Sdk, Field))
HC_SIZE(InitData, DVPINITEXDATA);
HC_SIZE(InitData, VFSINITDATA);
HC_SIZE(ViewerInfo<char>, VIEWERPLUGININFOA);
HC_SIZE(ViewerInfo<wchar_t>, VIEWERPLUGININFOW);
HC_SIZE(ViewerFileInfo<char>, VIEWERPLUGINFILEINFOA);
HC_SIZE(ViewerFileInfo<wchar_t>, VIEWERPLUGINFILEINFOW);
HC_SIZE(LoadTextData<char>, DVPLOADTEXTDATAA);
HC_SIZE(LoadTextData<wchar_t>, DVPLOADTEXTDATAW);
HC_SIZE(VfsInfo<wchar_t>, VFSPLUGININFOW);
HC_SIZE(VfsFileHeader, VFSFILEDATAHEADER);
HC_SIZE(VfsFileData, VFSFILEDATAW);
HC_SIZE(VfsFileColumn, VFSFILEDATACOLUMNW);
HC_SIZE(VfsCustomColumn<wchar_t>, VFSCUSTOMCOLUMNW);
HC_SIZE(VfsCustomColumn<char>, VFSCUSTOMCOLUMNA);
HC_OFFSET(VfsCustomColumn<wchar_t>, id_, VFSCUSTOMCOLUMNW, iID);
HC_SIZE(VfsReadDirectoryData, VFSREADDIRDATAW);
HC_SIZE(VfsBatchData, VFSBATCHDATAW);
HC_OFFSET(VfsBatchData, functionData_, VFSBATCHDATAW, lpFuncData);
static_assert(VfsBatchProperty == VFSPROP_BATCHOPERATION);
HC_OFFSET(InitData, language_, VFSINITDATA, pszLanguageName);
HC_OFFSET(ViewerInfo<wchar_t>, extensions_, VIEWERPLUGININFOW, lpszHandleExts);
HC_OFFSET(ViewerInfo<wchar_t>, minFile_, VIEWERPLUGININFOW, dwlMinFileSize);
HC_OFFSET(ViewerInfo<wchar_t>, id_, VIEWERPLUGININFOW, idPlugin);
HC_OFFSET(ViewerInfo<wchar_t>, smallIcon_, VIEWERPLUGININFOW, hIconSmall);
HC_OFFSET(ViewerFileInfo<wchar_t>, privateData_, VIEWERPLUGINFILEINFOW, dwPrivateData);
HC_OFFSET(ViewerFileInfo<wchar_t>, fileSize_, VIEWERPLUGINFILEINFOW, dwlFileSize);
HC_OFFSET(ViewerFileInfo<wchar_t>, colorSpace_, VIEWERPLUGINFILEINFOW, iColorSpace);
HC_OFFSET(VfsInfo<wchar_t>, prefix_, VFSPLUGININFOW, lpszHandlePrefix);
HC_OFFSET(VfsInfo<wchar_t>, smallIcon_, VFSPLUGININFOW, hIconSmall);
HC_OFFSET(VfsFileData, columns_, VFSFILEDATAW, lpvfsColumnData);
HC_OFFSET(VfsReadDirectoryData, files_, VFSREADDIRDATAW, lpFileData);
HC_OFFSET(LoadTextData<wchar_t>, abort_, DVPLOADTEXTDATAW, hAbortEvent);
static_assert(VfsReadOnly == VFSCAPABILITY_READONLY);
static_assert(ViewerResize == DVPLUGINMSG_RESIZE && ViewerLoadW == DVPLUGINMSG_LOADW);
static_assert(ViewerPreview == DVPCVF_Preview && ViewerSetAbort == DVPLUGINMSG_SETABORTEVENT);
static_assert(std::is_same_v<VfsCreate, PFNVFSCREATE>);
static_assert(std::is_same_v<VfsOpenFile, PFNVFSCREATEFILEW>);
static_assert(std::is_same_v<VfsReadFile, PFNVFSREADFILE>);
static_assert(std::is_same_v<VfsWriteFile, PFNVFSWRITEFILE>);
static_assert(std::is_same_v<VfsSetTime, PFNVFSSETFILETIMEW>);
static_assert(std::is_same_v<CreateViewer, PFNDVPCREATEVIEWER>);
int main() { return 0; }
