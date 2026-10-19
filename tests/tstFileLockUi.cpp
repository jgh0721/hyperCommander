#include "stdafx.h"
#include "HyperCommander/UIs/fileLockDialog.hpp"
#include "UniqueLibs/fileLockService.hpp"
#include <QtTest>
#include <QApplication>
#include <QFontDatabase>
#include <QTreeWidget>
#include <QPushButton>
#include <QCheckBox>
#include <QProcess>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <fmstyle/ThemeManager.h>
#include <fmwidgets/Button.h>
#include <iostream>

class FileLockUiTests : public QObject {
    Q_OBJECT
private slots:
    void actualThemeAndSafeConfirmation_data() {
        QTest::addColumn<bool>("Dark");QTest::newRow("dark")<<true;QTest::newRow("light")<<false;
    }
    void actualThemeAndSafeConfirmation() {
        QFETCH(bool,Dark);
        auto& Theme=fm::style::ThemeManager::instance();Theme.setScheme(Dark?fm::style::ThemeManager::Scheme::Dark:fm::style::ThemeManager::Scheme::Light);
        QTemporaryDir Root;const auto Path=Root.filePath("owned-file.txt");QFile File(Path);QVERIFY(File.open(QIODevice::WriteOnly));File.write("owned UI fixture");File.close();
        QProcess Holder;Holder.start(QCoreApplication::applicationFilePath(),{"--hold-ui",Path});QVERIFY(Holder.waitForStarted(5000));QVERIFY(Holder.waitForReadyRead(5000));
        const auto Cleanup=qScopeGuard([&]{Holder.write("release\n");Holder.waitForBytesWritten(1000);if(!Holder.waitForFinished(5000)){Holder.kill();Holder.waitForFinished(5000);}});
        QVERIFY(QJsonDocument::fromJson(Holder.readLine()).object()["ready"].toBool());
        bool Current=true;
        hc::ui::FileLockDialogRequest Request;Request.source_=Path;Request.reason_="win32:32";Request.current_=[&]{return Current;};
        hc::ui::FileLockDialog Dialog(Request,nullptr,[](const QStringList& Paths,bool){return std::make_shared<hc::filelocks::FileLockClient>(Paths,false,QCoreApplication::applicationFilePath());});
        Dialog.show();
        auto* Table=Dialog.findChild<QTreeWidget*>("hc.fileLocks.references");QVERIFY(Table);
        auto* Close=Dialog.findChild<QPushButton*>("hc.fileLocks.closeHandle");QVERIFY(Close);
        QTRY_VERIFY_WITH_TIMEOUT(Table->topLevelItemCount()>0,35000);
        QVERIFY(!Close->isEnabled());
        QTreeWidgetItem* Handle=nullptr;
        for(int I=0;I<Table->topLevelItemCount();++I){auto* Group=Table->topLevelItem(I);if(Group->text(1).toLongLong()!=Holder.processId())continue;
            for(int J=0;J<Group->childCount();++J)if(Group->child(J)->text(3)==QString::fromUtf8("파일 핸들 확인"))Handle=Group->child(J);}
        QVERIFY(Handle);Table->setCurrentItem(Handle);QTRY_VERIFY(Close->isEnabled());
        bool Checked=false,Cancelled=false;
        QTimer::singleShot(0,&Dialog,[&]{
            auto* Confirm=static_cast<hc::ui::DialogShell*>(Dialog.findChild<QDialog*>("hc.fileLocks.confirm"));
            if(!Confirm)return;
            auto* Consent=Confirm->findChild<QCheckBox*>("hc.fileLocks.riskConsent");if(!Consent)return;
            connect(Confirm,&QDialog::finished,&Dialog,[&](int Result){Cancelled=Result==QDialog::Rejected;});
            Checked=Confirm->CancelButton()->isDefault()&&!Confirm->AcceptButton()->isDefault()&&!Confirm->AcceptButton()->isEnabled();
            const auto Captures=qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
            if(!Captures.isEmpty()){QDir().mkpath(Captures);Checked=Checked&&Confirm->grab().save(QDir(Captures).filePath(Dark?"file-locks-confirm-dark.png":"file-locks-confirm-light.png"));}
            Consent->setChecked(true);Checked=Checked&&Confirm->AcceptButton()->isEnabled()&&Confirm->CancelButton()->isDefault();
            QTest::keyClick(Confirm,Qt::Key_Return);
        });
        Close->click();QVERIFY(Checked);QVERIFY(Cancelled);QCOMPARE(Holder.state(),QProcess::Running);
        const auto Captures=qEnvironmentVariable("HC_UI_ARTIFACT_DIR");
        if(!Captures.isEmpty()){QDir().mkpath(Captures);QVERIFY(Dialog.grab().save(QDir(Captures).filePath(Dark?"file-locks-dark.png":"file-locks-light.png")));}
        Current=false;
        QTRY_VERIFY(!Close->isEnabled());QVERIFY(!Dialog.AcceptButton()->isEnabled());
        QTest::keyClick(&Dialog,Qt::Key_Escape);QCOMPARE(Dialog.SelectedReply(),hc::ui::FileLockDialog::Reply::None);
    }
};

int main(int Argc,char** Argv) {
    if(Argc>1&&QString::fromLocal8Bit(Argv[1])=="--file-lock-session"){
        QCoreApplication App(Argc,Argv);
        const auto Request=QJsonDocument::fromJson(QByteArray::fromBase64(QByteArray(Argv[2]),QByteArray::Base64UrlEncoding)).object();
        return int(hc::filelocks::ExecuteFileLockSession(Request,QCoreApplication::applicationFilePath()));
    }
    if(Argc==3&&QString::fromLocal8Bit(Argv[1])=="--hold-ui"){
        QCoreApplication App(Argc,Argv);const auto Path=QDir::toNativeSeparators(QString::fromLocal8Bit(Argv[2])).toStdWString();
        HANDLE File=CreateFileW(Path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        std::cout<<QJsonDocument(QJsonObject{{"ready",File!=INVALID_HANDLE_VALUE}}).toJson(QJsonDocument::Compact).constData()<<std::endl;
        std::string Command;std::getline(std::cin,Command);if(File!=INVALID_HANDLE_VALUE)CloseHandle(File);return 0;
    }
    QApplication App(Argc,Argv);
    for(const auto* Font : {"C:/Windows/Fonts/malgun.ttf","C:/Windows/Fonts/malgunbd.ttf","C:/Windows/Fonts/segoeui.ttf","C:/Windows/Fonts/consola.ttf"})QFontDatabase::addApplicationFont(QString::fromLatin1(Font));
    auto& Theme=fm::style::ThemeManager::instance();Theme.setDesign(fm::style::Design::Watercolor);Theme.install(App);
    App.setFont(QFont("Malgun Gothic",11));FileLockUiTests Tests;return QTest::qExec(&Tests,Argc,Argv);
}
#include "tstFileLockUi.moc"
