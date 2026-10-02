#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace hc::columns
{
    struct TemplatePart { QString text_; bool field_ = false; };
    struct CompiledTemplate
    {
        QVector<TemplatePart> parts_;
        QStringList dependencies_;
        QString error_;
        int errorOffset_ = -1;
        bool Valid() const { return errorOffset_ < 0; }
    };
    // No evaluation language: literals, $$ and ${provider.field} only.
    inline CompiledTemplate CompileTemplate(const QString& Source)
    {
        CompiledTemplate Result;
        const auto Fail = [&](int Offset, const QString& Message) { Result.errorOffset_ = Offset; Result.error_ = Message; return Result; };
        if (Source.size() > 4096) return Fail(4096, QStringLiteral("템플릿은 4096자 이하여야 합니다."));
        QString Literal;
        for (int Index = 0; Index < Source.size();) {
            if (Source.mid(Index, 2) == "$$") { Literal += '$'; Index += 2; continue; }
            if (Source.mid(Index, 2) != "${") { Literal += Source[Index++]; continue; }
            if (!Literal.isEmpty()) { Result.parts_.append({Literal}); Literal.clear(); }
            const auto End = Source.indexOf('}', Index + 2);
            if (End < 0) return Fail(Index, QStringLiteral("필드 참조의 닫는 괄호 }가 필요합니다."));
            const auto Field = Source.mid(Index + 2, End - Index - 2);
            if (Field.isEmpty() || !Field.contains('.')) return Fail(Index, QStringLiteral("필드 식별자가 필요합니다."));
            for (const auto Ch : Field) if (!Ch.isLetterOrNumber() && Ch != '.' && Ch != '_' && Ch != '-')
                return Fail(Index, QStringLiteral("필드 참조에 사용할 수 없는 문자입니다."));
            Result.parts_.append({Field, true});
            if (!Result.dependencies_.contains(Field)) Result.dependencies_.append(Field);
            if (Result.dependencies_.size() > 32) return Fail(Index, QStringLiteral("참조 필드는 32개 이하여야 합니다."));
            Index = int(End) + 1;
        }
        if (!Literal.isEmpty()) Result.parts_.append({Literal});
        return Result;
    }
}
