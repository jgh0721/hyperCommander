#pragma once
#include "session.hpp"

namespace hc::compare
{
    struct MergeWork
    {
        QByteArray text_;
        std::vector<BlockState> state_;
        bool reviewRequired_ = false;
    };
    struct MergeWorkFile
    {
        MergeWork work_;
        storage::DestinationExpectation expected_;
        QJsonArray warnings_, recovery_;
    };
    QByteArray EncodeDecisionWork(const Request& Input, const Result& Session, const MergeWork& Work, const Budget& Limits);
    MergeWork DecodeDecisionWork(const Request& Input, const Result& Session, const QByteArray& Json, const Budget& Limits);
    MergeWorkFile LoadDecisionWorkFile(const QString& Path, const Request& Input, const Result& Session, const Budget& Limits);
    MergeWorkFile SaveDecisionWorkFile(const QString& Path, const Request& Input, const Result& Session, const MergeWork& Work,
        bool ReplaceExisting, const Budget& Limits);
    QByteArray EncodeMergeWork(const Request& Input, const Result& Session, const MergeWork& Work, const Budget& Limits);
    MergeWork DecodeMergeWork(const Request& Input, const Result& Session, const QByteArray& Json, const Budget& Limits, bool RecoverText = false);
    MergeWorkFile LoadMergeWorkFile(const QString& Path, const Request& Input, const Result& Session, const Budget& Limits, bool RecoverText = false);
    MergeWorkFile SaveMergeWorkFile(const QString& Path, const Request& Input, const Result& Session, const MergeWork& Work,
        const std::optional<storage::DestinationExpectation>& Expected, bool ReplaceExisting, const Budget& Limits);
}
