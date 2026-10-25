#pragma once
#include <QAction>
#include <QJsonObject>
#include <QKeySequence>
#include <QMap>
#include <QObject>
namespace hc::document {
struct DocumentShortcut {
    QString id_, label_, key_, scope_;
};
class DocumentShortcutRegistry {
public:
    static QList<DocumentShortcut> Commands()
    {
        return {
            {"file.new", QObject::tr("새 문서"), "Ctrl+N", "common"},
            {"file.openWorkspace", QObject::tr("문서 열기"), "Ctrl+O", "common"},
            {"file.save", QObject::tr("저장"), "Ctrl+S", "common"},
            {"file.saveAs", QObject::tr("다른 이름으로 저장"), "Ctrl+Shift+S", "common"},
            {"file.reload", QObject::tr("다시 읽기"), "Ctrl+R", "common"},
            {"file.cancel", QObject::tr("작업 취소"), "Ctrl+.", "common"},
            {"file.print", QObject::tr("인쇄"), "Ctrl+P", "common"},
            {"file.printPreview", QObject::tr("인쇄 미리보기"), "", "common"},
            {"capture.screen", QObject::tr("화면 캡처"), "Alt+Shift+S", "common"},
            {"tab.close", QObject::tr("현재 탭 닫기"), "Ctrl+W", "common"},
            {"tab.next", QObject::tr("다음 탭"), "Ctrl+Tab", "common"},
            {"tab.previous", QObject::tr("이전 탭"), "Ctrl+Shift+Tab", "common"},
            {"app.settings", QObject::tr("설정"), "Ctrl+I", "common"},
            {"view.displayOptions", QObject::tr("표시 옵션"), "Ctrl+Shift+M", "common"},
            {"edit.undo", QObject::tr("실행 취소"), "Ctrl+Z", "edit"},
            {"edit.redo", QObject::tr("다시 실행"), "Ctrl+Y", "edit"},
            {"edit.cut", QObject::tr("잘라내기"), "Ctrl+X", "edit"},
            {"edit.copy", QObject::tr("복사"), "Ctrl+C", "edit"},
            {"edit.paste", QObject::tr("붙여넣기"), "Ctrl+V", "edit"},
            {"edit.selectAll", QObject::tr("전체 선택"), "Ctrl+A", "edit"},
            {"text.find", QObject::tr("찾기"), "Ctrl+F", "text"},
            {"text.replace", QObject::tr("바꾸기"), "Ctrl+H", "text"},
            {"text.findNext", QObject::tr("다음 찾기"), "F3", "text"},
            {"text.findPrev", QObject::tr("이전 찾기"), "Shift+F3", "text"},
            {"text.goToLine", QObject::tr("줄 이동"), "Ctrl+G", "text"},
            {"text.wordWrap", QObject::tr("자동 줄바꿈 전환"), "Alt+Z", "text"},
            {"text.foldAll", QObject::tr("전체 접기"), "Ctrl+Shift+-", "text"},
            {"text.unfoldAll", QObject::tr("전체 펼치기"), "Ctrl+Shift++", "text"},
            {"epub.next", QObject::tr("EPUB 다음 화면"), "PgDown", "epub"},
            {"epub.previous", QObject::tr("EPUB 이전 화면"), "PgUp", "epub"},
            {"epub.next-chapter", QObject::tr("EPUB 다음 장"), "Ctrl+Alt+Right", "epub"},
            {"epub.previous-chapter", QObject::tr("EPUB 이전 장"), "Ctrl+Alt+Left", "epub"},
            {"epub.back", QObject::tr("EPUB 이전 위치"), "Alt+Left", "epub"},
            {"epub.focus-toc", QObject::tr("EPUB 목차"), "Ctrl+Alt+T", "epub"},
            {"epub.focus-settings", QObject::tr("EPUB 읽기 설정"), "Ctrl+Shift+E", "epub"},
            {"epub.focus-search", QObject::tr("EPUB 검색"), "Ctrl+F", "epub"},
            {"epub.bookmark-add", QObject::tr("EPUB 책갈피 추가"), "Ctrl+D", "epub"},
            {"epub.cycle-focus", QObject::tr("EPUB 영역 포커스 이동"), "F6", "epub"}
        };
    }
    static QJsonObject Bindings(const QJsonObject& Stored)
    {
        auto Result = Stored;
        const QMap<QString, QString> Aliases{{"file.open", "file.openWorkspace"}, {"file.close", "tab.close"},
            {"view.capture", "capture.screen"}, {"text.new", "file.new"}};
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
    static QString Validate(const QJsonObject& Values)
    {
        const auto Effective = Bindings(Values); QMap<QString, QKeySequence> Used;
        const QStringList Reserved{"Ctrl+Space", "Ctrl+Shift+Space", "Ctrl+L", "Ctrl+Shift+L", "Ctrl+T", "Ctrl+Shift+T",
            "Ctrl+D", "Ctrl+U", "Ctrl+Shift+U", "Ctrl+[", "Ctrl+Shift+[", "Ctrl+]", "Ctrl+Shift+]",
            "Ctrl+/", "Ctrl+Shift+/", "Ctrl+\\", "Ctrl+Shift+\\", "Ctrl++", "Ctrl+-", "Ctrl+0"};
        for (const auto& Item : Commands()) {
            const auto Value = Effective[Item.id_];
            if (!Value.isString()) return QObject::tr("%1: 단축키는 문자열이어야 합니다.").arg(Item.label_);
            if (Value.toString().isEmpty()) continue;
            const auto Key = QKeySequence::fromString(Value.toString(), QKeySequence::PortableText);
            if (Key.isEmpty() || Key.count() > 2) return QObject::tr("%1: 올바른 단축키를 입력하세요.").arg(Item.label_);
            for (int Index = 0; Index < Key.count(); ++Index)
                if (Key[Index].key() == Qt::Key_unknown) return QObject::tr("%1: 인식할 수 없는 키입니다.").arg(Item.label_);
            const auto First = Key[0]; const auto Mods = First.keyboardModifiers();
            const bool Tab = (First.key() == Qt::Key_Tab || First.key() == Qt::Key_Backtab)
                && (Mods == Qt::ControlModifier || Mods == (Qt::ControlModifier | Qt::ShiftModifier));
            const bool Reader = Item.scope_ == "epub";
            if ((!Tab && QList<Qt::Key>{Qt::Key_Tab, Qt::Key_Backtab, Qt::Key_Return, Qt::Key_Enter, Qt::Key_Escape,
                Qt::Key_Delete, Qt::Key_Backspace, Qt::Key_Insert, Qt::Key_Left, Qt::Key_Right, Qt::Key_Up,
                Qt::Key_Down, Qt::Key_Home, Qt::Key_End, Qt::Key_PageUp, Qt::Key_PageDown}.contains(First.key()) && !Reader)
                || (First.key() < Qt::Key_Escape && !(Mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))))
                return QObject::tr("%1: 문자 입력·이동·선택 기본키와 겹칩니다.").arg(Item.label_);
            if(Reader && QList<Qt::Key>{Qt::Key_Tab,Qt::Key_Backtab,Qt::Key_Return,Qt::Key_Enter,Qt::Key_Escape}.contains(First.key()))
                return QObject::tr("%1: 포커스·입력 기본키는 유지해야 합니다.").arg(Item.label_);
            for (const auto& ReservedKey : Reserved) if (!Reader && Overlaps(Key, QKeySequence(ReservedKey)))
                return QObject::tr("%1: 편집 기본키 또는 입력기 키 %2와 겹칩니다.").arg(Item.label_, ReservedKey);
            for (auto It = Used.begin(); It != Used.end(); ++It) if (Overlaps(Key, It.value()) && !(Reader && It.key().startsWith("text:") || !Reader && Item.scope_=="text" && It.key().startsWith("epub:")))
                return QObject::tr("단축키 충돌: %1 / %2 (%3)").arg(Item.label_, It.key(), Key.toString(QKeySequence::NativeText));
            Used[Item.scope_+":"+Item.label_] = Key;
            const QMap<QString, QString> Aliases{{"edit.redo", "Ctrl+Shift+Z"}, {"edit.copy", "Ctrl+Insert"}, {"edit.cut", "Shift+Delete"}, {"edit.paste", "Shift+Insert"}};
            if (Key == QKeySequence(Item.key_) && Aliases.contains(Item.id_)) {
                const auto Alias = QKeySequence(Aliases[Item.id_]);
                for (auto It = Used.begin(); It != Used.end(); ++It) if (Overlaps(Alias, It.value()))
                    return QObject::tr("단축키 충돌: %1 / %2").arg(Item.label_, It.key());
                Used[Item.label_ + QObject::tr(" (보조 키)")] = Alias;
            }
        }
        return {};
    }
    static bool Overlaps(const QKeySequence& A, const QKeySequence& B)
    { return !A.isEmpty() && !B.isEmpty() && (A.matches(B) != QKeySequence::NoMatch || B.matches(A) != QKeySequence::NoMatch); }
};
}
