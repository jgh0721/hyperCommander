#pragma once
// HC 파일 문맥 메뉴의 판정(설계 261110 5절 · 7절). Qt Core만 쓰고 I/O를 하지 않는다.
// 메뉴를 연 순간의 대상 스냅샷과 주입한 입력만으로 기본 동작 이름 · 제안 · 상태 칩을 정한다.
// 지연 조회(Zone · 링크 대상) 결과가 오면 대상 값을 채워 같은 함수를 다시 부른다.

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <functional>
#include <vector>

namespace hc::filecontext
{
    // 메뉴를 연 위치. 검색 결과는 searchResults_ 로 따로 표시한다.
    enum class Place { Local, Smb, Archive, Plugin, Shell };
    enum class LinkKind { None, Symlink, Junction, Shortcut };
    enum class Tone { Normal, Accent, Warn, Danger, Muted };
    enum class Icon { None, Open, Run, Play, Archive, Disc, Link, Compare, Folder, Shield, Warning, Clock, Search };
    // 디스크에서 읽어야 하는 필드의 상태. Unknown은 판정하지 않는 항목(폴더 · 클라우드 전용 등)이다.
    enum class Probe { Unknown, Pending, Ready, Empty, Unsupported };
    enum class DefaultKind { None, Folder, Archive, Audio, Run, Open };

    inline constexpr int MaxDetailed = 64;      // 상세 정보를 담는 대상 수(6절)
    inline constexpr int MaxSuggestions = 4;    // 제안 상한(5.3절)
    inline constexpr int MaxWaitedProbes = 8;   // 메뉴 표시를 짧게 미룰 수 있는 지연 판정 수(7.2절)
    inline const QString ZoneField = QStringLiteral("file.alternateDataStream.zone");
    inline const QString LinkField = QStringLiteral("file.linkTarget");

    inline QString Extension(const QString& Name)
    {
        const auto Dot = Name.lastIndexOf('.');
        return Dot > 0 ? Name.mid(Dot + 1).toLower() : QString{};
    }

    // 심볼릭 링크 · 정션은 reparse 태그로, 바로 가기는 확장자로 정한다(FileColorRules의 Link 조건과 같은 규칙).
    inline LinkKind LinkKindOf(quint32 Attributes, quint32 ReparseTag, const QString& Name)
    {
        if (Attributes & 0x400u) {
            if (ReparseTag == 0xA000000Cu) return LinkKind::Symlink;
            if (ReparseTag == 0xA0000003u) return LinkKind::Junction;
        }
        if (!(Attributes & 0x10u) && Extension(Name) == "lnk") return LinkKind::Shortcut;
        return LinkKind::None;
    }
    inline bool IsRunnableName(const QString& Name)
    {
        static const QStringList Runnable{"exe", "com", "bat", "cmd", "msi"};
        return Runnable.contains(Extension(Name));
    }
    // 내용을 읽으면 내려받기가 일어나는 항목(RECALL_ON_DATA_ACCESS · RECALL_ON_OPEN · OFFLINE).
    inline bool IsCloudOnly(quint32 Attributes) { return Attributes & (0x00400000u | 0x00040000u | 0x00001000u); }
    // Zone 3(인터넷) · 4(제한된 사이트)를 인터넷에서 받은 파일로 본다.
    inline bool IsInternetZone(Probe State, quint32 Zone) { return State == Probe::Ready && Zone >= 3; }
    inline bool IsNative(Place At) { return At == Place::Local || At == Place::Smb; }

    struct TargetItem
    {
        QString name_, path_, probePath_;   // probePath_: 필드 조회 경로(모델의 FieldQueryPath)
        quint32 attributes_ = 0, reparseTag_ = 0;
        qint64 size_ = -1;                  // 폴더는 계산된 크기가 있을 때만 0 이상
        QDateTime modified_;
        bool directory_ = false;
        Probe zone_ = Probe::Unknown; quint32 zoneValue_ = 0;
        Probe link_ = Probe::Unknown; QString linkTarget_;
    };

    struct ContextTarget
    {
        Place place_ = Place::Local;
        bool searchResults_ = false, background_ = false, selectAll_ = false;
        bool writable_ = true;              // 현재 위치가 쓰기를 받는가(플러그인 WriteProvider 등)
        QString folder_;                    // 현재 폴더(폴더 배경 메뉴 · 머리글)
        std::vector<TargetItem> items_;     // 앞의 MaxDetailed 개
        int totalCount_ = 0;                // items_ 밖의 대상을 포함한 개수
        int cursor_ = 0;                    // 기본 동작을 정하는 커서 행(items_ 안의 위치)
        int clipboardCount_ = 0;
    };

    struct ProbeInputs
    {
        std::function<bool(const QString& Name)> playable_;  // 제목표시줄에서 재생할 수 있는 이름인가(엔진이 없으면 비워 둔다)
        bool audioOpensInternally_ = false;                   // Enter가 오디오를 내부에서 재생하는가
        std::function<bool(const QString& Name)> archive_;   // 압축 내부로 들어갈 수 있는 이름인가
        bool compareAvailable_ = false;
    };

    struct Suggestion
    {
        QString commandId_; QVariantMap parameters_;
        QString label_, toolTip_;
        Icon icon_ = Icon::None; Tone tone_ = Tone::Normal; QChar mnemonic_;
    };
    struct Badge { QString text_, toolTip_; Tone tone_ = Tone::Muted; };
    struct DefaultAction { QString label_; Icon icon_ = Icon::None; DefaultKind kind_ = DefaultKind::None; };
    struct ProbeRequest { int item_ = 0; QString field_; };
    struct ProbeResult
    {
        DefaultAction default_;
        std::vector<Suggestion> suggestions_;
        std::vector<Badge> badges_;
        std::vector<ProbeRequest> pending_;   // 지연 조회가 필요한 값
        bool waiting_ = false;                // 지연 조회 결과로 제안이 더 생길 수 있다
    };
    struct Header { QString title_, detail_; };

    inline bool IsPlayable(const TargetItem& Item, const ProbeInputs& Inputs)
    {
        return !Item.directory_ && Inputs.playable_ && Inputs.playable_(Item.name_);
    }

    // Enter(hc.navigate.open)가 실제로 할 일을 ReturnOnCurrentTab 과 같은 순서로 정한다(5.1절).
    inline DefaultAction DefaultActionOf(const TargetItem& Item, const ProbeInputs& Inputs)
    {
        if (Item.directory_) return {QObject::tr("열기"), Icon::Folder, DefaultKind::Folder};
        if (Inputs.archive_ && Inputs.archive_(Item.name_)) return {QObject::tr("압축 내부 열기"), Icon::Archive, DefaultKind::Archive};
        if (Inputs.audioOpensInternally_ && IsPlayable(Item, Inputs)) return {QObject::tr("오디오 재생"), Icon::Play, DefaultKind::Audio};
        if (IsRunnableName(Item.name_)) return {QObject::tr("실행"), Icon::Run, DefaultKind::Run};
        return {QObject::tr("열기"), Icon::Open, DefaultKind::Open};
    }

    inline QString LinkLabel(LinkKind Kind)
    {
        switch (Kind) {
        case LinkKind::Symlink: return QObject::tr("심볼릭 링크");
        case LinkKind::Junction: return QObject::tr("정션");
        case LinkKind::Shortcut: return QObject::tr("바로 가기");
        default: return {};
        }
    }

    inline ProbeResult EvaluateContext(const ContextTarget& Target, const ProbeInputs& Inputs)
    {
        ProbeResult Result;
        if (Target.background_ || Target.items_.empty()) return Result;
        const auto& Cursor = Target.items_[size_t(qBound(0, Target.cursor_, int(Target.items_.size()) - 1))];
        Result.default_ = DefaultActionOf(Cursor, Inputs);
        // 전체 선택 목록은 개수만 보인다. 수십만 항목에서 표본을 읽지 않는다(6절).
        if (Target.selectAll_) return Result;
        const bool Single = Target.totalCount_ == 1 && Target.items_.size() == 1;
        const auto Add = [&](Suggestion Item) { if (int(Result.suggestions_.size()) < MaxSuggestions) Result.suggestions_.push_back(std::move(Item)); };

        // 1. 보안 신호: 인터넷에서 받은 파일.
        int Files = 0, Marked = 0; bool Runnable = false;
        for (int Index = 0; Index < int(Target.items_.size()); ++Index) {
            const auto& Item = Target.items_[size_t(Index)];
            if (Item.directory_) continue;
            ++Files;
            if (IsInternetZone(Item.zone_, Item.zoneValue_)) { ++Marked; Runnable |= IsRunnableName(Item.name_); }
            else if (Item.zone_ == Probe::Pending && IsNative(Target.place_) && !IsCloudOnly(Item.attributes_))
                Result.pending_.push_back({Index, ZoneField});
        }
        if (IsNative(Target.place_)) {
            if (Marked) {
                Suggestion Unblock{"hc.file.ads.allowInternetBlock", {}, QObject::tr("인터넷 파일 차단 해제"), {}, Icon::Warning, Tone::Warn, 'B'};
                if (Target.totalCount_ > 1) Unblock.label_ += QObject::tr(" (%1개 중 %2개)").arg(Target.totalCount_).arg(Marked);
                Unblock.toolTip_ = Runnable ? QObject::tr("인터넷에서 받은 실행 파일입니다. 출처를 확인한 뒤 해제하세요.")
                                            : QObject::tr("Zone.Identifier 표시(인터넷에서 받음)를 지웁니다. 파일 내용은 바꾸지 않습니다.");
                Add(Unblock);
                Result.badges_.push_back({QObject::tr("인터넷에서 받음"), QObject::tr("Zone.Identifier: 인터넷 영역(3 이상)"), Tone::Warn});
            }
            Result.waiting_ = !Result.pending_.empty();
        } else Result.pending_.clear();

        // 2. 대상 고유 동작.
        const auto Kind = Result.default_.kind_;
        if (Kind == DefaultKind::Archive && (IsNative(Target.place_) || Target.place_ == Place::Archive || Target.searchResults_))
            Add({"hc.archive.extract", {}, QObject::tr("압축 풀기…"), QObject::tr("선택한 압축 파일을 풉니다."), Icon::Archive, Tone::Normal, 'U'});
        if (Single && !Cursor.directory_ && Extension(Cursor.name_) == "iso" && IsNative(Target.place_))
            Add({"hc.iso.inspect", {}, QObject::tr("ISO 구조 확인"), QObject::tr("ISO 이미지의 파일 시스템 구조를 확인합니다."), Icon::Disc, Tone::Normal, 'S'});
        const auto Link = LinkKindOf(Cursor.attributes_, Cursor.reparseTag_, Cursor.name_);
        if (Single && Link != LinkKind::None && IsNative(Target.place_)) {
            Suggestion Go{"hc.navigate.linkTarget", {}, QObject::tr("링크 대상 위치로 이동"), {}, Icon::Link, Tone::Accent, 'G'};
            Go.toolTip_ = Cursor.link_ == Probe::Ready ? QObject::tr("대상: %1").arg(Cursor.linkTarget_) : LinkLabel(Link);
            Add(Go);
            if (Link != LinkKind::Shortcut && Cursor.link_ == Probe::Pending) Result.pending_.push_back({Target.cursor_, LinkField});
        }
        if (IsPlayable(Cursor, Inputs) && Kind != DefaultKind::Audio)
            Add({"hc.audio.playPause", {}, QObject::tr("오디오 재생"), QObject::tr("제목표시줄에서 재생합니다."), Icon::Play, Tone::Normal, 'N'});

        // 3. 상황 동작.
        if (Inputs.compareAvailable_)
            Add({"hc.file.compare", {}, QObject::tr("비교"), QObject::tr("HyperCompare로 비교합니다."), Icon::Compare, Tone::Normal, 'W'});
        if (Target.searchResults_ && Single)
            Add({"hc.search.openLocation", {}, QObject::tr("원본 위치 열기"), QObject::tr("검색 결과 항목이 있는 폴더로 이동합니다."), Icon::Search, Tone::Normal, 'F'});

        // 4. 권한 동작.
        if (Single && !Cursor.directory_ && IsRunnableName(Cursor.name_) && IsNative(Target.place_))
            Add({"hc.file.runAsAdmin", {}, QObject::tr("관리자 권한으로 실행"), QObject::tr("UAC 확인 후 관리자 권한으로 실행합니다."), Icon::Shield, Tone::Normal, 'K'});

        // 상태 칩: 단일 대상의 속성 · 링크 · 클라우드, 그리고 위치.
        if (Single) {
            if (Link != LinkKind::None) {
                auto Text = LinkLabel(Link);
                if (Cursor.link_ == Probe::Ready && !Cursor.linkTarget_.isEmpty()) Text += QStringLiteral(" → ") + Cursor.linkTarget_;
                Result.badges_.push_back({Text, Cursor.linkTarget_, Tone::Accent});
            }
            if (IsCloudOnly(Cursor.attributes_)) Result.badges_.push_back({QObject::tr("클라우드 전용"), QObject::tr("내용을 읽으면 내려받기가 일어나므로 내용 판정을 하지 않습니다."), Tone::Muted});
            if (Cursor.attributes_ & 0x1u) Result.badges_.push_back({QObject::tr("읽기 전용"), {}, Tone::Muted});
            if (Cursor.attributes_ & 0x2u) Result.badges_.push_back({QObject::tr("숨김"), {}, Tone::Muted});
            if (Cursor.attributes_ & 0x4u) Result.badges_.push_back({QObject::tr("시스템"), {}, Tone::Muted});
        }
        if (Target.place_ == Place::Archive) Result.badges_.push_back({QObject::tr("압축 내부"), {}, Tone::Muted});
        else if (!Target.writable_) Result.badges_.push_back({QObject::tr("읽기 전용 위치"), {}, Tone::Muted});
        return Result;
    }

    inline QString PlaceName(Place At)
    {
        switch (At) {
        case Place::Local: return QStringLiteral("local");
        case Place::Smb: return QStringLiteral("smb");
        case Place::Archive: return QStringLiteral("archive");
        case Place::Plugin: return QStringLiteral("plugin");
        default: return QStringLiteral("shell");
        }
    }

    // contextMenu.json 의 when 조건(10절 P2). kinds: file · folder · background(없으면 file · folder),
    // locations: local · smb · search · archive · plugin · shell, extensions: 모든 대상이 그 확장자의 파일,
    // count: single · multiple · any. 모르는 값은 일치하지 않는 것으로 본다.
    inline bool MatchesWhen(const QJsonObject& When, const ContextTarget& Target)
    {
        const auto List = [&](const QString& Key) {
            QStringList Values; for (const auto& Value : When.value(Key).toArray()) Values << Value.toString().trimmed().toLower();
            Values.removeAll(QString{}); return Values;
        };
        const auto Kinds = List("kinds");
        if (Target.background_) return Kinds.contains("background") && (List("locations").isEmpty()
            || List("locations").contains(Target.searchResults_ ? QStringLiteral("search") : PlaceName(Target.place_)));
        if (Target.items_.empty()) return false;
        bool Files = false, Folders = false;
        for (const auto& Item : Target.items_) (Item.directory_ ? Folders : Files) = true;
        const auto AllowedKinds = Kinds.isEmpty() ? QStringList{"file", "folder"} : Kinds;
        if ((Files && !AllowedKinds.contains("file")) || (Folders && !AllowedKinds.contains("folder"))) return false;
        const auto Locations = List("locations");
        if (!Locations.isEmpty() && !Locations.contains(Target.searchResults_ ? QStringLiteral("search") : PlaceName(Target.place_))) return false;
        const auto Count = When.value("count").toString("any");
        if ((Count == "single" && Target.totalCount_ != 1) || (Count == "multiple" && Target.totalCount_ < 2)
            || (Count != "single" && Count != "multiple" && Count != "any")) return false;
        const auto Extensions = List("extensions");
        if (!Extensions.isEmpty()) {
            if (Target.selectAll_ || int(Target.items_.size()) < Target.totalCount_) return false;
            for (const auto& Item : Target.items_)
                if (Item.directory_ || !Extensions.contains(Extension(Item.name_))) return false;
        }
        return true;
    }

    // 머리글 두 줄(4.2절). 파일 크기 · 시각은 표시 언어 형식을 쓴다.
    inline Header DescribeTarget(const ContextTarget& Target, const QLocale& Locale = QLocale())
    {
        const auto Time = [&](const QDateTime& Value) { return Value.isValid() ? Locale.toString(Value.toLocalTime(), QStringLiteral("yyyy-MM-dd HH:mm")) : QString{}; };
        const auto Join = [](QStringList Parts) { Parts.removeAll(QString{}); return Parts.join(QStringLiteral(" · ")); };
        if (Target.background_) return {Target.folder_, QObject::tr("현재 폴더")};
        if (Target.selectAll_) return {QObject::tr("%1개 항목 · 전체 선택").arg(Locale.toString(Target.totalCount_)), QObject::tr("판정 생략")};
        if (Target.items_.empty()) return {};
        if (Target.totalCount_ <= 1) {
            const auto& Item = Target.items_.front();
            const auto Size = Item.size_ >= 0 ? Locale.formattedDataSize(Item.size_, 1, QLocale::DataSizeTraditionalFormat) : QString{};
            return {Item.name_, Join({Item.directory_ ? QObject::tr("폴더") : Size, Item.directory_ && Item.size_ >= 0 ? Size : QString{}, Time(Item.modified_)})};
        }
        int Folders = 0; qint64 Bytes = 0; bool Partial = int(Target.items_.size()) < Target.totalCount_;
        for (const auto& Item : Target.items_) { Folders += Item.directory_; if (Item.size_ > 0) Bytes += Item.size_; }
        const int Files = int(Target.items_.size()) - Folders;
        QStringList Detail;
        if (!Partial) {
            if (Files) Detail << QObject::tr("파일 %1").arg(Files);
            if (Folders) Detail << QObject::tr("폴더 %1").arg(Folders);
            Detail << QObject::tr("합계 %1").arg(Locale.formattedDataSize(Bytes, 1, QLocale::DataSizeTraditionalFormat));
        } else Detail << QObject::tr("앞의 %1개만 판정").arg(MaxDetailed);
        return {QObject::tr("%1개 항목").arg(Locale.toString(Target.totalCount_)), Join(Detail)};
    }
}
