#pragma once
#include "session.hpp"
#include <QFile>
#include <QFileInfo>

namespace hc::compare
{
    // A local, flushed journal survives loss of the destination SMB connection.
    // Its lifetime must enclose the native write session using Checkpoint().
    class SaveRecoveryJournal final
    {
    public:
        explicit SaveRecoveryJournal(const QString& Directory = {});
        storage::WriteCheckpoint Checkpoint();
        QString Path() const { return file_.fileName(); }
        bool Exists() const { return QFileInfo::exists(Path()); }
        bool Remove();
        void Track(QJsonArray& Recovery) const;
        void Complete(const storage::WriteReceipt& Receipt, QJsonArray& Warnings, QJsonArray& Recovery, bool Verified = true);
    private:
        QFile file_;
        std::uint64_t sequence_ = 0;
        bool failed_ = false;
    };
    struct SaveRecoveryFailure : Failure
    {
        QJsonArray recovery_;
        SaveRecoveryFailure(std::string Code, QJsonArray Recovery) : Failure(std::move(Code)), recovery_(std::move(Recovery)) {}
    };
    QString SaveRecoveryDirectory();
    void RecoverSavedFile(const Request& Input, Result& Value, const Budget& Limits);
}
