#pragma once
#include <QString>
#include <QJsonObject>
#include <QTemporaryDir>
#include <memory>
namespace hc::registry {
class RegistryCompareLease final
{
public:
    explicit RegistryCompareLease(const QString& Root = Base());
    ~RegistryCompareLease();
    bool IsValid() const { return directory_ && directory_->isValid() && valid_; }
    QString FilePath(const QString& Name) const { return directory_ ? directory_->filePath(Name) : QString(); }
    bool Starting();
    void CancelStart();
    bool SetProcess(qint64 Pid);
    static QString Base();
    static int Sweep(const QString& Root = Base());
private:
    bool save();
    std::unique_ptr<QTemporaryDir> directory_;
    QJsonObject marker_;
    bool valid_ = false;
};
}