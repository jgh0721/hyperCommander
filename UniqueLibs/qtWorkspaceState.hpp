#pragma once
#include <QJsonObject>
#include <QMap>
#include "settings/appSettings.hpp"
#include <QStringList>
#include <QVector>
#include "commonLibs/storage/storage.hpp"

namespace hc::workspace {
struct TabState {
    QString id_;
    storage::Locator location_;
    QString provider_;
    QString nativeRoot_;
    QStringList selection_;
    QString focus_;
    QJsonObject view_;
    QVector<storage::Locator> history_;
    QJsonObject historyConnections_;
    int historyIndex_ = -1;
};
struct PaneState {
    QVector<TabState> tabs_;
    int active_ = 0;
};
struct State {
    QString id_ = "default";
    PaneState left_, right_;
    int activePane_ = 0;
    QByteArray geometry_, layout_;
    QJsonObject defaults_;
    QStringList commandHistory_;
};
struct Loaded {
    State state_;
    QByteArray original_, pending_;
    bool exists_ = false;
    bool writable_ = true;
    QString error_;
};
void RecordNavigation(TabState& Tab, const storage::Locator& Location, int RequestedIndex = -1);
QJsonObject EncodeLocation(const storage::Locator& Location);
std::optional<storage::Locator> DecodeLocation(const QJsonObject& Object);
QString LocationKey(const storage::Locator& Location);
QString PluginPath(const storage::Locator& Location);
// Persistent identity survives renames; session identity is never persisted.
QString SelectionKey(const storage::EntrySnapshot& Entry);
QJsonObject MergeView(const QJsonObject& Parent, const QJsonObject& Override);
Loaded Decode(const QByteArray& Bytes);
QByteArray Encode(const State& Value);
Loaded Load(hc::settings::JsonStore& Settings);
bool Save(hc::settings::JsonStore& Settings, Loaded& Previous, const State& Value);

// Metadata belongs to a published snapshot. A missing revision is never reused
// across enumerations; unchanged strong revisions can retain computed values.
class DirectoryIndex {
public:
    void Publish(const std::vector<storage::EntrySnapshot>& Entries);
    void Append(const std::vector<storage::EntrySnapshot>& Entries);
    bool Put(const storage::EntrySnapshot& Entry, const QMap<QString, QString>& Values, quint64 Generation);
    QMap<QString, QString> Metadata(const storage::EntrySnapshot& Entry) const;
    QMap<QString, QMap<QString, QString>> SearchMetadata() const {
        QMap<QString, QMap<QString, QString>> Result;
        for (const auto& Record : records_)
            if (!Record.values_.isEmpty()) Result.insert(QString::fromStdString(storage::Serialize(Record.entry_.id_)), Record.values_);
        return Result;
    }
    bool Matches(const storage::EntrySnapshot& Entry, const QString& Query) const;
    quint64 Generation() const { return generation_; }
private:
    struct Record { storage::EntrySnapshot entry_; QMap<QString, QString> values_; };
    QMap<QString, Record> records_;
    quint64 generation_ = 0;
};
}
