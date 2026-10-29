#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "epubResourceTree.hpp"
#include "epubProcessView.hpp"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QJsonArray>
#include <QMenu>
#include <QScopeGuard>
#include <QTabBar>
#include <QTreeView>
#include <QTreeWidget>
#include <Scintilla.h>
namespace hc::document {
namespace {
// The default item of a menu is the double click / Enter action and is shown in bold.
QAction* emphasize(QAction* Item){auto Font=Item->font();Font.setBold(true);Item->setFont(Font);return Item;}
}
void EpubSourceEditor::showTreeMenu(const QPoint& Position) {
    const auto Index=tree_->indexAt(Position);const auto Path=Index.data(EpubResourceModel::PathRole).toString();QMenu Menu(this);
    // Commands act on the item under the pointer, not on the open resource.
    setProperty("epubActionTarget",Path);updateActions();const auto Clear=qScopeGuard([this]{setProperty("epubActionTarget",QVariant{});updateActions();});
    if(Path.isEmpty()) {
        for(const auto& Name:{"epubAddChapter","epubAddImage","epubImportResource","","epubMetadataAction","epubEditNavigation","","epubExpandAll","epubCollapseAll","epubChangedOnly","","epubResourceFilter"}){if(QString(Name).isEmpty())Menu.addSeparator();else Menu.addAction(action(Name));}
        Menu.exec(tree_->viewport()->mapToGlobal(Position));return;
    }
    const auto Media=Index.data(EpubResourceModel::MediaRole).toString();const bool Spine=Index.data(EpubResourceModel::SpineRole).toBool(),Image=Media.startsWith("image/"),Editable=Index.data(EpubResourceModel::EditableRole).toBool();
    auto* Open=emphasize(Menu.addAction(tr("열기"),this,[this,Path]{if(openResource(Path))focusSource();}));Open->setEnabled(Editable);
    if(Spine){Menu.addAction(action("epubPreviewChapter"));Menu.addSeparator();for(const auto& Name:{"epubRenameResource","epubDuplicateResource","epubRemoveResource","","epubChapterUp","epubChapterDown","epubAddChapter","epubAddChapterImage","","epubEditNavigation","","epubCopyPath","epubCopyRelativePath"}){if(QString(Name).isEmpty())Menu.addSeparator();else Menu.addAction(action(Name));}}
    else if(Image){for(const auto& Name:{"epubImagePropertiesAction","","epubReplaceImage","epubConvertImage","epubCoverImage","","epubInsertImage","epubExportResource","","epubRenameResource","epubRemoveResource","","epubCopyPath","epubCopyRelativePath"}){if(QString(Name).isEmpty())Menu.addSeparator();else Menu.addAction(action(Name));}}
    else {
        Menu.addSeparator();if(Path==model_.Publication()["rootfile"].toString()){Menu.addAction(action("epubMetadataAction"));Menu.addAction(action("epubEditNavigation"));Menu.addSeparator();}
        for(const auto& Name:{"epubRenameResource","epubDuplicateResource","epubRemoveResource","","epubExportResource","epubCopyPath","epubCopyRelativePath"}){if(QString(Name).isEmpty())Menu.addSeparator();else Menu.addAction(action(Name));}
    }
    Menu.exec(tree_->viewport()->mapToGlobal(Position));
}
void EpubSourceEditor::showSourceMenu(const QPoint& Position) {
    auto* Backend=static_cast<ScintillaQtDirectBackend*>(text_->Backend());if(!sourceShown())return;
    // A click outside the selection moves the caret so the menu describes what was clicked.
    const auto Clicked=Backend->Send(SCI_POSITIONFROMPOINTCLOSE,quintptr(Position.x()),Position.y());
    if(Clicked>=0 && (Clicked<Backend->Send(SCI_GETSELECTIONSTART) || Clicked>Backend->Send(SCI_GETSELECTIONEND)))Backend->Send(SCI_GOTOPOS,quintptr(Clicked));
    updateActions();QMenu Menu(this);const int Caret=sourcePosition();const auto Fix=quickFixAt(Caret);const auto Link=linkAt(Caret);const bool Selection=!Backend->selectedText().isEmpty();
    // Context items name their target; they run the shared commands.
    if(!Fix.isEmpty())emphasize(Menu.addAction(tr("빠른 수정: %1").arg(Fix["label"].toString()),action("epubQuickFix")->shortcut(),this,[this]{quickFix();}))->setEnabled(action("epubQuickFix")->isEnabled());
    if(!Link.isEmpty()){Menu.addAction(tr("%1 열기").arg(QFileInfo(Link).fileName()),action("epubOpenLink")->shortcut(),this,[this]{openLink();});if(mediaOf(Link).startsWith("image/"))Menu.addAction(tr("이미지 속성…"),this,[this,Link]{inspectImage(Link);});}
    if(Selection){auto* Wrap=Menu.addMenu(tr("태그로 감싸기"));for(const auto& Tag:{"em","strong","span","a","ruby"})Wrap->addAction(action(QString("epubWrap_")+Tag));Wrap->addSeparator();Wrap->addAction(action("epubWrap_custom"));Wrap->setEnabled(action("epubWrap_em")->isEnabled());}
    Menu.addAction(action("epubLocateSource"));if(Selection)Menu.addAction(action("epubFindAll"));Menu.addSeparator();
    Menu.addAction(tr("실행 취소"),QKeySequence::Undo,this,&EpubSourceEditor::Undo)->setEnabled(CanUndo());Menu.addAction(tr("다시 실행"),QKeySequence::Redo,this,&EpubSourceEditor::Redo)->setEnabled(CanRedo());Menu.addSeparator();
    const bool Writable=!sourceReadOnly_;
    Menu.addAction(tr("잘라내기"),QKeySequence::Cut,this,[Backend]{Backend->Send(SCI_CUT);})->setEnabled(Writable && Selection);
    Menu.addAction(tr("복사"),QKeySequence::Copy,this,[Backend]{Backend->Send(SCI_COPY);})->setEnabled(Selection);
    Menu.addAction(tr("붙여넣기"),QKeySequence::Paste,this,[Backend]{Backend->Send(SCI_PASTE);})->setEnabled(Writable && Backend->Send(SCI_CANPASTE));
    Menu.addAction(tr("삭제"),this,[Backend]{Backend->Send(SCI_CLEAR);})->setEnabled(Writable && Selection);
    Menu.addAction(tr("모두 선택"),QKeySequence::SelectAll,this,[Backend]{Backend->Send(SCI_SELECTALL);});Menu.addSeparator();
    Menu.addAction(tr("찾기…"),this,[this]{text_->showFindBar(false);});Menu.addAction(tr("바꾸기…"),this,[this]{text_->showFindBar(true);})->setEnabled(Writable);
    Menu.addAction(tr("줄로 이동…"),this,[this]{if(auto* Go=text_->findChild<QAction*>("text.goToLine"))Go->trigger();});Menu.addSeparator();
    auto* Fold=Menu.addMenu(tr("접기"));Fold->addAction(tr("전체 접기"),this,[this]{text_->FoldAll(true);});Fold->addAction(tr("전체 펼치기"),this,[this]{text_->FoldAll(false);});
    auto* Wrap=Menu.addAction(tr("자동 줄바꿈"),this,[this](bool On){text_->setWordWrap(On);});Wrap->setCheckable(true);Wrap->setChecked(text_->isWordWrapEnabled());
    Menu.exec(SourceInput()->mapToGlobal(Position));
}
void EpubSourceEditor::showTabMenu(const QPoint& Position) {
    const int Index=tabs_->tabAt(Position);if(Index<0)return;const auto Path=tabs_->tabData(Index).toString();QMenu Menu(this);
    Menu.addAction(tr("닫기"),action("epubCloseTab")->shortcut(),this,[this,Path]{closeResource(Path,true);});
    Menu.addAction(tr("다른 탭 닫기"),this,[this,Path]{for(const auto& Other:QStringList(openResources_))if(Other!=Path && !closeResource(Other,true))break;})->setEnabled(tabs_->count()>1);
    Menu.addAction(tr("오른쪽 탭 모두 닫기"),this,[this,Index]{QList<int> Indexes;for(int I=Index+1;I<tabs_->count();++I)Indexes.append(I);closeTabs(Indexes);})->setEnabled(Index+1<tabs_->count());Menu.addSeparator();
    const bool Current=Path==current_;
    Menu.addAction(tr("변경 반영"),action("epubApplyResource")->shortcut(),this,[this]{flush();})->setEnabled(Current && action("epubApplyResource")->isEnabled());
    Menu.addAction(tr("반영 전으로 되돌리기"),this,[this]{revertResource(current_);})->setEnabled(Current && action("epubRevertResource")->isEnabled());Menu.addSeparator();
    Menu.addAction(tr("책 구조에서 보기"),this,[this,Path]{setPanelVisible("tree",true);selectResource(Path);tree_->setFocus();});
    setProperty("epubActionTarget",Path);updateActions();const auto Clear=qScopeGuard([this]{setProperty("epubActionTarget",QVariant{});updateActions();});
    Menu.addAction(action("epubPreviewChapter"));Menu.addAction(action("epubCopyPath"));Menu.exec(tabs_->mapToGlobal(Position));
}
void EpubSourceEditor::showProblemMenu(const QPoint& Position) {
    auto* Item=diagnostics_->itemAt(Position);QMenu Menu(this);const int Row=Item?Item->data(0,Qt::UserRole).toInt():-1;
    if(Item) {
        emphasize(Menu.addAction(tr("해당 줄로 이동"),this,[this,Row]{selectDiagnostic(Row);}));
        const auto Diagnostic=diagnosticRows_[Row].toObject();const bool Alt=Diagnostic["message"].toString().contains("alt",Qt::CaseInsensitive) && mediaOf(Diagnostic["resource"].toString())=="application/xhtml+xml";
        Menu.addAction(tr("빠른 수정: alt 속성 추가"),this,[this,Row]{if(selectDiagnostic(Row))quickFix();})->setEnabled(Alt && ready_ && !structuring_);Menu.addSeparator();
        Menu.addAction(tr("메시지 복사"),QKeySequence::Copy,this,[Item]{QApplication::clipboard()->setText(Item->text(1)+(Item->text(2)!="—"?":"+Item->text(2):QString{})+" "+Item->text(3));});
    }
    Menu.addAction(tr("모든 진단 복사"),this,[this]{QStringList Lines;for(const auto& Value:diagnosticRows_){const auto Row=Value.toObject();Lines.append(QString("%1\t%2:%3\t%4 · %5").arg(Row["severity"].toString(),Row["resource"].toString()).arg(Row["line"].toInt()).arg(Row["code"].toString(),Row["message"].toString()));}QApplication::clipboard()->setText(Lines.join('\n'));})->setEnabled(!diagnosticRows_.isEmpty());
    Menu.addSeparator();
    const auto Resource=Item?Item->text(1):QString{};auto* Only=Menu.addAction(tr("이 리소스의 문제만 보기"),this,[this,Resource](bool On){problemResource_=On?Resource:QString{};setDiagnostics(diagnosticRows_);});Only->setCheckable(true);Only->setChecked(!problemResource_.isEmpty());Only->setEnabled(!Resource.isEmpty() || !problemResource_.isEmpty());
    auto* Levels=Menu.addMenu(tr("수준별 표시"));
    for(const auto& [Key,Label]:{std::pair{QString("ERROR"),tr("오류")},std::pair{QString("WARNING"),tr("경고")},std::pair{QString("INFO"),tr("정보")}}){auto* Level=Levels->addAction(Label,this,[this,Key](bool On){if(problemFilter_.isEmpty())problemFilter_={"ERROR","WARNING","INFO"};if(On)problemFilter_.append(Key);else problemFilter_.removeAll(Key);problemFilter_.removeDuplicates();if(problemFilter_.size()==3)problemFilter_.clear();setDiagnostics(diagnosticRows_);});Level->setCheckable(true);Level->setChecked(problemFilter_.isEmpty() || problemFilter_.contains(Key));}
    Menu.addSeparator();auto* Again=Menu.addMenu(tr("다시 검사"));Again->addAction(action("epubBasicValidation"));Again->addAction(action("epubStandardValidation"));
    Menu.exec(diagnostics_->viewport()->mapToGlobal(Position));
}
void EpubSourceEditor::previewContextRequest(EpubProcessView* View) {
    // The reader runs in another process; it reports the click and the host shows the menu.
    const auto Request=View->ViewState()["contextRequest"].toObject();const auto Serial=Request["serial"].toInteger(-1);
    if(Serial<0 || Serial<=contextRequestSerial_ || View!=previewView_ || QApplication::activePopupWidget())return;contextRequestSerial_=Serial;
    const auto Anchor=Request["anchor"].toObject();const auto Selection=Request["selection"].toString().left(4096);const auto Target=Request["target"].toObject();
    QMenu Menu(this);bool SourceFocus=false;
    emphasize(Menu.addAction(tr("소스에서 이 위치 보기"),this,[this,Anchor,&SourceFocus]{if(selectPreviewSource(Anchor)){focusSource();SourceFocus=true;}}))->setEnabled(!Anchor.isEmpty() && IsSourceVisible());
    Menu.addAction(tr("선택한 텍스트를 소스에서 찾기"),this,[this,Anchor,Selection,&SourceFocus]{if(!Anchor.isEmpty())selectPreviewSource(Anchor);findSelectionInSource(Selection);SourceFocus=true;})->setEnabled(!Selection.trimmed().isEmpty() && IsSourceVisible());
    Menu.addAction(tr("복사"),QKeySequence::Copy,this,[Selection]{QApplication::clipboard()->setText(Selection);})->setEnabled(!Selection.isEmpty());
    if(Target["kind"]=="image"){const auto Path=Target["path"].toString();Menu.addAction(tr("이미지 리소스 열기"),this,[this,Path,&SourceFocus]{if(openResource(Path))SourceFocus=true;})->setEnabled(model_.IsEditable(Path));}
    Menu.addSeparator();Menu.addAction(action("epubPreviewRefresh"));Menu.addAction(action("epubAutoPreview"));Menu.addAction(action("epubPreviewSync"));Menu.addSeparator();
    Menu.addAction(tr("이전 장"),this,[View]{View->ReaderAction("previous-chapter");});Menu.addAction(tr("다음 장"),this,[View]{View->ReaderAction("next-chapter");});Menu.addAction(tr("목차"),this,[View]{View->ReaderAction("toggle-toc");});Menu.addSeparator();
    auto* Width=Menu.addMenu(tr("표시 폭"));
    for(const auto& [Pixels,Label]:{std::pair{360,tr("휴대폰 · 360px")},std::pair{768,tr("태블릿 · 768px")},std::pair{0,tr("창에 맞춤")}}){auto* Item=Width->addAction(Label,this,[this,Pixels]{previewWidth_=Pixels;if(previewView_)previewView_->ReaderAction("preview-width",Pixels);});Item->setCheckable(true);Item->setChecked(previewWidth_==Pixels);}
    Menu.exec(View->mapToGlobal(QPoint(Request["x"].toInt(),Request["y"].toInt())));
    if(!SourceFocus && previewView_==View)View->FocusInput();
}
}
