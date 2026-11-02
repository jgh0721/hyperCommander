#pragma once
#include "UniqueLibs/ProductDefaults_Edit.hpp"
#include <QAction>
#include <QHash>
#include <QJsonObject>
#include <QKeySequence>
#include <QMap>
#include <QObject>
namespace hc::document {
using DocumentShortcut = hc::defaults::edit::Command;
struct DocumentShortcutScope {
    QString id_, label_, description_;
    int depth_ = 0;
};
struct DocumentShortcutIssue {
    QString id_, other_, message_;
};
class DocumentShortcutRegistry {
public:
    // Keypad navigation keys and both Enter keys are one binding.
    static QKeyCombination NormalizeKey(QKeyCombination Key)
    {
        auto Code=Key.key();auto Mods=Key.keyboardModifiers();
        if(Code==Qt::Key_Return)Code=Qt::Key_Enter;
        if(Code==Qt::Key_PageUp || Code==Qt::Key_PageDown || Code==Qt::Key_Enter)Mods&=~Qt::KeypadModifier;
        return QKeyCombination(Mods,Code);
    }
    static QKeySequence NormalizeNavigationKeys(const QKeySequence& Sequence)
    {
        QKeyCombination Keys[4]{QKeyCombination::fromCombined(0),QKeyCombination::fromCombined(0),QKeyCombination::fromCombined(0),QKeyCombination::fromCombined(0)};
        for(int I=0;I<Sequence.count();++I)Keys[I]=NormalizeKey(Sequence[I]);
        return QKeySequence(Keys[0],Keys[1],Keys[2],Keys[3]);
    }
    // Display order of the settings page. A child scope is active only together with its parent.
    static QList<DocumentShortcutScope> Scopes()
    {
        return {
            {"common", QObject::tr("공통"), QObject::tr("문서 창 어디에서나 동작합니다.")},
            {"edit", QObject::tr("편집 공통"), QObject::tr("문서의 입력 영역에서 동작합니다.")},
            {"text", QObject::tr("일반 텍스트 편집기"), QObject::tr("텍스트 문서와 EPUB 소스 편집기에서 동작합니다.")},
            {"markdown", QObject::tr("마크다운"), QObject::tr("마크다운 소스와 미리보기에서 동작합니다.")},
            {"epub", QObject::tr("EPUB 뷰어"), QObject::tr("EPUB 읽기 화면에서 동작합니다.")},
            {"epub-edit", QObject::tr("EPUB 편집"), QObject::tr("EPUB 편집 모드의 편집기 안 어디에서나 동작합니다. 소스 편집기에서는 텍스트 편집기 단축키와 함께 동작합니다.")},
            {"epub-edit.tree", QObject::tr("책 구조"), QObject::tr("EPUB 편집의 책 구조 트리에 포커스가 있을 때 동작합니다."), 1},
            {"epub-edit.source", QObject::tr("소스 편집기"), QObject::tr("EPUB 소스 편집기에 포커스가 있을 때 동작합니다."), 1},
            {"image", QObject::tr("이미지 뷰어"), QObject::tr("이미지 문서와 만화·웹툰 보기, 이미지 전체 화면에서 동작합니다.")},
            {"image.comic", QObject::tr("코믹"), QObject::tr("코믹·웹툰 읽기 화면에서 동작합니다."), 1},
            {"pdf", QObject::tr("PDF 뷰어"), QObject::tr("PDF 문서에서 동작합니다.")}};
    }
    // Scopes active together for one focus position. Validation and routing share this table.
    static QList<QStringList> FocusContexts()
    {
        return {{"common","edit","text","markdown"},{"common","edit","markdown"},{"common","edit","text"},{"common","edit","epub"},{"common","edit","epub-edit","epub-edit.tree"},
            {"common","edit","text","epub-edit","epub-edit.source"},{"common","edit","epub-edit"},{"common","edit","image","image.comic"},{"common","edit","pdf"}};
    }
    static bool CoActive(const QString& A, const QString& B)
    {
        for(const auto& Context:FocusContexts())if(Context.contains(A) && Context.contains(B))return true;
        return false;
    }
    static QStringList ConflictScopes(const QString& Scope)
    {
        QStringList Result;for(const auto& Item:Scopes())if(Item.id_!=Scope && CoActive(Scope,Item.id_))Result.append(Item.id_);
        return Result;
    }
    // Scopes reachable while a text input owns the keyboard keep its editing keys.
    static bool InputScope(const QString& Scope){return QStringList{"common","edit","text","markdown","epub-edit","epub-edit.source"}.contains(Scope);}
    static QString ScopeLabel(const QString& Scope){for(const auto& Item:Scopes())if(Item.id_==Scope)return Item.label_;return Scope;}
    // Routing asks for scopes on every key press and refresh; ids and scopes never change.
    static QString ScopeOf(const QString& Id)
    {
        static const auto Index=[]{QHash<QString,QString> Result;for(const auto& Item:Commands())Result.insert(Item.id_,Item.scope_);return Result;}();
        return Index.value(Id);
    }
    static QList<DocumentShortcut> Commands()
    {
        return hc::defaults::edit::Commands();
    }

    static QJsonObject Bindings(const QJsonObject& Stored)
    {
        auto Result = Stored;
        const auto Aliases = hc::defaults::edit::CommandAliases();
        for (auto It = Aliases.begin(); It != Aliases.end(); ++It)
            if (!Result.contains(It.value()) && Result.contains(It.key())) Result[It.value()] = Result.value(It.key());
        for (const auto& Item : Commands()) if (!Result.contains(Item.id_)) Result[Item.id_] = Item.key_;
        return Result;
    }
    static QStringList Unknown(const QJsonObject& Stored)
    {
        QStringList Known{"file.open", "file.close", "view.capture", "text.new"}, Result;
        for (const auto& Item : Commands()) Known.append(Item.id_);
        for (auto It = Stored.begin(); It != Stored.end(); ++It) if (!Known.contains(It.key())) Result.append(It.key());
        return Result;
    }
    // Every problem with its command; conflicts name the other command so a page can show both.
    static QList<DocumentShortcutIssue> Issues(const QJsonObject& Values)
    {
        const auto Effective = Bindings(Values); QList<DocumentShortcutIssue> Result;
        struct Used { QString id_, scope_, label_; QKeySequence key_; };
        QList<Used> Seen;
        const auto Reserved = hc::defaults::edit::ReservedShortcuts();
        const auto Focus = hc::defaults::edit::InputFocusKeys();
        const auto Editing = hc::defaults::edit::EditingKeys();
        const auto Aliases = hc::defaults::edit::ShortcutAliases();
        const auto Conflict=[&](const DocumentShortcut& Item,const QKeySequence& Key,const QString& Label)->bool {
            for (const auto& Other : Seen) if (Overlaps(Key, Other.key_) && CoActive(Item.scope_, Other.scope_)) {
                Result.append({Item.id_, Other.id_, QObject::tr("단축키 충돌: %1 / %2 › %3 (%4)").arg(Label, ScopeLabel(Other.scope_), Other.label_, Key.toString(QKeySequence::NativeText))});
                return true;
            }
            return false;
        };
        for (const auto& Item : Commands()) {
            const auto Value = Effective[Item.id_];
            if (!Value.isString()) { Result.append({Item.id_, {}, QObject::tr("%1: 단축키는 문자열이어야 합니다.").arg(Item.label_)}); continue; }
            if (Value.toString().isEmpty()) continue;
            const auto Key = NormalizeNavigationKeys(QKeySequence::fromString(Value.toString(), QKeySequence::PortableText));
            if (Key.isEmpty() || Key.count() > 2) { Result.append({Item.id_, {}, QObject::tr("%1: 올바른 단축키를 입력하세요.").arg(Item.label_)}); continue; }
            bool Known = true; for (int Index = 0; Index < Key.count(); ++Index) Known &= Key[Index].key() != Qt::Key_unknown;
            if (!Known) { Result.append({Item.id_, {}, QObject::tr("%1: 인식할 수 없는 키입니다.").arg(Item.label_)}); continue; }
            const auto First = Key[0]; const auto Mods = First.keyboardModifiers() & ~Qt::KeypadModifier;
            const bool Modified = Mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
            const bool Input = InputScope(Item.scope_), Printable = Item.scope_ == "image" || Item.scope_ == "image.comic" || Item.scope_ == "pdf";
            const bool Tab = (First.key() == Qt::Key_Tab || First.key() == Qt::Key_Backtab)
                && (Mods == Qt::ControlModifier || Mods == (Qt::ControlModifier | Qt::ShiftModifier));
            // An input scope keeps its typing, movement and selection keys; only a modified Enter is free.
            const bool InputKey = Input && !Tab && (Editing.contains(First.key()) || First.key() == Qt::Key_Tab || First.key() == Qt::Key_Backtab
                || First.key() == Qt::Key_Escape || ((First.key() == Qt::Key_Enter) && !Modified));
            if (InputKey || (First.key() < Qt::Key_Escape && !Modified && !Printable)) {
                Result.append({Item.id_, {}, QObject::tr("%1: 문자 입력·이동·선택 기본키와 겹칩니다.").arg(Item.label_)}); continue;
            }
            if (!Input && Focus.contains(First.key())) { Result.append({Item.id_, {}, QObject::tr("%1: 포커스·입력 기본키는 유지해야 합니다.").arg(Item.label_)}); continue; }
            bool Rejected = false;
            if (Input) for (const auto& ReservedKey : Reserved) if (Overlaps(Key, QKeySequence(ReservedKey))) {
                Result.append({Item.id_, {}, QObject::tr("%1: 편집 기본키 또는 입력기 키 %2와 겹칩니다.").arg(Item.label_, ReservedKey)}); Rejected = true; break;
            }
            if (Rejected || Conflict(Item, Key, Item.label_)) continue;
            Seen.append({Item.id_, Item.scope_, Item.label_, Key});
            if (Key == QKeySequence(Item.key_) && Aliases.contains(Item.id_)) {
                const auto Alias = QKeySequence(Aliases[Item.id_]);
                if (!Conflict(Item, Alias, Item.label_ + QObject::tr(" (보조 키)"))) Seen.append({Item.id_, Item.scope_, Item.label_ + QObject::tr(" (보조 키)"), Alias});
            }
        }
        return Result;
    }
    static QString Validate(const QJsonObject& Values)
    {
        const auto Problems = Issues(Values);
        return Problems.isEmpty() ? QString{} : Problems.first().message_;
    }
    static bool Overlaps(const QKeySequence& A, const QKeySequence& B)
    { return !A.isEmpty() && !B.isEmpty() && (A.matches(B) != QKeySequence::NoMatch || B.matches(A) != QKeySequence::NoMatch); }
};
}
