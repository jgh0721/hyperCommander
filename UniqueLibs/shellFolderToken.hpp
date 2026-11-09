#pragma once

/// 모듈
/// UIs
/// 기타 헤더파일
/// 고유 라이브러리
/// 공통 라이브러리
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDir>
#include <QRegularExpression>
#include <QUuid>
#include <QVector>

namespace hc::shell
{
    struct FolderToken
    {
        enum class Kind { None, KnownFolder, KnownFolderName, Namespace, Invalid };
        Kind kind_ = Kind::None;
        QString token_;
        QUuid id_;
        QString name_, rest_;
    };
    inline FolderToken ParseFolderToken(const QString& Pattern)
    {
        FolderToken Result;
        auto Text = Pattern.trimmed();
        Text.replace('\\', '/');
        bool Shell = Text.startsWith("shell:", Qt::CaseInsensitive);
        if (Shell) Text.remove(0, 6);
        if (Text.startsWith("::")) {
            Result.kind_ = FolderToken::Kind::Namespace;
            Text.remove(0, 2);
        } else if (Text.startsWith('{')) Result.kind_ = FolderToken::Kind::KnownFolder;
        else if (Shell) {
            const auto End = Text.indexOf('/');
            Result.name_ = End < 0 ? Text : Text.left(End);
            Result.rest_ = End < 0 ? QString{} : Text.mid(End);
            Result.kind_ = Result.name_.isEmpty() ? FolderToken::Kind::Invalid : FolderToken::Kind::KnownFolderName;
            Result.token_ = "shell:" + Result.name_.toCaseFolded();
            return Result;
        } else return Result;
        static const QRegularExpression Guid(R"(\A\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\})");
        if (!Guid.match(Text).hasMatch() || (Text.size() > 38 && Text[38] != '/')) {
            Result.kind_ = FolderToken::Kind::Invalid;
            return Result;
        }
        Result.id_ = QUuid(Text.left(38));
        Result.token_ = (Result.kind_ == FolderToken::Kind::Namespace ? "::" : "") + Result.id_.toString();
        Result.rest_ = Text.mid(38);
        return Result;
    }
    struct ResolvedFolder { QString path_, label_, error_; };
    ResolvedFolder ResolveFolderToken(const FolderToken& Token);
    QString ResolveKnownFolderPath(const QUuid& Id);
    struct KnownFolderChoice { QString id_, label_; QUuid folder_; };
    QVector<KnownFolderChoice> KnownFolderChoices();
}
