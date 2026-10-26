#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrentRun>
namespace hc::document {
void EpubSourceEditor::setupStructure(QVBoxLayout* Layout) {
    auto* Row=new QHBoxLayout;structureActions_=new QToolButton(this);structureActions_->setObjectName("epubStructureActions");structureActions_->setText(tr("리소스·장 작업"));structureActions_->setPopupMode(QToolButton::InstantPopup);auto* Menu=new QMenu(structureActions_);structureActions_->setMenu(Menu);Row->addWidget(structureActions_);
    tocEdit_=new QPushButton(tr("목차 편집…"),this);tocEdit_->setObjectName("epubEditNavigation");Row->addWidget(tocEdit_);auto* Cancel=new QPushButton(tr("구조 작업 취소"),this);Cancel->setObjectName("epubStructureCancel");Row->addWidget(Cancel);Row->addStretch();Layout->addLayout(Row);
    auto* Add=Menu->addAction(tr("이미지 추가…"));connect(Add,&QAction::triggered,this,&EpubSourceEditor::addImageResource);
    auto* Chapter=Menu->addAction(tr("새 장 추가…"));connect(Chapter,&QAction::triggered,this,&EpubSourceEditor::addChapterResource);Menu->addSeparator();
    auto* Metadata=Menu->addAction(tr("서지정보 편집…"));Metadata->setObjectName("epubMetadataAction");connect(Metadata,&QAction::triggered,this,&EpubSourceEditor::editMetadata);
    auto* Properties=Menu->addAction(tr("이미지 사용 위치·속성…"));Properties->setObjectName("epubImagePropertiesAction");connect(Properties,&QAction::triggered,this,&EpubSourceEditor::inspectImage);Menu->addSeparator();
    connect(&imageInspection_,&QFutureWatcherBase::finished,this,[this]{if(imageInspectionToken_!=Token()){status_->setText(tr("사용 위치 조사 중 내용이 변경되었습니다. 다시 확인해 주세요."));return;}editImageProperties(imageInspection_.result());});
    auto* Rename=Menu->addAction(tr("선택 리소스 이름·경로 변경…"));connect(Rename,&QAction::triggered,this,[this]{bool Ok=false;const auto Path=QInputDialog::getText(this,tr("리소스 이름·경로 변경"),tr("EPUB 내부 경로 · 참조는 함께 갱신됩니다."),QLineEdit::Normal,current_,&Ok);if(Ok && Path!=current_)structure({{"operation","rename"},{"resource",current_},{"destination",Path}});});
    auto* Remove=Menu->addAction(tr("선택 리소스 삭제…"));connect(Remove,&QAction::triggered,this,[this]{
        QDialog Dialog(this);Dialog.setWindowTitle(tr("EPUB 리소스 삭제"));auto* Layout=new QVBoxLayout(&Dialog);auto* Note=new QLabel(tr("%1\n리소스를 삭제합니다. 남아 있는 참조가 있으면 작업을 중단합니다. 실행 취소가 가능하며 원본 파일은 유지됩니다.").arg(current_),&Dialog);Note->setTextFormat(Qt::PlainText);Note->setWordWrap(true);Layout->addWidget(Note);auto* Images=new QCheckBox(tr("이 이미지를 표시하는 img·SVG image 요소도 삭제"),&Dialog);Images->setVisible(resources_->currentData().toString().startsWith("image/"));Layout->addWidget(Images);auto* Buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&Dialog);Buttons->button(QDialogButtonBox::Ok)->setText(tr("확인"));Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소"));Layout->addWidget(Buttons);connect(Buttons,&QDialogButtonBox::accepted,&Dialog,&QDialog::accept);connect(Buttons,&QDialogButtonBox::rejected,&Dialog,&QDialog::reject);
        if(Dialog.exec()==QDialog::Accepted)structure({{"operation","remove"},{"resource",current_},{"removeImageReferences",Images->isChecked()}});
    });
    auto* Convert=Menu->addAction(tr("선택 이미지 PNG·JPEG 변환…"));connect(Convert,&QAction::triggered,this,&EpubSourceEditor::convertImageResource);
    auto* Cover=Menu->addAction(tr("선택 이미지로 표지 교체"));Cover->setToolTip(tr("표지 메타데이터와 기존 표지 이미지의 참조를 갱신합니다."));connect(Cover,&QAction::triggered,this,[this]{structure({{"operation","cover"},{"resource",current_}});});Menu->addSeparator();
    auto* Up=Menu->addAction(tr("선택 장의 읽기 순서 위로"));auto* Down=Menu->addAction(tr("선택 장의 읽기 순서 아래로"));Up->setObjectName("epubChapterUp");Down->setObjectName("epubChapterDown");
    connect(Up,&QAction::triggered,this,[this]{structure({{"operation","chapter-move"},{"resource",current_},{"delta",-1}});});connect(Down,&QAction::triggered,this,[this]{structure({{"operation","chapter-move"},{"resource",current_},{"delta",1}});});
    connect(Menu,&QMenu::aboutToShow,this,[this,Rename,Remove,Convert,Cover,Up,Down]{const auto Media=resources_->currentData().toString();const bool Package=current_==model_.Publication()["rootfile"].toString();Rename->setEnabled(!Package);Remove->setEnabled(!Package);Convert->setEnabled(QStringList{"image/png","image/jpeg","image/gif","image/webp"}.contains(Media));Cover->setEnabled(Media.startsWith("image/"));const auto Spine=model_.Publication()["spine"].toArray();int Index=-1;for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==current_)Index=I;Up->setEnabled(Index>0);Down->setEnabled(Index>=0 && Index+1<Spine.size());});
    connect(tocEdit_,&QPushButton::clicked,this,&EpubSourceEditor::editNavigation);connect(Cancel,&QPushButton::clicked,this,[this]{if(structureCancel_)structureCancel_->store(true);});
    connect(this,&EpubSourceEditor::StateChanged,this,[this,Cancel]{const bool Busy=structuring_;structureActions_->setEnabled(ready_ && !Busy);tocEdit_->setEnabled(ready_ && !Busy);Cancel->setEnabled(Busy);for(const auto& Name:QStringList{"epubApplyResource","epubReplaceImage","epubSaveCopy","epubBasicValidation","epubStandardValidation"})if(auto* Button=findChild<QPushButton*>(Name))Button->setEnabled(ready_ && !Busy);});Cancel->setEnabled(false);structureActions_->setEnabled(false);tocEdit_->setEnabled(false);
    connect(&structureWorker_,&QFutureWatcher<StructureOutput>::finished,this,[this]{
        const auto Result=structureWorker_.result();structuring_=false;text_->setReadOnly(false);resources_->setEnabled(ready_);structureError_=Result.error_;
        if(!structureError_.isEmpty())status_->setText(structureError_=="cancelled"?tr("구조 작업을 취소했습니다. 편집 내용은 유지됩니다."):tr("구조 작업을 반영하지 못했습니다. 편집 내용은 유지됩니다.\n%1").arg(structureError_));
        else {model_=Result.model_;bookUndoPriority_=true;bookRedoPriority_=false;setDiagnostics({});checkerVerifiedToken_.clear();checkerState_="idle";refreshResources(Result.selected_);tocDraft_={};status_->setText(tr("구조 변경을 반영했습니다. 실행 취소와 다른 이름 저장을 사용할 수 있습니다. 저장 전 검사를 다시 진행해 주세요."));emit Changed();}
        emit StateChanged();
    });
}
void EpubSourceEditor::refreshResources(const QString& Preferred) {
    const auto Publication=model_.Publication();const auto Spine=Publication["spine"].toArray();QStringList Paths;for(const auto& Value:Spine)Paths.append(Value.toObject()["path"].toString());
    const auto PreviousPaths=resources_->property("epubSpinePaths").toStringList();const int PreviousChapter=previewSnapshot()["state"].toObject()["chapterIndex"].toInt(previewRestoreChapter_);const auto PreviousPath=PreviousChapter>=0 && PreviousChapter<PreviousPaths.size()?PreviousPaths[PreviousChapter]:QString{};
    int Chapter=Paths.indexOf(Preferred);if(Chapter<0)Chapter=Paths.indexOf(PreviousPath);if(Chapter<0 && !Paths.isEmpty())Chapter=qBound(0,PreviousChapter,Paths.size()-1);
    const auto Edition=Publication["sourceSha256"].toString();if(resources_->property("epubEditionHash").toString()!=Edition){recoveryLocator_.clear();previewRecoveryPosition_=true;previewRestoreChapter_=Chapter;}
    {const QSignalBlocker Block(resources_);resources_->clear();for(const auto& Value:Publication["resources"].toArray()){const auto Resource=Value.toObject();if(model_.IsEditable(Resource["path"].toString()))resources_->addItem(Resource["path"].toString(),Resource["mediaType"].toString());}int Index=resources_->findText(Preferred);if(Index<0 && Chapter>=0)Index=resources_->findText(Paths[Chapter]);resources_->setCurrentIndex(Index<0?0:Index);}
    resources_->setProperty("epubEditionHash",Edition);resources_->setProperty("epubSpinePaths",Paths);select();
}
bool EpubSourceEditor::structure(QJsonObject Request) {
    if(!ready_ || structuring_ || nativeValidating_ || draftReader_.isRunning() || pendingRecovery_ || !pendingRecoveryPath_.isEmpty() || !flush())return false;
    if(checkerState_=="running")cancelEpubCheck("source-changed");validatedBytes_.clear();structureError_.clear();structureCancel_=std::make_shared<std::atomic_bool>(false);const auto Cancel=structureCancel_;
    auto Selected=Request["operation"]=="rename" || Request["operation"]=="image-convert"?Request["destination"].toString():Request["operation"]=="image-add" || Request["operation"]=="chapter-add"?Request["resource"].toString():current_;
    auto Model=model_;text_->setReadOnly(true);resources_->setEnabled(false);status_->setText(tr("EPUB 구조와 참조를 갱신하고 있습니다…"));
    structuring_=true;structureWorker_.setFuture(QtConcurrent::run([Model=std::move(Model),Request=std::move(Request),Selected,Cancel]()mutable{
        QString Error;
        if(Request["operation"]=="image-add" && Request.contains("imageFile")) {
            QFile File(Request["imageFile"].toString());QByteArray Data;
            if(!QFileInfo(File.fileName()).isAbsolute() || !File.open(QIODevice::ReadOnly) || File.size()>8*1024*1024)Error="edit-image-file-or-budget";
            while(Error.isEmpty() && !File.atEnd()){if(Cancel->load()){Error="cancelled";break;}const auto Chunk=File.read(64*1024);if(Chunk.isEmpty() || Data.size()+Chunk.size()>8*1024*1024){Error="edit-image-file-or-budget";break;}Data+=Chunk;}
            if(Error.isEmpty())Request["base64"]=QString::fromLatin1(Data.toBase64());
        }
        if(Error.isEmpty())Error=Model.Structure(Request,[Cancel]{return Cancel->load();});return StructureOutput{std::move(Model),Error,Selected};}));emit StateChanged();return true;
}
void EpubSourceEditor::addImageResource() {
    const auto FileName=QFileDialog::getOpenFileName(this,tr("EPUB에 이미지 추가"),{},tr("이미지 (*.png *.jpg *.jpeg *.gif *.webp)"));if(FileName.isEmpty())return;
    QDialog Dialog(this);Dialog.setWindowTitle(tr("이미지 추가"));auto* Form=new QFormLayout(&Dialog);auto* Path=new QLineEdit(QDir::cleanPath(QFileInfo(model_.Publication()["rootfile"].toString()).path()+"/Images/"+QFileInfo(FileName).fileName()),&Dialog);Path->setMaxLength(2048);Form->addRow(tr("EPUB 내부 경로"),Path);auto* Alt=new QLineEdit(&Dialog);Alt->setMaxLength(2048);Form->addRow(tr("대체 설명 · 장식 이미지는 빈 값"),Alt);
    auto* Insert=new QCheckBox(tr("현재 장 끝에 이미지 추가"),&Dialog);bool Chapter=false;for(const auto& Value:model_.Publication()["spine"].toArray())Chapter|=Value.toObject()["path"]==current_;Insert->setEnabled(Chapter);Insert->setChecked(Chapter);Form->addRow(Insert);auto* Cover=new QCheckBox(tr("이 이미지로 표지 교체"),&Dialog);Form->addRow(Cover);auto* Buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&Dialog);Buttons->button(QDialogButtonBox::Ok)->setText(tr("확인"));Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소"));Form->addRow(Buttons);connect(Buttons,&QDialogButtonBox::accepted,&Dialog,&QDialog::accept);connect(Buttons,&QDialogButtonBox::rejected,&Dialog,&QDialog::reject);
    if(Dialog.exec()==QDialog::Accepted)structure({{"operation","image-add"},{"resource",Path->text()},{"imageFile",FileName},{"alt",Alt->text()},{"chapter",Insert->isChecked()?current_:QString{}},{"cover",Cover->isChecked()}});
}
void EpubSourceEditor::convertImageResource() {
    QDialog Dialog(this);Dialog.setWindowTitle(tr("이미지 형식 변환"));auto* Form=new QFormLayout(&Dialog);auto* Format=new QComboBox(&Dialog);Format->addItem("PNG","png");Format->addItem("JPEG","jpeg");Form->addRow(tr("형식"),Format);auto* Quality=new QSpinBox(&Dialog);Quality->setRange(1,100);Quality->setValue(92);Form->addRow(tr("JPEG 품질"),Quality);auto* Path=new QLineEdit(&Dialog);Path->setMaxLength(2048);Form->addRow(tr("변환 후 EPUB 내부 경로"),Path);
    auto* Alpha=new QCheckBox(tr("JPEG 변환 시 투명 영역을 흰색으로 합치기"),&Dialog);Form->addRow(Alpha);auto* Animation=new QCheckBox(tr("애니메이션은 첫 프레임만 변환"),&Dialog);Form->addRow(Animation);
    const auto Update=[&]{const bool Jpeg=Format->currentData()=="jpeg";Quality->setEnabled(Jpeg);Alpha->setEnabled(Jpeg);Path->setText(QFileInfo(current_).path()+"/"+QFileInfo(current_).completeBaseName()+(Jpeg?".jpg":".png"));};connect(Format,&QComboBox::currentIndexChanged,&Dialog,Update);Update();auto* Buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&Dialog);Buttons->button(QDialogButtonBox::Ok)->setText(tr("확인"));Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소"));Form->addRow(Buttons);connect(Buttons,&QDialogButtonBox::accepted,&Dialog,&QDialog::accept);connect(Buttons,&QDialogButtonBox::rejected,&Dialog,&QDialog::reject);
    if(Dialog.exec()==QDialog::Accepted)structure({{"operation","image-convert"},{"resource",current_},{"destination",Path->text()},{"format",Format->currentData().toString()},{"quality",Quality->value()},{"flattenAlpha",Alpha->isChecked()},{"flattenAnimation",Animation->isChecked()}});
}
void EpubSourceEditor::addChapterResource() {
    QDialog Dialog(this);Dialog.setWindowTitle(tr("새 장 추가"));auto* Form=new QFormLayout(&Dialog);auto* Title=new QLineEdit(tr("새 장"),&Dialog);Title->setMaxLength(1024);Form->addRow(tr("제목"),Title);auto* Path=new QLineEdit(QFileInfo(model_.Publication()["rootfile"].toString()).path()+"/Text/chapter-"+QUuid::createUuid().toString(QUuid::Id128)+".xhtml",&Dialog);Path->setMaxLength(2048);Form->addRow(tr("EPUB 내부 경로"),Path);auto* Note=new QLabel(tr("선택한 장 뒤에 읽기 순서를 추가하고 목차에도 연결합니다. 장이 선택되지 않으면 마지막에 추가합니다."),&Dialog);Note->setWordWrap(true);Form->addRow(Note);auto* Buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&Dialog);Buttons->button(QDialogButtonBox::Ok)->setText(tr("확인"));Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소"));Form->addRow(Buttons);connect(Buttons,&QDialogButtonBox::accepted,&Dialog,&QDialog::accept);connect(Buttons,&QDialogButtonBox::rejected,&Dialog,&QDialog::reject);
    if(Dialog.exec()==QDialog::Accepted){const auto Spine=model_.Publication()["spine"].toArray();int Index=-1;for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==current_)Index=I;structure({{"operation","chapter-add"},{"resource",Path->text()},{"title",Title->text()},{"after",Index}});}
}
}
