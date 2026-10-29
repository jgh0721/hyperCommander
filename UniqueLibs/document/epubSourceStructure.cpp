#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "UniqueLibs/qtMessageDialog.hpp"
#include "UniqueLibs/qtDialogFrame.hpp"
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
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrentRun>
namespace hc::document {
void EpubSourceEditor::setupStructure() {
    connect(&imageInspection_,&QFutureWatcherBase::finished,this,[this]{if(imageInspectionToken_!=Token()){report(tr("사용 위치 조사 중 내용이 변경되었습니다. 다시 확인해 주세요."));return;}editImageProperties(imageInspection_.result());});
    connect(&structureWorker_,&QFutureWatcher<StructureOutput>::finished,this,[this]{
        const auto Result=structureWorker_.result();structuring_=false;setSourceReadOnly(false);structureError_=Result.error_;
        if(!structureError_.isEmpty())report(structureError_=="cancelled"?tr("구조 작업을 취소했습니다. 편집 내용은 유지됩니다."):tr("구조 작업을 반영하지 못했습니다. 편집 내용은 유지됩니다. %1").arg(structureError_));
        else {model_=Result.model_;bookUndoPriority_=true;bookRedoPriority_=false;setDiagnostics({});checkerVerifiedToken_.clear();checkerState_="idle";refreshResources(Result.selected_);tocDraft_={};report(tr("구조 변경을 반영했습니다. 실행 취소와 다른 이름 저장을 사용할 수 있습니다. 저장 전 검사를 다시 진행해 주세요."));emit Changed();}
        emit StateChanged();
    });
}
void EpubSourceEditor::refreshResources(const QString& Preferred) {
    const auto Publication=model_.Publication();const auto Spine=Publication["spine"].toArray();QStringList Paths;for(const auto& Value:Spine)Paths.append(Value.toObject()["path"].toString());
    const auto PreviousPaths=property("epubSpinePaths").toStringList();const int PreviousChapter=previewSnapshot()["state"].toObject()["chapterIndex"].toInt(previewRestoreChapter_);const auto PreviousPath=PreviousChapter>=0 && PreviousChapter<PreviousPaths.size()?PreviousPaths[PreviousChapter]:QString{};
    int Chapter=Paths.indexOf(Preferred);if(Chapter<0)Chapter=Paths.indexOf(PreviousPath);if(Chapter<0 && !Paths.isEmpty())Chapter=qBound(0,PreviousChapter,Paths.size()-1);
    const auto Edition=Publication["sourceSha256"].toString();if(property("epubEditionHash").toString()!=Edition){recoveryLocator_.clear();previewRecoveryPosition_=true;previewRestoreChapter_=Chapter;}
    setProperty("epubEditionHash",Edition);setProperty("epubSpinePaths",Paths);
    // Renamed or removed resources leave the editor tabs; their buffers go with them.
    for(const auto& Path:sourceBuffers_.keys())if(!model_.IsEditable(Path))releaseSource(Path);
    openResources_.removeIf([this](const QString& Path){return !model_.IsEditable(Path);});
    QString Next=model_.IsEditable(Preferred)?Preferred:model_.IsEditable(current_)?current_:Chapter>=0 && model_.IsEditable(Paths[Chapter])?Paths[Chapter]:QString{};
    if(Next.isEmpty())for(const auto& Value:Publication["resources"].toArray())if(model_.IsEditable(Value.toObject()["path"].toString())){Next=Value.toObject()["path"].toString();break;}
    saveSourceState();refreshTree();current_=Next;select();
}
bool EpubSourceEditor::structure(QJsonObject Request) {
    if(!ready_ || structuring_ || nativeValidating_ || draftReader_.isRunning() || pendingRecovery_ || !pendingRecoveryPath_.isEmpty() || !flush())return false;
    if(checkerState_=="running")cancelEpubCheck("source-changed");validatedBytes_.clear();structureError_.clear();structureCancel_=std::make_shared<std::atomic_bool>(false);const auto Cancel=structureCancel_;
    const auto Operation=Request["operation"].toString();
    auto Selected=Operation=="rename" || Operation=="image-convert" || Operation=="duplicate"?Request["destination"].toString():Operation=="image-add" || Operation=="chapter-add" || Operation=="resource-add"?Request["resource"].toString():current_;
    auto Model=model_;setSourceReadOnly(true);report(tr("EPUB 구조와 참조를 갱신하고 있습니다…"));
    structuring_=true;structureWorker_.setFuture(QtConcurrent::run([Model=std::move(Model),Request=std::move(Request),Selected,Cancel]()mutable{
        QString Error;
        // Local files are read on the worker: the request carries only bounded Base64 data into the model.
        if((Request["operation"]=="image-add" && Request.contains("imageFile")) || (Request["operation"]=="resource-add" && Request.contains("file"))) {
            const auto Limit=Request["operation"]=="image-add"?8*1024*1024:32*1024*1024;QFile File(Request[Request.contains("imageFile")?"imageFile":"file"].toString());QByteArray Data;
            if(!QFileInfo(File.fileName()).isAbsolute() || !File.open(QIODevice::ReadOnly) || File.size()>Limit)Error="edit-resource-file-or-budget";
            while(Error.isEmpty() && !File.atEnd()){if(Cancel->load()){Error="cancelled";break;}const auto Chunk=File.read(64*1024);if(Chunk.isEmpty() || Data.size()+Chunk.size()>Limit){Error="edit-resource-file-or-budget";break;}Data+=Chunk;}
            if(Error.isEmpty())Request["base64"]=QString::fromLatin1(Data.toBase64());
        }
        if(Error.isEmpty())Error=Model.Structure(Request,[Cancel]{return Cancel->load();});return StructureOutput{std::move(Model),Error,Selected};}));emit StateChanged();return true;
}
void EpubSourceEditor::renameResource(const QString& Path) {
    if(Path.isEmpty())return;bool Ok=false;const auto Next=hc::ui::InputDialog::getText(this,tr("리소스 이름·경로 변경"),tr("EPUB 내부 경로 · 참조는 함께 갱신됩니다."),QLineEdit::Normal,Path,&Ok);
    if(Ok && Next!=Path)structure({{"operation","rename"},{"resource",Path},{"destination",Next}});
}
void EpubSourceEditor::removeResource(const QString& Path) {
    if(Path.isEmpty())return;
    QDialog Dialog(this);Dialog.setWindowTitle(tr("EPUB 리소스 삭제"));auto* Layout=new QVBoxLayout(&Dialog);auto* Note=new QLabel(tr("%1\n리소스를 삭제합니다. 남아 있는 참조가 있으면 작업을 중단합니다. 실행 취소가 가능하며 원본 파일은 유지됩니다.").arg(Path),&Dialog);Note->setTextFormat(Qt::PlainText);Note->setWordWrap(true);Layout->addWidget(Note);auto* Images=new QCheckBox(tr("이 이미지를 표시하는 img·SVG image 요소도 삭제"),&Dialog);Images->setVisible(mediaOf(Path).startsWith("image/"));Layout->addWidget(Images);auto* Buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&Dialog);Buttons->button(QDialogButtonBox::Ok)->setText(tr("확인"));Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소"));Layout->addWidget(Buttons);connect(Buttons,&QDialogButtonBox::accepted,&Dialog,&QDialog::accept);connect(Buttons,&QDialogButtonBox::rejected,&Dialog,&QDialog::reject);
    hc::ui::ApplyDialogFrame(&Dialog,Buttons,{{},tr("선택한 리소스를 EPUB에서 삭제합니다."),fm::ui::glyph::Trash});
    if(Dialog.exec()==QDialog::Accepted)structure({{"operation","remove"},{"resource",Path},{"removeImageReferences",Images->isChecked()}});
}
void EpubSourceEditor::moveChapter(const QString& Path,int Delta){if(!Path.isEmpty())structure({{"operation","chapter-move"},{"resource",Path},{"delta",Delta}});}
void EpubSourceEditor::addImageResource(const QString& Chapter) {
    const auto FileName=QFileDialog::getOpenFileName(this,tr("EPUB에 이미지 추가"),{},tr("이미지 (*.png *.jpg *.jpeg *.gif *.webp)"));if(FileName.isEmpty())return;
    const auto Target=Chapter.isEmpty()?current_:Chapter;
    QDialog Dialog(this);Dialog.setWindowTitle(tr("이미지 추가"));auto* Form=new QFormLayout(&Dialog);auto* Path=new QLineEdit(QDir::cleanPath(QFileInfo(model_.Publication()["rootfile"].toString()).path()+"/Images/"+QFileInfo(FileName).fileName()),&Dialog);Path->setMaxLength(2048);Form->addRow(tr("EPUB 내부 경로"),Path);auto* Alt=new QLineEdit(&Dialog);Alt->setMaxLength(2048);Form->addRow(tr("대체 설명 · 장식 이미지는 빈 값"),Alt);
    auto* Insert=new QCheckBox(tr("%1 끝에 이미지 추가").arg(QFileInfo(Target).fileName()),&Dialog);bool InSpine=false;for(const auto& Value:model_.Publication()["spine"].toArray())InSpine|=Value.toObject()["path"]==Target;Insert->setEnabled(InSpine);Insert->setChecked(InSpine);Form->addRow(Insert);auto* Cover=new QCheckBox(tr("이 이미지로 표지 교체"),&Dialog);Form->addRow(Cover);auto* Buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&Dialog);Buttons->button(QDialogButtonBox::Ok)->setText(tr("확인"));Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소"));Form->addRow(Buttons);connect(Buttons,&QDialogButtonBox::accepted,&Dialog,&QDialog::accept);connect(Buttons,&QDialogButtonBox::rejected,&Dialog,&QDialog::reject);
    hc::ui::ApplyDialogFrame(&Dialog,Buttons,{{},tr("선택한 이미지 파일을 EPUB 리소스로 추가합니다."),fm::ui::glyph::PlusSmall});
    if(Dialog.exec()==QDialog::Accepted)structure({{"operation","image-add"},{"resource",Path->text()},{"imageFile",FileName},{"alt",Alt->text()},{"chapter",Insert->isChecked()?Target:QString{}},{"cover",Cover->isChecked()}});
}
void EpubSourceEditor::convertImageResource(const QString& Source) {
    if(Source.isEmpty())return;
    QDialog Dialog(this);Dialog.setWindowTitle(tr("이미지 형식 변환"));auto* Form=new QFormLayout(&Dialog);auto* Format=new QComboBox(&Dialog);Format->addItem("PNG","png");Format->addItem("JPEG","jpeg");Form->addRow(tr("형식"),Format);auto* Quality=new QSpinBox(&Dialog);Quality->setRange(1,100);Quality->setValue(92);Form->addRow(tr("JPEG 품질"),Quality);auto* Path=new QLineEdit(&Dialog);Path->setMaxLength(2048);Form->addRow(tr("변환 후 EPUB 내부 경로"),Path);
    auto* Alpha=new QCheckBox(tr("JPEG 변환 시 투명 영역을 흰색으로 합치기"),&Dialog);Form->addRow(Alpha);auto* Animation=new QCheckBox(tr("애니메이션은 첫 프레임만 변환"),&Dialog);Form->addRow(Animation);
    const auto Update=[&]{const bool Jpeg=Format->currentData()=="jpeg";Quality->setEnabled(Jpeg);Alpha->setEnabled(Jpeg);Path->setText(QFileInfo(Source).path()+"/"+QFileInfo(Source).completeBaseName()+(Jpeg?".jpg":".png"));};connect(Format,&QComboBox::currentIndexChanged,&Dialog,Update);Update();auto* Buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&Dialog);Buttons->button(QDialogButtonBox::Ok)->setText(tr("확인"));Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소"));Form->addRow(Buttons);connect(Buttons,&QDialogButtonBox::accepted,&Dialog,&QDialog::accept);connect(Buttons,&QDialogButtonBox::rejected,&Dialog,&QDialog::reject);
    hc::ui::ApplyDialogFrame(&Dialog,Buttons,{{},tr("선택한 이미지를 PNG 또는 JPEG로 변환합니다."),fm::ui::glyph::File});
    if(Dialog.exec()==QDialog::Accepted)structure({{"operation","image-convert"},{"resource",Source},{"destination",Path->text()},{"format",Format->currentData().toString()},{"quality",Quality->value()},{"flattenAlpha",Alpha->isChecked()},{"flattenAnimation",Animation->isChecked()}});
}
void EpubSourceEditor::addChapterResource(const QString& After) {
    const auto Spine=model_.Publication()["spine"].toArray();int Index=-1;for(int I=0;I<Spine.size();++I)if(Spine[I].toObject()["path"]==(After.isEmpty()?current_:After))Index=I;
    QDialog Dialog(this);Dialog.setWindowTitle(tr("새 장 추가"));auto* Form=new QFormLayout(&Dialog);auto* Title=new QLineEdit(tr("새 장"),&Dialog);Title->setMaxLength(1024);Form->addRow(tr("제목"),Title);auto* Path=new QLineEdit(QFileInfo(model_.Publication()["rootfile"].toString()).path()+"/Text/chapter-"+QUuid::createUuid().toString(QUuid::Id128)+".xhtml",&Dialog);Path->setMaxLength(2048);Form->addRow(tr("EPUB 내부 경로"),Path);
    auto* Note=new QLabel(Index>=0?tr("%1 뒤에 읽기 순서를 추가하고 목차에도 연결합니다.").arg(QFileInfo(Spine[Index].toObject()["path"].toString()).fileName()):tr("읽기 순서 마지막에 추가하고 목차에도 연결합니다."),&Dialog);Note->setWordWrap(true);Form->addRow(Note);auto* Buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&Dialog);Buttons->button(QDialogButtonBox::Ok)->setText(tr("확인"));Buttons->button(QDialogButtonBox::Cancel)->setText(tr("취소"));Form->addRow(Buttons);connect(Buttons,&QDialogButtonBox::accepted,&Dialog,&QDialog::accept);connect(Buttons,&QDialogButtonBox::rejected,&Dialog,&QDialog::reject);
    hc::ui::ApplyDialogFrame(&Dialog,Buttons,{{},tr("선택한 장 다음에 새 장을 추가합니다."),fm::ui::glyph::NewFile});
    if(Dialog.exec()==QDialog::Accepted)structure({{"operation","chapter-add"},{"resource",Path->text()},{"title",Title->text()},{"after",Index}});
}
}
