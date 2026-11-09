#pragma once
#include "session.hpp"
#include <QTemporaryFile>
#include <array>

namespace hc::compare
{
    QString BinaryProgressText(const ComparisonProgress::Snapshot& Value);
    struct BinaryBacking { QTemporaryFile file_; BinaryBacking(); };
    struct BinaryPiece { std::shared_ptr<BinaryBacking> backing_; qint64 offset_ = 0, size_ = 0; };
    struct BinarySourceLock { std::atomic<void*> handle_{nullptr}; ~BinarySourceLock(); void Release(); };
    struct BinaryDocument
    {
        Snapshot original_;
        std::shared_ptr<BinarySourceLock> lock_;
        std::vector<BinaryPiece> pieces_;
        qint64 size_ = 0;
        QByteArray hash_;
        quint64 revision_ = 0;
        QString format_ = "raw";
    };
    struct HexRun
    {
        std::array<qint64, 2> begin_{}, size_{};
        qint64 position_ = 0, length_ = 0;
        quint64 changed_ = 0;
    };
    struct BinaryComparison
    {
        std::array<BinaryDocument, 2> documents_;
        std::shared_ptr<BinaryBacking> runs_, changes_;
        qint64 runCount_ = 0, changeCount_ = 0, length_ = 0, different_ = 0;
        QString mode_ = "none";
        std::array<std::array<quint8, 4096>, 2> overview_{};
        HexRun Run(qint64 Index) const;
        HexRun Change(qint64 Index) const;
        HexRun At(qint64 Position) const;
        qint64 Position(int Side, qint64 Offset) const;
        QJsonObject Json() const;
    };
    void SetBinarySourceLock(BinaryDocument& Document, qint64 UnlockedMiB, const Budget& Limits);
    BinaryDocument OpenBinary(const QString& Path, const Budget& Limits);
    BinaryDocument ConvertBinary(const BinaryDocument& Document, const QString& Format, const Budget& Limits);
    BinaryDocument BinaryFromBytes(const QByteArray& Bytes);
    QByteArray ReadBinary(const BinaryDocument& Document, qint64 Offset, qint64 Count, const Budget& Limits = {});
    BinaryDocument ReplaceBinary(const BinaryDocument& Target, qint64 Begin, qint64 End,
        const BinaryDocument& Source, qint64 SourceBegin, qint64 SourceEnd);
    std::shared_ptr<BinaryComparison> CompareBinary(std::array<BinaryDocument, 2> Documents,
        const QString& Mode, const Budget& Limits);
    qint64 FindBinary(const BinaryDocument& Document, const QByteArray& Pattern, qint64 Start,
        bool Backward, const Budget& Limits);
    BinaryDocument SaveBinary(const Request& Input, const BinaryDocument& Document,
        const QString& Path, const storage::DestinationExpectation& Expected, const Budget& Limits);
    storage::DestinationExpectation InspectBinaryTarget(const QString& Path, const Budget& Limits);
    // Integer-only ratio with a bounded output domain; no offset * pixel overflow.
    qint64 HexScale(qint64 Value, qint64 Total, int Pixels);
}
