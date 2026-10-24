#include "stdafx.h"
#include "mergeWork.hpp"
#include "decisionPlan.hpp"
#include "UniqueLibs/document/documentTextCodec.hpp"
#include <QCryptographicHash>
#include <QJsonDocument>

namespace hc::compare
{
    namespace
    {
        constexpr qsizetype resultLimit = 32 * 1024 * 1024, chunkBytes = 512 * 1024;
        QString digest(QByteArrayView Bytes)
        { return QString::fromLatin1(QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex()); }
        qint64 integer(const QJsonValue& Value, qint64 Maximum)
        {
            const auto number = Value.toInteger(-1);
            if (!Value.isDouble() || number < 0 || number > Maximum || double(number) != Value.toDouble()) throw Failure("InvalidWorkNumber");
            return number;
        }
        void validate(const Result& Session, const MergeWork& Work, const Budget& Limits)
        {
            if (!Session.plan_ || Work.state_.size() != Session.plan_->blocks_.size()) throw Failure("InvalidWorkBlocks");
            if (Work.text_.size() > resultLimit || Work.state_.size() > decisionBlockLimit) throw Failure("WorkSizeLimit");
            if (Work.text_.contains('\0') || !document::DecodeDocumentText(Work.text_, "UTF-8").HasValue()) throw Failure("InvalidWorkText");
            const auto boundary = [&](std::size_t At) {
                return At <= std::size_t(Work.text_.size()) && (At == std::size_t(Work.text_.size()) ||
                    (static_cast<unsigned char>(Work.text_[qsizetype(At)]) & 0xc0) != 0x80);
            };
            std::size_t end = 0;
            for (std::size_t i = 0; i < Work.state_.size(); ++i)
            {
                Limits.Check(); const auto& state = Work.state_[i]; const auto& block = Session.plan_->blocks_[i];
                if (state.resolution_ < Resolution::Unresolved || state.resolution_ > Resolution::Omit ||
                    (state.resolution_ == Resolution::Base && !Session.plan_->hasBase_)) throw Failure("InvalidWorkResolution");
                if (!state.mapped_)
                {
                    if (state.resolution_ != Resolution::Unresolved && state.resolution_ != Resolution::Manual) throw Failure("InvalidWorkResolution");
                    continue;
                }
                if (state.begin_ < end || state.begin_ > state.end_ || !boundary(state.begin_) || !boundary(state.end_)) throw Failure("InvalidWorkRange");
                end = state.end_;
                std::string expected;
                switch (state.resolution_)
                {
                case Resolution::Left: expected = block.left_; break;
                case Resolution::Right: expected = block.right_; break;
                case Resolution::Base: expected = block.base_; break;
                case Resolution::LeftThenRight: expected = block.left_ + block.right_; break;
                case Resolution::RightThenLeft: expected = block.right_ + block.left_; break;
                case Resolution::Omit: break;
                default: continue;
                }
                if (QByteArrayView(Work.text_.constData() + state.begin_, qsizetype(state.end_ - state.begin_)) != QByteArrayView(expected))
                    throw Failure("InvalidWorkResolutionText");
            }
            Limits.Check();
        }
    }
    MergeWork DecodeDecisionWork(const Request& Input, const Result& Session, const QByteArray& Json, const Budget& Limits)
    {
        auto applied = Session; ApplyDecisionPlan(Input, applied, Json, Limits);
        return {applied.output_, applied.plan_->initial_, false};
    }
    QByteArray EncodeDecisionWork(const Request& Input, const Result& Session, const MergeWork& Work, const Budget& Limits)
    {
        validate(Session, Work, Limits);
        if (Work.reviewRequired_) throw Failure("DecisionWorkReviewRequired");
        auto plan = MakeDecisionPlan(Input, Session, Limits); auto blocks = plan["blocks"].toArray();
        const QStringList names{"unresolved", "left", "right", "base", "leftThenRight", "rightThenLeft", "manual", "omit"};
        for (std::size_t i = 0; i < Work.state_.size(); ++i)
        {
            Limits.Check(); const auto& state = Work.state_[i];
            if (!state.mapped_) throw Failure("DecisionWorkNotRepresentable");
            auto block = blocks[qsizetype(i)].toObject(); block["choice"] = names[int(state.resolution_)];
            if (state.resolution_ == Resolution::Manual)
                block["replacement"] = QString::fromUtf8(Work.text_.constData() + state.begin_, qsizetype(state.end_ - state.begin_));
            blocks[qsizetype(i)] = block;
        }
        plan["blocks"] = blocks; const auto json = QJsonDocument(plan).toJson(QJsonDocument::Compact);
        // Applying through the CLI validator proves that unchanged gaps and all
        // decisions survive conversion. Never silently discard free-form edits.
        const auto restored = DecodeDecisionWork(Input, Session, json, Limits);
        if (restored.text_ != Work.text_ || restored.state_ != Work.state_) throw Failure("DecisionWorkNotRepresentable");
        return json;
    }
    QByteArray EncodeMergeWork(const Request& Input, const Result& Session, const MergeWork& Work, const Budget& Limits)
    {
        validate(Session, Work, Limits); QJsonArray chunks, blocks;
        for (qsizetype at = 0; at < Work.text_.size(); at += chunkBytes)
        { Limits.Check(); chunks.append(QString::fromLatin1(Work.text_.mid(at, chunkBytes).toBase64())); }
        for (std::size_t i = 0; i < Work.state_.size(); ++i)
        {
            Limits.Check(); const auto& state = Work.state_[i];
            // Unmapped offsets can be stale after subsequent typing; they carry no authority.
            blocks.append(QJsonObject{{"id", qint64(Session.plan_->blocks_[i].id_)}, {"begin", qint64(state.mapped_ ? state.begin_ : 0)},
                {"end", qint64(state.mapped_ ? state.end_ : 0)}, {"resolution", int(state.resolution_)}, {"mapped", state.mapped_}});
        }
        const QJsonObject object{{"schemaVersion", 1}, {"kind", "text-merge-work"}, {"identity", DecisionIdentity(Input, Session, Limits)},
            {"takeAll", Input.takeAll_}, {"resultBytes", Work.text_.size()}, {"resultSha256", digest(Work.text_)},
            {"resultChunks", chunks}, {"blocks", blocks}, {"reviewRequired", Work.reviewRequired_}};
        const auto json = QJsonDocument(object).toJson(QJsonDocument::Compact);
        if (json.size() > decisionPlanBytes) throw Failure("WorkSizeLimit"); Limits.Check(); return json;
    }
    MergeWork DecodeMergeWork(const Request& Input, const Result& Session, const QByteArray& Json, const Budget& Limits, bool RecoverText)
    {
        auto object = ReadCompareJson(Json, Limits);
        QStringList keys{"schemaVersion", "kind", "identity", "takeAll", "resultBytes", "resultSha256", "resultChunks", "blocks", "reviewRequired"}; keys.sort();
        if (object.keys() != keys || object["schemaVersion"] != QJsonValue(1) || object["kind"] != QJsonValue("text-merge-work")) throw Failure("InvalidWorkSchema");
        auto expectedIdentity = DecisionIdentity(Input, Session, Limits); bool legacy = false;
        if (RecoverText)
        {
            // Recovery accepts changed bytes/encoding only for the same roles,
            // paths and comparison policy. Saved block authority is never reused.
            auto saved = object["identity"].toObject(); const auto generation = saved.take("generation");
            if (generation != QJsonValue(digest(QJsonDocument(saved).toJson(QJsonDocument::Compact)))) throw Failure("StaleWork");
            const auto engine = saved["engineVersion"].toString();
            if (engine != expectedIdentity["engineVersion"].toString() && engine != "myers-bounded-1" && engine != "myers-bounded-2" && engine != "myers-bounded-3") throw Failure("StaleWork");
            auto inputs = saved["inputs"].toArray(); const auto current = expectedIdentity["inputs"].toArray();
            if (inputs.size() != current.size()) throw Failure("StaleWork");
            for (qsizetype i = 0; i < inputs.size(); ++i)
            {
                Limits.Check(); auto before = inputs[i].toObject(); const auto after = current[i].toObject();
                const auto hash = before["sha256"].toString();
                if (before.keys() != after.keys() || hash.size() != 64 ||
                    std::any_of(hash.begin(), hash.end(), [](QChar C) { return !(C >= '0' && C <= '9') && !(C >= 'a' && C <= 'f'); }) ||
                    !before["encoding"].isString() || before["encoding"].toString().isEmpty() || !before["bom"].isBool()) throw Failure("StaleWork");
                for (const auto* key : {"sha256", "encoding", "bom"}) before[key] = after[key];
                inputs[i] = before;
            }
            saved["inputs"] = inputs; saved["engineVersion"] = expectedIdentity["engineVersion"];
            saved["generation"] = digest(QJsonDocument(saved).toJson(QJsonDocument::Compact));
            if (saved != expectedIdentity) throw Failure("StaleWork");
            legacy = true;
        }
        else if (object["identity"] != expectedIdentity)
        {
            const auto savedIdentity = object["identity"].toObject(); const auto engine = savedIdentity["engineVersion"].toString();
            if (engine != "myers-bounded-1" && engine != "myers-bounded-2" && engine != "myers-bounded-3") throw Failure("StaleWork");
            expectedIdentity.remove("generation"); expectedIdentity["engineVersion"] = engine;
            expectedIdentity["generation"] = digest(QJsonDocument(expectedIdentity).toJson(QJsonDocument::Compact));
            if (object["identity"] != expectedIdentity) throw Failure("StaleWork");
            legacy = true;
        }
        if (object["takeAll"] != QJsonValue(Input.takeAll_)) throw Failure("StaleWork");
        if (!object["reviewRequired"].isBool() || !object["resultChunks"].isArray() || !object["blocks"].isArray()) throw Failure("InvalidWorkSchema");
        MergeWork work; work.reviewRequired_ = object["reviewRequired"].toBool();
        const auto size = integer(object["resultBytes"], resultLimit); const auto chunks = object["resultChunks"].toArray();
        if (chunks.size() != (size + chunkBytes - 1) / chunkBytes) throw Failure("InvalidWorkChunks");
        work.text_.reserve(size);
        for (qsizetype i = 0; i < chunks.size(); ++i)
        {
            Limits.Check(); if (!chunks[i].isString()) throw Failure("InvalidWorkChunks");
            const auto encoded = chunks[i].toString().toUtf8(); const auto bytes = QByteArray::fromBase64(encoded);
            if (bytes.size() != std::min<qint64>(chunkBytes, size - work.text_.size()) || bytes.toBase64() != encoded) throw Failure("InvalidWorkChunks");
            work.text_.append(bytes);
        }
        if (object["resultSha256"] != QJsonValue(digest(work.text_))) throw Failure("WorkDigestMismatch");
        const auto blocks = object["blocks"].toArray();
        if (blocks.size() > qsizetype(decisionBlockLimit) || (!legacy && std::size_t(blocks.size()) != Session.plan_->blocks_.size())) throw Failure("InvalidWorkBlocks");
        work.state_.reserve(std::size_t(blocks.size()));
        const QStringList blockKeys{"begin", "end", "id", "mapped", "resolution"};
        for (qsizetype i = 0; i < blocks.size(); ++i)
        {
            Limits.Check(); if (!blocks[i].isObject()) throw Failure("InvalidWorkBlocks"); const auto block = blocks[i].toObject();
            if (block.keys() != blockKeys || !block["mapped"].isBool() ||
                block["id"] != QJsonValue(legacy ? qint64(i + 1) : qint64(Session.plan_->blocks_[std::size_t(i)].id_))) throw Failure("InvalidWorkBlocks");
            BlockState state{std::size_t(integer(block["begin"], size)), std::size_t(integer(block["end"], size)),
                static_cast<Resolution>(integer(block["resolution"], int(Resolution::Omit))), block["mapped"].toBool()};
            if (state.begin_ > state.end_ || (!state.mapped_ && (state.begin_ || state.end_))) throw Failure("InvalidWorkRange");
            work.state_.push_back(state);
        }
        if (legacy)
        {
            // Retain verified result text, but no old engine offsets or decisions
            // have authority over the newly computed block layout.
            work.state_.assign(Session.plan_->blocks_.size(), BlockState{0, 0, Resolution::Unresolved, false});
            work.reviewRequired_ = true;
        }
        validate(Session, work, Limits); return work;
    }
}
