#pragma once
#include "commonLibs/storage/search.hpp"
#include <QMap>
#include <QString>
class QWidget;
namespace hc::search {
using MetadataSnapshot = QMap<QString, QMap<QString, QString>>;
storage::Result<storage::SearchMatcher> Compile(const QString& Query, MetadataSnapshot Metadata = {});
std::shared_ptr<storage::ISearchProvider> MakeEverythingSearch(const QString& Executable);
void Show(QWidget* Parent, std::shared_ptr<storage::IStorageProvider> Provider, storage::Locator Root,
    MetadataSnapshot Metadata, std::function<void(const storage::EntrySnapshot&)> Activate);
}
