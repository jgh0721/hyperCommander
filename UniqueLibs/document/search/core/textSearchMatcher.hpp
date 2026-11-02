#pragma once
#include <QRegularExpression>
#include <QString>
#include <QVector>
#include <optional>

namespace hc::document::search {
struct Range {
    qint64 start_ = -1, length_ = 0;
    bool Valid(qint64 Size) const { return start_ >= 0 && length_ >= 0 && start_ <= Size && length_ <= Size - start_; }
};
struct Match {
    Range range_;
    QVector<Range> captures_;
    qint64 line_ = 1, column_ = 1;
};
struct Options { bool regex_ = false, caseSensitive_ = false; };
// A sparse CRLF map keeps snapshot storage proportional to line endings, not characters.
class TextMap {
public:
    explicit TextMap(QString Text = {});
    const QString& Original() const { return original_; }
    const QString& Normalized() const { return normalized_; }
    Range Source(Range Value) const;
    qint64 Line(qint64 Position) const;
    qint64 Column(qint64 Position) const;
private:
    QString original_, normalized_;
    QVector<qint64> removed_, lines_{0};
};
class Matcher {
public:
    Matcher(QString Query, Options Settings);
    QString Error() const;
    qint64 ErrorOffset() const { return expression_.patternErrorOffset(); }
    QStringList Groups() const { return expression_.namedCaptureGroups(); }
    void Start(const TextMap& Text);
    std::optional<Match> Next();
private:
    QRegularExpression expression_;
    QRegularExpressionMatchIterator iterator_;
    const TextMap* text_ = nullptr;
    bool empty_ = false;
    QString runtimeError_;
};
class Replacement {
public:
    Replacement(QString Value, bool Regex, const QStringList& Groups);
    QString Error() const { return error_; }
    std::optional<QString> Expand(const QString& Original, const Match& Hit, const QString& Newline = QStringLiteral("\n")) const;
private:
    struct Part { QString text_; int group_ = -1; };
    QVector<Part> parts_;
    QString error_;
};
// A plan owns its immutable preimage. Preview and application use the same expansion.
// Persistence, editor undo and document revision checks belong to the coordinator.
class ReplacementPlan {
public:
    ReplacementPlan(QString Original, QVector<Match> Hits, const Replacement& Replace, const QString& Newline = QStringLiteral("\n"));
    QString Error() const { return error_; }
    const QString& Before() const { return before_; }
    const QString& After() const { return after_; }
    std::optional<QString> ApplyTo(const QString& Current) const;
private:
    QString before_, after_, error_;
};
}
