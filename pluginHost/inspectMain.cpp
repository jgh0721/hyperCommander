#include "manifestFile.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>

int main(int Argc, char** Argv)
{
    QCoreApplication App(Argc, Argv);
    if (App.arguments().size() != 2) { std::cerr << "Usage: hc-plugin-inspect <manifest.json>\n"; return 2; }
    auto Result = hc::plugins::InspectManifestFile(App.arguments()[1]);
    if (!Result.HasValue()) { std::cerr << Result.Failure().code_ << '\n'; return 1; }
    const auto& Value = Result.Value();
    const auto Json = QJsonDocument(QJsonObject{{"id", QString::fromStdString(Value.manifest_.id_)},
        {"binary", Value.binaryPath_}, {"sha256", QString::fromStdString(Value.manifest_.sha256_)},
        {"inspection", "manifest-hash-pe-headers"}, {"loaded", false}, {"authorized", false}}).toJson();
    std::cout.write(Json.constData(), Json.size()); return 0;
}
