#pragma once

/// 플랫폼/프레임워크 헤더파일
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>

#include <functional>
#include <memory>

class QObject;
namespace hc::plugins { struct InspectedPlugin; }

namespace hc::packages
{
    struct VerificationRequest
    {
        QString root_;
        QMap<QString, QByteArray> hashes_;
    };

    class FileLease final
    {
    public:
        static std::shared_ptr<FileLease> Acquire(const VerificationRequest& Request);
        ~FileLease();
        // Uses the Windows file IDs captured while both sides hold their read leases.
        bool HasStartupVerification() const;

    private:
        FileLease();
        struct State;
        std::unique_ptr<State> state_;
        friend class VerificationService;
    };

    inline constexpr char VerificationEndpoint[] = "HC_PACKAGE_VERIFICATION_PIPE";
    inline constexpr char VerifyEachOpen[] = "HC_VERIFY_PACKAGE_EACH_OPEN";
    VerificationRequest ForPlugin(const plugins::InspectedPlugin& Plugin);
    void ConfigureVerification(QObject* Owner, bool Strict, std::function<QList<VerificationRequest>()> Requests);
}
