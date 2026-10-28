#include "stdafx.h"
#include "compareLanguage.hpp"
#include <QFileInfo>
#include <array>

namespace hc::compare
{
    namespace
    {
        // C#, Java and JavaScript keep the earlier C++-lexer approximation; they
        // have no menu entry and no structure analysis.
        const std::array<CompareLanguage, 9> languages{{
            {"none", "일반 텍스트", "null", false}, {"c", "C", "cpp", true}, {"cpp", "C++", "cpp", true},
            {"python", "Python", "python", true}, {"json", "JSON", "json", false}, {"xml", "XML", "xml", false},
            {"csharp", "C#", "cpp", false}, {"java", "Java", "cpp", false}, {"javascript", "JavaScript", "cpp", false}}};
        QString suffix(const QString& Path)
        {
            // Backup and editor temporaries keep the language of the name they extend.
            static const QStringList backups{"orig", "bak", "rej", "tmp"};
            auto name = QFileInfo(Path).fileName().toLower();
            for (;;)
            {
                while (name.endsWith('~')) name.chop(1);
                const auto dot = name.lastIndexOf('.');
                if (dot <= 0) return {};
                auto value = name.mid(dot + 1);
                if (!backups.contains(value)) return value;
                name.truncate(dot);
            }
        }
    }
    const QStringList& CompareSyntaxChoices()
    {
        static const QStringList choices{"c", "cpp", "python", "json", "xml", "none"};
        return choices;
    }
    const CompareLanguage& CompareLanguageById(const QString& Id)
    {
        for (const auto& language : languages) if (language.id_ == Id) return language;
        return languages[0];
    }
    const CompareLanguage& DetectCompareLanguage(const QString& Path, QByteArrayView Head)
    {
        static const QStringList cpp{"cpp", "cc", "cxx", "c++", "h", "hh", "hpp", "hxx", "h++", "inl", "ipp", "tcc", "ixx", "cppm"};
        const auto ext = suffix(Path);
        if (ext == "c") return languages[1];
        // .h is C++: C keywords are a subset, so C headers lose little.
        if (cpp.contains(ext)) return languages[2];
        if (ext == "py" || ext == "pyw" || ext == "pyi") return languages[3];
        if (ext == "json") return languages[4];
        if (ext == "xml" || ext == "html") return languages[5];
        if (ext == "cs") return languages[6];
        if (ext == "java") return languages[7];
        if (ext == "js" || ext == "ts") return languages[8];
        if (ext.isEmpty() && Head.size() > 2 && Head[0] == '#' && Head[1] == '!')
        {
            const QByteArray first(Head.data(), std::min<qsizetype>(Head.size(), 256));
            const auto end = first.indexOf('\n');
            if ((end < 0 ? first : first.left(end)).contains("python")) return languages[3];
        }
        return languages[0];
    }
    const CompareLanguage& CompareLanguageResolver::operator()(const QString& Path, QByteArrayView Head) const
    {
        if (override_ != "auto") return CompareLanguageById(override_);
        const auto& detected = DetectCompareLanguage(Path, Head);
        return detected.id_ == "none" && fallback_ ? *fallback_ : detected;
    }
    CompareLanguageResolver ResolveCompareLanguages(const QStringList& Paths, const QList<QByteArray>& Heads, const QString& Override)
    {
        CompareLanguageResolver result; result.override_ = Override.isEmpty() ? QString("auto") : Override;
        bool ambiguous = false;
        for (qsizetype i = 0; i < Paths.size(); ++i)
        {
            const auto& language = DetectCompareLanguage(Paths[i], i < Heads.size() ? QByteArrayView(Heads[i]) : QByteArrayView{});
            if (language.id_ == "none") continue;
            if (result.fallback_ && result.fallback_ != &language) ambiguous = true;
            result.fallback_ = &language;
        }
        if (ambiguous) result.fallback_ = nullptr;
        return result;
    }
}
