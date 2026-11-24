#pragma once

/// 플랫폼/프레임워크 헤더파일
#include <QByteArray>
#include <QImage>
#include <QSize>

// Loaded from the same Qt image plugin that supplies the final QImageReader decode.
#ifdef HC_JXL_PREVIEW_EXPORTS
#define HC_JXL_PREVIEW_API Q_DECL_EXPORT
#else
#define HC_JXL_PREVIEW_API
#endif
extern "C" {
    HC_JXL_PREVIEW_API void* HC_CreateJxlPreview(const QSize* Bounds);
    HC_JXL_PREVIEW_API void HC_DestroyJxlPreview(void* State);
    // Bytes contains the received prefix. Return true while the decoder needs more input.
    HC_JXL_PREVIEW_API bool HC_UpdateJxlPreview(void* State, const QByteArray* Bytes, QImage* Image);
}
#undef HC_JXL_PREVIEW_API
