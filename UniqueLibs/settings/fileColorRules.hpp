#pragma once
#include <QColor>
#include <QFont>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QFileInfo>
#include <QDateTime>
#include <QSet>
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
class FileColorRules
{
public:
    static QStringList Extensions(const QString& Input) {
        QStringList Result;
        for (auto Part : Input.toLower().split(QRegularExpression("[,;|\\s]+"), Qt::SkipEmptyParts)) {
            if (Part.startsWith('.')) Part.remove(0, 1);
            if (!Result.contains(Part)) Result.append(Part);
        }
        return Result;
    }
    static QJsonObject Empty() { return {{"version", 1}, {"mode", "first"}, {"groups", QJsonArray{}}}; }
    static bool Supported(const QJsonObject& Document) { return Document["version"] == QJsonValue(1) || Document["version"] == QJsonValue(2); }
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
    static QString Validate(const QJsonObject& Document) {
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
            for (const auto C : Conditions) {
                if (!C.isObject()) return QObject::tr("조건 형식이 올바르지 않습니다.");
                auto Condition = C.toObject(); const auto LegacyType = Condition["type"].toString();
                if (Document["version"] == QJsonValue(1)) {
                    if (!QStringList{"extension", "pattern", "folder", "attributes", "sizeMore", "sizeLess", "sizeEqual"}.contains(LegacyType)) return QObject::tr("알 수 없는 조건 종류입니다.");
                    Condition = NormalizeCondition(Condition, 1);
                }
                const auto Type = Condition["type"].toString(), Op = Condition["op"].toString(), Text = Condition["value"].toString();
                if (Type == "pattern") {
                    const auto Patterns = Condition["value"].toArray();
                    if ((Op != "matches" && Op != "notMatches") || !Condition["value"].isArray() || Patterns.isEmpty() || Patterns.size() > 32) return QObject::tr("파일명 패턴은 1~32개입니다.");
                    for (const auto P : Patterns) if (!P.isString() || P.toString().trimmed().isEmpty() || P.toString().size() > 512 || P.toString().contains('/') || P.toString().contains('\\') || P.toString().contains(QChar::Null)) return QObject::tr("파일명 패턴에는 경로 없이 1~512자를 입력하세요.");
                    continue;
                }
                if (!Condition["value"].isString() || Text.size() > 512 || Text.trimmed().isEmpty()) return QObject::tr("조건은 1~512자로 입력하세요.");
                if (Type == "extension") {
                    const auto Parts = Extensions(Text);
                    if ((Op != "in" && Op != "notIn") || Parts.isEmpty()) return QObject::tr("확장자 목록과 비교 방법을 확인하세요.");
                    for (const auto& Part : Parts) if (Part != "<none>" && !QRegularExpression("^[\\p{L}\\p{N}_+-]+$").match(Part).hasMatch()) return QObject::tr("단순 확장자를 입력하세요. 무확장자는 <none>, 파일명 패턴은 별도 조건을 사용합니다.");
                } else if (Type == "folder") {
                    if (Op != "is" || (Text != "true" && Text != "false")) return QObject::tr("종류 조건은 폴더 또는 파일입니다.");
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
    void Load(const QJsonObject& Document) {
        groups_.clear(); relativeDates_ = false; merge_ = Document["mode"] == "merge";
        if (!Validate(Document).isEmpty()) return;
        for (const auto Value : Document["groups"].toArray()) {
            const auto Object = Value.toObject(); if (!Object["enabled"].toBool(true)) continue;
            Group G; G.object_ = Object;
            for (const auto C : Object["conditions"].toArray()) {
                const auto O = NormalizeCondition(C.toObject(), Document["version"].toInt());
                Condition Item; Item.type_ = O["type"].toString(); Item.op_ = O["op"].toString(); Item.value_ = O["value"].toString();
                if (Item.type_ == "extension") Item.extensions_ = Extensions(Item.value_);
                if (Item.type_ == "pattern") for (const auto Pattern : O["value"].toArray()) Item.patterns_.append(QRegularExpression(QRegularExpression::wildcardToRegularExpression(Pattern.toString()), QRegularExpression::CaseInsensitiveOption));
                Item.number_ = number(O).value_or(0); Item.date_ = utcDate(Item.value_).value_or(0);
                relativeDates_ |= Item.type_ == "modified" && Item.op_ == "within";
                G.conditions_.append(Item);
            }
            groups_.append(G);
        }
    }
    bool HasRelativeDates() const { return relativeDates_; }
    FileRuleResult Evaluate(const QString& Name, quint32 Attributes, quint64 Size, bool Dark) const { return Evaluate(FileRuleInput{Name, Attributes, Size, Dark}); }
    FileRuleResult Evaluate(const FileRuleInput& Input) const {
        FileRuleResult Result; if (Input.name_ == "..") return Result;
        const bool Folder = (Input.attributes_ & 0x10) != 0;
        auto Ext = QFileInfo(Input.name_).suffix().toLower(); if (Ext.isEmpty()) Ext = "<none>";
        for (const auto& G : groups_) {
            const bool All = G.object_["join"] == "all"; bool Match = All;
            for (const auto& C : G.conditions_) {
                bool Hit = false;
                if (C.type_ == "extension") Hit = !Folder && (C.extensions_.contains(Ext) != (C.op_ == "notIn"));
                else if (C.type_ == "pattern") { for (const auto& Pattern : C.patterns_) Hit |= Pattern.match(Input.name_).hasMatch(); if (C.op_ == "notMatches") Hit = !Hit; }
                else if (C.type_ == "folder") Hit = Folder == (C.value_ == "true");
                else if (C.type_ == "attributes") Hit = C.op_ == "any" ? (Input.attributes_ & C.number_) != 0 : (Input.attributes_ & C.number_) == C.number_;
                else if (C.type_ == "size" && !Folder) {
                    if (C.op_ == "gt") Hit = Input.size_ > C.number_; else if (C.op_ == "ge") Hit = Input.size_ >= C.number_;
                    else if (C.op_ == "lt") Hit = Input.size_ < C.number_; else if (C.op_ == "le") Hit = Input.size_ <= C.number_;
                    else if (C.op_ == "eq") Hit = Input.size_ == C.number_; else Hit = Input.size_ != C.number_;
                } else if (C.type_ == "modified" && Input.modifiedUtcMs_) {
                    if (C.op_ == "before") Hit = *Input.modifiedUtcMs_ < C.date_; else if (C.op_ == "after") Hit = *Input.modifiedUtcMs_ > C.date_;
                    else {
                        Hit = *Input.modifiedUtcMs_ <= Input.referenceUtcMs_ && quint64(Input.referenceUtcMs_) - quint64(*Input.modifiedUtcMs_) <= C.number_;
                        std::optional<qint64> Change;
                        if (*Input.modifiedUtcMs_ > Input.referenceUtcMs_) Change = *Input.modifiedUtcMs_;
                        else if (C.number_ < quint64(std::numeric_limits<qint64>::max()) - quint64(*Input.modifiedUtcMs_)) Change = qint64(quint64(*Input.modifiedUtcMs_) + C.number_ + 1);
                        if (Change && *Change > Input.referenceUtcMs_ && (!Result.validUntilUtcMs_ || *Change < *Result.validUntilUtcMs_)) Result.validUntilUtcMs_ = Change;
                    }
                } else if (C.type_ == "link") {
                    const bool Shortcut = !Folder && (Ext == "lnk" || Ext == "url");
                    const bool Reparse = (Input.attributes_ & 0x400) != 0;
                    // Cloud placeholders and unknown reparse tags are not proof
                    // of a symbolic link. Never stat a file on the UI thread.
                    const bool Known = !Reparse || Input.reparseTag_.value_or(0) != 0;
                    const bool Symbolic = Reparse && (Input.reparseTag_ == 0xa000000cu || Input.reparseTag_ == 0xa0000003u);
                    if (C.value_ == "shortcut") Hit = Shortcut; else if (C.value_ == "symbolic") Hit = Symbolic;
                    else if (C.value_ == "any") Hit = Shortcut || Symbolic; else Hit = Known && !Shortcut && !Symbolic;
                }
                Match = All ? Match && Hit : Match || Hit;
            }
            if (!Match) continue;
            Result.matched_ = true; const auto Label = G.object_["name"].toString(); Result.groups_.append(Label);
            const auto Colors = FileColorRules::Colors(G.object_, Input.dark_);
            for (const auto& Key : {"foreground", "background"}) if (Colors.contains(Key) && !Result.owners_.contains(Key)) {
                (QString(Key) == "foreground" ? Result.foreground_ : Result.background_) = QColor(Colors[Key].toString()); Result.owners_[Key] = Label;
            }
            const auto Font = G.object_["font"].toObject();
            for (const auto& Key : {"bold", "italic", "underline", "strike"}) if (Font.contains(Key) && !Result.owners_.contains(Key)) { Result.font_[Key] = Font[Key]; Result.owners_[Key] = Label; }
            if (!merge_) break;
        }
        return Result;
    }
private:
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
    struct Condition { QString type_, op_, value_; QStringList extensions_; QList<QRegularExpression> patterns_; quint64 number_ = 0; qint64 date_ = 0; };
    struct Group { QJsonObject object_; QList<Condition> conditions_; };
    QList<Group> groups_;
    bool merge_ = false, relativeDates_ = false;
};
} // namespace hc::settings
