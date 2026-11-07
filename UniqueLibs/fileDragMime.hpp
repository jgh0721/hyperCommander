#pragma once

#include <QDir>
#include <QMimeData>
#include <QUrl>
#include <QWindowsMimeConverter>
#include <qt_windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <cstring>

namespace hc::ui {
// File drags must remain file drops. Qt's URI converter also advertises Internet
// URLs, which media players can prefer to CF_HDROP and try to play as a URL.
// Keep urls() for same-process panel drops, but export only the native file list.
class FileDragMime final : public QMimeData {
public:
    explicit FileDragMime(const QStringList& Paths) : converter_(this) {
        QList<QUrl> Urls;
        for (const auto& Path : Paths) Urls.append(QUrl::fromLocalFile(Path));
        setUrls(Urls);
        setData(marker(), {});
    }
    QStringList formats() const override {
        auto Formats = QMimeData::formats(); Formats.removeAll("text/uri-list"); return Formats;
    }
    bool hasFormat(const QString& Format) const override {
        return Format != "text/uri-list" && QMimeData::hasFormat(Format);
    }
private:
    static QString marker() { return QStringLiteral("application/x-hypercommander-native-files"); }
public:
    class Converter final : public QWindowsMimeConverter {
    public:
        explicit Converter(const QMimeData* Owner) : owner_(Owner) {}
        bool canConvertFromMime(const FORMATETC& Format, const QMimeData* Mime) const override {
            return Mime == owner_ && Format.cfFormat == CF_HDROP && Format.dwAspect == DVASPECT_CONTENT
                && Format.lindex == -1 && (Format.tymed & TYMED_HGLOBAL);
        }
        bool convertFromMime(const FORMATETC& Format, const QMimeData* Mime, STGMEDIUM* Medium) const override {
            if (!Medium || !canConvertFromMime(Format, Mime)) return false;
            QString Files;
            for (const auto& Url : Mime->urls()) {
                const auto Path = QDir::toNativeSeparators(Url.toLocalFile());
                if (!Url.isLocalFile() || !QDir::isAbsolutePath(Path) || Path.contains(QChar::Null)) return false;
                Files += Path; Files += QChar::Null;
            }
            if (Files.isEmpty()) return false;
            Files += QChar::Null;
            const auto Size = sizeof(DROPFILES) + size_t(Files.size()) * sizeof(char16_t);
            const auto Data = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, Size);
            if (!Data) return false;
            auto* Drop = static_cast<DROPFILES*>(GlobalLock(Data));
            if (!Drop) { GlobalFree(Data); return false; }
            Drop->pFiles = sizeof(DROPFILES); Drop->fWide = TRUE;
            std::memcpy(reinterpret_cast<char*>(Drop) + sizeof(DROPFILES), Files.utf16(), Size - sizeof(DROPFILES));
            GlobalUnlock(Data);
            Medium->tymed = TYMED_HGLOBAL; Medium->hGlobal = Data; Medium->pUnkForRelease = nullptr;
            return true;
        }
        QList<FORMATETC> formatsForMime(const QString& Type, const QMimeData* Mime) const override {
            if (Mime != owner_ || Type != marker()) return {};
            return {{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
        }
        bool canConvertToMime(const QString&, IDataObject*) const override { return false; }
        QVariant convertToMime(const QString&, IDataObject*, QMetaType) const override { return {}; }
        QString mimeForFormat(const FORMATETC&) const override { return {}; }
    private:
        const QMimeData* owner_;
    };
private:
    Converter converter_;
};
}
