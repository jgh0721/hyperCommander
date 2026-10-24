#include "stdafx.h"
#include "UniqueLibs/document/documentWorkspaceWidget.hpp"
#include "UniqueLibs/document/epubProcessView.hpp"
#include <QFile>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QMessageBox>
#include <QPdfWriter>
#include <QPainter>
#include <QtTest>

using namespace hc::document;
namespace {
QJsonObject document(DocumentWorkspaceWidget& Window, const QString& Id) {
    for(const auto& Value:Window.Snapshot()["documents"].toArray())
        if(Value.toObject()["documentId"]==Id)return Value.toObject();
    return {};
}
}
class EpubWorkspaceTests final : public QObject {
    Q_OBJECT
private slots:
    void cleanup() {
        EpubProcessView::RequestShutdown();
        QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        EpubProcessView::ReleaseLifecycle();
    }
    void inputBudget() {
        QTemporaryDir Temp;QFile File(Temp.filePath("oversized.epub"));
        QVERIFY(File.open(QIODevice::WriteOnly));QVERIFY(File.resize(65LL*1024*1024));File.close();
        const auto Result=ReadDocumentFile(File.fileName(),{},true);
        QVERIFY(!Result.HasValue());QCOMPARE(QString::fromStdString(Result.Failure().code_),QString("epub-input-budget"));
    }
    void sharedWorkspace_data() {
        QTest::addColumn<bool>("Hosted");QTest::addColumn<QString>("Mode");
        for(bool Hosted:{false,true})for(const auto* Mode:{"ready","cancel","invalid"})
            QTest::newRow(qPrintable(QString(Hosted?"viewer-":"editor-")+Mode))<<Hosted<<QString(Mode);
    }
    void sharedWorkspace() {
        QFETCH(bool,Hosted);QFETCH(QString,Mode);
        QTemporaryDir Temp;QVERIFY(Temp.isValid());
        QFile Text(Temp.filePath("keep.txt"));QVERIFY(Text.open(QIODevice::WriteOnly));Text.write("original");Text.close();
        const auto PdfPath=Temp.filePath("keep.pdf");
        { QPdfWriter Pdf(PdfPath);QPainter Painter(&Pdf);Painter.drawText(100,100,"Keep PDF tab"); }
        DocumentWorkspaceWidget Window(Temp.filePath("recovery"),Hosted);
        Window.ConfigureEpub(MakeProductEpubRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION")));
        const auto Hwp=qEnvironmentVariable("HC_EPUB_HWP");
        if(!Hwp.isEmpty()) Window.ConfigureHwp(MakeProductHwpRuntime(qEnvironmentVariable("HC_EPUB_PRODUCT_APPLICATION"), qgetenv("HC_EPUB_HWP_PIN")));
        Window.SetClosePrompt([](const QString&){return int(QMessageBox::Discard);});
        QVERIFY(Window.SetTabbed(true));Window.resize(1280,900);Window.show();
        const auto TextId=Window.Handle({{"action","open"},{"path",Text.fileName()}})["documentId"].toString();QVERIFY(!TextId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(document(Window,TextId)["lifecycle"]=="ready",30000);
        QVERIFY(!Window.Handle({{"action","replaceText"},{"documentId",TextId},{"text","keep unsaved text"}}).contains("error"));
        QVERIFY(document(Window,TextId)["dirty"].toBool());
        const auto PdfId=Window.Handle({{"action","open"},{"path",PdfPath}})["documentId"].toString();QVERIFY(!PdfId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(document(Window,PdfId)["lifecycle"]=="ready",30000);
        QCOMPARE(document(Window,PdfId)["view"].toObject()["viewId"],QJsonValue("pdf"));
        QString HwpId;
        if(Mode=="ready" && !Hwp.isEmpty()) {
            HwpId=Window.Handle({{"action","open"},{"path",Hwp}})["documentId"].toString(); QVERIFY(!HwpId.isEmpty());
            QTRY_VERIFY_WITH_TIMEOUT(document(Window,HwpId)["view"].toObject()["ready"].toBool(),60000);
        }
        QString Book=qEnvironmentVariable("HC_EPUB_POC_EPUB");
        if(Mode=="invalid") {Book=Temp.filePath("invalid.epub");QFile Bad(Book);QVERIFY(Bad.open(QIODevice::WriteOnly));Bad.write("invalid epub");}
        const auto Id=Window.Handle({{"action","open"},{"path",Book}})["documentId"].toString();QVERIFY(!Id.isEmpty());
        if(Mode=="cancel")QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["viewId"]=="epub",30000);
        if(Mode!="cancel") {
            if(Mode=="invalid")QTRY_VERIFY_WITH_TIMEOUT(!document(Window,Id)["error"].toString().isEmpty(),30000);
            else {
                QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["ready"].toBool(),60000);
                QVERIFY(document(Window,Id)["view"].toObject()["readOnly"].toBool());
                QVERIFY(!document(Window,Id)["view"].toObject()["epub"].toObject()["images"].toArray().isEmpty());
                QVERIFY(!document(Window,Id)["canSave"].toBool());QVERIFY(!document(Window,Id)["canSaveAs"].toBool());
                QVERIFY(Window.Handle({{"action","saveAs"},{"documentId",Id},{"path",Temp.filePath("out.epub")}}).contains("error"));
                QVERIFY(!QFile::exists(Temp.filePath("out.epub")));
                const auto Generation=document(Window,Id)["generation"].toString();
                QVERIFY(!Window.Handle({{"action","reload"},{"documentId",Id},{"discard",true}}).contains("error"));
                QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["generation"].toString()!=Generation,30000);
                QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id)["view"].toObject()["ready"].toBool(),60000);
            }
        }
        QVERIFY(!Window.Handle({{"action","close"},{"documentId",Id},{"discard",true}}).contains("error"));
        QTRY_VERIFY_WITH_TIMEOUT(document(Window,Id).isEmpty(),30000);
        QVERIFY(document(Window,TextId)["dirty"].toBool());
        QCOMPARE(Window.Snapshot()["documents"].toArray().size(),HwpId.isEmpty()?2:3);
        if(!HwpId.isEmpty()) QVERIFY(document(Window,HwpId)["view"].toObject()["ready"].toBool());
        QCOMPARE(document(Window,PdfId)["view"].toObject()["viewId"],QJsonValue("pdf"));
        QTRY_VERIFY_WITH_TIMEOUT(EpubProcessView::IsDrained(),30000);
        QTRY_VERIFY_WITH_TIMEOUT(Window.RequestClose(),30000);
    }
};
QTEST_MAIN(EpubWorkspaceTests)
#include "testWorkspace.moc"
