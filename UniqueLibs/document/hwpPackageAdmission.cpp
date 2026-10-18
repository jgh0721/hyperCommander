#include "hwpStartupTrace.hpp"
#include "hwpPackageAdmission.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
namespace hc::document
{
    std::shared_ptr<const HwpBundle> LoadApprovedHwpPackage(const QString& Root,
        const QByteArray& ExpectedManifestSha256, QString& Error)
    {
        startup::Scope Verification("helper.packageAdmission");
        Error.clear();
        auto Fail = [&](const char* Code) -> std::shared_ptr<const HwpBundle> { Error = QString::fromLatin1(Code); return {}; };
        if (ExpectedManifestSha256.size() != 64
            || QByteArray::fromHex(ExpectedManifestSha256).toHex() != ExpectedManifestSha256)
            return Fail("rhwp-package-policy-unavailable");
        if (Root != QStringLiteral(":/hc/rhwp/bundle") && !QFileInfo(Root).isAbsolute()) return Fail("rhwp-package-root");
        QFile Manifest(Root + "/asset-manifest.json");
        if (!Manifest.open(QIODevice::ReadOnly)) return Fail("rhwp-package-manifest-missing");
        const auto Bytes = Manifest.read(256 * 1024 + 1);
        if (Manifest.error() != QFileDevice::NoError || !Manifest.atEnd() || Bytes.size() > 256 * 1024)
            return Fail("rhwp-package-manifest-budget");
        if (QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex() != ExpectedManifestSha256)
            return Fail("rhwp-package-manifest-hash");
        QJsonParseError Parse;
        const auto Json = QJsonDocument::fromJson(Bytes, &Parse);
        if (Parse.error != QJsonParseError::NoError || !Json.isObject()) return Fail("rhwp-package-manifest-json");
        const auto Object = Json.object();
        if (Object.value("schemaVersion") != QJsonValue(1)
            || Object.value("sourceCommit") != QJsonValue("1a76570e833917d15817415a53c09ad61ab3203f"))
            return Fail("rhwp-package-source");
        if (Object.value("releaseEligible") != QJsonValue(true)) return Fail("rhwp-package-not-release-eligible");
        if (!Object.value("files").isArray()) return Fail("rhwp-package-files");
        const auto Files = Object.value("files").toArray();
        if (Files.isEmpty() || Files.size() > 512) return Fail("rhwp-package-files");
        QMap<QString, QByteArray> Pins;
        for (const auto& Value : Files) {
            if (!Value.isObject()) return Fail("rhwp-package-entry");
            const auto Entry = Value.toObject();
            if (!Entry.value("path").isString() || !Entry.value("sha256").isString()) return Fail("rhwp-package-entry");
            const auto Path = Entry.value("path").toString();
            if (Pins.contains(Path)) return Fail("rhwp-package-duplicate");
            Pins.insert(Path, Entry.value("sha256").toString().toLatin1());
        }
        return HwpBundle::LoadPinned(Root, Pins, Error);
    }
}
