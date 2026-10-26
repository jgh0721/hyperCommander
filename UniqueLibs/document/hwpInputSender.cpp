#include "hwpStartupTrace.hpp"
#include "hwpInputSender.hpp"
#include "hwpInputInspection.hpp"
#include "commonLibs/storage/materializedContent.hpp"
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace hc::document
{
    storage::Result<SourceIdentity> PrepareHwpSource(storage::IMaterializer& Materializer,
        storage::MaterializationRequest Request, storage::OperationContext Context)
    {
        startup::Scope Preparation("source.materializeAndHash");
        using namespace storage;
        if (auto Stop = Context.Check({"rhwp"})) return *Stop;
        if (!Request.source_) return Error{ErrorCategory::InvalidRequest, "rhwp-source-required"};
        const auto Entry = Request.source_->Describe();
        const auto Format = QFileInfo(QString::fromUtf8(Entry.name_)).suffix().toLower();
        if (Entry.directory_ || (Format != "hwp" && Format != "hwpx"))
            return Error{ErrorCategory::InvalidRequest, "rhwp-source-format"};
        if (!Entry.size_)
            return Error{ErrorCategory::BudgetExceeded, "rhwp-input-budget"};
        // Native materialization verifies the captured revision before and after
        // copying and retains parent leases. Never lend the original file itself.
        startup::Scope Copy("source.materialize");
        auto Copied = Materializer.Materialize(std::move(Request), Context).get();Copy.End(Entry.size_,1);
        if (!Copied.HasValue()) return Copied.Failure();
        if (auto Stop = Context.Check({"rhwp"})) return *Stop;
        SourceIdentity Source; Source.entry_ = Entry; Source.lease_ = std::move(Copied.Value());
        if (!Source.lease_) return Error{ErrorCategory::Unavailable, "rhwp-source-lease"};
        Source.localPath_ = Source.lease_->LocalPath();
        QFile File(QString::fromUtf8(Source.localPath_));
        if (!File.open(QIODevice::ReadOnly)) return Error{ErrorCategory::AccessDenied, "rhwp-input-open"};
        if (File.size() != static_cast<qint64>(Entry.size_))
            return Error{ErrorCategory::StaleRevision, "rhwp-source-size"};
        startup::Scope Hashing("source.snapshotReadAndHash");
        QCryptographicHash Hash(QCryptographicHash::Sha256);
        quint64 Total = 0;
        while (!File.atEnd()) {
            if (auto Stop = Context.Check({"rhwp"})) return *Stop;
            const auto Chunk = File.read(192 * 1024);
            if (Chunk.isEmpty()) return Error{ErrorCategory::Unavailable, "rhwp-input-read"};
            Total += Chunk.size();
            Hash.addData(Chunk);
        }
        if (File.error() != QFileDevice::NoError || Total != Entry.size_)
            return Error{ErrorCategory::StaleRevision, "rhwp-source-size"};
        if (auto Stop = Context.Check({"rhwp"})) return *Stop;
        Source.inputSha256_ = Hash.result().toHex().toStdString();Hashing.End(Total,1);
        // HwpAdmittedInput reopens with a native read lock, verifies this hash,
        // and checks the container before any IPC. Acquisition is not admission.
        return Source;
    }

    std::unique_ptr<HwpAdmittedInput> HwpAdmittedInput::Read(const DocumentOperationTicket& Ticket,
        const SourceIdentity& Source, quint64 BindingOwner, quint64 BindingGeneration,
        quint64 Epoch, const QString& Format, QString& Error)
    {
        startup::Scope Admission("editor.inputAdmission");
        Error = "rhwp-input-authority";
        const auto Id = QString::fromUtf8(Ticket.documentId_);
        const auto Sha = QByteArray::fromStdString(Source.inputSha256_);
        if(Ticket.operation_ != DocumentOperation::Loading || !Ticket.generation_ || !Ticket.token_
            || Id.isEmpty() || Id.size() > 128 || Id.toUtf8().toStdString() != Ticket.documentId_
            || !Ticket.lease_ || Ticket.lease_ != Source.lease_
            || !BindingOwner || !BindingGeneration || !Epoch
            || (Format != "hwp" && Format != "hwpx")
            || Sha.size() != 64 || QByteArray::fromHex(Sha).toHex() != Sha) return {};
        const auto LeasePath = Ticket.lease_->LocalPath();
        if(LeasePath != Source.localPath_ || Ticket.lease_->Describe().directory_) return {};
        const auto Path = QDir::fromNativeSeparators(QString::fromUtf8(LeasePath));
        // Only native materialized local drive paths. No device/UNC/ADS aliases.
        if(QString::fromUtf8(LeasePath).toUtf8().toStdString() != LeasePath
            || Path.size() < 4 || !((Path[0] >= 'A' && Path[0] <= 'Z') || (Path[0] >= 'a' && Path[0] <= 'z'))
            || Path[1] != ':' || Path[2] != '/' || Path.mid(2).contains(':')
            || Path.contains(QChar(0)) || QDir::cleanPath(Path) != Path) return {};
        const auto Native = QDir::toNativeSeparators(Path);
        Error = "rhwp-input-open";
        const auto Handle = CreateFileW(reinterpret_cast<LPCWSTR>(Native.utf16()), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if(Handle == INVALID_HANDLE_VALUE) return {};
        std::shared_ptr<void> File(Handle, [](void* Value) { CloseHandle(Value); });
        BY_HANDLE_FILE_INFORMATION Info{};
        LARGE_INTEGER Size{};
        if(GetFileType(Handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(Handle, &Info)
            || (Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            || !GetFileSizeEx(Handle, &Size)) return {};
        Error = "rhwp-input-budget";
        if(Size.QuadPart < 1 || quint64(Size.QuadPart)>quint64(std::numeric_limits<qsizetype>::max())) return {};
        auto Input = std::unique_ptr<HwpAdmittedInput>(new HwpAdmittedInput);
        Input->ticket_ = Ticket; Input->file_ = std::move(File);
        startup::Scope HashRead("input.lockedReadHash");
        Input->bytes_.resize(static_cast<qsizetype>(Size.QuadPart));
        qsizetype Offset = 0;QCryptographicHash Hash(QCryptographicHash::Sha256);
        Error = "rhwp-input-read";
        while(Offset < Input->bytes_.size()) {
            DWORD Count = 0;
            const auto Wanted = static_cast<DWORD>(qMin<qsizetype>(192 * 1024, Input->bytes_.size() - Offset));
            if(!ReadFile(Handle, Input->bytes_.data() + Offset, Wanted, &Count, nullptr) || !Count) return {};
            Hash.addData(QByteArrayView(Input->bytes_.constData()+Offset,Count));
            Offset += Count;
        }
        Error = "rhwp-input-integrity";
        if(Hash.result().toHex() != Sha) return {};
        // Inspect the exact locked/hash-verified bytes before any IPC transfer.
        // Container admission is not parser success or permission to edit.
        HashRead.End(Input->bytes_.size(),1);
        startup::Scope Container("input.inspectContainer");
        const auto Inspection = InspectHwpInput(Input->bytes_, Format);Container.End(Input->bytes_.size(),1);
        if (!Inspection.accepted_) { Error = Inspection.error_; return {}; }
        Input->restricted_ = Inspection.restricted_;
        Input->sha256_ = Sha; Input->format_ = Format;
        Input->identity_ = {{"documentId", Id}, {"generation", QString::number(Ticket.generation_)},
            {"token", QString::number(Ticket.token_)}, {"bindingOwner", QString::number(BindingOwner)},
            {"bindingGeneration", QString::number(BindingGeneration)}, {"epoch", QString::number(Epoch)}};
        Error.clear(); return Input;
    }

    bool HwpAdmittedInput::Send(const Exchange& Transport, const std::function<bool()>& Cancelled,
        QString& Error, int TimeoutMs) const
    {
        startup::Scope Transfer("editor.inputSend");
        Error = "rhwp-input-send-policy";
        if(!Transport || !Cancelled || TimeoutMs < 1 || TimeoutMs > 120000) return false;
        QDeadlineTimer Deadline(TimeoutMs, Qt::PreciseTimer);
        // Transport failures/cancellation leave cleanup to the owning worker.
        // InputDiscarded would only acknowledge byte disposal, not retirement.
        const auto RoundTrip = [&](QJsonObject Request, const char* Type, qsizetype Received) {
            if(Cancelled()) { Error = "rhwp-input-cancelled"; return false; }
            if(Deadline.hasExpired()) { Error = "rhwp-input-timeout"; return false; }
            Request["version"] = 1; Request["identity"] = identity_;
            QByteArray Reply; WebProcessError Failure;
            if(!Transport(QJsonDocument(Request).toJson(QJsonDocument::Compact), Reply,
                static_cast<quint32>(qBound<qint64>(qint64{1}, Deadline.remainingTime(), qint64{30000})), Failure)) {
                Error = "rhwp-input-transport"; return false;
            }
            if(Cancelled()) { Error = "rhwp-input-cancelled"; return false; }
            if(Deadline.hasExpired()) { Error = "rhwp-input-timeout"; return false; }
            Error = "rhwp-input-reply";
            if(Reply.size() > 4096) return false;
            QJsonParseError Parse;
            const auto Document = QJsonDocument::fromJson(Reply, &Parse);
            if(Parse.error != QJsonParseError::NoError || !Document.isObject()) return false;
            const auto Value = Document.object();
            if(Value.size() != 4 || Value["version"] != QJsonValue(1) || Value["type"] != QJsonValue(Type)
                || Value["identity"] != QJsonValue(identity_) || !Value["received"].isDouble()
                || Value["received"].toDouble(-1) != Received) return false;
            return true;
        };
        if(!RoundTrip({{"type", "InputBegin"}, {"format", format_}, {"readOnly", true},
            {"byteLength", bytes_.size()}, {"sha256", QString::fromLatin1(sha256_)}}, "InputAccepted", 0)) return false;
        for(qsizetype Offset = 0; Offset < bytes_.size();) {
            const auto Chunk = bytes_.mid(Offset, 192 * 1024);
            if(!RoundTrip({{"type", "InputChunk"}, {"offset", Offset},
                {"data", QString::fromLatin1(Chunk.toBase64())}}, "InputChunkAccepted", Offset + Chunk.size())) return false;
            Offset += Chunk.size();
        }
        if(!RoundTrip({{"type", "InputCommit"}}, "InputVerified", bytes_.size())) return false;
        Transfer.End(bytes_.size(),1);
        Error.clear(); return true;
    }
    bool HwpAdmittedInput::Send(DocumentWebProcess& Process, const std::function<bool()>& Cancelled,
        QString& Error, int TimeoutMs) const
    {
        return Send([&Process](const QByteArray& Request, QByteArray& Reply, quint32 Timeout, WebProcessError& Failure) {
            return Process.Exchange(Request, Reply, Timeout, Failure);
        }, Cancelled, Error, TimeoutMs);
    }
}
