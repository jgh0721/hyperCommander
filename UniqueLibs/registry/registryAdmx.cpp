#include "stdafx.h"
#include "registryAdmx.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QXmlStreamReader>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryAdmx, "hc.registry.admx")

namespace hc::registry {
namespace {
constexpr qsizetype MaxXmlBytes = 32 * 1024 * 1024;
constexpr int MaxNodes = 250000, MaxDepth = 96;
QString Reference(const QString& Text, const QMap<QString, QString>& Strings)
{
    if (Text.startsWith("$(string.") && Text.endsWith(')')) {
        const auto Id = Text.mid(9, Text.size() - 10);
        return Strings.value(Id, Id);
    }
    return Text;
}
QString Qualified(const QString& Name, const QString& Own, const QMap<QString, QString>& Imports)
{
    const auto Colon = Name.indexOf(':');
    return Colon < 0 ? Own + ':' + Name : Imports.value(Name.left(Colon), Name.left(Colon)) + Name.mid(Colon);
}
QJsonObject ToJson(const AdmxNode& Node)
{
    QJsonObject Attributes;
    for (auto I = Node.attributes.cbegin(); I != Node.attributes.cend(); ++I) Attributes.insert(I.key(), I.value());
    QJsonArray Children; for (const auto& Child : Node.children) Children.append(ToJson(Child));
    return {{"tag", Node.tag}, {"text", Node.text}, {"attributes", Attributes}, {"children", Children}};
}
std::optional<AdmxNode> FromJson(const QJsonObject& Object, int Depth, int& Count)
{
    if (++Count > MaxNodes || Depth > MaxDepth || !Object["tag"].isString() || !Object["attributes"].isObject()
        || !Object["children"].isArray() || !Object["text"].isString()) return {};
    AdmxNode Node; Node.tag = Object["tag"].toString(); Node.text = Object["text"].toString();
    const auto Attributes = Object["attributes"].toObject();
    for (auto I = Attributes.begin(); I != Attributes.end(); ++I) {
        if (!I.value().isString()) return {}; Node.attributes.insert(I.key(), I.value().toString());
    }
    for (const auto& Item : Object["children"].toArray()) {
        if (!Item.isObject()) return {};
        auto Child = FromJson(Item.toObject(), Depth + 1, Count); if (!Child) return {};
        Node.children.append(std::move(*Child));
    }
    return Node;
}
std::optional<AdmxNode> ReadDocument(const QString& Path, const QString& Cache, const QString& Language,
    AdmxCatalog& Result)
{
    QFile File(Path);
    if (!File.open(QIODevice::ReadOnly) || File.size() > MaxXmlBytes) {
        Result.diagnostics.append(Path + ": cannot read or XML size limit"); return {};
    }
    const auto Bytes = File.read(MaxXmlBytes + 1);
    if (Bytes.size() > MaxXmlBytes || File.error() != QFileDevice::NoError) { Result.diagnostics.append(Path + ": read failed"); return {}; }
    const auto Info = QFileInfo(File);
    const auto Hash = QString::fromLatin1(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex());
    const auto Identity = Info.absoluteFilePath() + '|' + QString::number(Info.size()) + '|'
        + QString::number(Info.lastModified().toMSecsSinceEpoch()) + '|' + Language + '|' + Hash;
    const auto CacheName = QString::fromLatin1(QCryptographicHash::hash(Identity.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".json";
    const auto CachePath = QDir(Cache).filePath(CacheName);
    if (!Cache.isEmpty()) {
        QFile Saved(CachePath);
        if (Saved.open(QIODevice::ReadOnly) && Saved.size() <= 3 * MaxXmlBytes) {
            const auto Data = QJsonDocument::fromJson(Saved.readAll()).object(); int Count = 0;
            if (Data["schema"].toInt() == 2 && Data["identity"].toString() == Identity) {
                auto Node = FromJson(Data["root"].toObject(), 0, Count);
                if (Node) { ++Result.cachedFiles; return Node; }
            }
        }
    }
    QString Error; auto Node = AdmxCatalog::Parse(Bytes, &Error);
    if (!Node) { Result.diagnostics.append(Path + ": " + Error); return {}; }
    ++Result.parsedFiles;
    if (!Cache.isEmpty() && QDir().mkpath(Cache)) {
        QSaveFile Saved(CachePath);
        const auto Encoded = QJsonDocument(QJsonObject{{"schema", 2}, {"identity", Identity}, {"root", ToJson(*Node)}}).toJson(QJsonDocument::Compact);
        if (Saved.open(QIODevice::WriteOnly) && Saved.write(Encoded) == Encoded.size()) Saved.commit();
    }
    return Node;
}
bool Matches(const AdmxNode& Node, const QString& ParentKey, const QString& Key, const std::optional<QString>& Name)
{
    const auto ThisKey = Node.Attribute("key", Node.Attribute("defaultKey", ParentKey));
    if (ThisKey.compare(Key, Qt::CaseInsensitive) == 0
        && (!Name || (Node.attributes.contains("valueName") && Node.Attribute("valueName").compare(*Name, Qt::CaseInsensitive) == 0)
            || Node.tag == "list")) return true;
    for (const auto& Child : Node.children) if (Matches(Child, ThisKey, Key, Name)) return true;
    return false;
}
}
const AdmxNode* AdmxNode::Child(const QString& Tag) const
{
    for (const auto& Item : children) if (Item.tag == Tag) return &Item;
    return nullptr;
}
QString AdmxNode::Attribute(const QString& Name, const QString& Default) const { return attributes.value(Name, Default); }
QString AdmxPolicy::Text(const QString& Ref) const { return Reference(Ref, strings); }
std::optional<AdmxNode> AdmxCatalog::Parse(const QByteArray& Bytes, QString* Error)
{
    const auto Fail = [&](const QString& Text) -> std::optional<AdmxNode> { if (Error) *Error = Text; return {}; };
    if (Bytes.size() > MaxXmlBytes) return Fail("XML size limit");
    QXmlStreamReader Reader(Bytes); QList<AdmxNode> Stack; std::optional<AdmxNode> Root; int Count = 0; QString DocumentNamespace;
    while (!Reader.atEnd()) {
        const auto Token = Reader.readNext();
        // No DTD, external entities, or network resolver is accepted.
        if (Token == QXmlStreamReader::DTD || Token == QXmlStreamReader::EntityReference) return Fail("DTD/entity is not allowed");
        if (Token == QXmlStreamReader::StartElement) {
            if (++Count > MaxNodes || Stack.size() >= MaxDepth) return Fail("XML depth/node limit");
            const auto Namespace = Reader.namespaceUri().toString();
            if (Count == 1) {
                DocumentNamespace = Namespace;
                if (!Namespace.isEmpty() && Namespace != "http://schemas.microsoft.com/GroupPolicy/2006/07/PolicyDefinitions"
                    && Namespace != "https://schemas.microsoft.com/GroupPolicy/2006/07/PolicyDefinitions"
                    && Namespace != "http://www.microsoft.com/GroupPolicy/PolicyDefinitions"
                    && Namespace != "https://www.microsoft.com/GroupPolicy/PolicyDefinitions") return Fail("unsupported XML namespace");
            } else if (Namespace != DocumentNamespace) return Fail("mixed XML namespaces");
            AdmxNode Node; Node.tag = Reader.name().toString();
            for (const auto& Attribute : Reader.attributes()) {
                const auto AttributeNamespace = Attribute.namespaceUri();
                if (!AttributeNamespace.isEmpty()) {
                    if (AttributeNamespace == u"http://www.w3.org/2001/XMLSchema-instance" || AttributeNamespace == u"http://www.w3.org/XML/1998/namespace") continue;
                    return Fail("unsupported namespaced attribute");
                }
                Node.attributes.insert(Attribute.name().toString(), Attribute.value().toString());
            }
            Stack.append(std::move(Node));
        } else if (Token == QXmlStreamReader::Characters && !Stack.isEmpty()) Stack.last().text += Reader.text();
        else if (Token == QXmlStreamReader::EndElement) {
            if (Stack.isEmpty()) return Fail("unbalanced XML");
            auto Node = Stack.takeLast();
            if (Stack.isEmpty()) { if (Root) return Fail("multiple roots"); Root = std::move(Node); }
            else Stack.last().children.append(std::move(Node));
        }
    }
    if (Reader.hasError()) return Fail(Reader.errorString());
    if (!Root || !Stack.isEmpty()) return Fail("missing XML root");
    return Root;
}
AdmxCatalog AdmxCatalog::Load(const QString& Directory, const QString& Language, const QString& CacheDirectory, const std::atomic_bool* Cancel)
{
    AdmxCatalog Result; QElapsedTimer Timer; Timer.start();
    if (Language.contains('/') || Language.contains('\\') || Language.contains("..")) {
        Result.diagnostics.append("invalid language directory"); return Result;
    }
    const auto Files = QDir(Directory).entryInfoList({"*.admx"}, QDir::Files, QDir::Name);
    QMap<QString, QString> CategoryParents; QSet<QString> SeenNamespaces, SeenPolicies;
    for (const auto& File : Files) {
        if (Cancel && Cancel->load()) break;
        const auto Definition = ReadDocument(File.absoluteFilePath(), CacheDirectory.isEmpty() ? QString() : QDir(CacheDirectory).filePath("definitions"), {}, Result);
        if (!Definition || Definition->tag != "policyDefinitions") continue;
        const auto* Namespaces = Definition->Child("policyNamespaces");
        const auto* Target = Namespaces ? Namespaces->Child("target") : nullptr;
        if (!Target || Target->Attribute("namespace").isEmpty()) { Result.diagnostics.append(File.fileName() + ": missing namespace"); continue; }
        const auto Own = Target->Attribute("namespace"); QMap<QString, QString> Imports;
        if (SeenNamespaces.contains(Own)) {
            Result.diagnostics.append(File.fileName() + ": duplicate policy namespace " + Own);
            for (auto& Policy : Result.policies) if (Policy.nameSpace == Own) Policy.unsupportedReason = "duplicate policy namespace";
            continue;
        }
        SeenNamespaces.insert(Own);
        for (const auto& Item : Namespaces->children) Imports.insert(Item.Attribute("prefix"), Item.Attribute("namespace"));
        QMap<QString, QString> Strings; QMap<QString, AdmxNode> Presentations;
        // English first, then requested language: fallback applies per resource.
        QStringList Languages{"en-US"}; if (Language.compare("en-US", Qt::CaseInsensitive) != 0) Languages.append(Language);
        for (const auto& Locale : Languages) {
            const auto Path = QDir(Directory).filePath(Locale + '/' + File.completeBaseName() + ".adml");
            if (!QFileInfo::exists(Path)) continue;
            const auto Translation = ReadDocument(Path, CacheDirectory.isEmpty() ? QString() : QDir(CacheDirectory).filePath("translations"), Locale, Result);
            const auto* Resources = Translation ? Translation->Child("resources") : nullptr; if (!Resources) continue;
            if (const auto* Table = Resources->Child("stringTable"))
                for (const auto& Item : Table->children) if (Item.tag == "string") Strings.insert(Item.Attribute("id"), Item.text);
            if (const auto* Table = Resources->Child("presentationTable"))
                for (const auto& Item : Table->children) if (Item.tag == "presentation") Presentations.insert(Item.Attribute("id"), Item);
        }
        const auto Qualify = [&](const QString& Name) { return Qualified(Name, Own, Imports); };
        if (const auto* Categories = Definition->Child("categories")) for (const auto& Item : Categories->children) {
            const auto Id = Qualify(Item.Attribute("name")); Result.categories.insert(Id, Reference(Item.Attribute("displayName"), Strings));
            if (const auto* Parent = Item.Child("parentCategory")) CategoryParents.insert(Id, Qualify(Parent->Attribute("ref")));
        }
        if (const auto* Supported = Definition->Child("supportedOn")) if (const auto* Definitions = Supported->Child("definitions"))
            for (const auto& Item : Definitions->children) Result.supported.insert(Qualify(Item.Attribute("name")), Reference(Item.Attribute("displayName"), Strings));
        const auto* Policies = Definition->Child("policies"); if (!Policies) continue;
        for (const auto& Item : Policies->children) {
            if (Item.tag != "policy") continue;
            AdmxPolicy Policy; Policy.id = Qualify(Item.Attribute("name")); Policy.nameSpace = Own; Policy.sourcePath = File.absoluteFilePath();
            if (Item.Attribute("name").isEmpty()) { Result.diagnostics.append(File.fileName() + ": missing policy name"); continue; }
            if (SeenPolicies.contains(Policy.id)) {
                Result.diagnostics.append(File.fileName() + ": duplicate policy " + Policy.id);
                for (auto& Existing : Result.policies) if (Existing.id == Policy.id) Existing.unsupportedReason = "duplicate policy name";
                continue;
            }
            SeenPolicies.insert(Policy.id);
            Policy.definition = Item; Policy.strings = Strings; Policy.title = Reference(Item.Attribute("displayName"), Strings);
            Policy.explanation = Reference(Item.Attribute("explainText"), Strings); Policy.scope = Item.Attribute("class"); Policy.key = Item.Attribute("key");
            if (const auto* Category = Item.Child("parentCategory")) Policy.category = Qualify(Category->Attribute("ref"));
            if (const auto* Supported = Item.Child("supportedOn")) Policy.supportedOn = Qualify(Supported->Attribute("ref"));
            auto Presentation = Item.Attribute("presentation");
            if (Presentation.startsWith("$(presentation.") && Presentation.endsWith(')')) Presentation = Presentation.mid(15, Presentation.size() - 16);
            Policy.presentation = Presentations.value(Presentation);
            if (!Item.Attribute("clientExtension").isEmpty()) Policy.unsupportedReason = "custom client extension";
            if (const auto* Elements = Item.Child("elements")) for (const auto& Element : Elements->children) {
                if (!QStringList{"boolean", "decimal", "longDecimal", "text", "multiText", "enum", "list"}.contains(Element.tag))
                    Policy.unsupportedReason = "unsupported element: " + Element.tag;
                if (!Element.Attribute("clientExtension").isEmpty()) Policy.unsupportedReason = "custom client extension";
            }
            if (Policy.scope != "User" && Policy.scope != "Machine" && Policy.scope != "Both") Policy.unsupportedReason = "unknown policy class";
            Result.policies.append(std::move(Policy));
        }
    }
    for (auto& Policy : Result.policies) {
        QStringList Trail; QSet<QString> Seen; auto Id = Policy.category;
        while (!Id.isEmpty() && !Seen.contains(Id) && Trail.size() < 64) {
            Seen.insert(Id); Trail.prepend(Result.categories.value(Id, Id)); Id = CategoryParents.value(Id);
        }
        Policy.category = Trail.join(" › "); Policy.supportedOn = Result.supported.value(Policy.supportedOn, Policy.supportedOn);
    }
    for (int I = 0; I < Result.policies.size(); ++I) {
        const auto& Policy = Result.policies[I]; QSet<QString> Keys;
        const auto Collect = [&](auto&& Self, const AdmxNode& Node, const QString& ParentKey) -> void {
            const auto Key = Node.Attribute("key", Node.Attribute("defaultKey", ParentKey));
            if (!Key.isEmpty()) Keys.insert(Key.toCaseFolded());
            for (const auto& Child : Node.children) Self(Self, Child, Key);
        };
        Collect(Collect, Policy.definition, Policy.key);
        for (const auto& Key : Keys) Result.keyIndex[Key].append(I);
    }
    Result.elapsedMs = Timer.elapsed(); return Result;
}
QList<int> AdmxCatalog::Find(const RegistryLocation& Location, const std::optional<QString>& ValueName) const
{
    QList<int> Result;
    for (const auto I : keyIndex.value(Location.key.toCaseFolded())) {
        const auto& Policy = policies[I];
        if ((Location.hive == Hive::CurrentUser && Policy.scope != "Machine")
            || (Location.hive == Hive::LocalMachine && Policy.scope != "User"))
            if (Matches(Policy.definition, Policy.key, Location.key, ValueName)) Result.append(I);
    }
    return Result;
}
} // namespace hc::registry
