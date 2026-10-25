#include "stdafx.h"
#include "zipArchive.hpp"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QSet>
#include <QStringConverter>

namespace hc::compare
{
    namespace
    {
        struct Extraction
        {
            const Budget& limits_; quint64 size_, received_ = 0; bool hashOnly_, stopped_ = false;
            QCryptographicHash hash_{QCryptographicHash::Sha256}; QByteArray bytes_;
        };
        size_t receive(void* Payload, mz_uint64 Offset, const void* Bytes, size_t Size)
        {
            auto& state = *static_cast<Extraction*>(Payload);
            if ((state.limits_.canceled_ && state.limits_.canceled_->load()) || std::chrono::steady_clock::now() >= state.limits_.deadline_)
            { state.stopped_ = true; return 0; }
            if (Offset != state.received_ || state.received_ > state.size_ || Size > state.size_ - state.received_) return 0;
            state.hash_.addData(QByteArrayView(static_cast<const char*>(Bytes), qsizetype(Size)));
            if (!state.hashOnly_) state.bytes_.append(static_cast<const char*>(Bytes), qsizetype(Size));
            state.received_ += Size; return Size;
        }
    }
    ZipArchive::ZipArchive(QByteArray Source, const Budget& Limits) : source_(std::move(Source))
    {
        Limits.Check(); if (source_.size() > 16 * 1024 * 1024) throw Failure("ArchiveInputLimit");
        if (!mz_zip_reader_init_mem(&archive_, source_.constData(), std::size_t(source_.size()), MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) throw Failure("InvalidZip");
        try
        {
            const auto count = mz_zip_reader_get_num_files(&archive_); if (count > 10000) throw Failure("ArchiveEntryLimit");
            QSet<QString> names; quint64 expanded = 0;
            for (mz_uint i = 0; i < count; ++i)
            {
                Limits.Check(); mz_zip_archive_file_stat stat{};
                if (!mz_zip_reader_file_stat(&archive_, i, &stat)) throw Failure("InvalidZipEntry");
                if (stat.m_is_encrypted) throw Failure("EncryptedZipUnsupported");
                if (!stat.m_is_supported) throw Failure("ZipCompressionUnsupported");
                const auto unixType = (stat.m_external_attr >> 16) & 0170000;
                if (unixType && unixType != 0100000 && unixType != 0040000) throw Failure("ZipSpecialEntryUnsupported");
                if (stat.m_uncomp_size > 16 * 1024 * 1024 || expanded > 64 * 1024 * 1024 - stat.m_uncomp_size) throw Failure("ArchiveExpandedLimit");
                expanded += stat.m_uncomp_size;
                const auto length = mz_zip_reader_get_filename(&archive_, i, nullptr, 0);
                if (length < 2 || length > 1025) throw Failure("ZipNameLimit");
                QByteArray raw(qsizetype(length), '\0'); if (mz_zip_reader_get_filename(&archive_, i, raw.data(), length) != length) throw Failure("InvalidZipName"); raw.chop(1);
                if (raw.contains('\0')) throw Failure("InvalidZipName");
                if (!(stat.m_bit_flag & (1 << 11)) && std::any_of(raw.begin(), raw.end(), [](char Value) { return quint8(Value) > 127; })) throw Failure("ZipNameEncodingUnsupported");
                QStringDecoder decoder(QStringDecoder::Utf8); auto path = QString(decoder(raw)); if (decoder.hasError()) throw Failure("ZipNameEncodingUnsupported");
                if (path.startsWith('/') || path.contains('\\') || path.contains(':')) throw Failure("ZipUnsafePath");
                if (stat.m_is_directory && path.endsWith('/')) path.chop(1);
                for (const auto& component : path.split('/'))
                {
                    if (component.isEmpty() || component == "." || component == "..") throw Failure("ZipUnsafePath");
                    for (auto character : component) if (character.unicode() < 32 || character.unicode() == 127) throw Failure("ZipUnsafePath");
                }
                if (names.contains(path)) throw Failure("ZipDuplicateNameUnsupported"); names.insert(path);
                if (stat.m_is_directory && stat.m_uncomp_size) throw Failure("ZipDirectoryPayloadUnsupported");
                if (!mz_zip_validate_file(&archive_, i, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY)) throw Failure("ZipHeaderMismatch");
                entries_.append({path, int(i), bool(stat.m_is_directory), stat.m_uncomp_size});
            }
            digest_ = QString::fromLatin1(QCryptographicHash::hash(source_, QCryptographicHash::Sha256).toHex());
        }
        catch (...) { mz_zip_reader_end(&archive_); throw; }
    }
    ZipArchive::~ZipArchive() { mz_zip_reader_end(&archive_); }
    QByteArray ZipArchive::extract(int Ordinal, const Budget& Limits, bool HashOnly)
    {
        Limits.Check(); if (Ordinal < 0 || Ordinal >= entries_.size() || entries_[Ordinal].directory_) throw Failure("ZipEntryUnavailable");
        Extraction state{Limits, entries_[Ordinal].size_, 0, HashOnly};
        if (!mz_zip_reader_extract_to_callback(&archive_, mz_uint(Ordinal), receive, &state, 0) || state.received_ != state.size_)
        { if (state.stopped_) { Limits.Check(); throw Failure("ArchiveTimeout"); } throw Failure("ZipContentInvalid"); }
        Limits.Check(); return HashOnly ? state.hash_.result().toHex() : state.bytes_;
    }
    QByteArray ZipArchive::Read(int Ordinal, const Budget& Limits) { return extract(Ordinal, Limits, false); }
    QString ZipArchive::Hash(int Ordinal, const Budget& Limits) { return QString::fromLatin1(extract(Ordinal, Limits, true)); }
    QJsonObject ZipArchive::Location(int Ordinal, const QString& Digest) const
    {
        if (Ordinal < 0 || Ordinal >= entries_.size()) throw Failure("ZipEntryUnavailable"); const auto& entry = entries_[Ordinal];
        return {{"schema", "archive.entry.v1"}, {"version", 1}, {"resourceRevision", "sha256:" + digest_},
            {"payload", QJsonObject{{"ordinal", Ordinal}, {"path", entry.path_}, {"contentSha256", Digest},
                {"locator", QJsonObject{{"kind", "archive"}, {"version", 1}, {"containerSha256", digest_}, {"entryOrdinal", Ordinal}, {"entryPath", entry.path_}}}}}};
    }
}
