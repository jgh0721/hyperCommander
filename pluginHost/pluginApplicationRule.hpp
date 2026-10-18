#pragma once
#include "pluginHost/wlxDetection.hpp"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <optional>

namespace hc::plugins {
// This is an additional host-side selection filter. It never replaces SDK
// detection, grants permissions, or turns an excluded plugin into a fallback.
class PluginApplicationRule final {
public:
    static storage::Result<PluginApplicationRule> Parse(const QJsonValue& Value) {
        PluginApplicationRule Rule;
        if (Value.isUndefined() || Value.isNull()) return Rule;
        if (!Value.isObject()) return invalid("plugin-rule-object");
        const auto Object = Value.toObject();
        if (Object.isEmpty()) return Rule;
        if (Object.value("version") != QJsonValue(1)) return invalid("plugin-rule-version");
        if (Object.contains("enabled") && !Object.value("enabled").isBool()) return invalid("plugin-rule-enabled");
        if (Object.contains("mode") && !Object.value("mode").isString()) return invalid("plugin-rule-mode");
        Rule.enabled_ = Object.value("enabled").toBool(true);
        Rule.mode_ = Object.value("mode").toString("all");
        if (Rule.mode_ == "all") return Rule;
        if (Rule.mode_ == "extensions") {
            if (!Object.value("extensions").isArray() || Object.value("extensions").toArray().size() > 256) return invalid("plugin-rule-extensions");
            for (const auto& Item : Object.value("extensions").toArray()) {
                if (!Item.isString()) return invalid("plugin-rule-extensions");
                auto Ext = Item.toString().trimmed().toCaseFolded();
                if (Ext.startsWith("*.")) Ext.remove(0, 2); else if (Ext.startsWith('.')) Ext.remove(0, 1);
                static const QRegularExpression Valid(QStringLiteral("^[\\p{L}\\p{N}_+.-]{1,64}$"));
                if (!Valid.match(Ext).hasMatch() || Ext.startsWith('.') || Ext.endsWith('.') || Ext.contains("..")) return invalid("plugin-rule-extension");
                if (!Rule.extensions_.contains(Ext)) Rule.extensions_.append(Ext);
            }
            return Rule;
        }
        const auto Expression = Object.value("expression");
        if (!Expression.isString() || Expression.toString().trimmed().isEmpty()) return invalid("plugin-rule-expression");
        Rule.expression_ = Expression.toString();
        if (Rule.mode_ == "tc") {
            const auto Encoded = Rule.expression_.toLocal8Bit();
            if (QString::fromLocal8Bit(Encoded) != Rule.expression_) return invalid("plugin-rule-encoding");
            auto Parsed = WlxDetectionExpression::Parse(Encoded); if (!Parsed.HasValue()) return Parsed.Failure();
            Rule.tc_ = std::move(Parsed.Value());
            // SIZE inside a string may conservatively require metadata too.
            Rule.needsSize_ = QRegularExpression(QStringLiteral("\\bSIZE\\b"), QRegularExpression::CaseInsensitiveOption).match(Rule.expression_).hasMatch();
            return Rule;
        }
        if (Rule.mode_ != "pattern") return invalid("plugin-rule-mode");
        if (Rule.expression_.size() > 512 || Rule.expression_.contains(QChar::Null)) return invalid("plugin-rule-pattern-length");
        // A documented filename-pattern subset: *, ?, (a|b), top-level | and
        // leading ~. No user-supplied regex, repetition syntax or arbitrary code.
        QString Regex; int Depth = 0; bool HasOperand = false;
        auto Pattern = Rule.expression_;
        if (Pattern.startsWith('~')) { Rule.negate_ = true; Pattern.remove(0, 1); }
        for (const auto Char : Pattern) {
            if (Char == '*') { Regex += ".*"; HasOperand = true; }
            else if (Char == '?') { Regex += '.'; HasOperand = true; }
            else if (Char == '(') { if (++Depth > 16) return invalid("plugin-rule-pattern-depth"); Regex += "(?:"; HasOperand = false; }
            else if (Char == ')') { if (!Depth-- || !HasOperand) return invalid("plugin-rule-pattern-syntax"); Regex += ')'; HasOperand = true; }
            else if (Char == '|') { if (!HasOperand) return invalid("plugin-rule-pattern-syntax"); Regex += '|'; HasOperand = false; }
            else { if (Char == '/' || Char == '\\' || Char == '[' || Char == ']') return invalid("plugin-rule-pattern-character"); Regex += QRegularExpression::escape(QString(Char)); HasOperand = true; }
        }
        if (Depth || !HasOperand) return invalid("plugin-rule-pattern-syntax");
        Rule.pattern_ = QRegularExpression("\\A(?:" + Regex + ")\\z", QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
        if (!Rule.pattern_.isValid()) return invalid("plugin-rule-pattern-syntax");
        return Rule;
    }
    bool Enabled() const { return enabled_; }
    bool NeedsPrefix() const { return tc_ && tc_->NeedsPrefix(); }
    bool NeedsSize() const { return needsSize_; }
    bool NameOnly() const { return !NeedsPrefix() && !NeedsSize(); }
    storage::Result<bool> Matches(const QString& Name, std::optional<quint64> Size = {}, std::optional<QByteArray> Prefix = {}) const {
        if (!enabled_) return false;
        if (mode_ == "all") return true;
        if (Name.size() > 4096 || Name.contains(QChar::Null)) return invalid("plugin-rule-name");
        const auto FileName = QFileInfo(Name).fileName();
        if (mode_ == "extensions") {
            for (const auto& Ext : extensions_) if (FileName.endsWith('.' + Ext, Qt::CaseInsensitive)) return true;
            return false;
        }
        if (mode_ == "pattern") return pattern_.match(FileName).hasMatch() != negate_;
        if ((NeedsPrefix() && !Prefix) || (needsSize_ && !Size)) return invalid("plugin-rule-metadata-required");
        WlxDetectionInput Input; Input.extension_ = QFileInfo(FileName).suffix().toUpper().toLocal8Bit();
        Input.size_ = Size.value_or(0); Input.prefix_ = Prefix.value_or(QByteArray{});
        auto Result = tc_->Evaluate(Input); if (!Result.HasValue()) return Result.Failure();
        return Result.Value().matches_.value_or(false);
    }
    static QString Describe(const QJsonValue& Value) {
        auto Parsed = Parse(Value); if (!Parsed.HasValue()) return QStringLiteral("조건 확인 필요");
        const auto& Rule = Parsed.Value();
        if (!Rule.enabled_) return QStringLiteral("사용 안 함");
        if (Rule.mode_ == "all") return QStringLiteral("기본 감지 범위");
        if (Rule.mode_ == "extensions") return Rule.extensions_.isEmpty() ? QStringLiteral("적용 형식 없음") : Rule.extensions_.join(" · ");
        return Rule.expression_;
    }
    // Worker-only evaluation against the original provider revision. Never use
    // a materialized filename or open the path again through a native QFile.
    storage::Result<bool> MatchesSource(storage::IContentSource& Source, storage::OperationContext Context) const {
        if (!enabled_) return false;
        if (mode_ == "all") return true;
        const auto Entry = Source.Describe(); std::optional<QByteArray> Prefix;
        if (NeedsPrefix()) {
            const auto Required = std::min<std::uint64_t>(Entry.size_, 8192);
            if (Required > Context.maxReadBytes_) return storage::Error{storage::ErrorCategory::BudgetExceeded, "plugin-rule-read-budget"};
            Prefix = QByteArray{};
            if (Required) {
                auto Stream = Source.OpenRead(Context).get(); if (!Stream.HasValue()) return Stream.Failure();
                if (!Stream.Value()) return invalid("plugin-rule-stream");
                while (std::uint64_t(Prefix->size()) < Required) {
                    if (const auto Stop = Context.Check(Entry.id_.providerId_)) return *Stop;
                    const auto Remaining = Required - std::uint64_t(Prefix->size());
                    auto Read = Stream.Value()->Read(std::size_t(Remaining), Context).get(); if (!Read.HasValue()) return Read.Failure();
                    if (Read.Value().bytes_.size() > Remaining || (Read.Value().bytes_.empty() && !Read.Value().eof_)) return invalid("plugin-rule-stream-progress");
                    Prefix->append(reinterpret_cast<const char*>(Read.Value().bytes_.data()), qsizetype(Read.Value().bytes_.size()));
                    if (Read.Value().eof_ && std::uint64_t(Prefix->size()) < Required) return storage::Error{storage::ErrorCategory::StaleRevision, "plugin-rule-premature-eof"};
                }
            }
        }
        auto Verified = Source.Verify(Context).get(); if (!Verified.HasValue()) return Verified.Failure();
        return Matches(QString::fromStdString(Entry.name_), Entry.size_, Prefix);
    }
private:
    static storage::Error invalid(const char* Code) { return {storage::ErrorCategory::InvalidRequest, Code, {"plugin-rule"}}; }
    bool enabled_ = true, negate_ = false, needsSize_ = false;
    QString mode_ = "all", expression_;
    QStringList extensions_;
    std::optional<WlxDetectionExpression> tc_;
    QRegularExpression pattern_;
};
}
