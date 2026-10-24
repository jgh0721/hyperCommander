#include "runtimeVerification.hpp"
#include "UniqueLibs/document/epubProcessView.hpp"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include <QApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QImage>
#include <QImageReader>
#include <QFontDatabase>
#include <QFontInfo>
#include <QPainter>
#include <QPdfWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QThread>
#include <QCryptographicHash>
#include <fpdfview.h>
#include <fpdf_edit.h>
#include <iostream>
#include <functional>
namespace {
bool Wait(const std::function<bool()>& Predicate) {
    QElapsedTimer Timer; Timer.start(); do { QApplication::processEvents(QEventLoop::AllEvents, 20); if (Predicate()) return true; QThread::msleep(2); } while (Timer.elapsed() < 20000); return false;
}
QByteArray Read(const QString& Path) { QFile File(Path); return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{}; }
QJsonObject Document(const QJsonObject& State, const QString& Id) { for (const auto& Item : State["documents"].toArray()) if (Item.toObject()["documentId"].toString() == Id) return Item.toObject(); return {}; }
}
int VerifyDocumentRuntime() {
    using namespace hc::document;
    QTemporaryDir Temp; QJsonObject Report{{"qtVersion", qVersion()}, {"applicationDirectory", QCoreApplication::applicationDirPath()}, {"libraryPaths", QJsonArray::fromStringList(QCoreApplication::libraryPaths())}};
    QJsonArray Errors, Codecs, Saves;
    const auto Fail = [&](const QString& Error) { Errors.append(Error); };
    for (const auto& Format : QImageReader::supportedImageFormats()) Codecs.append(QString::fromLatin1(Format)); Report["imageFormats"] = Codecs;
    for (const auto* Format : {"png", "jpeg", "gif", "tiff", "webp", "svg"}) if (!Codecs.contains(QString::fromLatin1(Format))) Fail("codec-missing:" + QString::fromLatin1(Format));
    const auto Font = QFontDatabase::systemFont(QFontDatabase::GeneralFont); Report["fontFamily"] = QFontInfo(Font).family();
    const bool Korean = QFontDatabase::writingSystems().contains(QFontDatabase::Korean); Report["koreanFontAvailable"] = Korean;
    if (QFontInfo(Font).family().isEmpty() || !Korean) Fail("font-fallback-unavailable");
    if (!Temp.isValid()) Fail("temporary-directory-unavailable");
    else {
        const auto TextPath = Temp.filePath("한글.txt"), PdfPath = Temp.filePath("document.pdf"), ImagePath = Temp.filePath("alpha.png");
        { QFile File(TextPath); if (!File.open(QIODevice::WriteOnly) || File.write("original\r\n") < 0) Fail("text-fixture-write"); }
        { QPdfWriter Writer(PdfPath); QPainter Painter(&Writer); if (!Painter.isActive()) Fail("pdf-fixture-write"); else { Painter.drawText(100, 300, "Runtime PDF text"); Painter.end(); } }
        { QImage Image(32, 24, QImage::Format_RGBA8888); Image.fill(QColor(70, 130, 190, 160)); if (!Image.save(ImagePath)) Fail("image-fixture-write"); }
        DocumentWorkspaceWidget Workspace(Temp.filePath("recovery")); Workspace.resize(960, 700);
        QStringList Ids;
        for (const auto& Path : {TextPath, PdfPath, ImagePath}) { const auto Opened = Workspace.Handle({{"action", "open"}, {"path", Path}}); Ids.append(Opened["documentId"].toString()); if (Opened.contains("error") || Ids.back().isEmpty()) Fail("document-open-rejected"); }
        if (!Wait([&] { return Workspace.IsIdle(); })) Fail("document-load-timeout");
        for (const auto& Id : Ids) { const auto State = Document(Workspace.Snapshot(), Id); if (State["lifecycle"].toString() != "ready" || !State["error"].toString().isEmpty()) Fail("document-load:" + State["error"].toString()); }
        if (Errors.empty()) {
            const QString Edited = QStringLiteral("P5 저장 확인\r\n");
            const auto Execute = [&](QJsonObject Request) { const auto Result = Workspace.Handle(Request); if (Result.contains("error")) Fail(Result["error"].toString()); };
            Execute({{"action", "replaceText"}, {"documentId", Ids[0]}, {"text", Edited}});
            Execute({{"action", "pdf.rotate"}, {"documentId", Ids[1]}, {"angle", 90}});
            Execute({{"action", "image.crop"}, {"documentId", Ids[2]}, {"x", 1}, {"y", 1}, {"width", 12}, {"height", 8}});
            for (const auto& Id : Ids) Execute({{"action", "save"}, {"documentId", Id}});
            if (!Wait([&] { return Workspace.IsIdle(); })) Fail("document-save-timeout");
            for (int Index = 0; Index < Ids.size(); ++Index) { const auto State = Document(Workspace.Snapshot(), Ids[Index]); if (State["dirty"].toBool() || !State["error"].toString().isEmpty()) Fail("document-save:" + State["error"].toString()); }
            if (Read(TextPath) != Edited.toUtf8()) Fail("text-save-roundtrip");
            auto Pdf = Read(PdfPath); auto Handle = FPDF_LoadMemDocument64(Pdf.constData(), Pdf.size(), nullptr);
            if (!Handle) Fail("pdf-save-roundtrip"); else { auto Page = FPDF_LoadPage(Handle, 0); if (!Page || FPDF_GetPageCount(Handle) != 1 || FPDFPage_GetRotation(Page) != 1) Fail("pdf-save-rotation"); if (Page) FPDF_ClosePage(Page); FPDF_CloseDocument(Handle); }
            if (QImage(ImagePath).size() != QSize(12, 8)) Fail("image-save-roundtrip");
            for (const auto& Path : {TextPath, PdfPath, ImagePath}) Saves.append(QJsonObject{{"name", QFileInfo(Path).fileName()}, {"sha256", QString::fromLatin1(QCryptographicHash::hash(Read(Path), QCryptographicHash::Sha256).toHex())}});
        }
        Report["documents"] = Workspace.Snapshot()["documents"]; Workspace.RequestClose();
    }
    Report["saves"] = Saves; Report["errors"] = Errors; Report["ok"] = Errors.empty();
    const auto Output = QJsonDocument(Report).toJson(QJsonDocument::Compact); std::cout.write(Output.constData(), Output.size()); std::cout << std::endl;
    return Errors.empty() ? 0 : 10;
}

int VerifyEpubRuntime(const QString& Path) {
    using namespace hc::document;
    QTemporaryDir Temp;
    QJsonArray Errors;
    QJsonObject Report{{"applicationDirectory", QCoreApplication::applicationDirPath()}};
    {
        DocumentWorkspaceWidget Workspace(Temp.filePath("recovery"));
        Workspace.resize(1100, 800); Workspace.show();
        const auto Opened = Workspace.Handle({{"action", "open"}, {"path", Path}});
        const auto Id = Opened["documentId"].toString();
        if (Id.isEmpty()) Errors.append("open-rejected");
        else {
            const bool Loaded = Wait([&] {
                const auto State = Document(Workspace.Snapshot(), Id);
                return State["view"].toObject()["ready"].toBool() || !State["error"].toString().isEmpty();
            });
            const auto State = Document(Workspace.Snapshot(), Id);
            const auto View = State["view"].toObject();
            Report["document"] = State;
            if (!Loaded || !View["ready"].toBool() || View["viewId"].toString() != "epub") Errors.append("epub-not-ready");
            if (!View["readOnly"].toBool() || State["canSave"].toBool()) Errors.append("epub-not-read-only");
            const auto Save = Workspace.Handle({{"action", "saveAs"}, {"documentId", Id}, {"path", Temp.filePath("forbidden.epub")}});
            Report["saveRejected"] = Save.contains("error") && !QFile::exists(Temp.filePath("forbidden.epub"));
            if (!Report["saveRejected"].toBool()) Errors.append("epub-save-allowed");
        }
        Workspace.RequestClose();
        EpubProcessView::RequestShutdown();
        if (!Wait([] { return EpubProcessView::IsDrained(); })) Errors.append("epub-drain-timeout");
    }
    // Owners are gone; their registry retains retiring workers until the Job drains.
    if (!Wait([] { return EpubProcessView::IsDrained(); })) Errors.append("epub-retirement-timeout");
    EpubProcessView::ReleaseLifecycle();
    Report["drained"] = EpubProcessView::IsDrained();
    Report["errors"] = Errors; Report["ok"] = Errors.empty();
    const auto Output = QJsonDocument(Report).toJson(QJsonDocument::Compact);
    std::cout.write(Output.constData(), Output.size()); std::cout << std::endl;
    return Errors.empty() ? 0 : 10;
}
