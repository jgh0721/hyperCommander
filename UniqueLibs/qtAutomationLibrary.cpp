#include "qtAutomation.hpp"
#include <QJsonArray>
#include "commonLibs/scripting/luaRuntime.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLockFile>
#include <QSettings>
#include <QSet>

namespace hc::automation {
namespace {
constexpr auto Key = "Automation/library";
Error LibraryError(const char* Code) { return {ErrorCategory::InvalidRequest, Code}; }
bool ValidText(const QJsonValue& Value, int Limit) { return Value.isString() && Value.toString().size() <= Limit && !Value.toString().contains(QChar::Null); }
}
Result<QList<SavedScript>> DecodeLibrary(const QByteArray& Bytes)
{
    if (Bytes.size() > 8 * 1024 * 1024) return LibraryError("automation-library-size");
    QJsonParseError Error; const auto Doc = QJsonDocument::fromJson(Bytes, &Error);
    if (Error.error != QJsonParseError::NoError || !Doc.isObject()) return LibraryError("automation-library-json");
    const auto Object = Doc.object(); const auto Version = Object.value("version").toInt(-1);
    if (!Object.value("version").isDouble() || Object.value("version").toDouble() != Version || Version < 0 || Version > 2) return LibraryError("automation-library-version");
    if (!Object.value("scripts").isArray() || Object.value("scripts").toArray().size() > 64) return LibraryError("automation-library-count");
    QList<SavedScript> Scripts; QSet<QString> Ids, Shortcuts;
    for (const auto& Value : Object.value("scripts").toArray()) {
        if (!Value.isObject()) return LibraryError("automation-library-record"); const auto Record = Value.toObject();
        if (!ValidText(Record["id"],64) || Record["id"].toString().isEmpty() || !ValidText(Record["title"],80) || Record["title"].toString().trimmed().isEmpty()
            || !ValidText(Record["source"],65536)) return LibraryError("automation-library-record");
        if (Version >= 1 && (!ValidText(Record["shortcut"],128) || !Record["toolbar"].isBool())) return LibraryError("automation-library-record");
        SavedScript Script{Record["id"].toString(),Record["title"].toString(),Record["source"].toString(),Record["shortcut"].toString(),Record["toolbar"].toBool()};
        if (Version >= 2) Script.language_ = Record["language"].toString();
        if (Script.language_ != "dsl" && Script.language_ != "lua") return LibraryError("automation-library-language");
        const bool Valid = Script.language_ == "lua" ? script::CheckLuaSyntax(Script.source_.toUtf8().toStdString()).HasValue() : Compile(Script.source_).HasValue();
        if (Ids.contains(Script.id_) || !Valid) return LibraryError("automation-library-id-or-source");
        Ids.insert(Script.id_);
        if (!Script.shortcut_.isEmpty()) {
            const QKeySequence Sequence(Script.shortcut_, QKeySequence::PortableText);
            const auto Required = Qt::ControlModifier | Qt::AltModifier;
            if (Sequence.count() != 1 || Sequence[0].key() == Qt::Key_unknown || (Sequence[0].keyboardModifiers() & Required) != Required)
                return LibraryError("automation-shortcut-requires-ctrl-alt");
            Script.shortcut_ = Sequence.toString(QKeySequence::PortableText);
            if (Shortcuts.contains(Script.shortcut_)) return LibraryError("automation-shortcut-duplicate");
            Shortcuts.insert(Script.shortcut_);
        }
        Scripts.append(std::move(Script));
    }
    return Scripts;
}
Result<QByteArray> EncodeLibrary(const QList<SavedScript>& Scripts)
{
    QJsonArray Records;
    for (const auto& Script : Scripts) Records.append(QJsonObject{{"id",Script.id_},{"title",Script.title_},{"source",Script.source_},{"shortcut",Script.shortcut_},{"toolbar",Script.toolbar_},{"language",Script.language_}});
    auto Bytes = QJsonDocument(QJsonObject{{"version",2},{"scripts",Records}}).toJson(QJsonDocument::Compact);
    auto Valid = DecodeLibrary(Bytes); if (!Valid.HasValue()) return Valid.Failure(); return Bytes;
}
Result<QList<SavedScript>> LibraryStore::Load()
{
    loaded_ = false; settings_.sync(); if (settings_.status() != QSettings::NoError) return LibraryError("automation-library-read");
    exists_ = settings_.contains(Key); original_ = settings_.value(Key).toByteArray();
    auto Result = exists_ ? DecodeLibrary(original_) : storage::Result<QList<SavedScript>>(QList<SavedScript>{});
    loaded_ = Result.HasValue(); return Result;
}
Result<Unit> LibraryStore::Save(const QList<SavedScript>& Scripts)
{
    if (!loaded_) return LibraryError("automation-library-not-loaded");
    auto Encoded = EncodeLibrary(Scripts); if (!Encoded.HasValue()) return Encoded.Failure();
    QLockFile Lock(settings_.fileName() + ".automation-lock"); if (!Lock.tryLock(0)) return LibraryError("automation-library-busy");
    settings_.sync();
    if (settings_.status() != QSettings::NoError || settings_.contains(Key) != exists_ || settings_.value(Key).toByteArray() != original_)
        return LibraryError("automation-library-changed");
    settings_.setValue(Key,Encoded.Value()); settings_.sync();
    if (settings_.status() != QSettings::NoError) return LibraryError("automation-library-write");
    original_ = Encoded.Value(); exists_ = true; return Unit{};
}
}
