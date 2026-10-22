#pragma once
#include <QColor>
#include <QFont>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QFileInfo>
#include <QDateTime>
#include <QSet>
#include <QCache>
#include <memory>
#include "commonLibs/fileNameGlob.hpp"
#include <limits>
#include <optional>

namespace hc::settings {
struct FileRuleInput
{
    QString name_;
    quint32 attributes_ = 0;
    quint64 size_ = 0;
    bool dark_ = false;
    std::optional<qint64> modifiedUtcMs_;
    std::optional<quint32> reparseTag_;
    qint64 referenceUtcMs_ = QDateTime::currentMSecsSinceEpoch();
};
struct FileRuleResult
{
    bool matched_ = false;
    QString error_;
    QColor foreground_, background_;
    QJsonObject font_;
    QStringList groups_;
    QMap<QString, QString> owners_;
    std::optional<qint64> validUntilUtcMs_;
    QFont Font(QFont Base) const {
        if (font_.contains("bold")) Base.setBold(font_["bold"].toBool());
        if (font_.contains("italic")) Base.setItalic(font_["italic"].toBool());
        if (font_.contains("underline")) Base.setUnderline(font_["underline"].toBool());
        if (font_.contains("strike")) Base.setStrikeOut(font_["strike"].toBool());
        return Base;
    }
};
struct FileRuleKey
{
    QString name_;
    quint32 attributes_ = 0;
    quint64 size_ = 0;
    std::optional<qint64> modified_;
    std::optional<quint32> tag_;
    quintptr identity_ = 0;
    static FileRuleKey From(const FileRuleInput& Input, quintptr Identity = 0) { return {Input.name_, Input.attributes_, Input.size_, Input.modifiedUtcMs_, Input.reparseTag_, Identity}; }
    bool operator==(const FileRuleKey&) const = default;
    friend size_t qHash(const FileRuleKey& Key, size_t Seed = 0) noexcept {
        return qHashMulti(Seed, Key.name_, Key.attributes_, Key.size_, Key.modified_.has_value(), Key.modified_.value_or(0), Key.tag_.has_value(), Key.tag_.value_or(0), Key.identity_);
    }
};
class FileColorRules
{
public:
    // Use the existing FS_FLAG_UNICODE_NFD criterion, including mixed names.
    // Compare the stored spelling; displaying or matching never renames it.
    static bool HasDecomposedName(const QString& Name) {
        return Name != Name.normalized(QString::NormalizationForm_C);
    }
    static QStringList Extensions(const QString& Input) {
        QStringList Result;
        for (auto Part : Input.toLower().split(QRegularExpression("[,;|\\s]+"), Qt::SkipEmptyParts)) {
            if (Part.startsWith('.')) Part.remove(0, 1);
            if (!Result.contains(Part)) Result.append(Part);
        }
        return Result;
    }
    static QJsonObject Empty() { return {{"version", 1}, {"mode", "first"}, {"groups", QJsonArray{}}}; }
    static bool Supported(const QJsonObject& Document) { return Document["version"] == QJsonValue(1) || Document["version"] == QJsonValue(2) || Document["version"] == QJsonValue(3); }
    static QString Syntax(const QJsonObject& C, int Version = 3) {
        return Version >= 3 && C.contains("syntax") ? C["syntax"].toString()
            : C["type"] == "pattern" ? QStringLiteral("wildcard") : QStringLiteral("literal");
    }
    static bool HasRegex(const QJsonObject& Document) {
        if (Document["version"].toInt() < 3) return false;
        for (const auto G : Document["groups"].toArray()) for (const auto C : G.toObject()["conditions"].toArray())
            if (C.toObject()["syntax"] == "regex") return true;
        return false;
    }
    static QRegularExpression Regex(const QString& Pattern) {
        // PCRE2 keeps the lower limit even if the expression supplies another.
        // This bounds engine work, not wall time; callers still run off the GUI.
        return QRegularExpression(QStringLiteral("(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=1000)") + Pattern,
            QRegularExpression::CaseInsensitiveOption);
    }
    // Derived colors are never persisted over the user's manual dark palette.
    // Keep hue, saturation and alpha; bound inverted lightness for dark surfaces.
    static QJsonObject Colors(const QJsonObject& Group, bool Dark) {
        if (!Dark || !Group["autoDark"].toBool()) return Group[Dark ? "dark" : "light"].toObject();
        QJsonObject Result;
        const auto Light = Group["light"].toObject();
        for (const auto* Key : {"foreground", "background"}) {
            const QColor Source(Light[Key].toString()); if (!Source.isValid()) continue;
            const bool Text = qstrcmp(Key, "foreground") == 0;
            const auto Value = QColor::fromHslF(Source.hslHueF(), Source.hslSaturationF(),
                Text ? qBound(0.65f, 1.0f - Source.lightnessF(), 0.90f) : qBound(0.08f, 1.0f - Source.lightnessF(), 0.22f), Source.alphaF()).toRgb();
            Result[Key] = Value.name(QColor::HexArgb);
        }
        return Result;
    }
    static std::optional<qint64> MetadataTime(qint64 UtcMs) {
        // Providers use zero for unavailable dates; zero native FILETIME can
        // also arrive as the 1601 epoch. Neither is a confirmed modification.
        if (!UtcMs || UtcMs == -11644473600000LL) return {};
        return UtcMs;
    }
    static QJsonObject NormalizeCondition(QJsonObject Condition, int Version) {
        if (Version != 1) return Condition;
        const auto Type = Condition["type"].toString();
        if (Type == "extension") Condition["op"] = "in";
        else if (Type == "pattern") { Condition["op"] = "matches"; Condition["value"] = QJsonArray{Condition["value"]}; }
        else if (Type == "folder") Condition["op"] = "is";
        else if (Type == "attributes") Condition["op"] = "all";
        else if (Type == "sizeMore" || Type == "sizeLess" || Type == "sizeEqual") { Condition["type"] = "size"; Condition["unit"] = "B"; Condition["op"] = Type == "sizeMore" ? "gt" : Type == "sizeLess" ? "lt" : "eq"; }
        return Condition;
    }
    static QJsonObject Upgrade(QJsonObject Document) {
        if (Document["version"] != QJsonValue(1)) return Document;
        auto Groups = Document["groups"].toArray();
        for (int I = 0; I < Groups.size(); ++I) { auto G = Groups[I].toObject(); QJsonArray Conditions;
            for (const auto C : G["conditions"].toArray()) Conditions.append(NormalizeCondition(C.toObject(), 1));
            G["conditions"] = Conditions; Groups[I] = G;
        }
        Document["groups"] = Groups; Document["version"] = 2; return Document;
    }
    static QString Validate(const QJsonObject& Document, bool CompilePatterns = true) {
        if (!Supported(Document)) return QObject::tr("지원하지 않는 파일 색상 규칙 버전입니다.");
        if (Document["mode"] != "first" && Document["mode"] != "merge") return QObject::tr("그룹 합성 정책이 올바르지 않습니다.");
        if (!Document["groups"].isArray() || Document["groups"].toArray().size() > 256) return QObject::tr("그룹은 최대 256개입니다.");
        QSet<QString> Ids, Names;
        for (const auto Value : Document["groups"].toArray()) {
            const auto Group = Value.toObject(); const auto Id = Group["id"].toString(); const auto Name = Group["name"].toString().trimmed();
            if (Id.isEmpty() || Ids.contains(Id) || Name.isEmpty() || Names.contains(Name.toCaseFolded())) return QObject::tr("그룹 ID와 이름은 비어 있거나 중복될 수 없습니다.");
            Ids.insert(Id); Names.insert(Name.toCaseFolded());
            if (Group.contains("enabled") && !Group["enabled"].isBool()) return QObject::tr("그룹 사용 여부는 true 또는 false입니다.");
            if (Group.contains("autoDark") && !Group["autoDark"].isBool()) return QObject::tr("다크 자동 색상은 true 또는 false입니다.");
            if (Group.contains("font") && !Group["font"].isObject()) return QObject::tr("글꼴 규칙 형식이 올바르지 않습니다.");
            const auto Font = Group["font"].toObject();
            for (const auto& Key : {"bold", "italic", "underline", "strike"}) if (Font.contains(Key) && !Font[Key].isBool()) return QObject::tr("글꼴 효과는 true 또는 false입니다.");
            if (Group["join"] != "any" && Group["join"] != "all") return QObject::tr("조건 결합 방식이 올바르지 않습니다.");
            const auto Conditions = Group["conditions"].toArray();
            if (Conditions.isEmpty() || Conditions.size() > 32) return QObject::tr("그룹마다 조건을 1~32개 입력하세요.");
            int ConditionIndex = 0;
            for (const auto C : Conditions) {
                ++ConditionIndex;
                if (!C.isObject()) return QObject::tr("조건 형식이 올바르지 않습니다.");
                auto Condition = C.toObject(); const auto LegacyType = Condition["type"].toString();
                if (Document["version"] == QJsonValue(1)) {
                    if (!QStringList{"extension", "pattern", "folder", "attributes", "sizeMore", "sizeLess", "sizeEqual"}.contains(LegacyType)) return QObject::tr("알 수 없는 조건 종류입니다.");
                    Condition = NormalizeCondition(Condition, 1);
                }
                const auto Type = Condition["type"].toString(), Op = Condition["op"].toString(), Text = Condition["value"].toString();
                const auto Mode = Syntax(Condition, Document["version"].toInt());
                if ((Type == "extension" || Type == "pattern") && !QStringList{"literal", "wildcard", "regex"}.contains(Mode)) return QObject::tr("조건의 비교 방식을 확인하세요.");
                if ((Type == "extension" || Type == "pattern") && Mode == "regex") {
                    if ((Op != "matches" && Op != "notMatches") || !Condition["value"].isString() || Text.isEmpty() || Text.size() > 512 || Text.contains(QChar::Null)) return QObject::tr("정규표현식은 1~512자로 입력하세요.");
                    if (CompilePatterns) {
                        const auto Re = Regex(Text);
                        if (!Re.isValid()) return QObject::tr("%1 · 조건 %2: 정규표현식 %3번째 위치 · %4").arg(Name).arg(ConditionIndex)
                            .arg(qMax<qsizetype>(1, Re.patternErrorOffset() - (Re.pattern().size() - Text.size()) + 1)).arg(Re.errorString());
                    }
                    continue;
                }
                if ((Type == "pattern" || Type == "extension") && Mode == "wildcard") {
                    const auto Patterns = Condition["value"].toArray();
                    if ((Op != "matches" && Op != "notMatches") || !Condition["value"].isArray() || Patterns.isEmpty() || Patterns.size() > 32) return QObject::tr("파일명 패턴은 1~32개입니다.");
                    for (const auto P : Patterns) if (!P.isString() || P.toString().trimmed().isEmpty() || P.toString().size() > 512 || P.toString().contains('/') || P.toString().contains('\\') || P.toString().contains(QChar::Null)) return QObject::tr("파일명 패턴에는 경로 없이 1~512자를 입력하세요.");
                    if (CompilePatterns && Document["version"].toInt() >= 3 && Condition.contains("syntax")) for (const auto P : Patterns) {
                        hc::text::FileNameGlob Glob; const auto Error = Glob.Compile(P.toString());
                        if (!Error.isEmpty()) return QObject::tr("%1 · 조건 %2: %3").arg(Name).arg(ConditionIndex).arg(Error);
                    }
                    continue;
                }
                if (!Condition["value"].isString() || Text.size() > 512 || (Type == "pattern" ? Text.isEmpty() : Text.trimmed().isEmpty())) return QObject::tr("조건은 1~512자로 입력하세요.");
                if (Type == "pattern") {
                    if ((Op != "contains" && Op != "notContains") || Text.contains(QChar::Null)) return QObject::tr("파일명 문자열의 포함 방법을 확인하세요.");
                } else if (Type == "extension") {
                    const auto Parts = Extensions(Text);
                    if ((Op != "in" && Op != "notIn") || Parts.isEmpty()) return QObject::tr("확장자 목록과 비교 방법을 확인하세요.");
                    for (const auto& Part : Parts) if (Part != "<none>" && !QRegularExpression("^[\\p{L}\\p{N}_+-]+$").match(Part).hasMatch()) return QObject::tr("단순 확장자를 입력하세요. 무확장자는 <none>, 파일명 패턴은 별도 조건을 사용합니다.");
                } else if (Type == "folder") {
                    if (Op != "is" || (Text != "true" && Text != "false")) return QObject::tr("종류 조건은 폴더 또는 파일입니다.");
                } else if (Type == "normalization") {
                    if (Op != "is" || (Text != "decomposed" && Text != "composed")) return QObject::tr("파일명 정규화 조건은 자소 분리 있음 또는 분리 없음입니다.");
                } else if (Type == "link") {
                    if (Op != "is" || !QStringList{"any", "shortcut", "symbolic", "none"}.contains(Text)) return QObject::tr("링크 종류를 확인하세요.");
                } else if (Type == "modified" && (Op == "before" || Op == "after")) {
                    if (!utcDate(Text)) return QObject::tr("수정 날짜는 시간대가 포함된 ISO 날짜입니다.");
                } else if (Type == "attributes" || Type == "size" || (Type == "modified" && Op == "within")) {
                    if (Type == "attributes" && Op != "any" && Op != "all") return QObject::tr("속성 비교 방법을 확인하세요.");
                    if (Type == "size" && !QStringList{"gt", "ge", "lt", "le", "eq", "ne"}.contains(Op)) return QObject::tr("크기 비교 방법을 확인하세요.");
                    if (!number(Condition)) return QObject::tr("크기·속성·기간은 범위 안의 0 이상 정수와 올바른 단위로 입력하세요.");
                } else return QObject::tr("알 수 없는 조건 종류 또는 비교 방법입니다.");
            }
            for (const auto& Scheme : {"light", "dark"}) {
                if (Group.contains(Scheme) && !Group[Scheme].isObject()) return QObject::tr("테마 색상 규칙 형식이 올바르지 않습니다.");
                const auto Colors = Group[Scheme].toObject();
                for (const auto& Key : {"foreground", "background"}) if (Colors.contains(Key) && !QColor(Colors[Key].toString()).isValid()) return QObject::tr("색은 #RRGGBB 형식으로 입력하거나 비워 두세요.");
            }
        }
        return {};
    }
    struct CachedExtension {
        QList<qint8> conditions_;
        QList<int> groups_;
        bool complete_ = false;
    };
    struct EvaluationContext {
        // One serialized worker owns a context. Compiled rules remain immutable.
        std::shared_ptr<const int> revision_;
        QCache<QString, CachedExtension> extensions_{4 * 1024 * 1024};
        quint64 hits_ = 0, misses_ = 0;
    };
    QString Load(const QJsonObject& Document) {
        QJsonObject ConditionDocument = Document; QJsonArray ConditionGroups;
        for (const auto V : Document["groups"].toArray()) {
            auto G = V.toObject(); for (const auto* Key : {"name", "light", "dark", "font", "autoDark"}) G.remove(Key);
            ConditionGroups.append(G);
        }
        ConditionDocument["groups"] = ConditionGroups;
        if (conditionDocument_ != ConditionDocument) revision_ = std::make_shared<const int>(0);
        conditionDocument_ = ConditionDocument;
        groups_.clear(); relativeDates_ = false; needsExtension_ = false; extensionOnly_ = true;
        cacheConditions_ = 0; merge_ = Document["mode"] == "merge";
        const auto Error = Validate(Document, false); if (!Error.isEmpty()) return Error;
        const int Version = Document["version"].toInt();
        for (const auto Value : Document["groups"].toArray()) {
            const auto Object = Value.toObject();
            if (!Object["enabled"].toBool(true)) {
                // Disabled groups still have to round-trip as valid settings.
                auto Disabled = Document; Disabled["groups"] = QJsonArray{Object};
                const auto Failure = Validate(Disabled); if (!Failure.isEmpty()) { groups_.clear(); return Failure; }
                continue;
            }
            Group G; G.all_ = Object["join"] == "all"; G.label_ = Object["name"].toString();
            G.font_ = Object["font"].toObject();
            for (int Dark = 0; Dark < 2; ++Dark) {
                const auto Palette = Colors(Object, Dark);
                G.foreground_[Dark] = QColor(Palette["foreground"].toString());
                G.background_[Dark] = QColor(Palette["background"].toString());
            }
            for (const auto C : Object["conditions"].toArray()) {
                const auto O = NormalizeCondition(C.toObject(), Version);
                Condition Item; const auto Type = O["type"].toString(), Op = O["op"].toString();
                Item.type_ = Field(typeNames().indexOf(Type)); Item.value_ = O["value"].toString();
                Item.op_ = Operator(opNames().indexOf(Op));
                const auto Mode = Syntax(O, Version);
                Item.syntax_ = Mode == "regex" ? RegexSyntax : Mode == "wildcard" ? GlobSyntax : LiteralSyntax;
                Item.negate_ = Op == "notIn" || Op == "notMatches" || Op == "notContains";
                if (Item.type_ == Extension || Item.type_ == Pattern) {
                    if (Item.syntax_ == GlobSyntax) for (const auto P : O["value"].toArray()) {
                        hc::text::FileNameGlob Glob; const auto Failure = Glob.Compile(P.toString());
                        // v1/v2 accepted malformed globs: preserve their no-match
                        // behavior on read; edited v3 conditions report the error.
                        if (!Failure.isEmpty() && Version >= 3 && O.contains("syntax")) { groups_.clear(); return QObject::tr("%1 · 조건 %2: %3").arg(G.label_).arg(G.conditions_.size() + 1).arg(Failure); }
                        Item.globs_.append(std::move(Glob));
                    }
                    else if (Item.syntax_ == RegexSyntax) {
                        Item.regex_ = Regex(Item.value_);
                        if (!Item.regex_.isValid()) { groups_.clear(); return Validate(Document); }
                        Item.regex_.optimize();
                    } else if (Item.type_ == Extension) {
                        const auto Parts = Extensions(Item.value_); Item.extensions_ = QSet<QString>(Parts.cbegin(), Parts.cend());
                    }
                }
                Item.number_ = number(O).value_or(0); Item.date_ = utcDate(Item.value_).value_or(0);
                if (Item.type_ == Modified && Item.op_ == Within) relativeDates_ = true;
                needsExtension_ |= Item.type_ == Extension || Item.type_ == Link;
                extensionOnly_ &= Item.type_ == Extension || Item.type_ == Folder;
                if (Item.type_ == Extension) Item.cacheIndex_ = cacheConditions_++;
                G.conditions_.append(std::move(Item));
            }
            groups_.append(std::move(G));
        }
        return {};
    }
    bool HasRules() const { return !groups_.isEmpty(); }
    bool HasRelativeDates() const { return relativeDates_; }
    bool SameConditions(const FileColorRules& Other) const { return revision_ == Other.revision_; }
    FileRuleResult Evaluate(const QString& Name, quint32 Attributes, quint64 Size, bool Dark) const { return Evaluate(FileRuleInput{Name, Attributes, Size, Dark}); }
    FileRuleResult EvaluateStyle(const FileRuleInput& Input, EvaluationContext* Context = nullptr) const { return evaluate(Input, Context, false, true); }
    FileRuleResult Matches(const FileRuleInput& Input, EvaluationContext* Context = nullptr) const { return evaluate(Input, Context, false, false); }
    FileRuleResult Evaluate(const FileRuleInput& Input, EvaluationContext* Context = nullptr) const { return evaluate(Input, Context, true, true); }
private:
    enum Field { Extension, Pattern, Folder, Normalization, Attributes, Size, Modified, Link };
    enum Operator { In, NotIn, MatchesOp, NotMatches, Contains, NotContains, Is, Any, All, Gt, Ge, Lt, Le, Eq, Ne, Before, After, Within };
    enum SyntaxKind { LiteralSyntax, GlobSyntax, RegexSyntax };
    struct Condition {
        Field type_ = Extension; Operator op_ = In; SyntaxKind syntax_ = LiteralSyntax;
        QString value_; QSet<QString> extensions_; QList<hc::text::FileNameGlob> globs_;
        QRegularExpression regex_; quint64 number_ = 0; qint64 date_ = 0;
        int cacheIndex_ = -1; bool negate_ = false;
    };
    struct Group {
        QString label_; bool all_ = false; QList<Condition> conditions_;
        QColor foreground_[2], background_[2]; QJsonObject font_;
    };
    static const QStringList& typeNames() {
        static const QStringList Names{"extension", "pattern", "folder", "normalization", "attributes", "size", "modified", "link"}; return Names;
    }
    static const QStringList& opNames() {
        static const QStringList Names{"in", "notIn", "matches", "notMatches", "contains", "notContains", "is", "any", "all", "gt", "ge", "lt", "le", "eq", "ne", "before", "after", "within"}; return Names;
    }
    static qint8 textHit(const Condition& C, const QString& Text) {
        bool Hit = false;
        if (C.syntax_ == GlobSyntax) {
            for (const auto& Glob : C.globs_) if (Glob.Matches(Text)) { Hit = true; break; }
        } else if (C.syntax_ == RegexSyntax) {
            const auto Match = C.regex_.match(Text);
            if (!Match.isValid()) return 2; // Never negate an execution error.
            Hit = Match.hasMatch();
        } else Hit = Text.contains(C.value_, Qt::CaseInsensitive);
        return Hit != C.negate_;
    }
    static qint8 conditionHit(const Condition& C, const FileRuleInput& Input, const QString& Ext, bool IsFolder, std::optional<bool>& Decomposed) {
        switch (C.type_) {
        case Extension:
            if (IsFolder) return 0;
            if (C.syntax_ == LiteralSyntax) return C.extensions_.contains(Ext.isEmpty() ? QStringLiteral("<none>") : Ext.toLower()) != C.negate_;
            return textHit(C, Ext);
        case Pattern: return textHit(C, Input.name_);
        case Folder: return IsFolder == (C.value_ == "true");
        case Normalization:
            if (!Decomposed) Decomposed = HasDecomposedName(Input.name_);
            return *Decomposed == (C.value_ == "decomposed");
        case Attributes: return C.op_ == Any ? (Input.attributes_ & C.number_) != 0 : (Input.attributes_ & C.number_) == C.number_;
        case Size:
            if (IsFolder) return 0;
            switch (C.op_) {
            case Gt: return Input.size_ > C.number_; case Ge: return Input.size_ >= C.number_;
            case Lt: return Input.size_ < C.number_; case Le: return Input.size_ <= C.number_;
            case Eq: return Input.size_ == C.number_; default: return Input.size_ != C.number_;
            }
        case Modified:
            if (!Input.modifiedUtcMs_) return 0;
            if (C.op_ == Before) return *Input.modifiedUtcMs_ < C.date_;
            if (C.op_ == After) return *Input.modifiedUtcMs_ > C.date_;
            return *Input.modifiedUtcMs_ <= Input.referenceUtcMs_ && quint64(Input.referenceUtcMs_) - quint64(*Input.modifiedUtcMs_) <= C.number_;
        case Link: {
            const bool Shortcut = !IsFolder && (Ext.compare("lnk", Qt::CaseInsensitive) == 0 || Ext.compare("url", Qt::CaseInsensitive) == 0);
            const bool Reparse = (Input.attributes_ & 0x400) != 0;
            const bool Known = !Reparse || Input.reparseTag_.value_or(0) != 0;
            const bool Symbolic = Reparse && (Input.reparseTag_ == 0xa000000cu || Input.reparseTag_ == 0xa0000003u);
            if (C.value_ == "shortcut") return Shortcut; if (C.value_ == "symbolic") return Symbolic;
            if (C.value_ == "any") return Shortcut || Symbolic;
            return Known && !Shortcut && !Symbolic;
        }
        }
        return 0;
    }
    FileRuleResult evaluate(const FileRuleInput& Input, EvaluationContext* Context, bool Diagnostics, bool Style) const {
        FileRuleResult Result;
        if (Input.name_ == ".." || groups_.isEmpty()) return Result;
        const bool IsFolder = (Input.attributes_ & 0x10) != 0;
        const auto Ext = needsExtension_ ? QFileInfo(Input.name_).suffix() : QString{};
        std::optional<bool> Decomposed;
        // Deadlines are independent of Boolean short circuiting. An earlier
        // currently false group may acquire precedence when its date changes.
        if (relativeDates_ && Input.modifiedUtcMs_) for (const auto& G : groups_) for (const auto& C : G.conditions_) {
            if (C.type_ != Modified || C.op_ != Within) continue;
            std::optional<qint64> Change;
            if (*Input.modifiedUtcMs_ > Input.referenceUtcMs_) Change = *Input.modifiedUtcMs_;
            else if (C.number_ < quint64(std::numeric_limits<qint64>::max()) - quint64(*Input.modifiedUtcMs_)) Change = qint64(quint64(*Input.modifiedUtcMs_) + C.number_ + 1);
            if (Change && *Change > Input.referenceUtcMs_ && (!Result.validUntilUtcMs_ || *Change < *Result.validUntilUtcMs_)) Result.validUntilUtcMs_ = Change;
        }
        CachedExtension* Cached = nullptr;
        if (Context && cacheConditions_) {
            if (Context->revision_ != revision_) { Context->extensions_.clear(); Context->revision_ = revision_; }
            // Keep original spelling: regex inline case-sensitive options can
            // deliberately distinguish suffixes even with default caseless mode.
            const auto Key = QString(IsFolder ? u'D' : u'F') + Ext;
            Cached = Context->extensions_.object(Key);
            if (Cached) ++Context->hits_;
            else {
                ++Context->misses_; Cached = new CachedExtension;
                Cached->conditions_.fill(-1, cacheConditions_);
                const int Cost = qMax(1024, int(sizeof(CachedExtension) + cacheConditions_ + groups_.size() * sizeof(int) + Key.size() * sizeof(QChar)));
                Context->extensions_.insert(Key, Cached, Cost);
            }
        }
        QList<int> Matched;
        if (Cached && extensionOnly_ && Cached->complete_) Matched = Cached->groups_;
        else {
            for (qsizetype Index = 0; Index < groups_.size(); ++Index) {
                const auto& G = groups_[Index]; bool Hit = G.all_, Failed = false;
                for (const auto& C : G.conditions_) {
                    qint8 Value = Cached && C.cacheIndex_ >= 0 ? Cached->conditions_[C.cacheIndex_] : -1;
                    if (Value < 0) {
                        Value = conditionHit(C, Input, Ext, IsFolder, Decomposed);
                        if (Cached && C.cacheIndex_ >= 0) Cached->conditions_[C.cacheIndex_] = Value;
                    }
                    if (Value == 2) { Failed = true; continue; }
                    if ((G.all_ && !Value) || (!G.all_ && Value)) { Hit = Value; Failed = false; break; }
                }
                if (Failed) { if (Result.error_.isEmpty()) Result.error_ = QObject::tr("%1: 정규표현식 실행 한도를 초과했거나 평가할 수 없습니다.").arg(G.label_); continue; }
                if (!Hit) continue;
                Matched.append(int(Index)); if (!merge_) break;
            }
            if (Cached && extensionOnly_ && Result.error_.isEmpty()) { Cached->groups_ = Matched; Cached->complete_ = true; }
        }
        Result.matched_ = !Matched.isEmpty();
        if (!Style && !Diagnostics) return Result;
        quint8 FontOwners = 0;
        for (const auto Index : Matched) {
            const auto& G = groups_[Index]; if (Diagnostics) Result.groups_.append(G.label_);
            if (!Result.foreground_.isValid() && G.foreground_[Input.dark_].isValid()) {
                Result.foreground_ = G.foreground_[Input.dark_]; if (Diagnostics) Result.owners_["foreground"] = G.label_;
            }
            if (!Result.background_.isValid() && G.background_[Input.dark_].isValid()) {
                Result.background_ = G.background_[Input.dark_]; if (Diagnostics) Result.owners_["background"] = G.label_;
            }
            quint8 Bit = 1;
            for (const auto* Key : {"bold", "italic", "underline", "strike"}) {
                if (!(FontOwners & Bit) && G.font_.contains(Key)) {
                    FontOwners |= Bit; Result.font_[Key] = G.font_[Key]; if (Diagnostics) Result.owners_[Key] = G.label_;
                }
                Bit <<= 1;
            }
        }
        return Result;
    }

    static std::optional<quint64> number(const QJsonObject& C) {
        bool Ok = false; const auto Value = C["value"].toString().toULongLong(&Ok); if (!Ok) return {};
        quint64 Factor = 1; const auto Type = C["type"].toString(), Unit = C["unit"].toString();
        if (Type == "size") { if (Unit == "KiB") Factor = 1024; else if (Unit == "MiB") Factor = 1024 * 1024; else if (Unit == "GiB") Factor = 1024ull * 1024 * 1024; else if (Unit != "B") return {}; }
        else if (Type == "modified") { if (Unit == "minutes") Factor = 60000; else if (Unit == "hours") Factor = 3600000; else if (Unit == "days") Factor = 86400000; else return {}; }
        if (Value > std::numeric_limits<quint64>::max() / Factor) return {};
        return Value * Factor;
    }
    static std::optional<qint64> utcDate(const QString& Text) {
        const auto Date = QDateTime::fromString(Text, Qt::ISODateWithMs);
        if (!Date.isValid() || (Date.timeSpec() != Qt::UTC && Date.timeSpec() != Qt::OffsetFromUTC)) return {};
        return Date.toMSecsSinceEpoch();
    }
    QList<Group> groups_;
    bool merge_ = false, relativeDates_ = false, needsExtension_ = false, extensionOnly_ = true;
    int cacheConditions_ = 0;
    std::shared_ptr<const int> revision_ = std::make_shared<const int>(0);
    QJsonObject conditionDocument_;
};
} // namespace hc::settings
