#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QList>
#include <QString>

namespace hc::document::epub {
// UTF-16 source coordinates, matching QTextCursor and DOM Range offsets.
class SourceMap final {
public:
    bool Load(const QString& Source);
    QJsonObject Anchor(int Position) const;
    int Position(const QJsonObject& Anchor) const;
private:
    struct Span {QJsonArray path_;int child_=-1,begin_=0,end_=0;QList<int> offsets_;};
    QList<Span> spans_;
};
}
