#pragma once
#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>

namespace hc::compare
{
    // One table decides the language of a comparison input. Syntax styling,
    // large-text style caches and source structure analysis use the same id.
    struct CompareLanguage
    {
        QString id_, label_;
        QByteArray lexer_;          // Lexilla module name; "null" leaves text unstyled
        bool structure_ = false;    // tree-sitter function analysis is available
    };
    // Values accepted by -syntax and the view menu, without "auto".
    const QStringList& CompareSyntaxChoices();
    const CompareLanguage& CompareLanguageById(const QString& Id);
    const CompareLanguage& DetectCompareLanguage(const QString& Path, QByteArrayView Head = {});
    // An override other than "auto" wins. An undetected input inherits the only
    // language detected among the comparison inputs (merge RESULT, mergetool temporaries).
    struct CompareLanguageResolver
    {
        QString override_ = "auto";
        const CompareLanguage* fallback_ = nullptr;
        const CompareLanguage& operator()(const QString& Path, QByteArrayView Head = {}) const;
    };
    CompareLanguageResolver ResolveCompareLanguages(const QStringList& Paths, const QList<QByteArray>& Heads, const QString& Override);
}
