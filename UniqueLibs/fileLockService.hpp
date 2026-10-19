#pragma once

#include <QJsonObject>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace hc::filelocks {
// Blocking I/O is called only from a UI-owned background task. Cancel is safe
// from the UI thread; destruction belongs to the last background owner.
class FileLockClient {
public:
    using Launcher = std::function<void*(const QString&, const QString&, bool)>;
    explicit FileLockClient(QStringList Paths, bool Elevated = false, QString Helper = {}, Launcher Launch = {});
    ~FileLockClient();
    QJsonObject Query();
    QJsonObject Act(const QString& Capability, const QString& Generation, const QString& Action, const QString& RequestId);
    void Cancel();
private:
    bool start();
    QJsonObject exchange(QJsonObject Message, unsigned long Timeout);
    QStringList paths_;
    bool elevated_ = false;
    QString helper_, nonce_;
    Launcher launcher_;
    void* pipe_ = nullptr;
    void* process_ = nullptr;
    std::atomic_bool cancelled_{false};
    std::mutex mutex_;
};
// ExpectedCaller is fixed by the product entry point. Test fixtures explicitly
// pass their own test binary; a command-line payload cannot override it.
unsigned long ExecuteFileLockSession(const QJsonObject& Bootstrap, const QString& ExpectedCaller = {});
}
