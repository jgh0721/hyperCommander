#pragma once

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QAction>
#include <QString>
#include <QVariant>
#include <initializer_list>

namespace hc::document
{
    // The document chrome (menus, toolbar rows, status fields, context menus) is chosen by the
    // active view kind. Items that do not apply to the kind are hidden; items that apply but are
    // temporarily unusable are only disabled.
    enum class DocumentViewKind : quint32
    {
        None = 1u << 0, Text = 1u << 1, MarkdownPreview = 1u << 2, MarkdownEdit = 1u << 3, Pdf = 1u << 4,
        Image = 1u << 5, Comic = 1u << 6, Webtoon = 1u << 7, EpubRead = 1u << 8, EpubEdit = 1u << 9, Hwp = 1u << 10,
        RstPreview = 1u << 11, RstEdit = 1u << 12,
    };

    namespace viewKinds
    {
        constexpr quint32 Of(DocumentViewKind Kind) { return static_cast<quint32>(Kind); }
        constexpr quint32 Of(std::initializer_list<DocumentViewKind> Kinds)
        {
            quint32 Mask = 0;
            for (const auto Kind : Kinds) Mask |= static_cast<quint32>(Kind);
            return Mask;
        }
        constexpr quint32 All = 0x1FFFu;
        constexpr quint32 Documents = All & ~Of(DocumentViewKind::None);
        constexpr quint32 TextEditing = Of({DocumentViewKind::Text, DocumentViewKind::MarkdownEdit, DocumentViewKind::RstEdit, DocumentViewKind::EpubEdit});
        // 소스와 미리보기를 함께 가진 문서( Markdown · reStructuredText ).
        constexpr quint32 Preview = Of({DocumentViewKind::MarkdownPreview, DocumentViewKind::RstPreview});
        constexpr quint32 Markup = Of({DocumentViewKind::MarkdownPreview, DocumentViewKind::MarkdownEdit, DocumentViewKind::RstPreview, DocumentViewKind::RstEdit});
        constexpr quint32 Reading = Of({DocumentViewKind::Comic, DocumentViewKind::Webtoon, DocumentViewKind::EpubRead});
        constexpr quint32 Epub = Of({DocumentViewKind::EpubRead, DocumentViewKind::EpubEdit});

        // Menu and toolbar actions carry their applicable kinds in this property.
        inline constexpr const char* Property = "documentKinds";
        inline void Set(QAction* Action, quint32 Mask) { if (Action) Action->setProperty(Property, QVariant::fromValue<quint32>(Mask)); }
        inline bool Has(const QAction* Action) { return Action && Action->property(Property).isValid(); }
        inline bool Applies(const QAction* Action, DocumentViewKind Kind)
        {
            return !Has(Action) || (Action->property(Property).value<quint32>() & Of(Kind)) != 0;
        }
        inline QString Name(DocumentViewKind Kind)
        {
            switch (Kind) {
            case DocumentViewKind::None: return "none"; case DocumentViewKind::Text: return "text";
            case DocumentViewKind::MarkdownPreview: return "markdownPreview"; case DocumentViewKind::MarkdownEdit: return "markdownEdit";
            case DocumentViewKind::Pdf: return "pdf"; case DocumentViewKind::Image: return "image"; case DocumentViewKind::Comic: return "comic";
            case DocumentViewKind::Webtoon: return "webtoon"; case DocumentViewKind::EpubRead: return "epubRead";
            case DocumentViewKind::EpubEdit: return "epubEdit"; case DocumentViewKind::Hwp: return "hwp";
            case DocumentViewKind::RstPreview: return "rstPreview"; case DocumentViewKind::RstEdit: return "rstEdit";
            }
            return {};
        }
    }
}
