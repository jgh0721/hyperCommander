#include "stdafx.h"
#include "registryPolicyDefinition.hpp"

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
#include "commonLibs/system/registryValue.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <limits>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryPolicyDefinition, "hc.registry.policyDefinition")
namespace hc::registry {
namespace {
bool Flag(const AdmxNode& Node, const QString& Name) { const auto Value = Node.Attribute(Name); return Value == "true" || Value == "1"; }
const AdmxNode* Presentation(const AdmxPolicy& Policy, const QString& Id)
{
    for (const auto& Control : Policy.presentation.children) if (Control.Attribute("refId") == Id) return &Control;
    return nullptr;
}
bool Number(const QString& Text, quint64& Value)
{
    if (Text.isEmpty() || Text.startsWith('-') || Text.startsWith('+') || Text != Text.trimmed()) return false;
    for (const auto Char : Text) if (Char < '0' || Char > '9') return false;
    bool Ok = false; Value = Text.toULongLong(&Ok); return Ok;
}
QString Address(const QString& Key, const QString& Name) { return Key.toCaseFolded() + QChar(0) + Name.toCaseFolded(); }
class Compiler
{
public:
    PolicyProgram Run(const AdmxPolicy& Policy, PolicyState State, const QVariantMap& Options)
    {
        if (!Policy.unsupportedReason.isEmpty()) { result_.error = Policy.unsupportedReason; return result_; }
        if (State != PolicyState::Enabled && State != PolicyState::Disabled && State != PolicyState::NotConfigured) {
            result_.error = "Choose an explicit policy state"; return result_;
        }
        if (!RegistryLocation::Parse("HKLM\\" + Policy.key) || Policy.key.isEmpty()) { result_.error = "Invalid policy key"; return result_; }
        const auto& Definition = Policy.definition;
        // Remove definitions from both branches before installing the selected
        // branch, so old enum choices and optional fields cannot survive silently.
        collect(Definition, Policy.key);
        if (State == PolicyState::NotConfigured) return finish();
        const bool Enabled = State == PolicyState::Enabled;
        if (Definition.attributes.contains("valueName")) {
            if (const auto* Value = Definition.Child(Enabled ? "enabledValue" : "disabledValue"))
                literal(*Value, Policy.key, Definition.Attribute("valueName"), false);
            else numeric(Policy.key, Definition.Attribute("valueName"), Enabled ? 1 : 0, false, false);
        }
        if (const auto* List = Definition.Child(Enabled ? "enabledList" : "disabledList")) valueList(*List, Policy.key);
        if (Enabled) if (const auto* Elements = Definition.Child("elements")) for (const auto& Element : Elements->children) {
            if (!result_.error.isEmpty()) break;
            const auto Id = Element.Attribute("id"), Key = Element.Attribute("key", Policy.key), Name = Element.Attribute("valueName");
            const auto Input = Options.value(Id);
            if (Element.tag == "boolean") {
                const bool Checked = Input.toBool();
                if (Element.attributes.contains("valueName")) {
                    if (const auto* Value = Element.Child(Checked ? "trueValue" : "falseValue")) literal(*Value, Key, Name, false);
                    else numeric(Key, Name, Checked ? 1 : 0, false, false);
                }
                if (const auto* List = Element.Child(Checked ? "trueList" : "falseList")) valueList(*List, Key);
            } else if (Element.tag == "decimal" || Element.tag == "longDecimal") {
                if (!Input.isValid() || Input.toString().isEmpty()) { if (Flag(Element, "required")) fail("Required number: " + Id); continue; }
                quint64 Value = 0, Minimum = 0, Maximum = 0;
                const bool Long = Element.tag == "longDecimal";
                if (!Number(Input.toString(), Value) || !Number(Element.Attribute("minValue", "0"), Minimum)
                    || !Number(Element.Attribute("maxValue", "9999"), Maximum) || Value < Minimum || Value > Maximum
                    || (!Long && Value > std::numeric_limits<quint32>::max())) { fail("Number outside policy bounds: " + Id); continue; }
                numeric(Key, Name, Value, Long, Flag(Element, "storeAsText"), Flag(Element, "soft"));
            } else if (Element.tag == "text" || Element.tag == "multiText") {
                const auto Text = Input.toString(); quint64 Maximum = 1023;
                if (Element.attributes.contains("maxLength") && !Number(Element.Attribute("maxLength"), Maximum)) { fail("Invalid maximum length"); continue; }
                bool LengthValid = quint64(Text.size()) <= Maximum;
                if (Element.tag == "multiText") {
                    quint64 MaxStrings = 0;
                    if (!Number(Element.Attribute("maxStrings", "0"), MaxStrings)) { fail("Invalid string count"); continue; }
                    const auto Lines = Text.split('\n', Qt::SkipEmptyParts); LengthValid = !MaxStrings || quint64(Lines.size()) <= MaxStrings;
                    for (const auto& Line : Lines) LengthValid &= quint64(Line.size()) <= Maximum;
                }
                if ((Flag(Element, "required") && Text.isEmpty()) || !LengthValid || Text.contains(QChar(0))) { fail("Invalid text: " + Id); continue; }
                if (!Input.isValid()) continue;
                const auto Type = Element.tag == "multiText" ? REG_MULTI_SZ : Flag(Element, "expandable") ? REG_EXPAND_SZ : REG_SZ;
                const auto Bytes = RegistryValueCodec::EncodeText(Type, Text);
                if (!Bytes) { fail("Text encoding failed"); continue; }
                put(Key, Name, RegistryValue{Name, quint32(Type), Bytes.value}, Flag(Element, "soft"));
            } else if (Element.tag == "enum") {
                bool Ok = false; const auto Choice = Input.toInt(&Ok);
                QList<const AdmxNode*> Items; for (const auto& Item : Element.children) if (Item.tag == "item") Items.append(&Item);
                if (!Ok || Choice < 0 || Choice >= Items.size()) { fail("Select an enumeration item: " + Id); continue; }
                const auto* Item = Items[Choice];
                if (const auto* Value = Item->Child("value")) literal(*Value, Key, Name, false);
                if (const auto* Values = Item->Child("valueList")) valueList(*Values, Key);
            } else if (Element.tag == "list") {
                const auto Items = Input.toList();
                if (Items.size() > 65536) { fail("Policy list is too large"); continue; }
                // Non-additive lists own this key's values. The caller must
                // enumerate and preview every affected value before applying.
                if (!Flag(Element, "additive") && !result_.clearKeys.contains(Key, Qt::CaseInsensitive)) result_.clearKeys.append(Key);
                QSet<QString> Names;
                for (int I = 0; I < Items.size(); ++I) {
                    const auto Item = Items[I].toMap();
                    const auto ItemName = Flag(Element, "explicitValue") ? Item.value("name").toString() : Element.Attribute("valuePrefix") + QString::number(I + 1);
                    if (Names.contains(ItemName.toCaseFolded())) { fail("Duplicate list value name"); break; }
                    Names.insert(ItemName.toCaseFolded());
                    const auto Type = Flag(Element, "expandable") ? REG_EXPAND_SZ : REG_SZ;
                    const auto Bytes = RegistryValueCodec::EncodeText(Type, Item.value("data").toString());
                    if (!Bytes) { fail("Invalid list text"); break; }
                    put(Key, ItemName, RegistryValue{ItemName, quint32(Type), Bytes.value});
                }
            } else fail("Unsupported policy element: " + Element.tag);
        }
        return finish();
    }
private:
    void fail(const QString& Error) { if (result_.error.isEmpty()) result_.error = Error; }
    void put(const QString& Key, const QString& Name, std::optional<RegistryValue> Value, bool Soft = false)
    {
        if (!WinRegistryBackend::ValidName(Name, false) || Key.isEmpty() || !RegistryLocation::Parse("HKLM\\" + Key)) { fail("Invalid registry address"); return; }
        if (Name.startsWith("**")) { fail("Reserved policy directive name"); return; }
        const auto Id = Address(Key, Name);
        if (active_.contains(Id) && assignments_[Id].value != Value) { fail("Conflicting assignments in policy"); return; }
        if (Value) Value->name = Name;
        const bool Deletion = !Value;
        assignments_[Id] = {Key, Name, std::move(Value), Soft, Deletion};
        active_.insert(Id);
    }
    void numeric(const QString& Key, const QString& Name, quint64 Number, bool Long, bool Text, bool Soft = false)
    {
        const quint32 Type = Text ? REG_SZ : Long ? REG_QWORD : REG_DWORD;
        const auto Bytes = Text ? RegistryValueCodec::EncodeText(Type, QString::number(Number)) : RegistryValueCodec::EncodeNumber(Type, Number);
        if (!Bytes) { fail("Invalid numeric value"); return; }
        put(Key, Name, RegistryValue{Name, Type, Bytes.value}, Soft);
    }
    void literal(const AdmxNode& Container, const QString& Key, const QString& Name, bool Soft)
    {
        if (Container.children.size() != 1) { fail("Invalid policy value"); return; }
        const auto& Value = Container.children.first();
        if (Value.tag == "delete") { put(Key, Name, {}, Soft); return; }
        if (Value.tag == "string") {
            const auto Bytes = RegistryValueCodec::EncodeText(REG_SZ, Value.text);
            if (!Bytes) { fail("Invalid literal text"); return; }
            put(Key, Name, RegistryValue{Name, REG_SZ, Bytes.value}, Soft); return;
        }
        quint64 NumberValue = 0;
        if ((Value.tag != "decimal" && Value.tag != "longDecimal") || !Number(Value.Attribute("value"), NumberValue)
            || (Value.tag == "decimal" && NumberValue > std::numeric_limits<quint32>::max())) { fail("Unsupported literal value"); return; }
        const quint32 Type = Value.tag == "decimal" ? REG_DWORD : REG_QWORD;
        const auto Bytes = RegistryValueCodec::EncodeNumber(Type, NumberValue);
        put(Key, Name, RegistryValue{Name, Type, Bytes.value}, Soft);
    }
    void valueList(const AdmxNode& List, const QString& ParentKey)
    {
        const auto Key = List.Attribute("defaultKey", ParentKey);
        for (const auto& Item : List.children) {
            if (Item.tag != "item") { fail("Unsupported value-list item"); continue; }
            const auto* Value = Item.Child("value"); if (!Value) { fail("Missing list value"); continue; }
            literal(*Value, Item.Attribute("key", Key), Item.Attribute("valueName"), Flag(Item, "soft"));
        }
    }
    void collect(const AdmxNode& Node, const QString& ParentKey)
    {
        const auto Key = Node.Attribute("key", Node.Attribute("defaultKey", ParentKey));
        if (Node.attributes.contains("valueName")) {
            const auto Name = Node.Attribute("valueName"); assignments_.insert(Address(Key, Name), {Key, Name, {}, Flag(Node, "soft")});
        }
        if (Node.tag == "list" && Flag(Node, "additive") && !result_.additiveKeys.contains(Key, Qt::CaseInsensitive)) result_.additiveKeys.append(Key);
        if (Node.tag == "list" && !Flag(Node, "additive") && !result_.clearKeys.contains(Key, Qt::CaseInsensitive)) result_.clearKeys.append(Key);
        for (const auto& Child : Node.children) collect(Child, Key);
    }
    PolicyProgram finish()
    {
        if (result_.error.isEmpty()) for (auto I = assignments_.cbegin(); I != assignments_.cend(); ++I) result_.assignments.append(I.value());
        return result_;
    }
    PolicyProgram result_;
    QMap<QString, PolicyAssignment> assignments_;
    QSet<QString> active_;
};
}
PolicyProgram RegistryPolicyDefinition::Compile(const AdmxPolicy& Policy, PolicyState State, const QVariantMap& Options) { return Compiler().Run(Policy, State, Options); }
QVariantMap RegistryPolicyDefinition::Defaults(const AdmxPolicy& Policy)
{
    QVariantMap Result; const auto* Elements = Policy.definition.Child("elements"); if (!Elements) return Result;
    for (const auto& Element : Elements->children) {
        const auto Id = Element.Attribute("id"); const auto* Control = Presentation(Policy, Id);
        if (Element.tag == "boolean") Result.insert(Id, Control && Flag(*Control, "defaultChecked"));
        else if (Element.tag == "enum") Result.insert(Id, Control ? Control->Attribute("defaultItem", "0").toInt() : 0);
        else if (Element.tag == "decimal" || Element.tag == "longDecimal")
            Result.insert(Id, Control ? Control->Attribute("defaultValue", "1") : Element.Attribute("minValue", "0"));
        else if (Element.tag == "list") Result.insert(Id, QVariantList());
        else {
            const auto* Value = Control ? Control->Child(Control->tag == "comboBox" ? "default" : "defaultValue") : nullptr;
            Result.insert(Id, Value ? Value->text : QString());
        }
    }
    return Result;
}
QString RegistryPolicyDefinition::OptionLabel(const AdmxPolicy& Policy, const AdmxNode& Element)
{
    const auto* Control = Presentation(Policy, Element.Attribute("id"));
    if (!Control) return Element.Attribute("id");
    const auto* Label = Control->Child("label");
    const auto Text = Label ? Label->text : Control->text.trimmed();
    return Text.isEmpty() ? Element.Attribute("id") : Policy.Text(Text);
}
QString RegistryPolicyDefinition::StateText(PolicyState State)
{
    switch (State) {
    case PolicyState::NotConfigured: return QObject::tr("구성되지 않음");
    case PolicyState::Enabled: return QObject::tr("사용");
    case PolicyState::Disabled: return QObject::tr("사용 안 함");
    case PolicyState::Partial: return QObject::tr("부분 구성");
    default: return QObject::tr("알 수 없음");
    }
}
} // namespace hc::registry
