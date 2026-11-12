#include "rstTestRunner.hpp"
#include "sphinxPreviewInputs.hpp"
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

namespace {
void put(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(data), data.size());
}
QString fingerprint(const QTemporaryDir& directory)
{
    QJsonArray files;
    const auto add = [&](const QString& path) {
        const QFileInfo info(path);
        files.append(QJsonObject{{"p", path}, {"s", info.size()},
            {"m", info.lastModified().toMSecsSinceEpoch() * 1000000}});
    };
    add(directory.filePath("conf.py"));
    QJsonArray roots;
    for (const auto& name : {"_templates", "theme", "missing"}) {
        const QString path = directory.filePath(name);
        QJsonArray names;
        QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const auto file = it.next();
            names.append(QDir(path).relativeFilePath(file));
            add(file);
        }
        roots.append(QJsonObject{{"p", path}, {"files", names}});
    }
    const auto path = directory.filePath("inputs.json");
    put(path, QJsonDocument(QJsonObject{{"schema", 2}, {"templatesComplete", true},
        {"templateDirs", roots}, {"files", files}}).toJson());
    return path;
}
}

class TestPreviewInputs : public QObject
{
    Q_OBJECT
private slots:
    void templateChangesInvalidate_data();
    void templateChangesInvalidate();
    void legacyAndIncompleteFingerprintsAreRejected();
    void staleFingerprintsArePruned();
};

void TestPreviewInputs::templateChangesInvalidate_data()
{
    QTest::addColumn<int>("change");
    QTest::newRow("edit") << 0;
    QTest::newRow("nested-add") << 1;
    QTest::newRow("delete") << 2;
    QTest::newRow("rename") << 3;
    QTest::newRow("inherited-theme") << 4;
    QTest::newRow("missing-override-created") << 5;
}

void TestPreviewInputs::templateChangesInvalidate()
{
    QFETCH(int, change);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    put(directory.filePath("conf.py"), "templates_path=['_templates']");
    put(directory.filePath("_templates/layout.html"), "original");
    put(directory.filePath("_templates/nested/existing.html"), "existing");
    put(directory.filePath("theme/layout.html"), "theme");
    const auto inputs = fingerprint(directory);
    QVERIFY(!hc::rst::previewInputsChanged(inputs));
    if (change == 0) put(directory.filePath("_templates/layout.html"), "changed template contents");
    if (change == 1) put(directory.filePath("_templates/nested/new.html"), "new override");
    if (change == 2) QVERIFY(QFile::remove(directory.filePath("_templates/layout.html")));
    if (change == 3) QVERIFY(QFile::rename(directory.filePath("_templates/layout.html"), directory.filePath("_templates/renamed.html")));
    if (change == 4) put(directory.filePath("theme/layout.html"), "changed inherited theme");
    if (change == 5) put(directory.filePath("missing/layout.html"), "newly created root");
    QVERIFY(hc::rst::previewInputsChanged(inputs));
}

void TestPreviewInputs::legacyAndIncompleteFingerprintsAreRejected()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    put(directory.filePath("conf.py"), "");
    const auto inputs = fingerprint(directory);
    QFile file(inputs);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto json = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    json["schema"] = 1;
    put(inputs, QJsonDocument(json).toJson());
    QVERIFY(hc::rst::previewInputsChanged(inputs));
    json["schema"] = 2;
    json["templatesComplete"] = false;
    put(inputs, QJsonDocument(json).toJson());
    QVERIFY(hc::rst::previewInputsChanged(inputs));
}

void TestPreviewInputs::staleFingerprintsArePruned()
{
    // 지문은 프로젝트마다 하나씩, 프로젝트 폴더가 아니라 환경 폴더의 state 에 생긴다. 프로젝트를 지우거나
    // 옮겨도 따라 없어지지 않으므로, 없어진 프로젝트와 오래 쓰지 않은 프로젝트의 것을 걷어 낸다.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString state = root.filePath("state");
    QVERIFY(QDir().mkpath(state));
    QVERIFY(QDir().mkpath(root.filePath("alive/source")));
    const auto write = [&](const QString& name, const QJsonObject& json) {
        QFile file(state + "/" + name);
        return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(json).toJson()) > 0;
    };
    QVERIFY(write("preview-inputs-alive-11111111.json", {{"schema", 2}, {"sourceDir", root.filePath("alive/source")}}));
    QVERIFY(write("preview-inputs-gone-22222222.json", {{"schema", 2}, {"sourceDir", root.filePath("gone/source")}}));
    QVERIFY(write("preview-inputs-old-33333333.json", {{"schema", 2}, {"sourceDir", root.filePath("alive/source")}}));
    QVERIFY(write("preview-inputs-broken-44444444.json", {}));
    // 이름이 다른 파일은 건드리지 않는다.
    QVERIFY(write("something-else.json", {{"sourceDir", root.filePath("gone")}}));
    {
        QFile old(state + "/preview-inputs-old-33333333.json");
        QVERIFY(old.open(QIODevice::ReadWrite));
        QVERIFY(old.setFileTime(QDateTime::currentDateTimeUtc().addDays(-200), QFileDevice::FileModificationTime));
    }

    QCOMPARE(hc::rst::prunePreviewInputs(state), 3);
    QVERIFY(QFileInfo::exists(state + "/preview-inputs-alive-11111111.json"));
    QVERIFY(!QFileInfo::exists(state + "/preview-inputs-gone-22222222.json"));
    QVERIFY(!QFileInfo::exists(state + "/preview-inputs-old-33333333.json"));
    QVERIFY(!QFileInfo::exists(state + "/preview-inputs-broken-44444444.json"));
    QVERIFY(QFileInfo::exists(state + "/something-else.json"));
    // 다시 돌려도 남은 것은 그대로다. 폴더가 없으면 아무 일도 없다.
    QCOMPARE(hc::rst::prunePreviewInputs(state), 0);
    QCOMPARE(hc::rst::prunePreviewInputs(root.filePath("missing")), 0);
    // 기간을 0 으로 주면 나이로는 지우지 않는다.
    QVERIFY(write("preview-inputs-old-55555555.json", {{"schema", 2}, {"sourceDir", root.filePath("alive/source")}}));
    {
        QFile old(state + "/preview-inputs-old-55555555.json");
        QVERIFY(old.open(QIODevice::ReadWrite));
        QVERIFY(old.setFileTime(QDateTime::currentDateTimeUtc().addDays(-200), QFileDevice::FileModificationTime));
    }
    QCOMPARE(hc::rst::prunePreviewInputs(state, 0), 0);
}

HC_RST_REGISTER_TEST(TestPreviewInputs);
#include "tstPreviewInputs.moc"
