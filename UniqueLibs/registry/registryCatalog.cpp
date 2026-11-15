#include "stdafx.h"
#include "registryCatalog.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QResource>
#include <tuple>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryCatalog, "hc.registry.catalog")
static void InitCatalog() { Q_INIT_RESOURCE(registryCatalog); }
namespace {
bool SegmentMatches(const QString& Pattern, const QString& Text)
{
    const auto Expression = QRegularExpression::wildcardToRegularExpression(Pattern, QRegularExpression::NonPathWildcardConversion);
    return QRegularExpression(Expression, QRegularExpression::CaseInsensitiveOption).match(Text).hasMatch();
}
bool Wild(const QString& Text) { return Text.contains('*') || Text.contains('?'); }
}
namespace hc::registry {
bool BuiltinRegistryCatalog::PathMatches(const QString& Pattern, const QString& Path)
{
    const auto Patterns = Pattern.split('\\'); const auto Parts = Path.split('\\');
    if (Patterns.size() > 64 || Parts.size() > 512) return false;
    QVector<bool> Previous(Parts.size() + 1, false); Previous[0] = true;
    for (const auto& Segment : Patterns) {
        QVector<bool> Next(Parts.size() + 1, false);
        if (Segment == "**") {
            Next[0] = Previous[0];
            for (int I = 1; I <= Parts.size(); ++I) Next[I] = Previous[I] || Next[I - 1];
        } else for (int I = 1; I <= Parts.size(); ++I) Next[I] = Previous[I - 1] && SegmentMatches(Segment, Parts[I - 1]);
        Previous = std::move(Next);
    }
    return Previous.last();
}
BuiltinRegistryCatalog BuiltinRegistryCatalog::Load(const QByteArray& Bytes, const QByteArray& English, const QByteArray& Korean)
{
    BuiltinRegistryCatalog Result;
    if (Bytes.size() > 8 * 1024 * 1024 || English.size() > 8 * 1024 * 1024 || Korean.size() > 8 * 1024 * 1024) {
        Result.errors_.append("catalog size limit"); return Result;
    }
    Result.catalog_ = QJsonDocument::fromJson(Bytes).object(); Result.english_ = QJsonDocument::fromJson(English).object();
    Result.korean_ = QJsonDocument::fromJson(Korean).object();
    if (Result.catalog_["schemaVersion"].toInt() != 1 || !Result.catalog_["entries"].isArray()) {
        Result.errors_.append("catalog schema"); return Result;
    }
    QSet<QString> Ids, Rules;
    for (const auto& Value : Result.catalog_["entries"].toArray()) {
        const auto Item = Value.toObject(); const auto Id = Item["id"].toString(); const auto Pattern = Item["pathPattern"].toString();
        const auto HiveName = Item["hive"].toString(); const auto ViewName = Item["view"].toString();
        const auto Rule = HiveName + '|' + ViewName + '|' + Pattern.toCaseFolded() + '|'
            + (Item["valueNamePattern"].isNull() ? "<key>" : Item["valueNamePattern"].toString().toCaseFolded())
            + '|' + QString::number(Item["priority"].toInt());
        if (Id.isEmpty() || Ids.contains(Id) || Rules.contains(Rule) || Pattern.size() > 2048
            || !QStringList{"HKCR", "HKCU", "HKLM", "HKU", "HKCC"}.contains(HiveName)
            || !QStringList{"both", "32", "64"}.contains(ViewName) || !Item.contains("valueNamePattern")
            || !Item["types"].isArray() || !Item["sourceUrls"].isArray()
            || !Result.english_[Item["titleKey"].toString()].isString() || !Result.english_[Item["descriptionKey"].toString()].isString()) {
            Result.errors_.append("invalid or conflicting catalog entry: " + Id); continue;
        }
        bool Invalid = false;
        for (const auto& Segment : Pattern.split('\\')) if (Segment.contains("**") && Segment != "**") Invalid = true;
        for (const auto& Url : Item["sourceUrls"].toArray()) if (!Url.toString().startsWith("https://")) Invalid = true;
        if (Invalid) { Result.errors_.append("unsafe pattern/source: " + Id); continue; }
        Ids.insert(Id); Rules.insert(Rule); Result.entries_.append(Item);
    }
    return Result;
}
BuiltinRegistryCatalog BuiltinRegistryCatalog::Bundled()
{
    InitCatalog();
    const auto Read = [](const QString& Name) { QFile File(":/hc/registry/" + Name); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray(); };
    return Load(Read("catalog.json"), Read("en-US.json"), Read("ko-KR.json"));
}
RegistryDescription BuiltinRegistryCatalog::Find(const RegistryLocation& Location, const std::optional<QString>& Name,
    std::optional<quint32> Type, const QString& Language) const
{
    RegistryDescription Result; std::tuple<int, int, int, int> Best{-1, -1, -1, -1};
    const auto HiveName = QStringList{"HKCR", "HKCU", "HKLM", "HKU", "HKCC"}.value(int(Location.hive));
    const bool Korean = Language.startsWith("ko", Qt::CaseInsensitive);
    const auto Text = [&](const QString& Key) { return Korean ? korean_.value(Key).toString(english_.value(Key).toString(Key)) : english_.value(Key).toString(Key); };
    for (const auto& Entry : entries_) {
        if (Entry["hive"].toString() != HiveName || (Name.has_value() == Entry["valueNamePattern"].isNull())) continue;
        const auto ViewName = Entry["view"].toString();
        if (ViewName != "both" && ViewName != (Location.view == View::Wow32 ? "32" : "64")) continue;
        const auto Pattern = Entry["pathPattern"].toString(); if (!PathMatches(Pattern, Location.key)) continue;
        const auto ValuePattern = Entry["valueNamePattern"].toString();
        if (Name && !SegmentMatches(ValuePattern, *Name)) continue;
        const auto Types = Entry["types"].toArray();
        if (Type && !Types.isEmpty() && !Types.contains(int(*Type))) continue;
        int Literals = 0; for (const auto& Segment : Pattern.split('\\')) if (!Wild(Segment)) ++Literals;
        const auto Rank = std::tuple{!Wild(Pattern) ? 1 : 0, Literals, Name && !Wild(ValuePattern) ? 1 : 0, Entry["priority"].toInt()};
        if (Rank < Best) continue;
        if (Rank == Best) { Result.ambiguous = true; continue; }
        Best = Rank; Result = {}; Result.id = Entry["id"].toString(); Result.title = Text(Entry["titleKey"].toString());
        Result.text = Text(Entry["descriptionKey"].toString()); Result.category = Entry["category"].toString(); Result.metadata = Entry;
        for (const auto& Warning : Entry["warnings"].toArray()) Result.warnings.append(Text(Warning.toString()));
        for (const auto& Url : Entry["sourceUrls"].toArray()) Result.sourceUrls.append(Url.toString());
    }
    return Result;
}
QJsonArray BuiltinRegistryCatalog::KnownLocations() const { return catalog_["knownLocations"].toArray(); }
} // namespace hc::registry
