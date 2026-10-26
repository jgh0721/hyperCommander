#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "UniqueLibs/qtDialogFrame.hpp"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QScreen>
#include <QGridLayout>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace hc::document {
void EpubSourceEditor::editNavigation() {
    if(!ready_ || structuring_)return;
    QDialog Dialog(this);Dialog.setObjectName("epubNavigationEditor");Dialog.setWindowTitle(tr("EPUB 목차 편집"));Dialog.resize(QSize(940,680).boundedTo(screen()->availableGeometry().size()-QSize(40,80)));auto* Layout=new QVBoxLayout(&Dialog);auto* Note=new QLabel(tr("제목·대상·앵커를 편집하고 버튼 또는 끌어서 계층과 순서를 조정합니다. 읽기 순서는 리소스·장 작업 메뉴에서 별도로 조정합니다."),&Dialog);Note->setWordWrap(true);Layout->addWidget(Note);
    auto* Tree=new QTreeWidget(&Dialog);Tree->setObjectName("epubNavigationTree");Tree->setAccessibleName(tr("EPUB 목차 계층"));Tree->setHeaderLabels({tr("제목"),tr("대상 리소스"),tr("앵커")});Tree->setColumnWidth(0,300);Tree->setColumnWidth(1,390);Tree->setDragDropMode(QAbstractItemView::InternalMove);Tree->setSelectionMode(QAbstractItemView::SingleSelection);Layout->addWidget(Tree,1);
    auto* ButtonsRow=new QGridLayout;int ButtonIndex=0;auto MakeButton=[&](const QString& Text){auto* Button=new QPushButton(Text,&Dialog);ButtonsRow->addWidget(Button,ButtonIndex/4,ButtonIndex%4);++ButtonIndex;return Button;};auto* Add=MakeButton(tr("항목 추가"));auto* Child=MakeButton(tr("하위 추가"));auto* Remove=MakeButton(tr("삭제"));auto* Up=MakeButton(tr("위로"));auto* Down=MakeButton(tr("아래로"));auto* Indent=MakeButton(tr("하위로"));auto* Outdent=MakeButton(tr("상위로"));auto* Reset=MakeButton(tr("읽기 순서로 생성"));Layout->addLayout(ButtonsRow);
    auto* Form=new QFormLayout;auto* Label=new QLineEdit(&Dialog);Label->setObjectName("epubNavigationLabel");Label->setMaxLength(1024);Form->addRow(tr("목차 제목"),Label);auto* Target=new QComboBox(&Dialog);Target->addItem(tr("제목 그룹 · 하위 항목만 표시"),QString{});
    for(const auto& Value:model_.Publication()["spine"].toArray()){const auto Path=Value.toObject()["path"].toString();Target->addItem(Path,Path);}
    Target->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);Target->setMinimumContentsLength(32);Form->addRow(tr("대상"),Target);auto* Fragment=new QLineEdit(&Dialog);Fragment->setObjectName("epubNavigationFragment");Fragment->setMaxLength(1024);Form->addRow(tr("앵커 ID · # 제외"),Fragment);Layout->addLayout(Form);
    auto* Error=new QLabel(&Dialog);Error->setTextFormat(Qt::PlainText);Error->setWordWrap(true);Layout->addWidget(Error);auto* Buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&Dialog);Buttons->button(QDialogButtonBox::Ok)->setText(tr("확인"));Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소"));Layout->addWidget(Buttons);connect(Buttons,&QDialogButtonBox::rejected,&Dialog,&QDialog::reject);
    const auto Fill=[&](QTreeWidgetItem* Item){const QSignalBlocker A(Label),B(Target),C(Fragment);Label->setEnabled(Item);Target->setEnabled(Item);Fragment->setEnabled(Item);Label->setText(Item?Item->text(0):QString{});Target->setCurrentIndex(Item?Target->findData(Item->text(1)):0);Fragment->setText(Item?Item->text(2):QString{});};
    connect(Tree,&QTreeWidget::currentItemChanged,&Dialog,[&](QTreeWidgetItem* Item,QTreeWidgetItem*){Fill(Item);});
    connect(Tree,&QTreeWidget::itemChanged,&Dialog,[&](QTreeWidgetItem* Item,int){if(Item==Tree->currentItem())Fill(Item);});
    connect(Label,&QLineEdit::textEdited,&Dialog,[&](const QString& Text){if(auto* Item=Tree->currentItem()){const QSignalBlocker Block(Tree);Item->setText(0,Text);}});
    connect(Target,&QComboBox::activated,&Dialog,[&]{if(auto* Item=Tree->currentItem())Item->setText(1,Target->currentData().toString());});connect(Fragment,&QLineEdit::textEdited,&Dialog,[&](const QString& Text){if(auto* Item=Tree->currentItem()){const QSignalBlocker Block(Tree);Item->setText(2,Text);}});
    const auto AddRow=[&](const QJsonObject& Row,QTreeWidgetItem* Parent){auto* Item=new QTreeWidgetItem;Item->setText(0,Row["label"].toString());Item->setText(1,Row["path"].toString());Item->setText(2,Row["fragment"].toString());Item->setFlags(Item->flags()|Qt::ItemIsEditable|Qt::ItemIsDragEnabled|Qt::ItemIsDropEnabled);if(Parent)Parent->addChild(Item);else Tree->addTopLevelItem(Item);return Item;};
    const auto Populate=[&](const QJsonArray& Rows){const QSignalBlocker Block(Tree);Tree->clear();QList<QTreeWidgetItem*> Parents;QTreeWidgetItem* Last=nullptr;for(const auto& Value:Rows){const auto Row=Value.toObject();const int Depth=qBound(0,Row["depth"].toInt(),32);while(Parents.size()>Depth)Parents.removeLast();if(Depth>Parents.size() && Last)Parents.append(Last);Last=AddRow(Row,Parents.isEmpty()?nullptr:Parents.last());}Tree->expandAll();if(Tree->topLevelItemCount())Tree->setCurrentItem(Tree->topLevelItem(0));Fill(Tree->currentItem());};
    QJsonArray Rows;bool Valid=true;std::function<void(QTreeWidgetItem*,int)> Collect;
    Collect=[&](QTreeWidgetItem* Parent,int Depth){if(Depth>32){Valid=false;return;}for(int I=0;I<Parent->childCount();++I){auto* Item=Parent->child(I);if(Rows.size()>=1024 || Item->text(0).trimmed().isEmpty() || Item->text(0).size()>1024 || !Item->text(1).isEmpty() && Target->findData(Item->text(1))<0 || Item->text(2).size()>1024){Valid=false;return;}Rows.append(QJsonObject{{"label",Item->text(0)},{"path",Item->text(1)},{"fragment",Item->text(2)},{"depth",Depth}});Collect(Item,Depth+1);}};
    const auto Serialize=[&]{Rows={};Valid=true;Collect(Tree->invisibleRootItem(),0);return Valid && !Rows.isEmpty();};
    Populate(tocDraftToken_==Token() && !tocDraft_.isEmpty()?tocDraft_:model_.Publication()["navigation"].toArray());
    const auto NewRow=[&](bool AsChild){Rows={};Valid=true;Collect(Tree->invisibleRootItem(),0);if(Rows.size()>=1024){Error->setText(tr("목차는 1,024개까지 지원합니다."));return;}QString Path;for(const auto& Value:model_.Publication()["spine"].toArray()){Path=Value.toObject()["path"].toString();if(Path==current_)break;}auto* Item=AddRow({{"label",tr("새 항목")},{"path",Path},{"fragment",""}},AsChild?Tree->currentItem():nullptr);Tree->setCurrentItem(Item);if(Item->parent())Item->parent()->setExpanded(true);Label->setFocus();Label->selectAll();};
    connect(Add,&QPushButton::clicked,&Dialog,[&]{NewRow(false);});connect(Child,&QPushButton::clicked,&Dialog,[&]{NewRow(true);});connect(Remove,&QPushButton::clicked,&Dialog,[&]{delete Tree->currentItem();});
    const auto Move=[&](int Delta){auto* Item=Tree->currentItem();if(!Item)return;auto* Parent=Item->parent()?Item->parent():Tree->invisibleRootItem();const int Index=Parent->indexOfChild(Item);if(Index+Delta<0 || Index+Delta>=Parent->childCount())return;Parent->takeChild(Index);Parent->insertChild(Index+Delta,Item);Tree->setCurrentItem(Item);};connect(Up,&QPushButton::clicked,&Dialog,[&]{Move(-1);});connect(Down,&QPushButton::clicked,&Dialog,[&]{Move(1);});
    connect(Indent,&QPushButton::clicked,&Dialog,[&]{auto* Item=Tree->currentItem();if(!Item)return;auto* Parent=Item->parent()?Item->parent():Tree->invisibleRootItem();const int Index=Parent->indexOfChild(Item);if(Index<=0)return;auto* Previous=Parent->child(Index-1);Parent->takeChild(Index);Previous->addChild(Item);Previous->setExpanded(true);Tree->setCurrentItem(Item);});
    connect(Outdent,&QPushButton::clicked,&Dialog,[&]{auto* Item=Tree->currentItem();if(!Item || !Item->parent())return;auto* Parent=Item->parent();auto* Grand=Parent->parent()?Parent->parent():Tree->invisibleRootItem();Parent->takeChild(Parent->indexOfChild(Item));Grand->insertChild(Grand->indexOfChild(Parent)+1,Item);Tree->setCurrentItem(Item);});
    connect(Reset,&QPushButton::clicked,&Dialog,[&]{QJsonArray Generated;const auto Current=model_.Publication()["navigation"].toArray();for(const auto& Value:model_.Publication()["spine"].toArray()){const auto Path=Value.toObject()["path"].toString();QString Caption=QFileInfo(Path).completeBaseName();for(const auto& Entry:Current)if(Entry.toObject()["path"]==Path){Caption=Entry.toObject()["label"].toString();break;}Generated.append(QJsonObject{{"label",Caption},{"path",Path},{"fragment",""},{"depth",0}});}Populate(Generated);});
    connect(Buttons,&QDialogButtonBox::accepted,&Dialog,[&]{if(!Serialize()){Error->setText(tr("목차 제목·대상·계층을 확인해 주세요. 최대 1,024개, 32단계까지 지원합니다."));return;}tocDraft_=Rows;tocDraftToken_=Token();Dialog.accept();});
    {auto* Footer=hc::ui::ApplyDialogFrame(&Dialog,Buttons,{{},tr("목차 항목의 제목·대상·앵커와 계층·순서를 편집합니다."),fm::ui::glyph::Rename});Error->setParent(Footer->parentWidget());Footer->insertWidget(0,Error,1);}
    if(Dialog.exec()==QDialog::Accepted)structure({{"operation","toc"},{"entries",tocDraft_}});
}
}
