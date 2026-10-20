#include "qtWorkspaceState.hpp"
#include "commonLibs/layout/productLayout.hpp"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace hc::workspace {
namespace {
constexpr int MaxTabs = 64, MaxSelections = 10000, MaxHistory = 64, MaxBytes = 4 * 1024 * 1024;
QString NewId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool Text(const QJsonValue& Value, int Limit = 32768) {
    return Value.isString() && Value.toString().size() <= Limit && !Value.toString().contains(QChar(0));
}
// Tabs store the absolute manifest path of their archive provider. Built-in manifests moved out of
// <ROOT>/BuiltInPlugin/<Name>/; map those to the current package so saved archive tabs still restore.
QString MigrateProvider(const QString& Provider) {
    static const std::pair<QLatin1StringView, const char*> Moved[] = {
        {QLatin1StringView("/BuiltInPlugin/IsoImage/"), hc::layout::IsoImagePackage},
        {QLatin1StringView("/BuiltInPlugin/SevenZip/"), hc::layout::SevenZipPackage},
        {QLatin1StringView("/BuiltInPlugin/AlzEgg/"), hc::layout::AlzEggPackage},
        {QLatin1StringView("/BuiltInPlugin/HyperViewer/"), hc::layout::HyperViewerPackage}};
    const auto Path = QDir::fromNativeSeparators(Provider);
    for (const auto& [Old, New] : Moved) {
        const auto At = Path.lastIndexOf(Old, -1, Qt::CaseInsensitive);
        const auto Name = At < 0 ? QString() : Path.mid(At + Old.size());
        if (!Name.isEmpty() && !Name.contains('/')) return Path.left(At + 1) + New + '/' + Name;
    }
    return Provider;
}
}
void RecordNavigation(TabState& Tab, const storage::Locator& Location, int RequestedIndex) {
    if (RequestedIndex >= 0 && RequestedIndex < Tab.history_.size() && Tab.history_[RequestedIndex] == Location) {
        Tab.historyIndex_ = RequestedIndex; return;
    }
    if (Tab.historyIndex_ >= 0 && Tab.historyIndex_ < Tab.history_.size() && Tab.history_[Tab.historyIndex_] == Location) return;
    Tab.history_.resize(std::clamp(Tab.historyIndex_ + 1, 0, int(Tab.history_.size())));
    Tab.history_.append(Location);
    if (Tab.history_.size() > MaxHistory) Tab.history_.removeFirst();
    Tab.historyIndex_ = int(Tab.history_.size()) - 1;
}
QJsonObject EncodeLocation(const storage::Locator& Location) {
    QJsonArray Hops;
    for (const auto& Hop : Location.nesting_) Hops.append(QJsonObject{{"provider", QString::fromStdString(Hop.provider_)}, {"entry", QString::fromStdString(Hop.entry_)}});
    return {{"kind", int(Location.kind_)}, {"root", QString::fromStdString(Location.root_)}, {"nesting", Hops}};
}
std::optional<storage::Locator> DecodeLocation(const QJsonObject& Object) {
    const auto Kind = Object.value("kind");
    if (!Kind.isDouble() || Kind.toDouble() != Kind.toInt(-1) || Kind.toInt() < 0 || Kind.toInt() > int(storage::LocatorKind::Plugin) || !Text(Object.value("root"))) return {};
    storage::Locator Result{storage::LocatorKind(Kind.toInt()), Object.value("root").toString().toStdString()};
    if (!Object.value("nesting").isArray() || Object.value("nesting").toArray().size() > 32) return {};
    for (const auto& Value : Object.value("nesting").toArray()) {
        if (!Value.isObject()) return {};
        const auto Hop = Value.toObject();
        if (!Text(Hop.value("provider"), 1024) || !Text(Hop.value("entry"))) return {};
        Result.nesting_.push_back({Hop.value("provider").toString().toStdString(), Hop.value("entry").toString().toStdString()});
    }
    return Result;
}
QString LocationKey(const storage::Locator& Location) {
    return QString::fromUtf8(QJsonDocument(EncodeLocation(Location)).toJson(QJsonDocument::Compact));
}
QString PluginPath(const storage::Locator& Location) {
    auto Path = QString::fromStdString(Location.root_);
    const auto Scheme = Path.indexOf("://");
    if (Scheme >= 0) { const auto Slash = Path.indexOf('/', Scheme + 3); Path = Slash < 0 ? "/" : Path.mid(Slash); }
    else if (Path.startsWith("wfx:") || Path.startsWith("dopus:")) {
        const auto End = Path.indexOf(':', Path.indexOf(':') + 1);
        if (End < 0) return {}; Path = Path.mid(End + 1);
    }
    Path.replace('/', '\\');
    if (!Path.startsWith('\\') || Path.startsWith("\\\\")) return {};
    for (const auto& Part : Path.split('\\')) if (Part == "." || Part == "..") return {};
    return Path;
}
QString SelectionKey(const storage::EntrySnapshot& Entry) {
    if (Entry.locator_.kind_ == storage::LocatorKind::Plugin) {
        auto Location = Entry.locator_; Location.root_ = PluginPath(Location).toStdString();
        return LocationKey(Location);
    }
    if (Entry.id_.scope_ == storage::IdentityScope::Persistent)
        return QString::fromUtf8(QJsonDocument(QJsonArray{QString::fromStdString(Entry.id_.providerId_.value_), QString::fromStdString(Entry.id_.key_)}).toJson(QJsonDocument::Compact));
    return LocationKey(Entry.locator_);
}
void DirectoryIndex::Publish(const std::vector<storage::EntrySnapshot>& Entries) {
    QMap<QString, Record> Next;
    for (const auto& Entry : Entries) {
        const auto Key = SelectionKey(Entry);
        auto Previous = records_.constFind(Key);
        Record Value{Entry, {}};
        if (Previous != records_.cend() && Entry.id_.revision_ && Previous->entry_.id_ == Entry.id_
            && Previous->entry_.locator_ == Entry.locator_ && Previous->entry_.size_ == Entry.size_
            && Previous->entry_.modifiedUtcMs_ == Entry.modifiedUtcMs_) Value.values_ = Previous->values_;
        Next.insert(Key, std::move(Value));
    }
    records_ = std::move(Next); ++generation_;
}
void DirectoryIndex::Append(const std::vector<storage::EntrySnapshot>& Entries) {
    for (const auto& Entry : Entries) records_.insert(SelectionKey(Entry), Record{Entry, {}});
}
bool DirectoryIndex::Put(const storage::EntrySnapshot& Entry, const QMap<QString, QString>& Values, quint64 Generation) {
    // A refresh may finish while metadata is being queried. An unchanged
    // revision still describes the same content; unversioned entries require
    // the exact snapshot generation to prevent delayed results being reused.
    if (Generation == 0 || Generation > generation_ || (Generation != generation_ && !Entry.id_.revision_)) return false;
    auto Found = records_.find(SelectionKey(Entry));
    if (Found == records_.end() || Found->entry_.id_ != Entry.id_ || Found->entry_.locator_ != Entry.locator_
        || Found->entry_.size_ != Entry.size_ || Found->entry_.modifiedUtcMs_ != Entry.modifiedUtcMs_
        || Found->entry_.attributes_ != Entry.attributes_) return false;
    for (auto It = Values.begin(); It != Values.end(); ++It) Found->values_.insert(It.key(), It.value());
    return true;
}
QMap<QString, QString> DirectoryIndex::Metadata(const storage::EntrySnapshot& Entry) const {
    const auto Found = records_.constFind(SelectionKey(Entry));
    return Found == records_.cend() || Found->entry_.id_ != Entry.id_ || Found->entry_.locator_ != Entry.locator_
        ? QMap<QString, QString>{} : Found->values_;
}
bool DirectoryIndex::Matches(const storage::EntrySnapshot& Entry, const QString& Query) const {
    if (Query.isEmpty()) return true;
    auto Fields = Metadata(Entry);
    Fields.insert("name", QString::fromStdString(Entry.name_));
    Fields.insert("type", Entry.directory_ ? "folder" : "file");
    Fields.insert("size", QString::number(Entry.size_));
    Fields.insert("path", QString::fromStdString(Entry.locator_.root_));
    const auto Name = QString::fromStdString(Entry.name_);
    Fields.insert("ext", !Entry.directory_ && Name.contains('.') ? Name.section('.', -1) : QString{});
    QString All; for (auto It = Fields.begin(); It != Fields.end(); ++It) All += It.value() + '\n';
    // Space-separated terms are ANDed. Field:value matches native or loaded
    // plugin metadata; unknown fields cannot accidentally match another column.
    for (const auto& Term : Query.split(' ', Qt::SkipEmptyParts)) {
        const auto Colon = Term.indexOf(':');
        if (Colon <= 0) { if (!All.contains(Term, Qt::CaseInsensitive)) return false; continue; }
        const auto Field = Term.left(Colon), Value = Term.mid(Colon + 1);
        bool Matched = false;
        for (auto It = Fields.begin(); It != Fields.end(); ++It)
            if (It.key().compare(Field, Qt::CaseInsensitive) == 0 && It.value().contains(Value, Qt::CaseInsensitive)) { Matched = true; break; }
        if (!Matched) return false;
    }
    return true;
}
QJsonObject MergeView(const QJsonObject& Parent, const QJsonObject& Override) {
    auto Result = Parent;
    for (auto It = Override.begin(); It != Override.end(); ++It)
        Result[It.key()] = It.value().isObject() && Result.value(It.key()).isObject()
            ? QJsonValue(MergeView(Result.value(It.key()).toObject(), It.value().toObject())) : QJsonValue(It.value());
    return Result;
}
QByteArray Encode(const State& Value) {
    const auto Pane = [](const PaneState& Input) {
        QJsonArray Tabs;
        for (const auto& Tab : Input.tabs_) {
            QJsonArray History; for (const auto& Location : Tab.history_) History.append(EncodeLocation(Location));
            Tabs.append(QJsonObject{{"id", Tab.id_}, {"location", EncodeLocation(Tab.location_)}, {"provider", Tab.provider_}, {"nativeRoot", Tab.nativeRoot_},
                {"selection", QJsonArray::fromStringList(Tab.selection_)}, {"focus", Tab.focus_}, {"view", Tab.view_}, {"history", History}, {"historyConnections", Tab.historyConnections_}, {"historyIndex", Tab.historyIndex_}});
        }
        return QJsonObject{{"active", Input.active_}, {"tabs", Tabs}};
    };
    return QJsonDocument(QJsonObject{{"version", 2}, {"id", Value.id_}, {"left", Pane(Value.left_)}, {"right", Pane(Value.right_)},
        {"activePane", Value.activePane_}, {"geometry", QString::fromLatin1(Value.geometry_.toBase64())},
        {"layout", QString::fromLatin1(Value.layout_.toBase64())}, {"defaults", Value.defaults_}, {"commandHistory", QJsonArray::fromStringList(Value.commandHistory_)}}).toJson(QJsonDocument::Compact);
}
Loaded Decode(const QByteArray& Bytes) {
    Loaded Result; Result.original_ = Bytes; Result.exists_ = true;
    const auto Fail = [&](const QString& Code) { Result.writable_ = false; Result.error_ = Code; return Result; };
    if (Bytes.size() > MaxBytes) return Fail("workspace-size-limit");
    QJsonParseError Error; const auto Document = QJsonDocument::fromJson(Bytes, &Error);
    if (Error.error != QJsonParseError::NoError || !Document.isObject()) return Fail("workspace-invalid-json");
    const auto Root = Document.object(); const auto Version = Root.value("version");
    if (!Version.isDouble() || (Version.toDouble() != 1 && Version.toDouble() != 2)) return Fail("workspace-unsupported-version");
    const bool Legacy = Version.toInt() == 1;
    if (!Text(Root.value("id"), 128) || Root.value("id").toString().isEmpty()) return Fail("workspace-invalid-id");
    Result.state_.id_ = Root.value("id").toString();
    if (!Root.value("activePane").isDouble() || (Root.value("activePane").toDouble() != 0 && Root.value("activePane").toDouble() != 1)) return Fail("workspace-invalid-pane");
    Result.state_.activePane_ = Root.value("activePane").toInt();
    QSet<QString> Ids;
    for (const auto& Name : {QString("left"), QString("right")}) {
        if (!Root.value(Name).isObject()) return Fail("workspace-invalid-pane");
        const auto Object = Root.value(Name).toObject();
        auto& Pane = Name == "left" ? Result.state_.left_ : Result.state_.right_;
        if (!Object.value("tabs").isArray()) return Fail("workspace-invalid-tabs");
        const auto Tabs = Object.value("tabs").toArray();
        if (Tabs.isEmpty() || Tabs.size() > MaxTabs || !Object.value("active").isDouble()) return Fail("workspace-invalid-tabs");
        Pane.active_ = Object.value("active").toInt(-1);
        if (Pane.active_ < 0 || Pane.active_ >= Tabs.size() || Object.value("active").toDouble() != Pane.active_) return Fail("workspace-invalid-active-tab");
        for (const auto& Item : Tabs) {
            if (!Item.isObject()) return Fail("workspace-invalid-tab");
            auto TabObject = Item.toObject(); TabState Tab;
            if (Legacy) {
                if (!Text(TabObject.value("path"))) return Fail("workspace-invalid-location");
                const auto Path = TabObject.value("path").toString();
                Tab.location_ = {Path.startsWith("\\\\") ? storage::LocatorKind::Smb : storage::LocatorKind::Local, Path.toStdString()};
                Tab.id_ = NewId();
            } else {
                if (!TabObject.value("location").isObject()) return Fail("workspace-invalid-location");
                const auto Location = DecodeLocation(TabObject.value("location").toObject());
                if (!Location || !Text(TabObject.value("id"), 128)) return Fail("workspace-invalid-location");
                Tab.location_ = *Location; Tab.id_ = TabObject.value("id").toString();
            }
            if (Tab.id_.isEmpty() || Ids.contains(Tab.id_)) return Fail("workspace-duplicate-tab");
            Ids.insert(Tab.id_);
            if (TabObject.contains("provider") && !Text(TabObject.value("provider"), 2048)) return Fail("workspace-invalid-provider");
            Tab.provider_ = MigrateProvider(TabObject.value("provider").toString());
            if (TabObject.contains("nativeRoot") && !Text(TabObject.value("nativeRoot"))) return Fail("workspace-invalid-provider-root");
            Tab.nativeRoot_ = TabObject.value("nativeRoot").toString();
            if (TabObject.contains("selection")) {
                if (!TabObject.value("selection").isArray() || TabObject.value("selection").toArray().size() > MaxSelections) return Fail("workspace-selection-limit");
                for (const auto& Selection : TabObject.value("selection").toArray()) {
                    if (!Text(Selection, 131072)) return Fail("workspace-invalid-selection");
                    Tab.selection_.append(Selection.toString());
                }
            }
            if (TabObject.contains("focus") && !Text(TabObject.value("focus"), 131072)) return Fail("workspace-invalid-focus");
            Tab.focus_ = TabObject.value("focus").toString();
            if (TabObject.contains("view") && !TabObject.value("view").isObject()) return Fail("workspace-invalid-view");
            Tab.view_ = TabObject.value("view").toObject();
            if (TabObject.contains("historyConnections")) {
                if (!TabObject.value("historyConnections").isObject()) return Fail("workspace-invalid-history-connections");
                Tab.historyConnections_ = TabObject.value("historyConnections").toObject();
                if (Tab.historyConnections_.size() > MaxHistory) return Fail("workspace-history-limit");
                for (auto It = Tab.historyConnections_.begin(); It != Tab.historyConnections_.end(); ++It) {
                    if (It.key().size() > 131072 || !It.value().isObject() || !Text(It.value().toObject().value("provider"), 2048)
                        || !Text(It.value().toObject().value("nativeRoot"))) return Fail("workspace-invalid-history-connection");
                    auto Connection = It.value().toObject();
                    Connection["provider"] = MigrateProvider(Connection["provider"].toString()); It.value() = Connection;
                }
            }
            if (TabObject.contains("history")) {
                if (!TabObject.value("history").isArray() || TabObject.value("history").toArray().size() > MaxHistory) return Fail("workspace-history-limit");
                for (const auto& Entry : TabObject.value("history").toArray()) {
                    if (!Entry.isObject()) return Fail("workspace-invalid-history");
                    const auto Location = DecodeLocation(Entry.toObject()); if (!Location) return Fail("workspace-invalid-history");
                    Tab.history_.append(*Location);
                }
            }
            Tab.historyIndex_ = int(Tab.history_.size()) - 1;
            if (TabObject.contains("historyIndex")) {
                const auto Cursor = TabObject.value("historyIndex");
                if (!Cursor.isDouble() || Cursor.toDouble() != Cursor.toInt(-2) || Cursor.toInt() < -1 || Cursor.toInt() >= Tab.history_.size()) return Fail("workspace-invalid-history-index");
                Tab.historyIndex_ = Cursor.toInt();
            }
            Pane.tabs_.append(std::move(Tab));
        }
    }
    for (const auto& Key : {QString("geometry"), QString("layout")}) {
        if (!Root.contains(Key)) continue;
        if (!Text(Root.value(Key), 262144)) return Fail("workspace-invalid-layout");
        const auto Decoded = QByteArray::fromBase64Encoding(Root.value(Key).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!Decoded) return Fail("workspace-invalid-layout");
        (Key == "geometry" ? Result.state_.geometry_ : Result.state_.layout_) = Decoded.decoded;
    }
    if (Root.contains("defaults") && !Root.value("defaults").isObject()) return Fail("workspace-invalid-defaults");
    if (Root.contains("commandHistory")) {
        if (!Root.value("commandHistory").isArray() || Root.value("commandHistory").toArray().size() > 100) return Fail("workspace-invalid-command-history");
        for (const auto& Command : Root.value("commandHistory").toArray()) {
            if (!Text(Command, 32768)) return Fail("workspace-invalid-command-history");
            Result.state_.commandHistory_.append(Command.toString());
        }
    }
    Result.state_.defaults_ = Root.value("defaults").toObject();
    return Result;
}
Loaded Load(hc::settings::JsonStore& Settings) {
    if (!Settings.Writable()) { Loaded Result; Result.writable_ = false; Result.error_ = Settings.Error(); return Result; }
    if (!Settings.Object().contains("workspace")) return {};
    return Decode(hc::settings::JsonBytes(Settings.Object()["workspace"]));
}
bool Save(hc::settings::JsonStore& Settings, Loaded& Previous, const State& Value) {
    if (!Previous.writable_) return false;
    const auto Bytes = Encode(Value);
    if (!Decode(Bytes).writable_) { Previous.error_ = "workspace-invalid-state"; return false; }
    const auto Current = hc::settings::JsonBytes(Settings.Object()["workspace"]);
    if (Current != Previous.original_ && (Previous.pending_.isEmpty() || Current != Previous.pending_)) {
        Previous.error_ = "workspace-changed-in-another-window"; return false;
    }
    auto Next = Settings.Object(); Next["workspace"] = hc::settings::JsonObject(Bytes);
    if (!Settings.Replace(Next)) { Previous.pending_ = Bytes; Previous.error_ = Settings.Error(); return false; }
    Previous = Decode(Bytes); return true;
}
}
