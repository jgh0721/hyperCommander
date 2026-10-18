#include "HyperCommander/UIs/fileAttributesDialog.hpp"
#include "UniqueLibs/storageProviders.hpp"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QPushButton>
#include <fmwidgets/Button.h>
#include <fmstyle/ThemeManager.h>

class AttributeUiTests : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        auto& Theme = fm::style::ThemeManager::instance();
        Theme.setScheme(qEnvironmentVariable("HC_ATTRIBUTE_DARK").isEmpty() ? fm::style::ThemeManager::Scheme::Light : fm::style::ThemeManager::Scheme::Dark);
        Theme.install(*qobject_cast<QApplication*>(QApplication::instance()));
    }
    void previewApplyAndRecursiveInvalidation()
    {
        QTemporaryDir Root; QVERIFY(Root.isValid()); QDir(Root.path()).mkdir("folder");
        QFile File(Root.filePath("folder/item.txt")); QVERIFY(File.open(QIODevice::WriteOnly)); File.write("unchanged"); File.close();
        const auto Reader = hc::storage::MakeNativeReadProvider(hc::storage::LocatorKind::Local);
        const hc::storage::Locator At{hc::storage::LocatorKind::Local, QDir::toNativeSeparators(Root.filePath("folder")).toStdString()};
        const auto Entry = Reader->Resolve(At, {}).get(); QVERIFY(Entry.HasValue());
        hc::ui::FileAttributesDialog Dialog({Entry.Value()}, Reader, {}); Dialog.show();
        auto* Value = Dialog.findChild<QComboBox*>("hc.attributes.value1"); QVERIFY(Value);
        auto* Recursive = Dialog.findChild<QCheckBox*>("hc.attributes.recursive"); QVERIFY(Recursive);
        QTRY_VERIFY_WITH_TIMEOUT(Recursive->isEnabled(), 5000);
        QVERIFY(!Dialog.AcceptButton()->isEnabled()); Value->setCurrentIndex(1);
        QTRY_VERIFY(Dialog.AcceptButton()->isEnabled());
        Recursive->setChecked(true); QCOMPARE(Dialog.AcceptButton()->text(), QString::fromUtf8("대상 확인"));
        QTest::mouseClick(Dialog.AcceptButton(), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(Dialog.AcceptButton()->text(), QString::fromUtf8("적용"), 5000);
        auto* Details = Dialog.findChild<QPlainTextEdit*>("hc.attributes.details"); QVERIFY(Details->toPlainText().contains("item.txt"));
        auto* TargetPath = Dialog.findChild<QLabel*>("hc.attributes.targetPath"); QVERIFY(TargetPath);
        QCOMPARE(TargetPath->text(), QDir::toNativeSeparators(Root.filePath("folder")));
        QVERIFY(TargetPath->mapTo(&Dialog, QPoint()).y() < Recursive->mapTo(&Dialog, QPoint()).y());
        QVERIFY(!Details->isVisible());
        auto* ShowDetails = Dialog.findChild<QPushButton*>("hc.attributes.showDetails"); QVERIFY(ShowDetails);
        QTest::mouseClick(ShowDetails, Qt::LeftButton); QVERIFY(Details->isVisible());
        QTest::mouseClick(ShowDetails, Qt::LeftButton); QVERIFY(!Details->isVisible());
        const auto Shot = qEnvironmentVariable("HC_ATTRIBUTE_SHOT"); if (!Shot.isEmpty()) { QTest::qWait(100); QVERIFY(Dialog.grab().save(Shot)); }
        QTest::mouseClick(Dialog.AcceptButton(), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(Dialog.AcceptButton()->text(), QString::fromUtf8("닫기"), 5000);
        QVERIFY(Dialog.Changed());
        QVERIFY(Details->isVisible());
        QVERIFY(GetFileAttributesW(Root.filePath("folder/item.txt").toStdWString().c_str()) & FILE_ATTRIBUTE_HIDDEN);
        QVERIFY(File.open(QIODevice::ReadOnly)); QCOMPARE(File.readAll(), QByteArray("unchanged")); File.close();
        SetFileAttributesW(Root.filePath("folder/item.txt").toStdWString().c_str(), FILE_ATTRIBUTE_NORMAL);
        SetFileAttributesW(Root.filePath("folder").toStdWString().c_str(), FILE_ATTRIBUTE_DIRECTORY);
    }
};
QTEST_MAIN(AttributeUiTests)
#include "tstFileAttributesUi.moc"
