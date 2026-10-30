#pragma once
#include "UniqueLibs/nativeFilePath.hpp"
#include <QtCore>
#include "commonLibs/operations/fileOperation.hpp"
#include "commonLibs/storage/writableStorage.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <memory>
#include <vector>
#include <functional>

namespace hc::filetools {
inline QString NativePath(const QString& Path) {
    return storage::native::FilePath(Path);
}
inline QString NameError(const QString& Name) {
    if (Name.isEmpty() || Name.size() > 255 || Name == "." || Name == ".." || Name.endsWith('.') || Name.endsWith(' ')) return QObject::tr("이름이 비었거나 끝 문자·길이가 올바르지 않습니다.");
    for (const auto C : Name) if (C.unicode() < 32 || QStringLiteral("<>:\"/\\|?*").contains(C)) return QObject::tr("파일명에 사용할 수 없는 문자가 있습니다.");
    const auto Base = Name.section('.', 0, 0).toUpper();
    if (QRegularExpression("^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])$").match(Base).hasMatch()) return QObject::tr("Windows 예약 이름입니다.");
    return {};
}
inline bool LocalFolder(const QString& Folder) {
    if (!QRegularExpression("^[A-Za-z]:[/\\\\]").match(Folder).hasMatch()) return false;
    const auto Root = QDir::toNativeSeparators(Folder.left(3));
    const auto Type = GetDriveTypeW(reinterpret_cast<LPCWSTR>(Root.utf16()));
    return Type == DRIVE_FIXED || Type == DRIVE_REMOVABLE || Type == DRIVE_RAMDISK;
}
inline bool NativeFolder(const QString& Folder) {
    const auto Path = QDir::toNativeSeparators(Folder);
    if (Path.startsWith("\\\\") && !Path.startsWith("\\\\?\\") && !Path.startsWith("\\\\.\\")) {
        const auto Parts = Folder.mid(2).split(QRegularExpression("[/\\\\]"), Qt::SkipEmptyParts);
        return Parts.size() >= 2 && !Parts[0].contains(':') && !Parts[1].contains(':');
    }
    if (LocalFolder(Folder)) return true;
    return QRegularExpression("^[A-Za-z]:[/\\\\]").match(Folder).hasMatch()
        && GetDriveTypeW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(Folder.left(3)).utf16())) == DRIVE_REMOTE;
}
struct Handle {
    HANDLE value_ = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE Value) : value_(Value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
inline QByteArray Identity(HANDLE File) {
    BY_HANDLE_FILE_INFORMATION Info{};
    if (!GetFileInformationByHandle(File, &Info)) return {};
    return QByteArray::number(Info.dwVolumeSerialNumber) + ':' + QByteArray::number(Info.nFileIndexHigh) + ':' + QByteArray::number(Info.nFileIndexLow);
}
inline DWORD RenameHandle(HANDLE File, const QString& Destination) {
    const auto Path = NativePath(Destination); const auto Bytes = Path.size() * sizeof(wchar_t);
    std::vector<unsigned char> Buffer(sizeof(FILE_RENAME_INFO) + Bytes);
    auto* Info = reinterpret_cast<FILE_RENAME_INFO*>(Buffer.data());
    Info->ReplaceIfExists = FALSE; Info->RootDirectory = nullptr; Info->FileNameLength = DWORD(Bytes);
    memcpy(Info->FileName, Path.utf16(), Bytes);
    return SetFileInformationByHandle(File, FileRenameInfo, Info, DWORD(Buffer.size())) ? ERROR_SUCCESS : GetLastError();
}
struct Entry { QString path_; QByteArray identity_; QDateTime modified_; bool directory_ = false; quint64 size_ = 0; QDateTime captured_; };
inline Entry Inspect(const QString& Path) {
    Entry E; E.path_ = QDir::cleanPath(Path);
    const auto Native = NativePath(E.path_);
    Handle File(CreateFileW(reinterpret_cast<LPCWSTR>(Native.utf16()), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (File.value_ == INVALID_HANDLE_VALUE) return E;
    BY_HANDLE_FILE_INFORMATION Info{};
    if (!GetFileInformationByHandle(File.value_, &Info) || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return E;
    E.size_ = (quint64(Info.nFileSizeHigh) << 32) | Info.nFileSizeLow;
    E.identity_ = Identity(File.value_); E.directory_ = Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    const quint64 Ticks = (quint64(Info.ftLastWriteTime.dwHighDateTime) << 32) | Info.ftLastWriteTime.dwLowDateTime;
    E.modified_ = QDateTime::fromMSecsSinceEpoch(qint64(Ticks / 10000) - 11644473600000LL).toLocalTime();
    return E;
}
inline QString PinNativeParents(const QString& Folder, std::vector<std::unique_ptr<Handle>>& Pins) {
    if (!NativeFolder(Folder)) return QObject::tr("로컬·SMB 파일 경로만 지원합니다.");
    const auto Path = QDir::toNativeSeparators(QDir::cleanPath(Folder)); int End = 3;
    if (Path.startsWith("\\\\")) {
        const auto Server = Path.indexOf('\\',2); End = Server < 0 ? -1 : Path.indexOf('\\',Server+1);
        if (End < 0) End = Path.size();
    }
    QString Current = Path.left(End); QStringList Parents{Current};
    for (const auto& Part : Path.mid(End).split('\\',Qt::SkipEmptyParts)) { if (!Current.endsWith('\\')) Current += '\\'; Current += Part; Parents.append(Current); }
    for (const auto& Parent : Parents) {
        const auto Native = NativePath(Parent);
        auto Pin = std::make_unique<Handle>(CreateFileW(reinterpret_cast<LPCWSTR>(Native.utf16()),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if (Pin->value_ == INVALID_HANDLE_VALUE) return QObject::tr("폴더 잠금 실패: Windows 오류 %1 · %2").arg(GetLastError()).arg(Parent);
        BY_HANDLE_FILE_INFORMATION Info{};
        if (!GetFileInformationByHandle(Pin->value_, &Info) || !(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return QObject::tr("링크 폴더 또는 폴더 확인 실패: %1").arg(Parent);
        Pins.push_back(std::move(Pin));
    }
    return {};
}
struct Template { QString label_, extension_, fixedName_, text_; };
inline QList<Template> Templates() {
    return {{QObject::tr("빈 파일"), {}, {}, {}}, {QObject::tr("텍스트 문서"), "txt", {}, {}},
        {"Markdown", "md", {}, "# 새 문서\r\n"}, {QObject::tr("C++ 소스"), "cpp", {}, "// C++ source\r\n"},
        {QObject::tr("C++ 헤더"), "h", {}, "#pragma once\r\n"}, {"CMake", {}, "CMakeLists.txt", "# CMake project\r\n"},
        {"JSON", "json", {}, "{\r\n}\r\n"}, {QObject::tr("Qt 디자이너 폼"), "ui", {}, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<ui version=\"4.0\"><class>Form</class><widget class=\"QWidget\" name=\"Form\"/><resources/><connections/></ui>\r\n"}};
}
inline QString TemplateName(const QString& Current, int Index) {
    const auto T = Templates().value(Index); if (!T.fixedName_.isEmpty()) return T.fixedName_;
    auto Stem = QFileInfo(Current).completeBaseName(); if (Stem.isEmpty() || Current == "CMakeLists.txt") Stem = QObject::tr("새 파일");
    return Stem + (T.extension_.isEmpty() ? QString() : '.' + T.extension_);
}
inline QString Create(const QString& Folder, const QString& Name, int Type, bool Bom) {
    if (!LocalFolder(Folder)) return QObject::tr("현재는 로컬 드라이브에서만 새 파일을 만들 수 있습니다.");
    if (const auto Error = NameError(Name); !Error.isEmpty()) return Error;
    if (Type < 0 || Type >= Templates().size()) return QObject::tr("알 수 없는 템플릿입니다.");
    std::vector<std::unique_ptr<Handle>> ParentPins;
    if (const auto Error = PinNativeParents(Folder, ParentPins); !Error.isEmpty()) return Error;
    const auto Destination = NativePath(QDir(Folder).filePath(Name));
    Handle File(CreateFileW(reinterpret_cast<LPCWSTR>(Destination.utf16()), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (File.value_ == INVALID_HANDLE_VALUE) return QObject::tr("생성 실패: 기존 파일·권한을 확인하세요. Windows 오류 %1").arg(GetLastError());
    const auto Data = (Bom && Type != 0 ? QByteArray::fromHex("efbbbf") : QByteArray()) + Templates()[Type].text_.toUtf8();
    DWORD Written = 0;
    if (!WriteFile(File.value_, Data.constData(), DWORD(Data.size()), &Written, nullptr) || Written != Data.size() || !FlushFileBuffers(File.value_)) {
        const auto Code = GetLastError(); FILE_DISPOSITION_INFO Delete{TRUE};
        const bool Cleaned = SetFileInformationByHandle(File.value_, FileDispositionInfo, &Delete, sizeof(Delete));
        return QObject::tr("쓰기 실패: Windows 오류 %1. %2").arg(Code).arg(Cleaned ? QObject::tr("미완성 파일 정리됨") : QObject::tr("미완성 파일이 남아 있습니다"));
    }
    return {};
}
// Reads only classic TIFF/JPEG APP1 metadata, never image pixels or an unbounded
// file. Missing/invalid DateTimeOriginal is explicit, never silently mtime.
inline QDateTime ExifDate(const QByteArray& Data) {
    QByteArray Tiff;
    if (Data.startsWith("II") || Data.startsWith("MM")) Tiff = Data;
    else if (Data.startsWith(QByteArray::fromHex("ffd8"))) {
        qsizetype Pos = 2;
        while (Pos + 4 <= Data.size()) {
            if (uchar(Data[Pos++]) != 0xff) return {};
            while (Pos < Data.size() && uchar(Data[Pos]) == 0xff) ++Pos;
            if (Pos >= Data.size()) return {};
            const auto Marker = uchar(Data[Pos++]);
            if (Marker == 0xda || Marker == 0xd9) break;
            if (Marker == 0x01 || (Marker >= 0xd0 && Marker <= 0xd7)) continue;
            if (Pos + 2 > Data.size()) return {};
            const auto Length = (uchar(Data[Pos]) << 8) | uchar(Data[Pos+1]);
            if (Length < 2 || Length > Data.size() - Pos) return {};
            if (Marker == 0xe1 && Data.mid(Pos+2, 6) == QByteArray("Exif\0\0", 6)) { Tiff = Data.mid(Pos+8, Length-8); break; }
            Pos += Length;
        }
    }
    if (Tiff.size() < 8 || (Tiff.left(2) != "II" && Tiff.left(2) != "MM")) return {};
    const bool Little = Tiff.startsWith("II");
    const auto U16 = [&](quint64 At) -> quint32 {
        if (At > quint64(Tiff.size()) || quint64(Tiff.size()) - At < 2) return 0;
        return Little ? uchar(Tiff[At]) | (uchar(Tiff[At+1]) << 8) : (uchar(Tiff[At]) << 8) | uchar(Tiff[At+1]);
    };
    const auto U32 = [&](quint64 At) -> quint32 { return Little ? U16(At) | (U16(At+2) << 16) : (U16(At) << 16) | U16(At+2); };
    if (U16(2) != 42) return {};
    quint64 Offset = U32(4), Exif = 0; QByteArray Date;
    for (int Pass = 0; Pass < 2; ++Pass) {
        if (!Offset || Offset > quint64(Tiff.size()) || quint64(Tiff.size()) - Offset < 2) return {};
        const auto Count = U16(Offset);
        if (Count > 512 || quint64(Tiff.size()) - Offset < 2 + quint64(Count) * 12) return {};
        for (quint32 I = 0; I < Count; ++I) {
            const auto At = Offset + 2 + quint64(I) * 12; const auto Tag = U16(At), Type = U16(At+2), Size = U32(At+4);
            if (Tag == 0x8769 && Type == 4 && Size == 1) Exif = U32(At+8);
            if (Pass == 1 && Tag == 0x9003 && Type == 2 && Size == 20) {
                const quint64 Value = U32(At+8);
                if (Value > quint64(Tiff.size()) || quint64(Tiff.size()) - Value < Size) return {};
                Date = Tiff.mid(Value, Size);
            }
        }
        Offset = Exif;
    }
    if (Date.size() != 20 || Date.back() != '\0') return {};
    const auto Text = QString::fromLatin1(Date.constData(), 19);
    const auto DatePart = QDate::fromString(Text.left(10), "yyyy:MM:dd");
    const auto TimePart = QTime::fromString(Text.mid(11), "HH:mm:ss");
    // Preserve camera wall time independently of the PC timezone/DST rules.
    const auto Value = QDateTime(DatePart, TimePart, QTimeZone::UTC);
    if (!Value.isValid() || Value.toString("yyyy:MM:dd HH:mm:ss") != Text) return {};
    return Value;
}
inline QDateTime ReadExifDate(const QString& Path) {
    QFile File(Path); if (!File.open(QIODevice::ReadOnly)) return {};
    return ExifDate(File.read(1024 * 1024));
}
// Provider capability, staging and commit run on the scheduler, never the UI.
// Commit is entered only after the content is complete; cancellation before it
// aborts the private stage and cannot expose a partially written destination.
class CreateTask final : public operations::IJobTask {
public:
    CreateTask(std::shared_ptr<storage::IWritableStorage> Writer, storage::Locator Target, QByteArray Data, std::shared_ptr<operations::IFileElevationService> Elevation = {}, bool AskForName = false)
        : writer_(std::move(Writer)), target_(std::move(Target)), data_(std::move(Data)), elevation_(std::move(Elevation)), askForName_(AskForName) {}
    operations::JobStep Advance(operations::JobStepContext Context) override {
        using namespace storage; using namespace operations;
        const auto Fail = [&](Error E) {
            if(E.category_==ErrorCategory::AccessDenied && elevation_ && phase_<100) {
                FileElevationRequest Request;Request.jobId_=Context.jobId_;Request.action_=FileElevationAction::CreateContent;Request.destination_=target_;
                Request.content_.assign(reinterpret_cast<const std::byte*>(data_.constData()),reinterpret_cast<const std::byte*>(data_.constData()+data_.size()));
                if(elevation_->CanExecute(Request)) {
                    if(session_) {auto Aborted=session_->Abort();if(!Aborted.HasValue()){JobStep S{JobState::Failed};S.error_=Aborted.Failure();return S;}session_.reset();}
                    permissionError_=E;permissionRequest_=std::move(Request);phase_=100;return JobStep{JobState::Running};
                }
            }
            JobStep S{JobState::Failed}; S.error_=std::move(E); return S;
        };
        if(child_)return child_->Advance(Context);
        if(phase_==101) {
            if(!Context.answer_) {
                JobStep S{JobState::WaitingForUser}; S.interaction_=InteractionRequest{"",InteractionKind::Rename,"destination-exists",
                    {InteractionChoice::Rename,InteractionChoice::Cancel},{},{},Serialize(target_)}; return S;
            }
            const auto Name=QString::fromStdString(Context.answer_->value_);
            if(Context.answer_->choice_!=InteractionChoice::Rename)return JobStep{JobState::Cancelled};
            if(!NameError(Name).isEmpty())return Fail(Error{ErrorCategory::InvalidRequest,"unsafe-destination-name"});
            const auto End=target_.root_.find_last_of("/\\");
            if(End==std::string::npos)return Fail(Error{ErrorCategory::InvalidRequest,"destination-parent-required"});
            target_.root_.resize(End+1);target_.root_+=Name.toStdString();
            phase_=0; return JobStep{JobState::Running};
        }
        if(phase_==100) {
            if(!Context.answer_) {
                JobStep S{JobState::WaitingForUser};S.interaction_=InteractionRequest{"",InteractionKind::Elevation,permissionError_->code_,{InteractionChoice::Elevate,InteractionChoice::Retry,InteractionChoice::Cancel},{},"",target_.root_};return S;
            }
            if(Context.answer_->choice_==InteractionChoice::Retry){phase_=0;return JobStep{JobState::Running};}
            if(Context.answer_->choice_!=InteractionChoice::Elevate)return JobStep{JobState::Cancelled};
            child_=std::make_unique<FileElevationJob>(permissionRequest_,elevation_);return JobStep{JobState::Running};
        }
        auto Operation = Context.operation_; Operation.expectedWriteBytes_ = data_.size();
        JobStep S; S.progress_ = JobProgress{phase_ >= 3 ? std::uint64_t(data_.size()) : 0, std::uint64_t(data_.size()), target_.root_, 0, std::uint64_t(data_.size()), 1};
        S.progress_->destination_ = Serialize(target_);
        S.progress_->phase_ = phase_ == 0 ? JobPhase::Preparing : phase_ >= 3 ? JobPhase::Verifying : JobPhase::Creating;
        if (phase_ == 0) {
            auto Support = writer_->Supports(Mutation::CreateContent, target_, Operation);
            if (!Support.HasValue()) return Fail(Support.Failure());
            if (Support.Value().availability_ == Availability::Unsupported) return Fail(Error{ErrorCategory::Unsupported, Support.Value().reason_.empty() ? "create-content-unavailable" : Support.Value().reason_, writer_->Id()});
            if(askForName_) {
                auto Existing=writer_->Inspect(target_,Operation);if(!Existing.HasValue())return Fail(Existing.Failure());
                if(Existing.Value()) {phase_=101;return JobStep{JobState::Running};}
            }
        } else if (phase_ == 1) {
            auto Started = writer_->BeginWrite(target_, {}, Operation);
            if (!Started.HasValue()) return Fail(Started.Failure()); session_ = std::move(Started.Value());
        } else if (phase_ == 2) {
            const auto Bytes = std::span(reinterpret_cast<const std::byte*>(data_.constData()), std::size_t(data_.size()));
            if (!Bytes.empty()) { auto Wrote = session_->Append(Bytes, Operation); if (!Wrote.HasValue()) return Fail(Wrote.Failure()); }
            S.progress_->bytesDone_ = data_.size(); S.progress_->currentBytes_ = data_.size();
        } else if (phase_ == 3) S.next_ = JobState::Committing;
        else {
            auto Published = session_->Commit(Operation); if (!Published.HasValue()) return Fail(Published.Failure());
            cleanup_ = Published.Value().cleanupErrors_; committed_ = Published.Value().destinationCommitted_;
            if (!committed_) return Fail(Error{ErrorCategory::Unavailable, "create-not-published", writer_->Id()});
            S.next_ = JobState::Completed; S.progress_->bytesDone_ = data_.size(); S.progress_->currentBytes_ = data_.size();
            ItemOutcome Item; Item.item_ = target_.root_; Item.destinationCommitted_ = true; S.item_ = std::move(Item);
        }
        ++phase_; return S;
    }
    std::vector<storage::Error> Finalize(operations::JobState State) override {
        if(child_)return child_->Finalize(State);
        if (session_ && !committed_) { auto Aborted = session_->Abort(); if (!Aborted.HasValue()) cleanup_.push_back(Aborted.Failure()); }
        return cleanup_;
    }
private:
    std::shared_ptr<storage::IWritableStorage> writer_;
    storage::Locator target_;
    QByteArray data_;
    std::unique_ptr<storage::IWriteSession> session_;
    std::vector<storage::Error> cleanup_;
    std::shared_ptr<operations::IFileElevationService> elevation_;
    std::unique_ptr<operations::IJobTask> child_;
    operations::FileElevationRequest permissionRequest_;
    std::optional<storage::Error> permissionError_;
    int phase_ = 0; bool committed_ = false, askForName_ = false;
};
inline storage::Result<operations::PreparedJob> PrepareCreate(std::shared_ptr<storage::IWritableStorage> Writer, storage::Locator Parent,
    const QString& Name, int Type, bool Bom, storage::OperationContext Context = {}, std::shared_ptr<operations::IFileElevationService> Elevation = {}, bool AskForName = false) {
    using namespace storage; using namespace operations;
    if (!Writer || !NameError(Name).isEmpty() || Type < 0 || Type >= Templates().size()
        || (Parent.kind_ != LocatorKind::Local && Parent.kind_ != LocatorKind::Smb && Parent.kind_ != LocatorKind::Plugin))
        return Error{ErrorCategory::InvalidRequest, "invalid-new-file-request"};
    const auto Data = (Bom && Type ? QByteArray::fromHex("efbbbf") : QByteArray()) + Templates()[Type].text_.toUtf8();
    auto Target = Parent;
    // Plugin locators are provider names, not OS filenames. QDir normalization
    // would destroy their root prefix or change separators rejected by WFX.
    if (!Target.root_.ends_with('/') && !Target.root_.ends_with('\\')) Target.root_ += '\\';
    Target.root_ += Name.toStdString();
    PreparedJob Job; Job.request_.operation_ = Context; Job.request_.maxItemResults_ = 256;
    Job.request_.resourceLanes_ = {FileOperationLane(Parent)};
    Job.request_.metadata_.destination_ = Target.root_; Job.request_.metadata_.options_ = "create-new; template=" + std::to_string(Type) + (Bom && Type ? "; UTF-8 BOM" : "; UTF-8");
    Job.request_.metadata_.kind_ = JobKind::MakeFile;
    Job.task_ = std::make_unique<CreateTask>(std::move(Writer), std::move(Target), Data, std::move(Elevation), AskForName); return Job;
}
struct Rules {
    QString name_ = "[N]", extension_ = "[E]", find_, replace_, dateFormat_ = "yyyy-MM-dd";
    bool regex_ = false, caseSensitive_ = false, includeExtension_ = false;
    int dateSource_ = 0, dateOffsetSeconds_ = 0;
    int nameCase_ = 0, extensionCase_ = 0, start_ = 1, step_ = 1, digits_ = 3;
};
struct RenameRow { Entry source_; QString target_, error_; };
inline QList<RenameRow> Plan(const QList<Entry>& Sources, const Rules& Rule, const std::function<bool()>& Stop = {}) {
    QList<RenameRow> Rows; QSet<QString> Original, Targets;
    for (const auto& E : Sources) Original.insert(E.path_.toCaseFolded());
    QRegularExpression Find(Rule.find_, Rule.caseSensitive_ ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
    QString MaskError;
    const auto Expand = [&](const QString& Mask, const Entry& E, int Index) {
        const QFileInfo File(E.path_); const auto Date = (Rule.dateSource_ == 1 ? E.captured_ : E.modified_).addSecs(Rule.dateOffsetSeconds_);
        const QMap<QString, QString> Values{{"N", E.directory_ ? File.fileName() : File.completeBaseName()}, {"E", E.directory_ ? QString() : File.suffix()},
            {"C", QString::number(qint64(Rule.start_) + qint64(Index) * Rule.step_).rightJustified(Rule.digits_, '0')},
            {"P", File.dir().dirName()}, {"Y", Date.toString("yyyy")}, {"M", Date.toString("MM")}, {"D", Date.toString("dd")},
            {"t", Date.toString("HHmmss")}, {"date", Date.toString(Rule.dateFormat_)}};
        QString Out; int Position = 0;
        while (Position < Mask.size()) {
            if (Mask[Position] != '[') { if (Mask[Position] == ']') MaskError = QObject::tr("닫는 괄호 앞에 여는 괄호가 없습니다."); Out += Mask[Position++]; continue; }
            const auto End = Mask.indexOf(']', Position+1);
            if (End < 0) { MaskError = QObject::tr("마스크 괄호를 닫으세요."); break; }
            const auto Token = Mask.mid(Position+1, End-Position-1); QString Value;
            const auto Slice = QRegularExpression("^([NE]):(-?[1-9][0-9]*):([1-9][0-9]*)$").match(Token);
            const auto Range = QRegularExpression("^([NE])([1-9][0-9]*)-([1-9][0-9]*)$").match(Token);
            const auto Counter = QRegularExpression("^C:([+-]?[0-9]+)$").match(Token);
            if (Values.contains(Token)) Value = Values.value(Token);
            else if (Slice.hasMatch() || Range.hasMatch()) {
                const auto Match = Slice.hasMatch() ? Slice : Range; const auto Chars = Values.value(Match.captured(1)).toUcs4();
                bool ValidStart = false, ValidLength = false;
                const auto Start = Match.captured(2).toInt(&ValidStart), Last = Match.captured(3).toInt(&ValidLength);
                const qint64 At = Start > 0 ? qint64(Start)-1 : qint64(Chars.size())+Start;
                const qint64 Count = Slice.hasMatch() ? Last : qint64(Last)-Start+1;
                if (!ValidStart || !ValidLength || Count <= 0 || At < 0 || At >= Chars.size()) MaskError = QObject::tr("부분 문자열 범위를 확인하세요.");
                else { const auto Part = Chars.mid(At, Count); Value = QString::fromUcs4(Part.constData(), Part.size()); }
            } else if (Counter.hasMatch()) {
                bool Valid = false; const auto Offset = Counter.captured(1).toInt(&Valid);
                if (!Valid) MaskError = QObject::tr("카운터 오프셋 범위를 확인하세요.");
                else Value = QString::number(qint64(Rule.start_) + qint64(Index) * Rule.step_ + Offset).rightJustified(Rule.digits_, '0');
            } else MaskError = QObject::tr("알 수 없는 마스크 토큰: [%1]").arg(Token);
            if (QStringList{"Y", "M", "D", "t", "date"}.contains(Token) && !Date.isValid()) MaskError = QObject::tr("선택한 날짜 메타데이터가 없습니다.");
            Out += Value; Position = End+1;
        }
        return Out;
    };
    for (int I = 0; I < Sources.size(); ++I) {
        if (Stop && Stop()) return {};
        RenameRow Row; Row.source_ = Sources[I]; const QFileInfo File(Row.source_.path_);
        MaskError.clear();
        auto Name = Expand(Rule.name_, Row.source_, I), Ext = Expand(Rule.extension_, Row.source_, I);
        if (!Rule.find_.isEmpty()) {
            if (Rule.regex_ && !Find.isValid()) Row.error_ = QObject::tr("정규식 오류: %1").arg(Find.errorString());
            else {
                auto Subject = Name + (Rule.includeExtension_ && !Ext.isEmpty() ? '.' + Ext : QString());
                if (Rule.regex_) Subject.replace(Find, Rule.replace_);
                else Subject.replace(Rule.find_, Rule.replace_, Rule.caseSensitive_ ? Qt::CaseSensitive : Qt::CaseInsensitive);
                if (Rule.includeExtension_ && !Row.source_.directory_) { const auto Dot = Subject.lastIndexOf('.'); Name = Dot > 0 ? Subject.left(Dot) : Subject; Ext = Dot > 0 ? Subject.mid(Dot+1) : QString(); }
                else Name = Subject;
            }
        }
        if (Rule.nameCase_ == 1) Name = Name.toLower(); else if (Rule.nameCase_ == 2) Name = Name.toUpper();
        if (Rule.extensionCase_ == 1) Ext = Ext.toLower(); else if (Rule.extensionCase_ == 2) Ext = Ext.toUpper();
        const auto Leaf = Name + (Ext.isEmpty() ? QString() : '.' + Ext); Row.target_ = File.dir().filePath(Leaf);
        if (Row.error_.isEmpty()) Row.error_ = NameError(Leaf);
        if (Row.source_.identity_.isEmpty()) Row.error_ = QObject::tr("원본 확인 실패 또는 링크 미지원");
        if (!MaskError.isEmpty()) Row.error_ = MaskError;
        if (Targets.contains(Row.target_.toCaseFolded())) Row.error_ = QObject::tr("충돌 · 중복 대상 이름");
        Targets.insert(Row.target_.toCaseFolded()); Rows.append(Row);
    }
    QHash<QString, int> Counts; for (const auto& Row : Rows) ++Counts[Row.target_.toCaseFolded()];
    for (auto& Row : Rows) if (Counts[Row.target_.toCaseFolded()] > 1) Row.error_ = QObject::tr("충돌 · 중복 대상 이름");
    return Rows;
}
struct BatchResult { QList<Entry> current_; QString error_; bool complete_ = false; };
// All sources are pinned before mutation. File IDs are checked again at execution
// and undo, and every rename refuses replacement, including rollback operations.
inline BatchResult Execute(const QList<RenameRow>& Rows, const std::function<bool(int)>& Stop = {}) {
    BatchResult Result; for (const auto& Row : Rows) Result.current_.append(Row.source_);
    if (Rows.isEmpty()) { Result.error_ = QObject::tr("선택 항목이 없습니다."); return Result; }
    const auto Folder = QFileInfo(Rows.front().source_.path_).absolutePath();
    if (!NativeFolder(Folder)) { Result.error_ = QObject::tr("로컬·SMB 파일 경로만 지원합니다."); return Result; }
    std::vector<std::unique_ptr<Handle>> ParentPins;
    if (const auto Error = PinNativeParents(Folder, ParentPins); !Error.isEmpty()) { Result.error_ = Error; return Result; }
    QSet<QString> Sources, Targets; std::vector<std::unique_ptr<Handle>> Handles;
    for (const auto& Row : Rows) {
        const auto Key = Row.source_.path_.toCaseFolded(), TargetKey = Row.target_.toCaseFolded();
        if (!Row.error_.isEmpty() || !NameError(QFileInfo(Row.target_).fileName()).isEmpty() || Sources.contains(Key) || Targets.contains(TargetKey)
            || QFileInfo(Row.source_.path_).absolutePath().compare(Folder, Qt::CaseInsensitive) || QFileInfo(Row.target_).absolutePath().compare(Folder, Qt::CaseInsensitive)) {
            Result.error_ = QObject::tr("이름 변경 계획에 오류가 있습니다."); return Result;
        }
        Sources.insert(Key); Targets.insert(TargetKey);
        const auto Path = NativePath(Row.source_.path_);
        auto File = std::make_unique<Handle>(CreateFileW(reinterpret_cast<LPCWSTR>(Path.utf16()), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        const auto Current = Inspect(Row.source_.path_);
        if (File->value_ == INVALID_HANDLE_VALUE || Row.source_.identity_.isEmpty() || Identity(File->value_) != Row.source_.identity_
            || Current.modified_ != Row.source_.modified_ || Current.size_ != Row.source_.size_ || Current.directory_ != Row.source_.directory_) {
            Result.error_ = QObject::tr("원본이 바뀌었거나 잠글 수 없습니다: %1").arg(Row.source_.path_); return Result;
        }
        Handles.push_back(std::move(File));
    }
    for (const auto& Row : Rows) if (!Sources.contains(Row.target_.toCaseFolded()) && GetFileAttributesW(reinterpret_cast<LPCWSTR>(NativePath(Row.target_).utf16())) != INVALID_FILE_ATTRIBUTES) {
        Result.error_ = QObject::tr("충돌 · 이미 있음: %1").arg(Row.target_); return Result;
    }
    QStringList Temporary; for (int I = 0; I < Rows.size(); ++I) Temporary.append(QDir(Folder).filePath(".hc-rename-" + QUuid::createUuid().toString(QUuid::Id128)));
    int Step = 0;
    const auto Move = [&](int I, const QString& Path, bool Interruptible) {
        if (Interruptible && Stop && Stop(Step++)) { Result.error_ = QObject::tr("작업 중지 요청"); return false; }
        const auto Error = RenameHandle(Handles[I]->value_, Path);
        if (Error) { Result.error_ = QObject::tr("이름 변경 실패: Windows 오류 %1 · %2").arg(Error).arg(Path); return false; }
        Result.current_[I].path_ = Path; return true;
    };
    bool Success = true;
    for (int I = 0; I < Rows.size(); ++I) if (Rows[I].source_.path_ != Rows[I].target_ && !Move(I, Temporary[I], true)) { Success = false; break; }
    if (Success) for (int I = 0; I < Rows.size(); ++I) if (Rows[I].source_.path_ != Rows[I].target_ && !Move(I, Rows[I].target_, true)) { Success = false; break; }
    if (!Success) {
        const auto Failure = Result.error_; bool Restored = true;
        for (int I = 0; I < Rows.size(); ++I) if (Result.current_[I].path_ != Rows[I].source_.path_ && Result.current_[I].path_ != Temporary[I]) Restored = Move(I, Temporary[I], false) && Restored;
        for (int I = 0; I < Rows.size(); ++I) if (Result.current_[I].path_ != Rows[I].source_.path_) Restored = Move(I, Rows[I].source_.path_, false) && Restored;
        Result.error_ = Failure + (Restored ? QObject::tr("\n원래 이름으로 복구했습니다.") : QObject::tr("\n일부 복구 실패. 목록의 실제 경로를 확인하세요.")); return Result;
    }
    Result.complete_ = true; return Result;
}
} // namespace hc::filetools
