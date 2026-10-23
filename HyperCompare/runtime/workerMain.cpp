#include "stdafx.h"
#include "workerHost.hpp"
#include "semantic.hpp"
#include "extensionEngine.hpp"
#include "zipArchive.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>

namespace hc::compare
{
    int WorkerMain()
    {
        QFile input, output; if (!input.open(stdin, QIODevice::ReadOnly) || !output.open(stdout, QIODevice::WriteOnly)) return 2;
        const auto bytes = input.read(64 * 1024 * 1024 + 1); if (bytes.size() > 64 * 1024 * 1024) return 2;
        QJsonParseError error; const auto envelope = QJsonDocument::fromJson(bytes, &error).object();
        const auto nonce = envelope["nonce"].toString(), engine = envelope["engine"].toString(), digest = envelope["digest"].toString();
        const auto payload = envelope["request"].toObject();
        if (error.error != QJsonParseError::NoError || envelope["schema"].toInt() != 1 || nonce.size() != 36 || digest.size() != 64 ||
            digest != QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(payload).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())) return 2;
        QJsonObject result;
        try
        {
            if (engine == "source.tree-sitter") result = SemanticWorkerRequest(payload);
            else if (engine == "image.png") result = PngWorkerRequest(payload);
            else if (engine == "container.zip") result = ArchiveWorkerRequest(payload);
            else if (engine == "office.docx") result = DocxWorkerRequest(payload);
            else throw Failure("WorkerEngineUnsupported");
        }
        catch (const std::exception& failure) { result = {{"status", "Unavailable"}, {"reason", QString::fromUtf8(failure.what())}}; }
        result["schema"] = 1; result["nonce"] = nonce; result["engine"] = engine; result["digest"] = digest;
        const auto response = QJsonDocument(result).toJson(QJsonDocument::Compact);
        if (response.size() > 64 * 1024 * 1024 || output.write(response) != response.size() || !output.flush()) return 2;
        return 0;
    }
}
