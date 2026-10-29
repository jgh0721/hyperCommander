#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "epubResourceTree.hpp"
#include "epubProcessView.hpp"
#include "core/AppSettings.h"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include "UniqueLibs/document/documentShortcutRegistry.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include <type_traits>
#include <QAction>
#include <QPushButton>
#include <QActionGroup>
#include <QApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <Scintilla.h>
#include <fmstyle/StyleProps.h>
namespace hc::document {
// Splitter whose handle folds the neighbouring panel on double click.
class EpubSplitter final:public QSplitter {
public:
    EpubSplitter(Qt::Orientation Orientation,QWidget* Parent,std::function<void()> Fold):QSplitter(Orientation,Parent),fold_(std::move(Fold)){setChildrenCollapsible(false);setHandleWidth(5);}
protected:
    QSplitterHandle* createHandle()override {
        struct Handle final:QSplitterHandle {
            Handle(Qt::Orientation Orientation,EpubSplitter* Parent):QSplitterHandle(Orientation,Parent),owner_(Parent){setToolTip(QObject::tr("끌어서 크기 조절 · 두 번 누르면 패널 접기"));}
            void mouseDoubleClickEvent(QMouseEvent* Event)override{if(owner_->fold_)owner_->fold_();Event->accept();}
            EpubSplitter* owner_;
        };
        return new Handle(orientation(),this);
    }
private:
    std::function<void()> fold_;
};
namespace {
const QMap<QString,QString>& commandActions() {
    static const QMap<QString,QString> Map{{"epubEdit.apply","epubApplyResource"},{"epubEdit.validate","epubBasicValidation"},{"epubEdit.epubcheck","epubStandardValidation"},{"epubEdit.filter","epubResourceFilter"},
        {"epubEdit.previewRefresh","epubPreviewRefresh"},{"epubEdit.scrollSync","epubPreviewScrollSync"},{"epubEdit.closeTab","epubCloseTab"},{"epubEdit.toc","epubEditNavigation"},{"epubEdit.metadata","epubMetadataAction"},{"epubEdit.toggleTree","epubToggleTree"},
        {"epubEdit.togglePreview","epubTogglePreview"},{"epubEdit.toggleProblems","epubToggleProblems"},{"epubEdit.orientation","epubToggleOrientation"},{"epubEdit.recover","epubRecoverDraft"},
        {"epubTree.rename","epubRenameResource"},{"epubTree.delete","epubRemoveResource"},{"epubTree.moveUp","epubChapterUp"},{"epubTree.moveDown","epubChapterDown"},{"epubTree.copyPath","epubCopyPath"},
        {"epubSource.quickFix","epubQuickFix"},{"epubSource.openLink","epubOpenLink"},{"epubSource.findAll","epubFindAll"},{"epubSource.locate","epubLocateSource"},{"epubSource.wrapEm","epubWrap_em"},
        {"epubSource.wrapStrong","epubWrap_strong"},{"epubSource.wrapSpan","epubWrap_span"},{"epubSource.wrapLink","epubWrap_a"},{"epubSource.wrapRuby","epubWrap_ruby"}};
    return Map;
}
// Saved preview toggles: settings key under epubEditor/layout and the action they restore.
QList<std::pair<const char*,const char*>> previewToggles(){return {{"autoPreview","epubAutoPreview"},{"positionSync","epubPreviewSync"},{"scrollSync","epubPreviewScrollSync"}};}
QLabel* statusLabel(QWidget* Parent,const QString& Name){auto* Label=new QLabel(Parent);Label->setObjectName(Name);Label->setTextFormat(Qt::PlainText);return Label;}
}
void EpubSourceEditor::setupLayout() {
    auto* Outer=new QVBoxLayout(this);Outer->setContentsMargins(0,0,0,0);Outer->setSpacing(0);
    outerSplitter_=new EpubSplitter(Qt::Horizontal,this,[this]{setPanelVisible("tree",false);});outerSplitter_->setObjectName("epubOuterSplitter");Outer->addWidget(outerSplitter_);
    treePanel_=new QWidget(outerSplitter_);treePanel_->setObjectName("epubResourcePane");setupResources(treePanel_);
    workSplitter_=new EpubSplitter(Qt::Vertical,outerSplitter_,[this]{setPanelVisible("problems",false);});workSplitter_->setObjectName("epubWorkSplitter");
    previewSplitter_=new EpubSplitter(Qt::Horizontal,workSplitter_,[this]{setPanelVisible("preview",false);});previewSplitter_->setObjectName("epubPreviewSplitter");
    sourcePanel_=new QWidget(previewSplitter_);sourcePanel_->setObjectName("epubSourceArea");auto* Source=new QVBoxLayout(sourcePanel_);Source->setContentsMargins(0,0,0,0);Source->setSpacing(0);
    tabs_=new QTabBar(sourcePanel_);tabs_->setObjectName("epubSourceTabs");tabs_->setAccessibleName(tr("열린 EPUB 리소스"));tabs_->setTabsClosable(true);tabs_->setDocumentMode(true);tabs_->setExpanding(false);tabs_->setElideMode(Qt::ElideNone);tabs_->setUsesScrollButtons(true);tabs_->setContextMenuPolicy(Qt::CustomContextMenu);Source->addWidget(tabs_);
    recoveryBar_=new QWidget(sourcePanel_);recoveryBar_->setObjectName("epubRecoveryBar");recoveryBar_->setAutoFillBackground(true);recoveryBar_->setBackgroundRole(QPalette::AlternateBase);
    {auto* Row=new QHBoxLayout(recoveryBar_);Row->setContentsMargins(10,4,6,4);Row->setSpacing(6);recoveryText_=new QLabel(recoveryBar_);recoveryText_->setTextFormat(Qt::PlainText);recoveryText_->setWordWrap(false);Row->addWidget(recoveryText_,1);
     for(const auto& [Name,Label]:{std::pair{"epubRecoveryRestore",tr("초안 복원")},std::pair{"epubRecoveryOther",tr("다른 초안…")},std::pair{"epubRecoveryRemove",tr("삭제")}}){auto* Button=new QToolButton(recoveryBar_);Button->setObjectName(Name);Button->setText(Label);Button->setAutoRaise(false);Row->addWidget(Button);}
     auto* Close=new QToolButton(recoveryBar_);Close->setObjectName("epubRecoveryDismiss");Close->setText(QStringLiteral("×"));Close->setAccessibleName(tr("알림 닫기"));Close->setAutoRaise(true);Row->addWidget(Close);}
    recoveryBar_->hide();Source->addWidget(recoveryBar_);
    sourceStack_=new QStackedWidget(sourcePanel_);Source->addWidget(sourceStack_,1);
    text_=new QTextView(sourceStack_);text_->setObjectName("epubEditSource");text_->setAccessibleName(tr("EPUB 리소스 소스"));text_->setHotExitEnabled(false);sourceStack_->addWidget(text_);
    // A new view reads only its display settings; the text editor font arrives with the full reload.
    text_->ReloadPreferences();
    SourceInput()->setObjectName("epubEditSourceInput");SourceInput()->setAccessibleName(tr("EPUB 리소스 소스"));SourceInput()->installEventFilter(this);SourceInput()->setAttribute(Qt::WA_NativeWindow);SourceInput()->setContextMenuPolicy(Qt::CustomContextMenu);
    static_cast<ScintillaQtDirectBackend*>(text_->Backend())->Send(SCI_USEPOPUP,SC_POPUP_NEVER);setFocusProxy(SourceInput());
    image_=new QLabel(sourceStack_);image_->setObjectName("epubImagePreview");image_->setAlignment(Qt::AlignCenter);image_->setMinimumHeight(100);sourceStack_->addWidget(image_);
    auto* Empty=new QLabel(tr("책 구조에서 편집할 리소스를 고르세요."),sourceStack_);Empty->setObjectName("epubSourceEmpty");Empty->setAlignment(Qt::AlignCenter);Empty->setEnabled(false);sourceStack_->addWidget(Empty);
    previewPanel_=new QWidget(previewSplitter_);previewPanel_->setObjectName("epubPreviewPanel");previewPanel_->hide();
    problemsPanel_=new QWidget(workSplitter_);problemsPanel_->setObjectName("epubProblems");setupProblems(problemsPanel_);
    outerSplitter_->setStretchFactor(0,0);outerSplitter_->setStretchFactor(1,1);workSplitter_->setStretchFactor(0,1);workSplitter_->setStretchFactor(1,0);previewSplitter_->setStretchFactor(0,1);previewSplitter_->setStretchFactor(1,1);
    outerSplitter_->setSizes({240,900});workSplitter_->setSizes({560,150});previewSplitter_->setSizes({560,460});
    layoutTimer_.setSingleShot(true);layoutTimer_.setInterval(400);connect(&layoutTimer_,&QTimer::timeout,this,&EpubSourceEditor::saveLayout);
    for(auto* Splitter:{static_cast<QSplitter*>(outerSplitter_),static_cast<QSplitter*>(workSplitter_),static_cast<QSplitter*>(previewSplitter_)})connect(Splitter,&QSplitter::splitterMoved,this,[this]{if(layoutRestored_)layoutTimer_.start();});
    connect(tabs_,&QTabBar::currentChanged,this,[this](int Index){if(updating_ || Index<0)return;const auto Path=tabs_->tabData(Index).toString();if(Path!=current_)openResource(Path);});
    connect(tabs_,&QTabBar::tabCloseRequested,this,&EpubSourceEditor::closeTab);
    connect(tabs_,&QTabBar::customContextMenuRequested,this,&EpubSourceEditor::showTabMenu);
    connect(SourceInput(),&QWidget::customContextMenuRequested,this,&EpubSourceEditor::showSourceMenu);
}
void EpubSourceEditor::setupResources(QWidget* Panel) {
    auto* Layout=new QVBoxLayout(Panel);Layout->setContentsMargins(6,6,4,4);Layout->setSpacing(4);
    auto* Header=new QHBoxLayout;Header->setSpacing(2);auto* Title=new QLabel(tr("책 구조"),Panel);auto Font=Title->font();Font.setWeight(QFont::DemiBold);Title->setFont(Font);Header->addWidget(Title,1);
    auto* Add=new QToolButton(Panel);Add->setObjectName("epubResourceAdd");Add->setText(QStringLiteral("+"));Add->setToolTip(tr("리소스 추가"));Add->setAccessibleName(tr("리소스 추가"));Add->setAutoRaise(true);Add->setPopupMode(QToolButton::InstantPopup);Header->addWidget(Add);
    auto* Collapse=new QToolButton(Panel);Collapse->setObjectName("epubResourceCollapse");Collapse->setText(QStringLiteral("⌃"));Collapse->setToolTip(tr("모두 접기"));Collapse->setAccessibleName(tr("모두 접기"));Collapse->setAutoRaise(true);Header->addWidget(Collapse);Layout->addLayout(Header);
    treeFilter_=new QLineEdit(Panel);treeFilter_->setObjectName("epubResourceFilter");treeFilter_->setClearButtonEnabled(true);treeFilter_->setAccessibleName(tr("리소스 필터"));Layout->addWidget(treeFilter_);
    resourceModel_=new EpubResourceModel(this);resourceFilter_=new EpubResourceFilter(this);resourceFilter_->setSourceModel(resourceModel_);
    resourceModel_->SetDecorations([this](const QString& Path){return touchedResources_.contains(Path) || model_.Changes().contains(Path) || (Path==current_ && sourceShown() && sourceModified());},
        [this](const QString& Path){int Count=0;for(const auto& Value:diagnosticRows_)Count+=Value.toObject()["resource"].toString()==Path;return Count;});
    tree_=new QTreeView(Panel);tree_->setObjectName("epubResourceTree");tree_->setAccessibleName(tr("EPUB 책 구조"));tree_->setModel(resourceFilter_);tree_->setHeaderHidden(true);tree_->setUniformRowHeights(false);tree_->setIndentation(14);tree_->setItemDelegate(new EpubResourceDelegate(tree_));tree_->setContextMenuPolicy(Qt::CustomContextMenu);tree_->setEditTriggers(QAbstractItemView::NoEditTriggers);tree_->setProperty("documentShortcutScope","epub-edit.tree");Layout->addWidget(tree_,1);
    treeSummary_=new QLabel(Panel);treeSummary_->setObjectName("epubResourceSummary");treeSummary_->setTextFormat(Qt::PlainText);treeSummary_->setForegroundRole(QPalette::PlaceholderText);Layout->addWidget(treeSummary_);
    connect(treeFilter_,&QLineEdit::textChanged,this,[this](const QString& Text){resourceFilter_->SetText(Text);tree_->expandAll();});
    connect(Collapse,&QToolButton::clicked,tree_,&QTreeView::collapseAll);
    connect(tree_->selectionModel(),&QItemSelectionModel::currentChanged,this,[this](const QModelIndex& Index){if(updating_ || !Index.isValid())return;const auto Path=Index.data(EpubResourceModel::PathRole).toString();if(!Path.isEmpty() && Index.data(EpubResourceModel::EditableRole).toBool() && Path!=current_)openResource(Path);updateActions();});
    connect(tree_,&QTreeView::activated,this,[this](const QModelIndex& Index){const auto Path=Index.data(EpubResourceModel::PathRole).toString();if(!Path.isEmpty() && openResource(Path))focusSource();});
    connect(tree_,&QTreeView::customContextMenuRequested,this,&EpubSourceEditor::showTreeMenu);
    auto* AddMenu=new QMenu(Add);Add->setMenu(AddMenu);connect(AddMenu,&QMenu::aboutToShow,this,[this,AddMenu]{AddMenu->clear();for(const auto& Name:{"epubAddChapter","epubAddImage","epubImportResource"})AddMenu->addAction(action(Name));});
}
void EpubSourceEditor::setupProblems(QWidget* Panel) {
    auto* Layout=new QVBoxLayout(Panel);Layout->setContentsMargins(0,0,0,0);Layout->setSpacing(0);
    problems_=new QTabWidget(Panel);problems_->setObjectName("epubProblemTabs");problems_->setDocumentMode(true);Layout->addWidget(problems_);
    problemsSummary_=new QLabel(problems_);problemsSummary_->setObjectName("epubProblemSummary");problemsSummary_->setTextFormat(Qt::PlainText);problemsSummary_->setContentsMargins(0,0,8,0);problemsSummary_->setForegroundRole(QPalette::PlaceholderText);problems_->setCornerWidget(problemsSummary_,Qt::TopRightCorner);
    diagnostics_=new QTreeWidget(problems_);diagnostics_->setObjectName("epubEditDiagnostics");diagnostics_->setAccessibleName(tr("EPUB 소스 검사 결과"));diagnostics_->setHeaderLabels({tr("수준"),tr("리소스"),tr("행"),tr("설명")});diagnostics_->setRootIsDecorated(false);diagnostics_->setColumnWidth(0,70);diagnostics_->setColumnWidth(1,220);diagnostics_->setColumnWidth(2,50);diagnostics_->setContextMenuPolicy(Qt::CustomContextMenu);problems_->addTab(diagnostics_,tr("문제"));
    checkerOutput_=new QPlainTextEdit(problems_);checkerOutput_->setObjectName("epubCheckerOutput");checkerOutput_->setReadOnly(true);checkerOutput_->setAccessibleName(tr("EPUBCheck 출력"));checkerOutput_->setLineWrapMode(QPlainTextEdit::NoWrap);problems_->addTab(checkerOutput_,tr("EPUBCheck 출력"));
    searchResults_=new QTreeWidget(problems_);searchResults_->setObjectName("epubSearchResults");searchResults_->setAccessibleName(tr("EPUB 리소스 검색 결과"));searchResults_->setHeaderLabels({tr("리소스"),tr("행"),tr("내용")});searchResults_->setRootIsDecorated(false);searchResults_->setColumnWidth(0,220);searchResults_->setColumnWidth(1,50);problems_->addTab(searchResults_,tr("검색 결과"));
    connect(diagnostics_,&QTreeWidget::customContextMenuRequested,this,&EpubSourceEditor::showProblemMenu);
    connect(searchResults_,&QTreeWidget::itemActivated,this,[this](QTreeWidgetItem* Item,int){if(!Item)return;if(openResource(Item->data(0,Qt::UserRole).toString())){const int Line=Item->data(1,Qt::UserRole).toInt(),Column=Item->data(2,Qt::UserRole).toInt();text_->goToPosition(Line,Column);focusSource();}});
}
void EpubSourceEditor::setupActions() {
    const auto Make=[this](const QString& Name,const QString& Text,auto Slot,bool Checkable=false)->QAction* {
        auto* Item=new QAction(Text,this);Item->setObjectName(Name);Item->setCheckable(Checkable);actions_[Name]=Item;
        for(auto I=commandActions().begin();I!=commandActions().end();++I)if(I.value()==Name)Item->setProperty("documentCommandId",I.key());
        // A checkable command follows its state, so setChecked() from code behaves like a click.
        if constexpr(std::is_invocable_v<decltype(Slot),bool>){if(Checkable)connect(Item,&QAction::toggled,this,Slot);else connect(Item,&QAction::triggered,this,Slot);}else connect(Item,&QAction::triggered,this,[Slot]{Slot();});
        return Item;
    };
    const auto Target=[this]{return treeResource();};
    Make("epubApplyResource",tr("변경 반영"),[this]{flush();});
    Make("epubSaveCopy",tr("검증 후 다른 이름으로 저장…"),[this]{validate(true);})->setToolTip(tr("검사를 통과한 편집본을 원본과 다른 파일로 저장합니다. 원본 EPUB은 변경하지 않습니다."));
    Make("epubBasicValidation",tr("기본 검사"),[this]{validate(false);});
    Make("epubStandardValidation",tr("EPUBCheck 검사"),[this]{validate(false,true);});
    Make("epubValidationCancel",tr("검사 취소"),[this]{cancelEpubCheck("cancelled");});
    Make("epubAddChapter",tr("새 장 추가…"),[this,Target]{addChapterResource(Target());});
    Make("epubAddImage",tr("이미지 추가…"),[this]{addImageResource();});
    Make("epubAddChapterImage",tr("이 장에 이미지 추가…"),[this,Target]{addImageResource(Target());});
    Make("epubImportResource",tr("스타일 · 글꼴 가져오기…"),[this]{importResources();});
    Make("epubMetadataAction",tr("서지정보 편집…"),[this]{editMetadata();});
    Make("epubImagePropertiesAction",tr("이미지 사용 위치·속성…"),[this,Target]{inspectImage(Target());});
    Make("epubRenameResource",tr("이름·경로 변경…"),[this,Target]{renameResource(Target());});
    Make("epubRemoveResource",tr("삭제…"),[this,Target]{removeResource(Target());});
    Make("epubDuplicateResource",tr("복제"),[this,Target]{duplicateResource(Target());});
    Make("epubReplaceImage",tr("같은 형식 이미지로 교체…"),[this,Target]{if(openResource(Target()))importImage();});
    Make("epubConvertImage",tr("PNG · JPEG 변환…"),[this,Target]{convertImageResource(Target());});
    Make("epubCoverImage",tr("표지로 지정"),[this,Target]{structure({{"operation","cover"},{"resource",Target()}});});
    Make("epubChapterUp",tr("읽기 순서 위로"),[this,Target]{moveChapter(Target(),-1);});
    Make("epubChapterDown",tr("읽기 순서 아래로"),[this,Target]{moveChapter(Target(),1);});
    Make("epubStructureCancel",tr("구조 작업 취소"),[this]{if(structureCancel_)structureCancel_->store(true);});
    Make("epubEditNavigation",tr("목차 편집…"),[this]{editNavigation();});
    Make("epubCopyPath",tr("경로 복사"),[this,Target]{copyPath(Target(),false);});
    Make("epubCopyRelativePath",tr("링크용 상대 경로 복사"),[this,Target]{copyPath(Target(),true);});
    Make("epubExportResource",tr("파일로 내보내기…"),[this,Target]{exportResource(Target());});
    Make("epubInsertImage",tr("편집기 커서 위치에 <img> 넣기"),[this,Target]{insertImageReference(Target());});
    Make("epubPreviewChapter",tr("미리보기에서 보기"),[this,Target]{
        const auto Path=Target();const auto Spine=model_.Publication()["spine"].toArray();
        for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==Path && previewView_){setPanelVisible("preview",true);previewView_->ReaderAction("chapter",I);}
    });
    Make("epubRevealResource",tr("책 구조에서 보기"),[this]{setPanelVisible("tree",true);selectResource(current_);tree_->setFocus();});
    Make("epubResourceFilter",tr("리소스 필터"),[this]{setPanelVisible("tree",true);treeFilter_->setFocus();treeFilter_->selectAll();});
    Make("epubChangedOnly",tr("수정·경고 있는 리소스만 보기"),[this](bool On){resourceFilter_->SetChangedOnly(On);tree_->expandAll();},true);
    Make("epubExpandAll",tr("모두 펼치기"),[this]{tree_->expandAll();});
    Make("epubCollapseAll",tr("모두 접기"),[this]{tree_->collapseAll();});
    Make("epubCloseTab",tr("닫기"),[this]{closeTab(tabs_->currentIndex());});
    Make("epubCloseOtherTabs",tr("다른 탭 닫기"),[this]{QList<int> Indexes;for(int I=0;I<tabs_->count();++I)if(tabs_->tabData(I).toString()!=current_)Indexes.append(I);closeTabs(Indexes);});
    Make("epubCloseRightTabs",tr("오른쪽 탭 모두 닫기"),[this]{QList<int> Indexes;for(int I=tabs_->currentIndex()+1;I<tabs_->count();++I)Indexes.append(I);closeTabs(Indexes);});
    Make("epubRevertResource",tr("반영 전으로 되돌리기"),[this]{revertResource(current_);});
    Make("epubQuickFix",tr("빠른 수정"),[this]{quickFix();});
    Make("epubOpenLink",tr("링크된 리소스 열기"),[this]{openLink();});
    Make("epubFindAll",tr("모든 리소스에서 찾기"),[this]{const auto Selected=text_->Backend()->selectedText();findInResources(Selected.isEmpty()?text_->Backend()->WordAtCaret():Selected);});
    Make("epubLocateSource",tr("미리보기에서 이 위치 보기"),[this]{sourceSyncPending_=true;sourceSyncAttempts_=0;locateSource();});
    for(const auto& Tag:{QString("em"),QString("strong"),QString("span"),QString("a"),QString("ruby")})Make("epubWrap_"+Tag,Tag=="em"?tr("<em> 기울임"):Tag=="strong"?tr("<strong> 굵게"):Tag=="span"?tr("<span class=\"…\">…"):Tag=="a"?tr("<a href=\"…\"> 링크…"):tr("<ruby> 읽기 표기…"),[this,Tag]{wrapSelection(Tag);});
    Make("epubWrap_custom",tr("사용자 지정 태그…"),[this]{wrapSelection({});});
    Make("epubToggleTree",tr("책 구조 패널"),[this](bool On){setPanelVisible("tree",On);},true);
    Make("epubTogglePreview",tr("미리보기 패널"),[this](bool On){setPanelVisible("preview",On);},true);
    Make("epubToggleProblems",tr("문제 패널"),[this](bool On){setPanelVisible("problems",On);},true);
    Make("epubToggleOrientation",tr("좌우 · 상하 배치 전환"),[this]{applyOrientation(previewSplitter_->orientation()==Qt::Horizontal?Qt::Vertical:Qt::Horizontal);});
    auto* Horizontal=Make("epubLayoutHorizontal",tr("좌우 배치"),[this]{applyOrientation(Qt::Horizontal);},true);auto* Vertical=Make("epubLayoutVertical",tr("상하 배치"),[this]{applyOrientation(Qt::Vertical);},true);
    auto* Orientation=new QActionGroup(this);Orientation->addAction(Horizontal);Orientation->addAction(Vertical);Horizontal->setChecked(true);
    Make("epubAutoPreview",tr("실시간 미리보기"),[this](bool On){if(On)requestPreview();else previewTimer_.stop();if(layoutRestored_)layoutTimer_.start();},true);
    Make("epubPreviewSync",tr("소스·본문 위치 동기화"),[this](bool On){
        // Scroll sync rides on the chapter following of position sync; it keeps its own state while disabled.
        action("epubPreviewScrollSync")->setEnabled(On);if(On){syncPreviewSource(true);scrollSyncPending_=true;lastScrollLine_=-1;}if(layoutRestored_)layoutTimer_.start();},true);
    Make("epubPreviewScrollSync",tr("소스·본문 스크롤 동기화"),[this](bool On){if(On){scrollSyncPending_=true;lastScrollLine_=-1;syncPreviewScroll();}if(layoutRestored_)layoutTimer_.start();},true);
    // Initial states are set quietly: their handlers expect every panel and command to exist.
    for(const auto& Name:{"epubToggleTree","epubTogglePreview","epubToggleProblems","epubAutoPreview","epubPreviewSync","epubPreviewScrollSync"}){const QSignalBlocker Block(action(Name));action(Name)->setChecked(true);}
    Make("epubPreviewRefresh",tr("미리보기 갱신"),[this]{requestPreview(true);});
    // The preview header shows the short labels; menus keep the full ones.
    action("epubAutoPreview")->setIconText(tr("실시간"));action("epubPreviewSync")->setIconText(tr("위치 동기화"));action("epubPreviewScrollSync")->setIconText(tr("스크롤 동기화"));action("epubPreviewRefresh")->setIconText(tr("갱신"));
    Make("epubRecoverDraft",tr("초안 복원"),[this]{if(!recoveryEntries_.isEmpty())restoreRecovery(recoveryEntries_.first().second);});
    Make("epubAutoRecovery",tr("자동 복구"),[this](bool On){AppSettings Settings;Settings.setValue("textView/recoveryEnabled",On);SetRecoveryEnabled(On);},true);
    for(const auto& Name:{"epubFindInSource","epubPreviewCopy","epubPreviewPreviousChapter","epubPreviewNextChapter","epubPreviewToc"}){auto* Item=new QAction(this);Item->setObjectName(Name);actions_[Name]=Item;}
    action("epubPreviewPreviousChapter")->setText(tr("이전 장"));action("epubPreviewNextChapter")->setText(tr("다음 장"));action("epubPreviewToc")->setText(tr("목차"));action("epubPreviewCopy")->setText(tr("복사"));action("epubFindInSource")->setText(tr("선택한 텍스트를 소스에서 찾기"));
    action("epubSaveCopy")->setProperty("fmRole","primary");
    validateMenu_=new QMenu(tr("검사"),this);validateMenu_->setObjectName("epubValidateMenu");validateMenu_->addActions({action("epubBasicValidation"),action("epubStandardValidation")});validateMenu_->addSeparator();validateMenu_->addAction(action("epubValidationCancel"));
    structureMenu_=new QMenu(tr("구조"),this);structureMenu_->setObjectName("epubStructureMenu");
    for(const auto& Name:{"epubAddChapter","epubAddImage","epubImportResource","","epubMetadataAction","epubImagePropertiesAction","","epubRenameResource","epubRemoveResource","epubDuplicateResource","","epubReplaceImage","epubConvertImage","epubCoverImage","","epubChapterUp","epubChapterDown","","epubStructureCancel"}){if(QString(Name).isEmpty())structureMenu_->addSeparator();else structureMenu_->addAction(action(Name));}
    connect(structureMenu_,&QMenu::aboutToShow,this,&EpubSourceEditor::updateActions);
    draftMenu_=new QMenu(tr("초안"),this);draftMenu_->setObjectName("epubDraftMenu");
    connect(draftMenu_,&QMenu::aboutToShow,this,[this]{
        draftMenu_->clear();if(recoveryEntries_.isEmpty()){auto* None=draftMenu_->addAction(tr("보관된 다른 초안 없음"));None->setEnabled(false);}
        for(const auto& Entry:recoveryEntries_){auto* Sub=draftMenu_->addMenu(Entry.first);const auto Path=Entry.second;connect(Sub->addAction(tr("복원")),&QAction::triggered,this,[this,Path]{restoreRecovery(Path);});connect(Sub->addAction(tr("삭제…")),&QAction::triggered,this,[this,Path]{removeRecovery(Path);});}
        draftMenu_->addSeparator();auto* Auto=action("epubAutoRecovery");{const QSignalBlocker Block(Auto);Auto->setChecked(recoveryEnabled_);}draftMenu_->addAction(Auto);
    });
    layoutMenu_=new QMenu(tr("배치"),this);layoutMenu_->setObjectName("epubLayoutMenu");layoutMenu_->addActions({action("epubLayoutHorizontal"),action("epubLayoutVertical")});layoutMenu_->addSeparator();layoutMenu_->addActions({action("epubToggleTree"),action("epubTogglePreview"),action("epubToggleProblems")});
    const auto MenuAction=[this](QMenu* Menu,const QString& Name){auto* Item=Menu->menuAction();Item->setObjectName(Name);actions_[Name]=Item;return Item;};
    MenuAction(validateMenu_,"epubValidateMenuAction");MenuAction(structureMenu_,"epubStructureMenuAction");MenuAction(draftMenu_,"epubDraftMenuAction");MenuAction(layoutMenu_,"epubLayoutMenuAction");
    const auto Separator=[this]{auto* Item=new QAction(this);Item->setSeparator(true);return Item;};
    toolbarActions_={action("epubApplyResource"),action("epubValidateMenuAction"),action("epubStructureMenuAction"),action("epubEditNavigation"),action("epubDraftMenuAction"),Separator(),action("epubToggleTree"),action("epubTogglePreview"),action("epubToggleProblems"),action("epubLayoutMenuAction"),Separator(),action("epubSaveCopy")};
    menuActions_={action("epubApplyResource"),action("epubBasicValidation"),action("epubStandardValidation"),Separator(),action("epubAddChapter"),action("epubAddImage"),action("epubImportResource"),action("epubMetadataAction"),action("epubEditNavigation"),Separator(),action("epubResourceFilter"),action("epubFindAll"),Separator(),action("epubLayoutHorizontal"),action("epubLayoutVertical"),action("epubToggleTree"),action("epubTogglePreview"),action("epubToggleProblems"),Separator(),action("epubSaveCopy")};
}
QAction* EpubSourceEditor::action(const QString& Id)const{return actions_.value(Id);}
QList<QAction*> EpubSourceEditor::ToolbarActions()const{return toolbarActions_;}
QList<QAction*> EpubSourceEditor::MenuActions()const{return menuActions_;}
QWidget* EpubSourceEditor::StatusStrip()const{return statusStrip_;}
bool EpubSourceEditor::CanRunCommand(const QString& Id)const{auto* Item=action(commandActions().value(Id));return Item && Item->isEnabled();}
bool EpubSourceEditor::RunCommand(const QString& Id) {
    auto* Item=action(commandActions().value(Id));if(!Item || !Item->isEnabled())return false;
    if(Item->isCheckable())Item->toggle();else Item->trigger();return true;
}
void EpubSourceEditor::SetShortcutBindings(const QJsonObject& Bindings) {
    // Keys are owned by the workspace router; the actions only show them.
    for(auto I=commandActions().begin();I!=commandActions().end();++I)if(auto* Item=action(I.value())){Item->setShortcut(QKeySequence::fromString(Bindings[I.key()].toString(),QKeySequence::PortableText));Item->setShortcutContext(Qt::WidgetShortcut);}
}
void EpubSourceEditor::setupStatus() {
    statusStrip_=new QWidget(this);statusStrip_->setObjectName("epubStatusStrip");statusStrip_->hide();auto* Row=new QHBoxLayout(statusStrip_);Row->setContentsMargins(0,0,0,0);Row->setSpacing(12);
    statusValidation_=statusLabel(statusStrip_,"epubStatusValidation");Row->addWidget(statusValidation_);
    statusProblems_=new QToolButton(statusStrip_);statusProblems_->setObjectName("epubStatusProblems");statusProblems_->setAutoRaise(true);statusProblems_->setToolTip(tr("문제 패널 표시"));Row->addWidget(statusProblems_);connect(statusProblems_,&QToolButton::clicked,this,[this]{setPanelVisible("problems",true);problems_->setCurrentWidget(diagnostics_);});
    statusMessage_=statusLabel(statusStrip_,"epubStatusMessage");statusMessage_->setMinimumWidth(80);statusMessage_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);Row->addWidget(statusMessage_,1);
    statusDraft_=statusLabel(statusStrip_,"epubStatusDraft");statusDraft_->setForegroundRole(QPalette::PlaceholderText);Row->addWidget(statusDraft_);
    statusCaret_=statusLabel(statusStrip_,"epubStatusCaret");Row->addWidget(statusCaret_);statusFormat_=statusLabel(statusStrip_,"epubStatusFormat");Row->addWidget(statusFormat_);
    auto* Original=statusLabel(statusStrip_,"epubStatusOriginal");Original->setText(tr("원본 유지 · 다른 이름으로 저장"));Original->setForegroundRole(QPalette::PlaceholderText);Original->setToolTip(tr("편집본은 검사를 통과한 뒤 다른 이름으로만 저장합니다. 원본 EPUB은 변경하지 않습니다."));Row->addWidget(Original);
}
void EpubSourceEditor::updateStatus() {
    if(!statusStrip_)return;
    QString Validation;int Errors=0,Warnings=0;for(const auto& Value:diagnosticRows_){const auto Severity=Value.toObject()["severity"].toString();if(Severity=="ERROR" || Severity=="FATAL")++Errors;else if(Severity=="WARNING")++Warnings;}
    if(nativeValidating_)Validation=tr("검사 중…");else if(checkerState_=="running")Validation=tr("EPUBCheck 검사 중…");
    else if(ready_ && validatedToken_==Token() && !validatedBytes_.isEmpty())Validation=tr("✓ 기본 검사 통과")+(checkerState_=="passed"?(Warnings?tr(" · EPUBCheck 경고 %1").arg(Warnings):tr(" · EPUBCheck 통과")):checkerState_=="failed"?tr(" · EPUBCheck 오류 %1").arg(Errors):QString{});
    else if(!error_.isEmpty())Validation=tr("⚠ 검사 실패");else Validation=tr("검사 전");
    statusValidation_->setText(Validation);statusValidation_->setToolTip(validationSnapshot()["state"].toString());
    statusProblems_->setText(tr("문제 %1").arg(diagnosticRows_.size()));statusProblems_->setVisible(problemsPanel_ && problemsPanel_->isHidden());
    const auto Message=!previewError_.isEmpty() && message_.isEmpty()?previewMessage_:message_;statusMessage_->setText(QFontMetrics(statusMessage_->font()).elidedText(QString(Message).replace('\n',' '),Qt::ElideRight,qMax(80,statusMessage_->width())));statusMessage_->setToolTip(Message);
    statusDraft_->setText(!recoveryEnabled_?tr("자동 복구 꺼짐"):draftSavedAt_>0?tr("초안 자동 보관 %1").arg(QDateTime::fromMSecsSinceEpoch(draftSavedAt_).toString("HH:mm")):tr("자동 복구 켜짐"));statusDraft_->setToolTip(draftMessage_);
    if(sourceShown()){statusCaret_->setText(tr("줄 %1, 열 %2").arg(text_->currentLine()).arg(text_->currentColumn()));const auto Eol=static_cast<ScintillaQtDirectBackend*>(text_->Backend())->Send(SCI_GETEOLMODE);statusFormat_->setText(QString("UTF-8 · %1 · %2").arg(Eol==SC_EOL_CRLF?"CRLF":Eol==SC_EOL_CR?"CR":"LF",text_->currentLanguage()));}
    else {statusCaret_->clear();statusFormat_->setText(current_.isEmpty()?QString{}:mediaOf(current_));}
    problemsSummary_->setText(checkerVersion_.isEmpty()?tr("오류 %1 · 경고 %2 · 정보 %3").arg(Errors).arg(Warnings).arg(diagnosticRows_.size()-Errors-Warnings):tr("EPUBCheck %1 · 오류 %2 · 경고 %3 · 정보 %4").arg(checkerVersion_).arg(Errors).arg(Warnings).arg(diagnosticRows_.size()-Errors-Warnings));
    problems_->setTabText(0,diagnosticRows_.isEmpty()?tr("문제"):tr("문제 %1").arg(diagnosticRows_.size()));
    if(previewChip_) {
        const auto Preview=previewSnapshot();const bool Busy=Preview["busy"].toBool(),Current=Preview["current"].toBool();
        previewChip_->setText(!previewError_.isEmpty()?tr("⚠ 검사 실패 · 마지막 정상 화면"):Preview["waitingForFocus"].toBool()?tr("창이 활성화되면 갱신"):Busy || !Current?tr("갱신 대기"):tr("✓ 최신 · %1").arg(QDateTime::currentDateTime().toString("HH:mm")));
        // Saved toggles survive restarts, so the chip says which ones are off.
        QStringList Notes{previewMessage_};if(!action("epubAutoPreview")->isChecked())Notes.append(tr("실시간 꺼짐 · 편집 뒤에는 [갱신]을 누르세요"));if(!action("epubPreviewSync")->isChecked())Notes.append(tr("위치 동기화 꺼짐"));
        else if(!action("epubPreviewScrollSync")->isChecked())Notes.append(tr("스크롤 동기화 꺼짐"));
        if(sourceBuffers_.value(current_).shiftsOverflow_)Notes.append(tr("이전 판 기준 · 스크롤 위치가 조금 어긋날 수 있습니다"));
        previewChip_->setToolTip(Notes.join('\n'));
        const auto State=Preview["state"].toObject();const int Chapter=State["chapterIndex"].toInt(-1);const int Count=model_.Publication()["spine"].toArray().size();previewChapterLabel_->setText(Chapter>=0 && Count>0?tr("%1 / %2장").arg(Chapter+1).arg(Count):QString{});
    }
    if(treeSummary_ && resourceModel_){int Changed=0,Problem=0;for(const auto& Value:model_.Publication()["resources"].toArray()){const auto Path=Value.toObject()["path"].toString();Changed+=touchedResources_.contains(Path) || model_.Changes().contains(Path);}for(const auto& Value:diagnosticRows_)Problem+=!Value.toObject()["resource"].toString().isEmpty();treeSummary_->setText(tr("%1개 리소스 · 수정 %2 · 문제 %3").arg(resourceModel_->ResourceCount()).arg(Changed).arg(Problem));}
}
QString EpubSourceEditor::treeResource()const {
    if(tree_ && tree_->hasFocus()){const auto Path=tree_->currentIndex().data(EpubResourceModel::PathRole).toString();if(!Path.isEmpty())return Path;}
    const auto Override=property("epubActionTarget").toString();return Override.isEmpty()?current_:Override;
}
void EpubSourceEditor::updateActions() {
    if(actions_.isEmpty())return;
    const bool Idle=ready_ && !structuring_,Editable=Idle && !sourceReadOnly_;const auto Target=treeResource();const auto Media=mediaOf(Target);const auto Publication=model_.Publication();
    const bool Package=Target==Publication["rootfile"].toString() || Media=="application/x-dtbncx+xml";const auto Spine=Publication["spine"].toArray();int Chapter=-1;for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==Target)Chapter=I;
    const bool Raster=QStringList{"image/png","image/jpeg","image/gif","image/webp"}.contains(Media),Image=Media.startsWith("image/");
    const bool Selection=sourceShown() && !text_->Backend()->selectedText().isEmpty();
    for(const auto& Name:{"epubApplyResource","epubSaveCopy","epubBasicValidation","epubStandardValidation","epubAddChapter","epubAddImage","epubImportResource","epubMetadataAction","epubEditNavigation","epubFindAll"})action(Name)->setEnabled(Idle);
    action("epubApplyResource")->setEnabled(Idle && sourceShown() && sourceModified());action("epubValidationCancel")->setEnabled(checkerState_=="running");action("epubStructureCancel")->setEnabled(structuring_);
    action("epubRenameResource")->setEnabled(Idle && !Package && !Target.isEmpty());action("epubRemoveResource")->setEnabled(Idle && !Package && !Target.isEmpty() && !(Chapter>=0 && Spine.size()<=1));
    action("epubDuplicateResource")->setEnabled(Idle && !Package && model_.IsEditable(Target) && !Image);action("epubReplaceImage")->setEnabled(Idle && Raster);action("epubConvertImage")->setEnabled(Idle && Raster);action("epubCoverImage")->setEnabled(Idle && Image);
    action("epubImagePropertiesAction")->setEnabled(Idle && Image);action("epubAddChapterImage")->setEnabled(Idle && Chapter>=0);action("epubInsertImage")->setEnabled(Editable && Image && sourceShown() && mediaOf(current_)=="application/xhtml+xml");
    action("epubChapterUp")->setEnabled(Idle && Chapter>0);action("epubChapterDown")->setEnabled(Idle && Chapter>=0 && Chapter+1<Spine.size());action("epubPreviewChapter")->setEnabled(Idle && Chapter>=0 && previewView_);
    action("epubCopyPath")->setEnabled(!Target.isEmpty());action("epubCopyRelativePath")->setEnabled(!Target.isEmpty() && !current_.isEmpty() && Target!=current_);action("epubExportResource")->setEnabled(Idle && !Target.isEmpty());
    action("epubCloseTab")->setEnabled(tabs_->count()>0);action("epubCloseOtherTabs")->setEnabled(tabs_->count()>1);action("epubCloseRightTabs")->setEnabled(tabs_->currentIndex()+1<tabs_->count());action("epubRevertResource")->setEnabled(Editable && sourceShown() && sourceModified());
    action("epubQuickFix")->setEnabled(Editable && sourceShown() && !quickFixAt(sourcePosition()).isEmpty());action("epubOpenLink")->setEnabled(Idle && sourceShown() && !linkAt(sourcePosition()).isEmpty());
    action("epubLocateSource")->setEnabled(Idle && sourceShown() && previewView_);action("epubPreviewRefresh")->setEnabled(Idle && previewRuntime_.has_value());
    action("epubPreviewScrollSync")->setEnabled(action("epubPreviewSync")->isChecked());
    for(const auto& Tag:{"em","strong","span","a","ruby","custom"})action(QString("epubWrap_")+Tag)->setEnabled(Editable && Selection && mediaOf(current_)=="application/xhtml+xml");
    action("epubRecoverDraft")->setEnabled(Idle && !recoveryEntries_.isEmpty());action("epubDraftMenuAction")->setEnabled(true);action("epubValidateMenuAction")->setEnabled(ready_);action("epubStructureMenuAction")->setEnabled(ready_);
    action("epubLayoutHorizontal")->setChecked(previewSplitter_->orientation()==Qt::Horizontal);action("epubLayoutVertical")->setChecked(previewSplitter_->orientation()==Qt::Vertical);
    action("epubLayoutMenuAction")->setText(previewSplitter_->orientation()==Qt::Horizontal?tr("좌우 배치"):tr("상하 배치"));
}
void EpubSourceEditor::restoreLayout() {
    AppSettings Settings;const auto Orientation=Settings.value("epubEditor/layout/orientation").toString();
    // Restored quietly: a saved toggle alone must not request or move the preview.
    for(const auto& [Key,Name]:previewToggles()){const auto Value=Settings.value(QString("epubEditor/layout/")+Key);if(!Value.isValid())continue;const QSignalBlocker Block(action(Name));action(Name)->setChecked(Value.toBool());}
    action("epubPreviewScrollSync")->setEnabled(action("epubPreviewSync")->isChecked());
    applyOrientation(Orientation=="vertical"?Qt::Vertical:Orientation=="horizontal"?Qt::Horizontal:window()->width()<980?Qt::Vertical:Qt::Horizontal,false);
    for(const auto& [Key,Splitter]:{std::pair{QString("outer"),static_cast<QSplitter*>(outerSplitter_)},std::pair{QString("work"),static_cast<QSplitter*>(workSplitter_)},std::pair{QString("main"),static_cast<QSplitter*>(previewSplitter_)}}){const auto State=QByteArray::fromBase64(Settings.value("epubEditor/layout/"+Key).toString().toLatin1());if(!State.isEmpty())Splitter->restoreState(State);}
    for(const auto& Panel:{QString("tree"),QString("preview"),QString("problems")}){const auto Value=Settings.value("epubEditor/layout/"+Panel);if(Value.isValid())setPanelVisible(Panel,Value.toBool());}
    layoutRestored_=true;
}
void EpubSourceEditor::saveLayout() {
    if(!layoutRestored_)return;layoutTimer_.stop();AppSettings Settings;
    Settings.setValue("epubEditor/layout/orientation",previewSplitter_->orientation()==Qt::Horizontal?"horizontal":"vertical");
    for(const auto& [Key,Splitter]:{std::pair{QString("outer"),static_cast<QSplitter*>(outerSplitter_)},std::pair{QString("work"),static_cast<QSplitter*>(workSplitter_)},std::pair{QString("main"),static_cast<QSplitter*>(previewSplitter_)}})Settings.setValue("epubEditor/layout/"+Key,QString::fromLatin1(Splitter->saveState().toBase64()));
    for(const auto& [Panel,Name]:{std::pair{"tree","epubToggleTree"},std::pair{"preview","epubTogglePreview"},std::pair{"problems","epubToggleProblems"}})Settings.setValue(QString("epubEditor/layout/")+Panel,action(Name)->isChecked());
    for(const auto& [Key,Name]:previewToggles())Settings.setValue(QString("epubEditor/layout/")+Key,action(Name)->isChecked());
}
void EpubSourceEditor::applyOrientation(Qt::Orientation Value,bool Save) {
    if(previewSplitter_->orientation()!=Value){previewSplitter_->setOrientation(Value);const int Total=Value==Qt::Horizontal?previewSplitter_->width():previewSplitter_->height();previewSplitter_->setSizes({Total*55/100,Total*45/100});}
    updateActions();if(Save && layoutRestored_)layoutTimer_.start();
}
void EpubSourceEditor::setPanelVisible(const QString& Panel,bool Visible) {
    auto* Toggle=action(Panel=="tree"?"epubToggleTree":Panel=="problems"?"epubToggleProblems":"epubTogglePreview");
    {const QSignalBlocker Block(Toggle);Toggle->setChecked(Visible);}
    const bool Source=!sourcePanel_->isHidden(),WasPreview=!previewPanel_->isHidden();
    // Reader mode keeps only the preview. A hidden preview stays alive and only pauses automatic refresh.
    treePanel_->setVisible(Source && action("epubToggleTree")->isChecked());problemsPanel_->setVisible(Source && action("epubToggleProblems")->isChecked());
    previewPanel_->setVisible(previewRuntime_.has_value() && (!Source || action("epubTogglePreview")->isChecked()));
    if(previewPanel_->isHidden())previewTimer_.stop();else if(!WasPreview && (action("epubAutoPreview")->isChecked() || previewMissing()) && previewActiveToken_!=Token())previewTimer_.start();
    for(auto* Widget:{treePanel_,problemsPanel_,previewPanel_})if(Widget->isHidden() && Widget->isAncestorOf(QApplication::focusWidget()))focusSource();
    updateStatus();if(layoutRestored_)layoutTimer_.start();
}
void EpubSourceEditor::refreshTree() {
    if(!resourceModel_)return;const QSignalBlocker Block(tree_->selectionModel());updating_=true;
    resourceModel_->Reset(model_.Publication(),[this](const QString& Path){return model_.IsEditable(Path);});tree_->expandAll();updating_=false;selectResource(current_);updateStatus();
}
void EpubSourceEditor::selectResource(const QString& Path) {
    if(!tree_)return;const auto Index=resourceFilter_->mapFromSource(resourceModel_->IndexOf(Path));
    const QSignalBlocker Block(tree_->selectionModel());if(Index.isValid()){tree_->setCurrentIndex(Index);tree_->scrollTo(Index);}else tree_->clearSelection();resourceModel_->Refresh();
}
void EpubSourceEditor::refreshTabs() {
    if(!tabs_)return;const bool Old=updating_;updating_=true;
    openResources_.removeIf([this](const QString& Path){return !model_.Publication().isEmpty() && !model_.IsEditable(Path);});
    bool Same=tabs_->count()==openResources_.size();for(int I=0;Same && I<tabs_->count();++I)Same=tabs_->tabData(I).toString()==openResources_[I];
    if(!Same){while(tabs_->count())tabs_->removeTab(0);for(const auto& Path:openResources_){const int Index=tabs_->addTab(QFileInfo(Path).fileName());tabs_->setTabData(Index,Path);tabs_->setTabToolTip(Index,Path);}}
    for(int I=0;I<tabs_->count();++I){const auto Path=tabs_->tabData(I).toString();tabs_->setTabText(I,QFileInfo(Path).fileName()+(Path==current_ && sourceShown() && sourceModified()?QStringLiteral(" ●"):QString{}));}
    tabs_->setCurrentIndex(openResources_.indexOf(current_));updating_=Old;if(resourceModel_)resourceModel_->Refresh();updateActions();
}
void EpubSourceEditor::closeTab(int Index){if(Index>=0 && Index<tabs_->count())closeResource(tabs_->tabData(Index).toString(),true);}
void EpubSourceEditor::closeTabs(const QList<int>& Indexes){QStringList Paths;for(const int Index:Indexes)Paths.append(tabs_->tabData(Index).toString());for(const auto& Path:Paths)if(!closeResource(Path,true))break;}
bool EpubSourceEditor::closeResource(const QString& Path,bool Ask) {
    const int Position=openResources_.indexOf(Path);if(Position<0)return false;
    if(Path==current_ && Ask && sourceShown() && sourceModified()) {
        hc::ui::MessageDialog Box(QMessageBox::Question,tr("편집기 탭 닫기"),tr("%1에 반영하지 않은 변경이 있습니다. 탭을 닫기 전에 책에 반영할까요?").arg(QFileInfo(Path).fileName()),QMessageBox::Save|QMessageBox::Discard|QMessageBox::Cancel,this);
        Box.button(QMessageBox::Save)->setText(tr("반영 후 닫기"));Box.button(QMessageBox::Discard)->setText(tr("버리고 닫기"));Box.button(QMessageBox::Cancel)->setText(tr("취소"));Box.setDefaultButton(QMessageBox::Save);
        const auto Answer=Box.exec()==-1?QMessageBox::Cancel:Box.standardButton(Box.clickedButton());
        if(Answer==QMessageBox::Cancel)return false;
        if(Answer==QMessageBox::Save && !flush()){setPanelVisible("problems",true);return false;}
        if(Answer==QMessageBox::Discard)revertResource(Path);
    } else if(Path==current_ && sourceShown() && sourceModified() && !flush())return false;
    openResources_.removeAt(Position);
    if(Path==current_) {
        saveSourceState();releaseSource(Path);
        if(!openResources_.isEmpty()){current_=openResources_.value(qMin(Position,int(openResources_.size())-1));select();}
        else {current_.clear();bufferResource_.clear();sourceStack_->setCurrentWidget(sourceStack_->findChild<QLabel*>("epubSourceEmpty"));selectResource({});}
    } else releaseSource(Path);
    refreshTabs();updateStatus();emit StateChanged();return true;
}
void EpubSourceEditor::revertResource(const QString& Path) {
    if(Path!=current_ || !sourceShown() || !sourceModified() || !sourceBuffers_.contains(Path))return;
    updating_=true;static_cast<ScintillaQtDirectBackend*>(text_->Backend())->SetTextUtf8(sourceBuffers_[Path].applied_);updating_=false;
    refreshTabs();report(tr("%1의 반영하지 않은 변경을 되돌렸습니다.").arg(QFileInfo(Path).fileName()));emit Changed();
}
}
