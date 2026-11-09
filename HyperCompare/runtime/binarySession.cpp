#include "stdafx.h"
#include "binarySession.hpp"

/// 모듈
#include "controllerMemory.hpp"
#include "saveRecovery.hpp"
#include "semantic.hpp"
#include "requestFile.hpp"
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
#include "UniqueLibs/storageProviders.hpp"
#include "UniqueLibs/nativeWritableStorage.hpp"
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QProcess>
#include <QTemporaryDir>
#include <unordered_map>
#include <cstring>
#include <Windows.h>

Q_LOGGING_CATEGORY(binarySessionLog, "hc.compare.binary")
namespace hc::compare
{
    namespace
    {
        constexpr qint64 Block = 1024 * 1024;
        template<class T> T checked(storage::Result<T> Value)
        { if (!Value.HasValue()) throw Failure(Value.Failure().code_); return std::move(Value.Value()); }
        storage::Locator locator(const QString& Path)
        { return {Path.startsWith("//") || Path.startsWith("\\\\") ? storage::LocatorKind::Smb : storage::LocatorKind::Local, Path.toUtf8().toStdString()}; }
        storage::OperationContext context(const Budget& Limits)
        { storage::OperationContext Context; Context.deadline_ = Limits.deadline_; Context.workerRead_ = true; Context.denyConcurrentMutation_ = true; return Context; }
        void append(QFile& File, const char* Data, qint64 Count)
        { if (File.write(Data, Count) != Count) throw Failure("BinaryTemporaryWriteFailed"); }
        void finish(QFile& File) { if (!File.flush()) throw Failure("BinaryTemporaryFlushFailed"); }
        HexRun readRun(const std::shared_ptr<BinaryBacking>& Backing, qint64 Index, qint64 Count)
        {
            if (Index < 0 || Index >= Count) throw Failure("BinaryRunRange");
            QFile File(Backing->file_.fileName()); HexRun Run;
            if (!File.open(QIODevice::ReadOnly) || !File.seek(Index * qint64(sizeof(Run))) || File.read(reinterpret_cast<char*>(&Run), sizeof(Run)) != sizeof(Run)) throw Failure("BinaryIndexReadFailed");
            return Run;
        }
        void range(qint64 Begin, qint64 End, qint64 Size)
        { if (Begin < 0 || End < Begin || End > Size) throw Failure("BinaryRange"); }
        void pieces(const BinaryDocument& Document, qint64 Begin, qint64 End, std::vector<BinaryPiece>& Output)
        {
            qint64 At = 0;
            for (const auto& Piece : Document.pieces_) {
                const auto First = std::max(Begin, At), Last = std::min(End, At + Piece.size_);
                if (First < Last) {
                    BinaryPiece Part{Piece.backing_, Piece.offset_ + First - At, Last - First};
                    if (!Output.empty() && Output.back().backing_ == Part.backing_ && Output.back().offset_ + Output.back().size_ == Part.offset_) Output.back().size_ += Part.size_;
                    else Output.push_back(std::move(Part));
                }
                At += Piece.size_; if (At >= End) break;
            }
        }
    }
    BinarySourceLock::~BinarySourceLock() { Release(); }
    void BinarySourceLock::Release() { if(auto Handle=handle_.exchange(nullptr))CloseHandle(Handle); }
    void SetBinarySourceLock(BinaryDocument& Document,qint64 UnlockedMiB,const Budget& Limits)
    {
        if(UnlockedMiB<0||Document.size_<=UnlockedMiB*Block||Document.original_.path_.isEmpty()){if(Document.lock_)Document.lock_->Release();Document.lock_.reset();return;}
        if(Document.lock_&&Document.lock_->handle_.load())return;
        Limits.Check();auto Lock=std::make_shared<BinarySourceLock>();const auto Path=QDir::toNativeSeparators(Document.original_.path_);
        const auto Handle=CreateFileW(reinterpret_cast<LPCWSTR>(Path.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(Handle==INVALID_HANDLE_VALUE)throw Failure("BinarySourceLockFailed");Lock->handle_.store(Handle);
        auto At=locator(Document.original_.path_);auto Provider=storage::MakeNativeReadProvider(At.kind_);
        if(checked(Provider->Resolve(At,context(Limits)).get()).id_.revision_!=Document.original_.entry_.id_.revision_)throw Failure("BinaryChangedBeforeLock");
        Document.lock_=std::move(Lock);
    }
    BinaryBacking::BinaryBacking()
    {
        file_.setFileTemplate(QDir::tempPath() + "/hypercompare-hex-XXXXXX");
        if (!file_.open()) throw Failure("BinaryTemporaryOpenFailed");
    }
    qint64 HexScale(qint64 Value, qint64 Total, int Pixels)
    {
        if (Total <= 0 || Pixels <= 0 || Value <= 0) return 0;
        Value = std::min(Value, Total);
        // Binary long division: quotient is bounded by Pixels, remainder never exceeds Total.
        quint64 Remainder = 0, Quotient = 0, Denominator = quint64(Total), Numerator = quint64(Value);
        for (int Bit = 30; Bit >= 0; --Bit) {
            Quotient *= 2;
            if (Remainder >= Denominator - Remainder) { Remainder -= Denominator - Remainder; ++Quotient; } else Remainder *= 2;
            if ((Pixels >> Bit) & 1) {
                if (Remainder >= Denominator - Numerator) { Remainder -= Denominator - Numerator; ++Quotient; } else Remainder += Numerator;
            }
        }
        return qint64(Quotient);
    }
    BinaryDocument OpenBinary(const QString& Path, const Budget& Limits)
    {
        Limits.Check(); const auto At = locator(QFileInfo(Path).absoluteFilePath()); const auto Provider = storage::MakeNativeReadProvider(At.kind_); auto Context = context(Limits);
        const auto Entry = checked(Provider->Resolve(At, Context).get());
        if (Entry.directory_ || (Entry.attributes_ & FILE_ATTRIBUTE_REPARSE_POINT) || Entry.size_ > quint64(std::numeric_limits<qint64>::max())) throw Failure("BinaryExpectedRegularFile");
        auto Backing = std::make_shared<BinaryBacking>(); const auto Stream = checked(Provider->OpenRead(Entry.id_, Context).get()); QCryptographicHash Hash(QCryptographicHash::Sha256);
        qint64 Size = 0;
        for (;;) {
            Limits.Check(); const auto Chunk = checked(Stream->Read(Block, Context).get());
            if (Chunk.bytes_.empty() && !Chunk.eof_) throw Failure("ReadMadeNoProgress");
            if (Chunk.bytes_.size() > size_t(Block) || quint64(Size) + Chunk.bytes_.size() > Entry.size_) throw Failure("ChangedDuringRead");
            const char* Data = reinterpret_cast<const char*>(Chunk.bytes_.data()); append(Backing->file_, Data, qint64(Chunk.bytes_.size()));
            Hash.addData(QByteArrayView(Data, qsizetype(Chunk.bytes_.size()))); Size += qint64(Chunk.bytes_.size()); if (Chunk.eof_) break;
        }
        if (quint64(Size) != Entry.size_ || checked(Provider->Stat(Entry.id_, Context).get()).id_.revision_ != Entry.id_.revision_) throw Failure("ChangedDuringRead");
        finish(Backing->file_); BinaryDocument Result; Result.original_.path_ = QFileInfo(Path).absoluteFilePath(); Result.original_.entry_ = Entry;
        Result.size_ = Size; Result.hash_ = Hash.result(); if (Size) Result.pieces_.push_back({Backing, 0, Size}); return Result;
    }
    BinaryDocument ConvertBinary(const BinaryDocument& Document, const QString& Format, const Budget& Limits)
    {
        if(Format == "raw") return Document;
        if (Format.startsWith("external:")) {
            const auto Config=QJsonDocument::fromJson(Format.mid(9).toUtf8()).object();
            const auto Program=Config["program"].toString();
            if(Program.isEmpty()||!QFileInfo(Program).isAbsolute()||!QFileInfo::exists(Program))throw Failure("BinaryConversionProgramInvalid");
            QTemporaryDir Work(QDir::tempPath()+"/hypercompare-convert-XXXXXX");if(!Work.isValid())throw Failure("BinaryConversionTemporaryFailed");const auto SourcePath=Work.filePath("source.bin"),TargetPath=Work.filePath("target.bin");QFile Source(SourcePath);if(!Source.open(QIODevice::WriteOnly|QIODevice::NewOnly))throw Failure("BinaryConversionTemporaryFailed");
            for(qint64 At=0;At<Document.size_;At+=Block){Limits.Check();const auto Bytes=ReadBinary(Document,At,Block,Limits);append(Source,Bytes.constData(),Bytes.size());}
            finish(Source);Source.close();
            QStringList Arguments;for(const auto& Value:Config["arguments"].toArray()){auto Argument=Value.toString();Argument.replace("%s",SourcePath).replace("%t",TargetPath).replace("%o",Document.original_.path_);Arguments.append(Argument);}
            QProcess Process;Process.setStandardOutputFile(QProcess::nullDevice());Process.setStandardErrorFile(QProcess::nullDevice());
            Process.start(Program,Arguments);
            try{if(!Process.waitForStarted(3000))throw Failure("BinaryConversionStartFailed");while(!Process.waitForFinished(50))Limits.Check();}
            catch(...){Process.kill();Process.waitForFinished(3000);throw;}
            if(Process.exitStatus()!=QProcess::NormalExit||Process.exitCode()!=0||QFileInfo(TargetPath).size()==0)throw Failure("BinaryConversionFailed");
            auto Result=OpenBinary(TargetPath,Limits);Result.original_=Document.original_;Result.format_=Format;return Result;
        }
        if(Format != "hex" && Format != "base64") throw Failure("BinaryFormatUnsupported");
        auto Backing = std::make_shared<BinaryBacking>(); QByteArray Pending; bool Padding = false;
        for(qint64 At = 0; At < Document.size_; At += Block) {
            Limits.Check(); auto Bytes = ReadBinary(Document, At, Block, Limits);
            for(char C : Bytes) {
                if(C == ' ' || C == '\r' || C == '\n' || C == '\t') continue;
                if(Padding) throw Failure("BinaryConversionTrailingData");
                if(Format == "hex" && !((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f') || (C >= 'A' && C <= 'F'))) throw Failure("BinaryInvalidHexText");
                Pending.append(C);
                if(Format == "base64" && Pending.size() >= 4 && Pending.endsWith('=')) {
                    const auto End = Pending.size() - Pending.size() % 4;
                    if(End == Pending.size()) {
                        auto Decoded = QByteArray::fromBase64Encoding(Pending, QByteArray::AbortOnBase64DecodingErrors);
                        if(!Decoded) throw Failure("BinaryInvalidBase64"); append(Backing->file_, Decoded.decoded.constData(), Decoded.decoded.size()); Pending.clear(); Padding = true;
                    }
                }
            }
            if(!Padding) {
                const qsizetype Unit = Format == "hex" ? 2 : 4; const qsizetype Count = Pending.size() - Pending.size() % Unit;
                const auto Complete = Pending.left(Count); QByteArray Decoded;
                if(Format == "hex") Decoded = QByteArray::fromHex(Complete);
                else { auto Value = QByteArray::fromBase64Encoding(Complete, QByteArray::AbortOnBase64DecodingErrors); if(!Value) throw Failure("BinaryInvalidBase64"); Decoded = Value.decoded; }
                append(Backing->file_, Decoded.constData(), Decoded.size()); Pending.remove(0, Count);
            }
        }
        if(!Pending.isEmpty()) throw Failure("BinaryConversionIncompleteUnit"); finish(Backing->file_);
        auto Result = Document; Result.pieces_.clear(); Result.size_ = Backing->file_.size(); Result.format_ = Format; Result.hash_.clear();
        if(Result.size_) Result.pieces_.push_back({Backing,0,Result.size_}); return Result;
    }
    BinaryDocument BinaryFromBytes(const QByteArray& Bytes)
    {
        if (Bytes.size() > 16 * Block) throw Failure("BinaryClipboardLimit16MiB");
        BinaryDocument Result; Result.size_ = Bytes.size(); Result.original_.exists_ = false;
        auto Backing = std::make_shared<BinaryBacking>(); append(Backing->file_, Bytes.constData(), Bytes.size()); finish(Backing->file_);
        if (!Bytes.isEmpty()) Result.pieces_.push_back({Backing, 0, Bytes.size()}); return Result;
    }
    QByteArray ReadBinary(const BinaryDocument& Document, qint64 Offset, qint64 Count, const Budget& Limits)
    {
        Limits.Check(); if (Count < 0 || Count > 16 * Block || Offset < 0 || Offset > Document.size_) throw Failure("BinaryReadRange");
        Count = std::min(Count, Document.size_ - Offset); QByteArray Result; Result.reserve(qsizetype(Count));
        std::vector<BinaryPiece> Parts; pieces(Document, Offset, Offset + Count, Parts);
        for (const auto& Part : Parts) {
            Limits.Check(); QFile File(Part.backing_->file_.fileName());
            if (!File.open(QIODevice::ReadOnly) || !File.seek(Part.offset_)) throw Failure("BinaryReadFailed");
            auto Bytes = File.read(Part.size_); if (Bytes.size() != Part.size_) throw Failure("BinaryReadFailed"); Result += Bytes;
        }
        if (Result.size() != Count) throw Failure("BinaryPieceCoverage"); return Result;
    }
    BinaryDocument ReplaceBinary(const BinaryDocument& Target, qint64 Begin, qint64 End, const BinaryDocument& Source, qint64 SourceBegin, qint64 SourceEnd)
    {
        range(Begin, End, Target.size_); range(SourceBegin, SourceEnd, Source.size_);
        if (SourceEnd - SourceBegin > std::numeric_limits<qint64>::max() - (Target.size_ - (End - Begin))) throw Failure("BinarySizeOverflow");
        BinaryDocument Result = Target; Result.pieces_.clear();
        pieces(Target, 0, Begin, Result.pieces_); pieces(Source, SourceBegin, SourceEnd, Result.pieces_); pieces(Target, End, Target.size_, Result.pieces_);
        if (Result.pieces_.size() > 1000000) throw Failure("BinaryPieceBudgetExceeded");
        Result.size_ += SourceEnd - SourceBegin - (End - Begin); ++Result.revision_; Result.hash_.clear(); return Result;
    }
    HexRun BinaryComparison::Run(qint64 Index) const { return readRun(runs_, Index, runCount_); }
    HexRun BinaryComparison::Change(qint64 Index) const { return readRun(changes_, Index, changeCount_); }
    HexRun BinaryComparison::At(qint64 Position) const
    {
        if (!runCount_) return {};
        Position = std::clamp(Position, qint64(0), std::max(qint64(0), length_ - 1)); qint64 First = 0, Last = runCount_;
        while (First + 1 < Last) { auto Middle = First + (Last - First) / 2; if (Run(Middle).position_ <= Position) First = Middle; else Last = Middle; }
        return Run(First);
    }
    qint64 BinaryComparison::Position(int Side, qint64 Offset) const
    {
        if (Side < 0 || Side > 1 || Offset < 0 || Offset > documents_[Side].size_) throw Failure("BinaryAddressRange");
        if (Offset == documents_[Side].size_) return length_;
        qint64 First = 0, Last = runCount_;
        while (First < Last) { auto Middle = First + (Last - First) / 2; auto R = Run(Middle); if (R.begin_[Side] + R.size_[Side] <= Offset) First = Middle + 1; else Last = Middle; }
        const auto R = Run(First); return R.position_ + Offset - R.begin_[Side];
    }
    std::shared_ptr<BinaryComparison> CompareBinary(std::array<BinaryDocument, 2> Documents, const QString& Mode, const Budget& Limits)
    {
        if (Mode != "none" && Mode != "fast" && Mode != "complete") throw Failure("BinaryAlignmentMode");
        const auto Lease = ProcessControllerMemory()->ReserveController(16 * Block);
        auto Result = std::make_shared<BinaryComparison>(); Result->documents_ = std::move(Documents); Result->mode_ = Mode;
        Result->runs_ = std::make_shared<BinaryBacking>(); Result->changes_ = std::make_shared<BinaryBacking>();
        std::optional<HexRun> Pending; std::array<qint64, 2> At{};
        const auto Flush = [&] {
            if (!Pending) return; const auto& R = *Pending;
            append(Result->runs_->file_, reinterpret_cast<const char*>(&R), sizeof(R)); ++Result->runCount_;
            if (R.changed_) { append(Result->changes_->file_, reinterpret_cast<const char*>(&R), sizeof(R)); ++Result->changeCount_; Result->different_ += R.length_; }
            Pending.reset();
        };
        const auto Emit = [&](qint64 Left, qint64 Right, bool Changed) {
            if (!Left && !Right) return;
            const qint64 Length = std::max(Left, Right);
            if (Length > std::numeric_limits<qint64>::max() - Result->length_) throw Failure("BinaryAlignmentOverflow");
            // Merge only runs with identical gap shape; unequal substitutions retain their gap boundary.
            if (Pending && Pending->changed_ == quint64(Changed) && ((Pending->size_[0] == Pending->size_[1] && Left == Right) || (!Pending->size_[0] && !Left) || (!Pending->size_[1] && !Right))) {
                Pending->size_[0] += Left; Pending->size_[1] += Right; Pending->length_ += Length;
            } else { Flush(); Pending = HexRun{At, {Left, Right}, Result->length_, Length, quint64(Changed)}; }
            At[0] += Left; At[1] += Right; Result->length_ += Length;
        };
        std::array<QByteArray, 2> Buffers; std::array<qint64, 2> Bases{-1, -1};
        const auto WindowBytes = [&](int Side) {
            if (Bases[Side] < 0 || At[Side] >= Bases[Side] + Buffers[Side].size()) {
                Bases[Side] = At[Side]; Buffers[Side] = ReadBinary(Result->documents_[Side], At[Side], Block, Limits);
            }
            return QByteArrayView(Buffers[Side]).sliced(At[Side] - Bases[Side]);
        };
        const qint64 Window = Mode == "complete" ? 65536 : 4096;
        while (At[0] < Result->documents_[0].size_ || At[1] < Result->documents_[1].size_) {
            Limits.Check(); auto Left = WindowBytes(0), Right = WindowBytes(1);
            if (Left.isEmpty() || Right.isEmpty()) { Emit(Left.size(), Right.size(), true); continue; }
            qint64 Common = 0, Count = std::min(Left.size(), Right.size());
            while (Common < Count && Left[Common] == Right[Common]) ++Common;
            if (Common) { Emit(Common, Common, false); continue; }
            if(Mode=="complete"&&Left.size()<=128&&Right.size()<=128){
                const int M=int(Left.size()),N=int(Right.size());std::vector<int> Cost(size_t((M+1)*(N+1)));
                const auto Cell=[&](int I,int J)->int&{return Cost[size_t(I*(N+1)+J)];};
                for(int I=M;I>=0;--I)for(int J=N;J>=0;--J){if(I==M)Cell(I,J)=N-J;else if(J==N)Cell(I,J)=M-I;else Cell(I,J)=std::min({Cell(I+1,J)+1,Cell(I,J+1)+1,Cell(I+1,J+1)+(Left[I]==Right[J]?0:1)});}
                int I=0,J=0;while(I<M||J<N){Limits.Check();if(I<M&&J<N&&Cell(I,J)==Cell(I+1,J+1)+(Left[I]==Right[J]?0:1)){Emit(1,1,Left[I]!=Right[J]);++I;++J;}else if(I<M&&Cell(I,J)==Cell(I+1,J)+1){Emit(1,0,true);++I;}else{Emit(0,1,true);++J;}}
                continue;
            }
            qint64 L = -1, R = -1;
            if (Mode != "none") {
                // Bounded content anchors. Equal spans are always byte-verified, including hash collisions.
                std::unordered_map<quint64, int> Anchors; const int Stride = Mode == "fast" ? 16 : 1;
                const auto Key = [](const char* Data) { quint64 Value; std::memcpy(&Value, Data, sizeof(Value)); return Value; };
                const int MaxL = int(std::min<qint64>(Window, Left.size() - 16)), MaxR = int(std::min<qint64>(Window, Right.size() - 16));
                for (int I = 0; I <= MaxL; I += Stride) Anchors.try_emplace(Key(Left.data() + I), I);
                int Best = std::numeric_limits<int>::max();
                for (int I = 0; I <= MaxR; ++I) {
                    if ((I & 4095) == 0) Limits.Check();
                    const auto Found = Anchors.find(Key(Right.data() + I));
                    if (Found != Anchors.end() && Found->second + I < Best && std::memcmp(Left.data() + Found->second, Right.data() + I, 16) == 0) { L = Found->second; R = I; Best = int(L+R); }
                }
            }
            if (L >= 0 && (L || R)) { const auto Both = std::min(L, R); Emit(Both, Both, true); Emit(L - Both, R - Both, true); continue; }
            qint64 Different = 1;
            // No anchor: retain exact offset comparison over a bounded window, then seek anchors again.
            const qint64 Stop = Mode == "none" ? Count : std::min(Count, Window);
            while (Different < Stop && Left[Different] != Right[Different]) ++Different;
            Emit(Different, Different, true);
        }
        Flush(); finish(Result->runs_->file_); finish(Result->changes_->file_);
        QFile Index(Result->changes_->file_.fileName()); if (!Index.open(QIODevice::ReadOnly)) throw Failure("BinaryIndexReadFailed");
        for (qint64 I = 0; I < Result->changeCount_; ++I) {
            if ((I & 4095) == 0) Limits.Check(); HexRun Run;
            if (Index.read(reinterpret_cast<char*>(&Run), sizeof(Run)) != sizeof(Run)) throw Failure("BinaryIndexReadFailed");
            const int First = int(HexScale(Run.position_, Result->length_, 4095));
            const int Last = int(HexScale(Run.position_ + Run.length_ - 1, Result->length_, 4095));
            for (int Side = 0; Side < 2; ++Side) for (int Bin = First; Bin <= Last; ++Bin)
                Result->overview_[Side][Bin] |= !Run.size_[Side] ? 8 : !Run.size_[1-Side] ? (Side ? 2 : 4) : 1;
        }
        return Result;
    }
    QJsonObject BinaryComparison::Json() const
    {
        QJsonArray Documents, Ranges;
        for (const auto& D : documents_) Documents.append(QJsonObject{{"path", D.original_.path_}, {"size", QString::number(D.size_)}, {"sha256", QString::fromLatin1(D.hash_.toHex())}});
        for (qint64 I = 0; I < std::min(qint64(4096), changeCount_); ++I) {
            const auto R = Change(I);
            Ranges.append(QJsonObject{{"offset", QString::number(R.begin_[0])}, {"rightOffset", QString::number(R.begin_[1])}, {"length", QString::number(R.length_)},
                {"leftHex", QString::fromLatin1(ReadBinary(documents_[0], R.begin_[0], std::min(qint64(32), R.size_[0])).toHex(' '))},
                {"rightHex", QString::fromLatin1(ReadBinary(documents_[1], R.begin_[1], std::min(qint64(32), R.size_[1])).toHex(' '))}});
        }
        return {{"engine", "binary.streaming"}, {"payloadSchema", "binary.offset.v1"}, {"alignment", mode_}, {"scopeComplete", true},
            {"documents", Documents}, {"differentBytes", QString::number(different_)}, {"rangeCount", QString::number(changeCount_)}, {"rangesComplete", changeCount_ <= 4096}, {"ranges", Ranges}};
    }
    qint64 FindBinary(const BinaryDocument& Document, const QByteArray& Pattern, qint64 Start, bool Backward, const Budget& Limits)
    {
        if (Pattern.isEmpty() || Pattern.size() > Block) throw Failure("BinarySearchPatternLimit");
        if (!Backward) {
            Start = std::clamp(Start, qint64(0), Document.size_);
            for (qint64 At = Start; At < Document.size_; At += Block) {
                Limits.Check(); const auto Bytes = ReadBinary(Document, At, Block + Pattern.size() - 1, Limits); const auto Found = Bytes.indexOf(Pattern);
                if (Found >= 0) return At + Found;
            }
        } else {
            qint64 End = std::min(Document.size_, std::max(qint64(0), Start) + qint64(Pattern.size()));
            while (End > 0) {
                Limits.Check(); const qint64 Begin = std::max(qint64(0), End - Block - Pattern.size() + 1);
                const auto Bytes = ReadBinary(Document, Begin, End-Begin, Limits); const auto Found = Bytes.lastIndexOf(Pattern);
                if (Found >= 0 && Begin + Found <= Start) return Begin + Found;
                if (!Begin) break; End = Begin + Pattern.size() - 1;
            }
        }
        return -1;
    }
    storage::DestinationExpectation InspectBinaryTarget(const QString& Path, const Budget& Limits)
    {
        const auto At = locator(Path); auto Provider = storage::MakeNativeReadProvider(At.kind_); auto Writer = storage::MakeNativeWritableStorage(At.kind_, Provider);
        auto Entry = checked(Writer->Inspect(At, context(Limits))); return {bool(Entry), Entry ? Entry->id_.revision_ : std::nullopt};
    }
    BinaryDocument SaveBinary(const Request& Input, const BinaryDocument& Document, const QString& Path, const storage::DestinationExpectation& Expected, const Budget& Limits)
    {
        Limits.Check(); CheckProtectedInputs(Input, {Path}, Limits);
        if(Document.lock_)Document.lock_->Release();
        if (Document.format_ != "raw") { auto Guard = Input; Guard.protectedInputs_.append(Document.original_.path_); CheckProtectedInputs(Guard, {Path}, Limits); }
        const auto At = locator(Path); auto Provider = storage::MakeNativeReadProvider(At.kind_); auto Writer = storage::MakeNativeWritableStorage(At.kind_, Provider); auto Context = context(Limits);
        auto GuardedExpected = Expected;
        if (!Document.original_.path_.isEmpty()) {
            // Save As must retain the open-time revision when it names the original, including hard links.
            auto Target = checked(Writer->Inspect(At, Context));
            const auto OriginalPath = QFileInfo(Document.original_.path_).absoluteFilePath();
            bool Same = QFileInfo(Path).absoluteFilePath().compare(OriginalPath, Qt::CaseInsensitive) == 0;
            if (Target && Target->id_.revision_ && Document.original_.entry_.id_.revision_) {
                const auto A = QString::fromStdString(Target->id_.revision_->token_);
                const auto B = QString::fromStdString(Document.original_.entry_.id_.revision_->token_);
                const auto OffsetA = A.indexOf(":identity:"), OffsetB = B.indexOf(":identity:");
                Same = Same || (OffsetA >= 0 && OffsetB >= 0 && A.mid(OffsetA) == B.mid(OffsetB));
            }
            if (Same) GuardedExpected = {Document.original_.exists_, Document.original_.entry_.id_.revision_};
        }
        SaveRecoveryJournal Journal; Context.writeCheckpoint_ = Journal.Checkpoint(); std::unique_ptr<storage::IWriteSession> Output; bool Committed = false;
        try {
            Output = checked(Writer->BeginWrite(At, GuardedExpected, Context)); QCryptographicHash Hash(QCryptographicHash::Sha256);
            for (qint64 Offset = 0; Offset < Document.size_; Offset += Block) {
                Limits.Check(); const auto Bytes = ReadBinary(Document, Offset, Block, Limits); Hash.addData(Bytes);
                checked(Output->Append({reinterpret_cast<const std::byte*>(Bytes.constData()), size_t(Bytes.size())}, Context));
            }
            Limits.Check(); const auto Receipt = checked(Output->Commit(Context)); Committed = Receipt.destinationCommitted_; if (!Committed) throw Failure("NotCommitted");
            auto Verification = Limits; Verification.canceled_ = nullptr; Verification.deadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(10);
            auto Saved = OpenBinary(Path, Verification); if (Saved.hash_ != Hash.result()) throw Failure("BinaryChangedAfterSave");
            QJsonArray Warnings, Recovery; Journal.Complete(Receipt, Warnings, Recovery);
            if (!Warnings.isEmpty() || !Recovery.isEmpty()) throw SaveRecoveryFailure("BinarySavedWithCleanupWarnings", Recovery);
            return Saved;
        } catch (...) {
            if (Output) Output->Abort(); QJsonArray Recovery; Journal.Track(Recovery);
            try { throw; } catch (const std::exception& Error) { throw SaveRecoveryFailure(Committed ? std::string("BinaryCommittedVerificationFailed:") + Error.what() : Error.what(), Recovery); }
        }
    }
}
