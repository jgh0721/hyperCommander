#include "documentHandlerMenu.hpp"
#include <QMenu>
#include <QCursor>
#include <QTimer>
namespace hc::document {
namespace {
void Add(QMenu& Menu, const DocumentHandlerCatalog& Catalog, const std::vector<std::string>& Ids,
    const std::vector<std::size_t>& Indexes, const std::string& Default, std::map<QAction*, DocumentHandlerChoice>& Choices) {
    for (const auto& Id : Ids) {
        const auto& Handler = Catalog.targets_.at(Id).descriptor_;
        auto* Action = Menu.addAction(QString::fromStdString(Handler.name_) + (Handler.kind_ == HandlerKind::Plugin ? QObject::tr(" · 플러그인") : QObject::tr(" · 실행 파일")));
        Action->setObjectName("hc.document.handler." + QString::fromStdString(Id));
        if (Id == Default) { auto Font = Action->font(); Font.setBold(true); Action->setFont(Font); Menu.setDefaultAction(Action); }
        Choices.emplace(Action, DocumentHandlerChoice{Id, Indexes});
    }
}
}
DocumentHandlerChoice DocumentHandlerMenu::Choose(QWidget* Parent, const DocumentHandlerCatalog& Catalog,
    DocumentIntent Intent, const HandlerSelection& Selection, const HandlerResolution& Resolution) {
    QMenu Menu(Parent); Menu.setObjectName("hc.document.handlerMenu");
    std::map<QAction*, DocumentHandlerChoice> Choices;
    const auto Default = Intent == DocumentIntent::View ? Catalog.defaultView_ : Catalog.defaultEdit_;
    std::vector<std::size_t> All; for (std::size_t I = 0; I < Selection.names_.size(); ++I) All.push_back(I);
    Add(Menu, Catalog, Resolution.candidates_, All, Default, Choices);
    for (const auto& [Extension, Indexes] : Resolution.groups_) {
        auto* Group = Menu.addMenu(QObject::tr("%1 · %2개 파일만 열기").arg(Extension.empty() ? QObject::tr("확장자 없음") : QString::fromStdString(Extension)).arg(Indexes.size()));
        HandlerSelection Subset; for (auto I : Indexes) {
            Subset.names_.push_back(Selection.names_[I]);
            Subset.textContent_.push_back(I < Selection.textContent_.size() && Selection.textContent_[I]);
        }
        const auto Resolved = DocumentHandlerResolver::Resolve(Catalog.registry_, Intent, Subset, Default);
        Add(*Group, Catalog, Resolved.candidates_, Indexes, Default, Choices);
        if (Resolved.candidates_.empty()) Group->addAction(QObject::tr("이 형식의 사용 가능한 프로그램이 없습니다"))->setEnabled(false);
    }
    if (Choices.empty() && Resolution.groups_.empty()) Menu.addAction(QObject::tr("사용 가능한 프로그램이 없습니다"))->setEnabled(false);
    Menu.addSeparator(); auto* Configure = Menu.addAction(QObject::tr("외부 편집기 설정…")); Configure->setObjectName("hc.document.configure"); Menu.addAction(QObject::tr("취소"));
    auto* Initial = Menu.defaultAction();
    if (!Initial) for (auto* Action : Menu.actions()) if (Action->isEnabled() && !Action->isSeparator()) { Initial = Action; break; }
    Menu.setActiveAction(Initial);
    QTimer::singleShot(0, &Menu, [&Menu, Initial] { Menu.setActiveAction(Initial); Menu.setFocus(Qt::PopupFocusReason); });
    auto* Selected = Menu.exec(QCursor::pos()); if (Selected == Configure) return {{}, {}, true}; const auto At = Choices.find(Selected);
    return At == Choices.end() ? DocumentHandlerChoice{} : At->second;
}
}
