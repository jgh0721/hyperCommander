#include "stdafx.h"
#include "commonLibs/document/epub/epubPackage.hpp"

/// Framework headers
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QJsonDocument>

int main(int Argc, char** Argv)
{
    QCoreApplication App(Argc, Argv);
    QCommandLineParser Parser; Parser.addHelpOption();
    Parser.addPositionalArgument("epub", "Immutable EPUB input");
    Parser.addOption({"cancel-after", "Test cancellation after N checkpoints", "count", "0"});
    Parser.process(App);
    if(Parser.positionalArguments().size() != 1) Parser.showHelp(2);
    int Checks = 0; const auto CancelAfter = Parser.value("cancel-after").toInt();
    const auto Result = hc::document::epub::PrepareFile(Parser.positionalArguments().first(), [&] { return CancelAfter > 0 && ++Checks >= CancelAfter; });
    QFile Output; Output.open(stdout, QIODevice::WriteOnly);
    Output.write(QJsonDocument(Result ? Result.publication_ : QJsonObject{{"error", Result.error_}, {"location", Result.location_}}).toJson(QJsonDocument::Compact));
    return Result ? 0 : 2;
}
