#pragma once
#include <QAction>
#include <QHash>
#include <QJsonObject>
#include <QKeySequence>
#include <QMap>
#include <QObject>
namespace hc::document {
struct DocumentShortcut {
    QString id_, label_, key_, scope_;
};
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
            {"pdf", QObject::tr("PDF 뷰어"), QObject::tr("PDF 문서에서 동작합니다.")}};
    }
    // Scopes active together for one focus position. Validation and routing share this table.
    static QList<QStringList> FocusContexts()
    {
        return {{"common","edit","text","markdown"},{"common","edit","markdown"},{"common","edit","text"},{"common","edit","epub"},{"common","edit","epub-edit","epub-edit.tree"},
            {"common","edit","text","epub-edit","epub-edit.source"},{"common","edit","epub-edit"},{"common","edit","image"},{"common","edit","pdf"}};
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
        return {
            {"markdown.toggleSource",QObject::tr("소스·미리보기 전환"),"Ctrl+Alt+M","markdown"},
            {"markdown.preview",QObject::tr("미리보기"),{},"markdown"},
            {"markdown.source",QObject::tr("소스 보기·편집"),{},"markdown"},
            {"markdown.split",QObject::tr("나란히 보기"),{},"markdown"},
            {"markdown.toggleToc",QObject::tr("개요 표시 전환"),"Ctrl+Alt+O","markdown"},
            {"markdown.positionSync",QObject::tr("소스·본문 위치 동기화"),{},"markdown"},
            {"markdown.syncScroll",QObject::tr("소스·본문 스크롤 동기화"),{},"markdown"},
            {"markdown.refresh",QObject::tr("미리보기 갱신"),"Ctrl+Alt+R","markdown"},
            {"markdown.focusOtherPane",QObject::tr("소스·미리보기 포커스 이동"),"F6","markdown"},
            {"markdown.find",QObject::tr("마크다운 찾기"),{},"markdown"},
            {"markdown.zoomIn",QObject::tr("미리보기 확대"),{},"markdown"},
            {"markdown.zoomOut",QObject::tr("미리보기 축소"),{},"markdown"},
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
            {"epub.cycle-focus", QObject::tr("EPUB 영역 포커스 이동"), "F6", "epub"},
            {"epub.fullscreen", QObject::tr("EPUB 전체 화면"), "F11", "epub"},
            {"epubEdit.apply", QObject::tr("변경 반영"), "Ctrl+Enter", "epub-edit"},
            {"epubEdit.validate", QObject::tr("기본 검사"), "F7", "epub-edit"},
            {"epubEdit.epubcheck", QObject::tr("EPUBCheck 검사"), "Shift+F7", "epub-edit"},
            {"epubEdit.filter", QObject::tr("리소스 필터"), "Ctrl+E", "epub-edit"},
            {"epubEdit.previewRefresh", QObject::tr("미리보기 갱신"), "F5", "epub-edit"},
            {"epubEdit.scrollSync", QObject::tr("스크롤 동기화"), "", "epub-edit"},
            {"epubEdit.closeTab", QObject::tr("편집기 탭 닫기"), "Ctrl+F4", "epub-edit"},
            {"epubEdit.toc", QObject::tr("목차 편집…"), "", "epub-edit"},
            {"epubEdit.metadata", QObject::tr("서지정보 편집…"), "", "epub-edit"},
            {"epubEdit.toggleTree", QObject::tr("책 구조 패널 표시"), "", "epub-edit"},
            {"epubEdit.togglePreview", QObject::tr("미리보기 패널 표시"), "", "epub-edit"},
            {"epubEdit.toggleProblems", QObject::tr("문제 패널 표시"), "", "epub-edit"},
            {"epubEdit.orientation", QObject::tr("좌우 · 상하 배치 전환"), "", "epub-edit"},
            {"epubEdit.recover", QObject::tr("초안 복원"), "", "epub-edit"},
            {"epubTree.rename", QObject::tr("이름·경로 변경…"), "F2", "epub-edit.tree"},
            {"epubTree.delete", QObject::tr("삭제…"), "Del", "epub-edit.tree"},
            {"epubTree.moveUp", QObject::tr("읽기 순서 위로"), "Alt+Up", "epub-edit.tree"},
            {"epubTree.moveDown", QObject::tr("읽기 순서 아래로"), "Alt+Down", "epub-edit.tree"},
            {"epubTree.copyPath", QObject::tr("경로 복사"), "Ctrl+Shift+C", "epub-edit.tree"},
            {"epubSource.quickFix", QObject::tr("빠른 수정"), "Alt+Enter", "epub-edit.source"},
            {"epubSource.openLink", QObject::tr("링크된 리소스 열기"), "F12", "epub-edit.source"},
            {"epubSource.findAll", QObject::tr("모든 리소스에서 찾기"), "Ctrl+Shift+F", "epub-edit.source"},
            {"epubSource.locate", QObject::tr("미리보기에서 이 위치 보기"), "", "epub-edit.source"},
            {"epubSource.wrapEm", QObject::tr("태그로 감싸기 · em"), "", "epub-edit.source"},
            {"epubSource.wrapStrong", QObject::tr("태그로 감싸기 · strong"), "", "epub-edit.source"},
            {"epubSource.wrapSpan", QObject::tr("태그로 감싸기 · span"), "", "epub-edit.source"},
            {"epubSource.wrapLink", QObject::tr("태그로 감싸기 · a"), "", "epub-edit.source"},
            {"epubSource.wrapRuby", QObject::tr("태그로 감싸기 · ruby"), "", "epub-edit.source"},
            {"image.next", QObject::tr("다음 이미지 · 다음 쪽"), "Space", "image"},
            {"image.previous", QObject::tr("이전 이미지 · 이전 쪽"), "Backspace", "image"},
            {"image.scrollDown", QObject::tr("아래로 스크롤"), "Down", "image"},
            {"image.scrollUp", QObject::tr("위로 스크롤"), "Up", "image"},
            {"image.pageNext", QObject::tr("다음 화면"), "PgDown", "image"},
            {"image.pagePrevious", QObject::tr("이전 화면"), "PgUp", "image"},
            {"image.pageNextSingle", QObject::tr("한 쪽 앞으로"), "Shift+PgDown", "image"},
            {"image.pagePreviousSingle", QObject::tr("한 쪽 뒤로"), "Shift+PgUp", "image"},
            {"image.first", QObject::tr("첫 쪽"), "Home", "image"},
            {"image.last", QObject::tr("마지막 쪽"), "End", "image"},
            {"image.volumeNext", QObject::tr("다음 권"), "]", "image"},
            {"image.volumePrevious", QObject::tr("이전 권"), "[", "image"},
            {"image.right", QObject::tr("오른쪽 쪽"), "Right", "image"},
            {"image.left", QObject::tr("왼쪽 쪽"), "Left", "image"},
            {"pdf.pageNext", QObject::tr("다음 쪽"), "PgDown", "pdf"},
            {"pdf.pagePrevious", QObject::tr("이전 쪽"), "PgUp", "pdf"},
            {"pdf.first", QObject::tr("첫 쪽"), "Home", "pdf"},
            {"pdf.last", QObject::tr("마지막 쪽"), "End", "pdf"}
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
    // Every problem with its command; conflicts name the other command so a page can show both.
    static QList<DocumentShortcutIssue> Issues(const QJsonObject& Values)
    {
        const auto Effective = Bindings(Values); QList<DocumentShortcutIssue> Result;
        struct Used { QString id_, scope_, label_; QKeySequence key_; };
        QList<Used> Seen;
        const QStringList Reserved{"Ctrl+Space", "Ctrl+Shift+Space", "Ctrl+L", "Ctrl+Shift+L", "Ctrl+T", "Ctrl+Shift+T",
            "Ctrl+D", "Ctrl+U", "Ctrl+Shift+U", "Ctrl+[", "Ctrl+Shift+[", "Ctrl+]", "Ctrl+Shift+]",
            "Ctrl+/", "Ctrl+Shift+/", "Ctrl+\\", "Ctrl+Shift+\\", "Ctrl++", "Ctrl+-", "Ctrl+0"};
        const QList<Qt::Key> Focus{Qt::Key_Tab, Qt::Key_Backtab, Qt::Key_Return, Qt::Key_Enter, Qt::Key_Escape};
        const QList<Qt::Key> Editing{Qt::Key_Delete, Qt::Key_Backspace, Qt::Key_Insert, Qt::Key_Left, Qt::Key_Right, Qt::Key_Up,
            Qt::Key_Down, Qt::Key_Home, Qt::Key_End, Qt::Key_PageUp, Qt::Key_PageDown};
        const QMap<QString, QString> Aliases{{"edit.redo", "Ctrl+Shift+Z"}, {"edit.copy", "Ctrl+Insert"}, {"edit.cut", "Shift+Delete"}, {"edit.paste", "Shift+Insert"}};
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
            const bool Input = InputScope(Item.scope_), Printable = Item.scope_ == "image" || Item.scope_ == "pdf";
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
