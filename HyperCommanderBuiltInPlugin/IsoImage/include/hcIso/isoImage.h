// SPDX-License-Identifier: LGPL-2.1-only
// Copyright (C) 2015-2022 Chris Lalancette; C++ modifications (C) 2026 HyperCommander contributors.
#pragma once
#include <stdint.h>

#if defined(_WIN32)
#if defined(HC_ISO_BUILD)
#define HC_ISO_API __declspec(dllexport)
#else
#define HC_ISO_API __declspec(dllimport)
#endif
#else
#define HC_ISO_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HcIsoImage HcIsoImage;
enum HcIsoStatus { HC_ISO_OK, HC_ISO_INVALID, HC_ISO_IO, HC_ISO_UNSUPPORTED, HC_ISO_CANCELLED, HC_ISO_END };
enum HcIsoNamespace { HC_ISO_AUTO, HC_ISO_PRIMARY, HC_ISO_JOLIET, HC_ISO_UDF };
// HC_ISO_UDF_BRIDGE includes Joliet; NO_JOLIET preserves longer UDF names.
enum HcIsoProfile { HC_ISO_9660, HC_ISO_9660_JOLIET, HC_ISO_UDF_BRIDGE, HC_ISO_UDF_NO_JOLIET };
// Returning zero cancels. Callbacks run synchronously on the calling thread.
typedef int (*HcIsoProgress)(void* Context, uint64_t Completed, uint64_t Total);
typedef struct HcIsoSource {
    const char* diskPath_; // UTF-8, null for an empty directory
    const char* imagePath_; // relative UTF-8, '/' separator
    uint32_t directory_;
} HcIsoSource;
typedef struct HcIsoOptions {
    uint32_t structSize_;
    uint32_t profile_;
    const char* volumeLabel_; // ASCII, max 32 ISO / 16 Joliet / 30 UDF without Joliet
    uint64_t maxImageBytes_; // zero selects 128 GiB
    int64_t timestampUtc_; // seconds since epoch; zero selects a reproducible epoch
    HcIsoProgress progress_;
    void* context_;
} HcIsoOptions;

enum HcIsoBuildFlags { HC_ISO_SHARE_DUPLICATE_CONTENTS = 1, HC_ISO_EXPLICIT_TIMESTAMP = 2 };
typedef struct HcIsoOptionsEx {
    uint32_t structSize_;
    uint32_t version_; // 1; unknown versions and flags are rejected.
    HcIsoOptions base_;
    uint32_t flags_;
} HcIsoOptionsEx;
typedef struct HcIsoInfo {
    uint32_t structSize_;
    uint32_t namespaces_; // bit (1 << HcIsoNamespace)
    uint32_t selectedNamespace_;
    uint32_t udfRevision_; // on-disc domain identifier, not requested writer profile
    uint64_t imageBytes_;
    uint64_t entryCount_;
    char volumeLabel_[129];
    uint32_t features_; // bit 0: boot record; bit 1: primary system-use metadata
} HcIsoInfo;
typedef struct HcIsoEntry {
    uint32_t structSize_;
    uint32_t directory_;
    uint64_t size_;
    const char* path_; // borrowed until hcIsoClose
} HcIsoEntry;

// Borrowed random-access source; callbacks and Context must survive hcIsoClose.
// Calls are synchronous. Return an HcIsoStatus; short reads are allowed, but a
// successful nonempty request must make progress. Serialize calls per image.
typedef int (*HcIsoReadAt)(void* Context, uint64_t Offset, void* Buffer, uint32_t Capacity, uint32_t* Read);
typedef struct HcIsoInput {
    uint32_t structSize_;
    uint64_t size_;
    HcIsoReadAt readAt_;
    void* context_;
} HcIsoInput;

HC_ISO_API const char* hcIsoLastError(void);
HC_ISO_API int hcIsoOpen(const char* Path, uint32_t Namespace, HcIsoImage** Image);
HC_ISO_API int hcIsoOpenInput(const HcIsoInput* Input, uint32_t Namespace, HcIsoImage** Image);
HC_ISO_API void hcIsoClose(HcIsoImage* Image);
HC_ISO_API int hcIsoGetInfo(const HcIsoImage* Image, HcIsoInfo* Info);
HC_ISO_API int hcIsoGetEntry(const HcIsoImage* Image, uint64_t Index, HcIsoEntry* Entry);
// Root directory uses UINT64_MAX; HC_ISO_END means no such path/child.
HC_ISO_API int hcIsoFindEntry(const HcIsoImage* Image, const char* Path, uint64_t* Index);
HC_ISO_API int hcIsoGetChild(const HcIsoImage* Image, uint64_t Directory, uint64_t Child, uint64_t* Index);
HC_ISO_API int hcIsoRead(HcIsoImage* Image, uint64_t Index, uint64_t Offset,
                       void* Buffer, uint32_t Capacity, uint32_t* Read);
// Creates a new staging path exclusively; never replaces an existing target.
// Reopens every generated namespace and compares all payload bytes before success.
HC_ISO_API int hcIsoBuild(const char* StagingPath, const HcIsoSource* Sources,
                        uint64_t Count, const HcIsoOptions* Options);
HC_ISO_API int hcIsoBuildEx(const char* StagingPath, const HcIsoSource* Sources,
                          uint64_t Count, const HcIsoOptionsEx* Options);

#ifdef __cplusplus
}
#endif
