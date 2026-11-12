#pragma once

#include <QDir>
#include <QFile>
#include <QTemporaryFile>
#include <memory>

namespace hc::rst {

/// Each queued/running build keeps its own immutable, fully written shadow file.
class PreviewShadowSnapshot final
{
public:
    static std::shared_ptr<const PreviewShadowSnapshot> create(
        const QString& directory, const QByteArray& utf8)
    {
        if (directory.isEmpty() || !QDir().mkpath(directory))
            return {};
        QTemporaryFile file(QDir(directory).filePath(QStringLiteral("preview-XXXXXX.shadow")));
        if (!file.open() || file.write(utf8) != utf8.size() || !file.flush())
            return {};
        const QString path = file.fileName();
        file.close();
        auto snapshot = std::shared_ptr<const PreviewShadowSnapshot>(new PreviewShadowSnapshot(path));
        file.setAutoRemove(false);
        return snapshot;
    }
    ~PreviewShadowSnapshot() { QFile::remove(path_); }
    QString filePath() const { return path_; }
private:
    explicit PreviewShadowSnapshot(const QString& path) : path_(path) {}
    QString path_;
};

} // namespace hc::rst
