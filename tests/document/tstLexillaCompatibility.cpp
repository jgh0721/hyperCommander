#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <ScintillaEditBase.h>
#include <Scintilla.h>
#include <SciLexer.h>
#include <ILexer.h>
#include <Lexilla.h>

class LexillaCompatibilityTest : public QObject
{
    Q_OBJECT
private slots:
    void registeredLanguages()
    {
        QFile File(QStringLiteral(HC_LEXER_REGISTRY));
        QVERIFY(File.open(QIODevice::ReadOnly));
        const auto Languages = QJsonDocument::fromJson(File.readAll()).object()["languages"].toArray();
        QVERIFY(!Languages.isEmpty());
        for (const auto& Value : Languages) {
            const auto Key = Value.toObject()["lexerKey"].toString().toUtf8();
            if (Key == "none") continue;
            auto* Lexer = CreateLexer(Key.constData());
            QVERIFY2(Lexer, Key.constData());
            Lexer->Release();
        }
    }
    void highlightingAndFolding()
    {
        ScintillaEditBase Editor;
        auto* Lexer = CreateLexer("cpp");
        QVERIFY(Lexer);
        Editor.send(SCI_SETILEXER, 0, reinterpret_cast<sptr_t>(Lexer));
        Editor.send(SCI_SETCODEPAGE, SC_CP_UTF8);
        Editor.sends(SCI_SETKEYWORDS, 0, "int return");
        Editor.send(SCI_SETPROPERTY, reinterpret_cast<uptr_t>("fold"), reinterpret_cast<sptr_t>("1"));
        const QByteArray Source("int main() {\n  // comment\n  return 42;\n}\n");
        Editor.sends(SCI_SETTEXT, 0, Source.constData());
        Editor.send(SCI_COLOURISE, 0, -1);
        QCOMPARE(Editor.send(SCI_GETLEXER), sptr_t(SCLEX_CPP));
        QCOMPARE(Editor.send(SCI_GETSTYLEAT, 0), sptr_t(SCE_C_WORD));
        QCOMPARE(Editor.send(SCI_GETSTYLEAT, Source.indexOf("comment")), sptr_t(SCE_C_COMMENTLINE));
        QCOMPARE(Editor.send(SCI_GETSTYLEAT, Source.indexOf("42")), sptr_t(SCE_C_NUMBER));
        QVERIFY(Editor.send(SCI_GETFOLDLEVEL, 0) & SC_FOLDLEVELHEADERFLAG);
        Editor.send(SCI_FOLDLINE, 0, SC_FOLDACTION_CONTRACT);
        QCOMPARE(Editor.send(SCI_GETLINEVISIBLE, 1), sptr_t(0));
        Editor.send(SCI_FOLDLINE, 0, SC_FOLDACTION_EXPAND);
        QCOMPARE(Editor.send(SCI_GETLINEVISIBLE, 1), sptr_t(1));
    }
    void yamlBlankLineInBlock()
    {
        ScintillaEditBase Editor;
        auto* Lexer = CreateLexer("yaml");
        QVERIFY(Lexer);
        Editor.send(SCI_SETILEXER, 0, reinterpret_cast<sptr_t>(Lexer));
        const QByteArray Source("key: |\n  first\n\n  second\nnext: value\n");
        Editor.sends(SCI_SETTEXT, 0, Source.constData());
        Editor.send(SCI_COLOURISE, 0, -1);
        QCOMPARE(Editor.send(SCI_GETSTYLEAT, Source.indexOf("second")), sptr_t(SCE_YAML_TEXT));
        QVERIFY(Editor.send(SCI_GETSTYLEAT, Source.indexOf("next")) != SCE_YAML_TEXT);
    }
};
QTEST_MAIN(LexillaCompatibilityTest)
#include "tstLexillaCompatibility.moc"
