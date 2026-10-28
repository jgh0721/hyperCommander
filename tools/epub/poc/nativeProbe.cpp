#include "stdafx.h"
#include "commonLibs/document/epub/epubPackage.hpp"
#include "commonLibs/document/epub/epubEditModel.hpp"

/// Framework headers
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>

int main(int Argc, char** Argv)
{
    QCoreApplication App(Argc, Argv);
    QCommandLineParser Parser; Parser.addHelpOption();
    Parser.addPositionalArgument("epub", "Immutable EPUB input");
    Parser.addOption({"cancel-after", "Test cancellation after N checkpoints", "count", "0"});
    Parser.addOption({"metadata", "Inspect native metadata"});
    Parser.addOption({"session", "Validate the metadata and all resources without a Base64 snapshot"});
    Parser.addOption({"inspect-image", "Inspect image occurrences", "resource"});
    Parser.addOption({"edit-requests", "Apply bounded structural/property JSON requests", "json"});
    Parser.addOption({"output", "Create a new EPUB copy; existing paths are refused", "epub"});
    Parser.process(App);
    if(Parser.positionalArguments().size() != 1) Parser.showHelp(2);
    if(Parser.isSet("session")) {
        QFile Input(Parser.positionalArguments().first());
        if(!Input.open(QIODevice::ReadOnly) || Input.size()>hc::document::epub::InputLimit) return 2;
        const auto Session=hc::document::epub::PublicationSession::Open(Input.read(hc::document::epub::InputLimit+1));
        auto Result=Session->Index();
        if(Result) {
            const auto Validation=Session->ValidateAll();
            if(!Validation) Result={{},Validation.error_,Validation.location_};
        }
        QFile Output;Output.open(stdout,QIODevice::WriteOnly);
        Output.write(QJsonDocument(Result?Result.publication_:QJsonObject{{"error",Result.error_},{"location",Result.location_}}).toJson(QJsonDocument::Compact));
        return Result?0:2;
    }
    if(Parser.isSet("metadata") || Parser.isSet("inspect-image") || Parser.isSet("edit-requests")) {
        QFile Input(Parser.positionalArguments().first());QJsonObject Report;int Exit=2;
        if(Input.open(QIODevice::ReadOnly) && Input.size()<=64*1024*1024) {
            hc::document::epub::EditModel Model;if(Model.Load(Input.read(64*1024*1024+1))) {
                if(Parser.isSet("metadata")){Report=Model.Metadata();Exit=Report.contains("error")?2:0;}
                else if(Parser.isSet("inspect-image")){Report=Model.InspectImage(Parser.value("inspect-image"));Exit=Report.contains("error")?2:0;}
                else {
                    QFile Requests(Parser.value("edit-requests"));QString Error;
                    if(!Parser.isSet("output") || !Requests.open(QIODevice::ReadOnly) || Requests.size()>1024*1024)Error="edit-request-file";
                    QJsonParseError Parse;const auto Decoded=Error.isEmpty()?QJsonDocument::fromJson(Requests.readAll(),&Parse):QJsonDocument{};
                    if(Error.isEmpty() && (Parse.error!=QJsonParseError::NoError || !Decoded.isArray() || Decoded.array().size()>32))Error="edit-request-json";
                    if(Error.isEmpty())for(const auto& Value:Decoded.array()){if(!Value.isObject()){Error="edit-request-object";break;}Error=Model.Structure(Value.toObject());if(!Error.isEmpty())break;}
                    const auto Copy=Error.isEmpty()?Model.Export():hc::document::epub::EditOutput{};
                    if(Error.isEmpty() && !Copy)Error=Copy.error_;
                    if(Error.isEmpty()){QFile Destination(Parser.value("output"));if(!Destination.open(QIODevice::WriteOnly|QIODevice::NewOnly))Error="edit-output-existing-or-denied";else if(Destination.write(Copy.bytes_)!=Copy.bytes_.size() || !Destination.flush())Error="edit-output-write";else {Report={{"sha256",QString::fromLatin1(QCryptographicHash::hash(Copy.bytes_,QCryptographicHash::Sha256).toHex())},{"metadata",Model.Metadata()}};Exit=0;}}
                    if(!Error.isEmpty())Report={{"error",Error}};
                }
            }else Report={{"error",Model.Error()}};
        }else Report={{"error","input-file-or-budget"}};
        QFile Output;Output.open(stdout,QIODevice::WriteOnly);Output.write(QJsonDocument(Report).toJson(QJsonDocument::Compact));return Exit;
    }
    int Checks = 0; const auto CancelAfter = Parser.value("cancel-after").toInt();
    const auto Result = hc::document::epub::PrepareFile(Parser.positionalArguments().first(), [&] { return CancelAfter > 0 && ++Checks >= CancelAfter; });
    QFile Output; Output.open(stdout, QIODevice::WriteOnly);
    Output.write(QJsonDocument(Result ? Result.publication_ : QJsonObject{{"error", Result.error_}, {"location", Result.location_}}).toJson(QJsonDocument::Compact));
    return Result ? 0 : 2;
}
