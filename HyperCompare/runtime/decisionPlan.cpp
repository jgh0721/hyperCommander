#include "stdafx.h"
#include "decisionPlan.hpp"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSet>

namespace hc::compare
{
    namespace
    {
        constexpr qsizetype replacementLimit = 1024 * 1024, resultLimit = 32 * 1024 * 1024;
        QString digest(QByteArrayView Bytes)
        { return QString::fromLatin1(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex()); }
        QByteArray seed(const Result& Session)
        { return Session.plan_->hasBase_ ? QByteArray::fromStdString(Session.merged_.text_) : Session.inputs_[0].utf8_; }
        // Qt validates JSON syntax. This bounded lexical pass additionally rejects
        // duplicate decoded keys and malformed Unicode before object construction.
        class JsonGuard
        {
        public:
            JsonGuard(const QByteArray& Bytes, const Budget& Limits) : bytes_(Bytes), limits_(Limits) {}
            void Check() { value(0); whitespace(); if (at_ != bytes_.size()) throw Failure("InvalidDecisionJson"); }
        private:
            char peek() const { return at_ < bytes_.size() ? bytes_[at_] : '\0'; }
            void whitespace()
            {
                while (at_ < bytes_.size() && QByteArrayView(" \t\r\n").contains(bytes_[at_]))
                { if ((at_ & 4095) == 0) limits_.Check(); ++at_; }
            }
            void expect(char Char) { whitespace(); if (peek() != Char) throw Failure("InvalidDecisionJson"); ++at_; }
            QString string(bool Key)
            {
                expect('"'); const auto begin = at_ - 1;
                while (at_ < bytes_.size())
                {
                    if ((at_ & 4095) == 0) limits_.Check();
                    const auto ch = bytes_[at_++];
                    if (static_cast<unsigned char>(ch) < 0x20) throw Failure("InvalidDecisionJson");
                    if (at_ - begin > (Key ? 1024 : replacementLimit * 6 + 2)) throw Failure("DecisionStringLimit");
                    if (ch == '\\') { if (at_ == bytes_.size()) throw Failure("InvalidDecisionJson"); ++at_; continue; }
                    if (ch != '"') continue;
                    QJsonParseError error;
                    const auto doc = QJsonDocument::fromJson('[' + bytes_.mid(begin, at_ - begin) + ']', &error);
                    if (error.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() != 1 || !doc.array()[0].isString())
                        throw Failure("InvalidDecisionJson");
                    const auto text = doc.array()[0].toString();
                    for (qsizetype i = 0; i < text.size(); ++i)
                    {
                        const auto code = text[i];
                        if (code.unicode() == 0 || code.isLowSurrogate()) throw Failure("InvalidDecisionUnicode");
                        if (code.isHighSurrogate() && (++i == text.size() || !text[i].isLowSurrogate())) throw Failure("InvalidDecisionUnicode");
                    }
                    if ((Key && text.size() > 128) || text.toUtf8().size() > replacementLimit) throw Failure("DecisionStringLimit");
                    return text;
                }
                throw Failure("InvalidDecisionJson");
            }
            void value(int Depth)
            {
                limits_.Check(); if (Depth > 8 || ++nodes_ > 1500000) throw Failure("DecisionStructureLimit");
                whitespace(); const auto ch = peek();
                if (ch == '"') { string(false); return; }
                if (ch == '{' || ch == '[')
                {
                    const bool object = ch == '{'; const char end = object ? '}' : ']'; ++at_; whitespace();
                    if (peek() == end) { ++at_; return; }
                    QSet<QString> keys;
                    for (;;)
                    {
                        if (object)
                        {
                            const auto key = string(true);
                            if (keys.contains(key)) throw Failure("DuplicateDecisionKey");
                            keys.insert(key); expect(':');
                        }
                        value(Depth + 1); whitespace();
                        if (peek() == end) { ++at_; return; }
                        expect(',');
                    }
                }
                const auto begin = at_;
                while (at_ < bytes_.size() && !QByteArrayView(" \t\r\n,]}").contains(peek()))
                { if (at_ - begin > 128) throw Failure("InvalidDecisionJson"); ++at_; }
                if (begin == at_) throw Failure("InvalidDecisionJson");
            }
            const QByteArray& bytes_;
            const Budget& limits_;
            qsizetype at_ = 0;
            std::size_t nodes_ = 0;
        };
    }
    QJsonObject ReadCompareJson(const QByteArray& Json, const Budget& Limits)
    {
        if (Json.size() > decisionPlanBytes) throw Failure("DecisionPlanSizeLimit");
        const auto bytes = Json.startsWith("\xef\xbb\xbf") ? Json.mid(3) : Json;
        JsonGuard(bytes, Limits).Check(); QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) throw Failure("InvalidDecisionJson");
        return doc.object();
    }
    QJsonObject DecisionIdentity(const Request& Input, const Result& Session, const Budget& Limits)
    {
        if (!Session.plan_ || Session.inputs_.size() < 2) throw Failure("InvalidDecisionSession");
        if (Session.plan_->blocks_.size() > decisionBlockLimit) throw Failure("DecisionBlockLimit");
        QJsonArray inputs;
        for (qsizetype i = 0; i < Session.inputs_.size(); ++i)
        {
            Limits.Check(); const auto& input = Session.inputs_[i];
            inputs.append(QJsonObject{{"role", i == 0 ? "left" : i == 1 ? "right" : "base"},
                {"path", input.path_}, {"sha256", digest(input.raw_)}, {"encoding", input.encoding_}, {"bom", input.bom_}});
        }
        const QJsonObject policy{{"encoding", Input.encoding_}, {"comparison", "exact-lines-v1"},
            {"seed", Session.plan_->hasBase_ ? "base-with-independent-changes" : "left"}, {"outputEncoding", "left"}};
        QJsonObject result{{"schemaVersion", 1}, {"kind", "text-merge-decisions"}, {"engineVersion", "myers-bounded-1"},
            {"policyDigest", digest(QJsonDocument(policy).toJson(QJsonDocument::Compact))}, {"inputs", inputs}};
        const auto generation = digest(QJsonDocument(result).toJson(QJsonDocument::Compact)); result["generation"] = generation;
        return result;
    }
    QJsonObject MakeDecisionPlan(const Request& Input, const Result& Session, const Budget& Limits)
    {
        if (!Input.takeAll_.isEmpty()) throw Failure("InvalidDecisionSession");
        auto result = DecisionIdentity(Input, Session, Limits); const auto generation = result["generation"].toString();
        const auto initial = seed(Session); QJsonArray blocks;
        for (std::size_t i = 0; i < Session.plan_->blocks_.size(); ++i)
        {
            Limits.Check(); const auto& block = Session.plan_->blocks_[i]; const auto& range = Session.plan_->initial_[i];
            if (range.begin_ > range.end_ || range.end_ > std::size_t(initial.size())) throw Failure("InvalidMergeLocation");
            const auto spanDigest = digest(QByteArrayView(initial.constData() + range.begin_, qsizetype(range.end_ - range.begin_)));
            blocks.append(QJsonObject{{"changeId", digest(generation.toUtf8() + ':' + QByteArray::number(qulonglong(i)) + ':' + spanDigest.toUtf8())},
                {"expectedSpanDigest", spanDigest}, {"leftLine", qint64(block.leftLine_ + 1)}, {"rightLine", qint64(block.rightLine_ + 1)},
                {"baseLine", Session.plan_->hasBase_ ? qint64(block.baseLine_ + 1) : 0}, {"choice", "unresolved"}, {"replacement", ""}});
        }
        result["blocks"] = blocks; return result;
    }
    void ApplyDecisionPlan(const Request& Input, Result& Session, const QByteArray& Json, const Budget& Limits)
    {
        auto document = ReadCompareJson(Json, Limits); auto expected = MakeDecisionPlan(Input, Session, Limits);
        if (document.keys() != expected.keys()) throw Failure("InvalidDecisionPlan");
        if (document["schemaVersion"] != QJsonValue(1)) throw Failure("UnsupportedDecisionSchema");
        if (!document["blocks"].isArray()) throw Failure("InvalidDecisionPlan");
        const auto choices = document.take("blocks").toArray(), blocks = expected.take("blocks").toArray();
        if (document != expected) throw Failure("StalePlan");
        if (choices.size() != blocks.size()) throw Failure("IncompleteDecisionPlan");
        const auto initial = seed(Session); QByteArray output; output.reserve(initial.size());
        MergeModel model(Session.plan_); auto plan = std::make_shared<MergePlan>(*Session.plan_);
        const QStringList names{"unresolved", "left", "right", "base", "leftThenRight", "rightThenLeft", "manual"};
        std::size_t cursor = 0, unresolved = 0;
        const auto append = [&](QByteArrayView Bytes) {
            if (Bytes.size() > resultLimit - output.size()) throw Failure("ResultSizeLimit");
            output.append(Bytes.data(), Bytes.size());
        };
        for (qsizetype i = 0; i < choices.size(); ++i)
        {
            Limits.Check(); if (!choices[i].isObject()) throw Failure("InvalidDecisionPlan");
            auto choice = choices[i].toObject(), reference = blocks[i].toObject();
            if (choice.keys() != reference.keys() || !choice["choice"].isString() || !choice["replacement"].isString()) throw Failure("InvalidDecisionPlan");
            const auto name = choice.take("choice").toString(); const auto manual = choice.take("replacement").toString().toUtf8();
            reference.remove("choice"); reference.remove("replacement"); if (choice != reference) throw Failure("StalePlan");
            const auto index = names.indexOf(name);
            if (index < 0 || (index == 3 && !plan->hasBase_) || (index != 6 && !manual.isEmpty())) throw Failure("InvalidDecisionChoice");
            const auto resolution = static_cast<Resolution>(index); const auto& range = model.State()[std::size_t(i)];
            if (range.begin_ < cursor || range.end_ > std::size_t(initial.size())) throw Failure("InvalidMergeLocation");
            append(QByteArrayView(initial.constData() + cursor, qsizetype(range.begin_ - cursor)));
            const auto begin = std::size_t(output.size());
            if (resolution == Resolution::Unresolved)
            { ++unresolved; append(QByteArrayView(initial.constData() + range.begin_, qsizetype(range.end_ - range.begin_))); }
            else if (resolution == Resolution::Manual) append(manual);
            else append(QByteArray::fromStdString(model.Prepare(std::uint64_t(i + 1), model.Revision(), resolution).replacement_));
            plan->initial_[std::size_t(i)] = {begin, std::size_t(output.size()), resolution}; cursor = range.end_;
        }
        append(QByteArrayView(initial.constData() + cursor, initial.size() - qsizetype(cursor))); Limits.Check();
        // Publish only after every decision is validated. Unresolved plans stay in
        // memory and never authorize writing a partial RESULT.
        Session.plan_ = std::move(plan); Session.output_ = std::move(output);
        Session.exitCode_ = unresolved ? 1 : 0; Session.error_ = unresolved ? "DecisionsRequired" : "";
        Session.outcome_ = unresolved ? "Conflicted" : "Planned";
    }
}
