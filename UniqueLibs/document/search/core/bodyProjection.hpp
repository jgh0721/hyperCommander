#pragma once
#include "textSearchMatcher.hpp"
namespace hc::document::search {
// A sparse, reversible projection. Markup is never reserialized.
class BodyProjection {
public:
    explicit BodyProjection(QString Source);
    QString Error() const { return error_; }
    const QString& Text() const { return text_; }
    const QString& Source() const { return source_; }
    std::optional<QVector<Range>> Map(Range BodyRange) const;
    static QString Escape(QString Text);
private:
    struct Segment { Range body_, source_; bool atomic_ = false; };
    void boundary();
    void text(qint64 Begin, qint64 End, bool Cdata);
    QString source_, text_, error_;
    QVector<Segment> segments_;
};
}
