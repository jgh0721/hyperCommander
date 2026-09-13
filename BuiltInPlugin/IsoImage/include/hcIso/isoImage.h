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
enum HcIsoStatus { HC_ISO_OK, HC_ISO_INVALID, HC_ISO_IO, HC_ISO_UNSUPPORTED, HC_ISO_CANCELLED };
enum HcIsoNamespace { HC_ISO_AUTO, HC_ISO_PRIMARY, HC_ISO_JOLIET, HC_ISO_UDF };
enum HcIsoProfile { HC_ISO_9660, HC_ISO_9660_JOLIET, HC_ISO_UDF_BRIDGE };
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
    const char* volumeLabel_; // ASCII, 1..32 characters
    uint64_t maxImageBytes_; // zero selects 128 GiB
    int64_t timestampUtc_; // seconds since epoch; zero selects a reproducible epoch
    HcIsoProgress progress_;
    void* context_;
} HcIsoOptions;
typedef struct HcIsoInfo {
    uint32_t structSize_;
    uint32_t namespaces_; // bit (1 << HcIsoNamespace)
    uint32_t selectedNamespace_;
    uint32_t udfRevision_; // on-disc domain identifier, not requested writer profile
    uint64_t imageBytes_;
    uint64_t entryCount_;
    char volumeLabel_[129];
} HcIsoInfo;
typedef struct HcIsoEntry {
    uint32_t structSize_;
    uint32_t directory_;
    uint64_t size_;
    const char* path_; // borrowed until hcIsoClose
} HcIsoEntry;

HC_ISO_API const char* hcIsoLastError(void);
HC_ISO_API int hcIsoOpen(const char* Path, uint32_t Namespace, HcIsoImage** Image);
HC_ISO_API void hcIsoClose(HcIsoImage* Image);
HC_ISO_API int hcIsoGetInfo(const HcIsoImage* Image, HcIsoInfo* Info);
HC_ISO_API int hcIsoGetEntry(const HcIsoImage* Image, uint64_t Index, HcIsoEntry* Entry);
HC_ISO_API int hcIsoRead(HcIsoImage* Image, uint64_t Index, uint64_t Offset,
                       void* Buffer, uint32_t Capacity, uint32_t* Read);
// Creates a new staging path exclusively; never replaces an existing target.
// Reopens every generated namespace and compares all payload bytes before success.
HC_ISO_API int hcIsoBuild(const char* StagingPath, const HcIsoSource* Sources,
                        uint64_t Count, const HcIsoOptions* Options);

#ifdef __cplusplus
}
#endif
