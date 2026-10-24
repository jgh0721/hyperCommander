#include "stdafx.h"
#include "epubSourceEditor.hpp"
#include "documentThemeAdapter.hpp"
#include <Windows.h>
#include <QComboBox>
#include <QDir>
#include <QImageReader>
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QtConcurrentRun>
namespace hc::document {
namespace {
QByteArray identity(const QString& Path) {
    const auto Native=QDir::toNativeSeparators(QFileInfo(Path).absoluteFilePath());
    const auto Handle=CreateFileW(reinterpret_cast<LPCWSTR>(Native.utf16()),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(Handle==INVALID_HANDLE_VALUE)return {};FILE_ID_INFO Info{};const bool Ok=GetFileInformationByHandleEx(Handle,FileIdInfo,&Info,sizeof(Info));CloseHandle(Handle);return Ok?QByteArray(reinterpret_cast<const char*>(&Info),sizeof(Info)):QByteArray{};
}
}
EpubSourceEditor::EpubSourceEditor(QByteArray Bytes,const QString& Path,QWidget* Parent):QWidget(Parent) {
    protectedPaths_.append(QFileInfo(Path).absoluteFilePath().toCaseFolded());const auto Id=identity(Path);if(!Id.isEmpty())identities_.append(Id);
    baseToken_=QCryptographicHash::hash(Bytes,QCryptographicHash::Sha256);
    auto* Layout=new QVBoxLayout(this);auto* Note=new QLabel(tr("EPUB 소스 편집 · XHTML / CSS / OPF 메타데이터 / 이미지\n소스 초안의 자동 복구는 지원하지 않습니다. 원본 읽기 화면은 입력 파일을 표시합니다. 변경본은 검증 후 다른 이름으로 저장하고 다시 열어 확인합니다."),this);Note->setWordWrap(true);Layout->addWidget(Note);
    resources_=new QComboBox(this);resources_->setObjectName("epubEditResources");resources_->setAccessibleName(tr("편집할 EPUB 리소스"));resources_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);resources_->setMinimumContentsLength(24);Layout->addWidget(resources_);
    text_=new QPlainTextEdit(this);text_->setObjectName("epubEditSource");text_->setAccessibleName(tr("EPUB 리소스 소스"));text_->setLineWrapMode(QPlainTextEdit::NoWrap);text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));DocumentThemeAdapter::Attach(text_);Layout->addWidget(text_,1);
    image_=new QLabel(this);image_->setAlignment(Qt::AlignCenter);image_->setMinimumHeight(100);image_->hide();Layout->addWidget(image_,1);
    auto* Actions=new QHBoxLayout;auto* Apply=new QPushButton(tr("리소스 변경 반영"),this);auto* Replace=new QPushButton(tr("선택 이미지 교체…"),this);auto* Save=new QPushButton(tr("검증 후 다른 이름으로 저장…"),this);Actions->addWidget(Apply);Actions->addWidget(Replace);Actions->addWidget(Save);Layout->addLayout(Actions);
    status_=new QLabel(this);status_->setWordWrap(true);Layout->addWidget(status_);
    connect(Apply,&QPushButton::clicked,this,[this]{flush();});connect(Replace,&QPushButton::clicked,this,&EpubSourceEditor::importImage);connect(Save,&QPushButton::clicked,this,[this]{validate(true);});
    connect(resources_,&QComboBox::currentIndexChanged,this,[this]{if(!updating_){if(!flush()){const QSignalBlocker Block(resources_);resources_->setCurrentIndex(resources_->findText(current_));return;}select();}});
    connect(text_,&QPlainTextEdit::textChanged,this,[this]{if(updating_)return;if(text_->toPlainText().toUtf8().size()>2*1024*1024){const QSignalBlocker Block(text_);text_->undo();status_->setText(tr("소스 편집은 리소스당 2 MiB까지 지원합니다."));}emit Changed();});
    connect(&worker_,&QFutureWatcher<epub::EditModel>::finished,this,[this]{
        model_=worker_.result();ready_=!model_.Publication().isEmpty();error_=model_.Error();setEnabled(ready_);status_->setText(ready_?tr("변경 내용은 원본을 유지하고 사본으로 저장합니다. 출판용 EPUB 표준 검증은 별도입니다."):error_);
        if(ready_){reference_=model_.Fingerprint();updating_=true;resources_->clear();for(const auto& V:model_.Publication()["resources"].toArray()){const auto R=V.toObject();const auto Media=R["mediaType"].toString();if(QStringList{"application/xhtml+xml","application/oebps-package+xml","text/css","image/svg+xml","image/png","image/jpeg","image/gif"}.contains(Media))resources_->addItem(R["path"].toString(),Media);}updating_=false;current_.clear();select();}emit StateChanged();
    });
    connect(&validation_,&QFutureWatcher<epub::EditOutput>::finished,this,[this]{
        const auto Result=validation_.result();if(validationToken_!=Token()){status_->setText(tr("검사 중 소스가 변경되었습니다. 다시 검사해 주세요."));emit StateChanged();return;}
        if(!Result){error_=Result.error_+": "+Result.location_;status_->setText(tr("소스 검사를 통과하지 못했습니다.\n%1").arg(error_));validatedBytes_.clear();}
        else {error_.clear();validatedToken_=validationToken_;validatedBytes_=Result.bytes_;status_->setText(tr("기본 구조 검사 통과 · 다른 이름 저장 가능 · 출판용 EPUB 표준 검증은 별도입니다."));}
        emit StateChanged();if(Result && saveAfter_)emit SaveRequested();
    });load(std::move(Bytes));
}
EpubSourceEditor::~EpubSourceEditor(){if(cancel_)cancel_->store(true);}
void EpubSourceEditor::load(QByteArray Bytes) {
    ready_=false;setEnabled(false);status_->setText(tr("EPUB 편집 구조를 검사하고 있습니다…"));cancel_=std::make_shared<std::atomic_bool>(false);auto Cancel=cancel_;
    worker_.setFuture(QtConcurrent::run([Bytes=std::move(Bytes),Cancel]{epub::EditModel Model;Model.Load(Bytes,[Cancel]{return Cancel->load();});return Model;}));
}
QMap<QString,QByteArray> EpubSourceEditor::draft()const {if(ready_ && !current_.isEmpty() && !text_->isHidden() && text_->document()->isModified())return {{current_,text_->toPlainText().toUtf8()}};return {};}
QByteArray EpubSourceEditor::Token()const {if(!ready_)return baseToken_;const auto Value=model_.Fingerprint(draft());return Value==reference_?baseToken_:Value;}
bool EpubSourceEditor::flush(){if(!ready_)return false;const auto Draft=draft();for(auto I=Draft.begin();I!=Draft.end();++I){const auto Error=model_.Replace(I.key(),I.value());if(!Error.isEmpty()){status_->setText(Error);return false;}}if(!Draft.isEmpty()){text_->document()->setModified(false);text_->document()->clearUndoRedoStacks();emit Changed();}return true;}
void EpubSourceEditor::select() {
    if(!ready_)return;current_=resources_->currentText();const auto Data=model_.Read(current_);const auto Media=resources_->currentData().toString();const bool Raster=QStringList{"image/png","image/jpeg","image/gif"}.contains(Media);
    updating_=true;text_->setVisible(!Raster);image_->setVisible(Raster);
    if(Raster){QBuffer Buffer;Buffer.setData(Data);Buffer.open(QIODevice::ReadOnly);QImageReader Reader(&Buffer);const auto Size=Reader.size();const auto Image=Size.isValid() && Size.width()<=4096 && Size.height()<=4096 && qint64(Size.width())*Size.height()<=4194304?Reader.read():QImage{};image_->setPixmap(QPixmap::fromImage(Image.scaled(640,420,Qt::KeepAspectRatio,Qt::SmoothTransformation)));image_->setAccessibleName(tr("이미지 미리보기: %1").arg(current_));}
    else {text_->setPlainText(QString::fromUtf8(Data));text_->document()->setModified(false);}updating_=false;
}
void EpubSourceEditor::importImage() {
    if(!ready_ || !QStringList{"image/png","image/jpeg","image/gif"}.contains(resources_->currentData().toString())){status_->setText(tr("교체할 PNG·JPEG·GIF 리소스를 먼저 선택해 주세요. 경로와 형식은 유지합니다."));return;}
    const auto Path=QFileDialog::getOpenFileName(this,tr("같은 형식의 이미지로 교체"),{},tr("이미지 (*.png *.jpg *.jpeg *.gif)"));if(Path.isEmpty())return;QFile File(Path);if(!File.open(QIODevice::ReadOnly) || File.size()>8*1024*1024){status_->setText(tr("이미지 입력 한도는 8 MiB입니다."));return;}
    const auto Error=model_.Replace(current_,File.read(8*1024*1024+1));if(!Error.isEmpty()){status_->setText(Error);return;}select();emit Changed();
}
bool EpubSourceEditor::CanUndo()const{return ready_ && ((!text_->isHidden() && text_->document()->isUndoAvailable()) || model_.CanUndo());}
bool EpubSourceEditor::CanRedo()const{return ready_ && ((!text_->isHidden() && text_->document()->isRedoAvailable()) || model_.CanRedo());}
void EpubSourceEditor::Undo(){if(!ready_)return;if(!text_->isHidden() && text_->document()->isUndoAvailable())text_->undo();else{model_.Undo();select();emit Changed();}}
void EpubSourceEditor::Redo(){if(!ready_)return;if(!text_->isHidden() && text_->document()->isRedoAvailable())text_->redo();else{model_.Redo();select();emit Changed();}}
QJsonObject EpubSourceEditor::Snapshot()const {QJsonArray Rows;for(int I=0;I<resources_->count();++I)Rows.append(QJsonObject{{"path",resources_->itemText(I)},{"mediaType",resources_->itemData(I).toString()}});return {{"ready",ready_},{"error",error_},{"resources",Rows},{"current",current_},{"canUndo",CanUndo()},{"canRedo",CanRedo()},{"saveAsOnly",true},{"validating",validation_.isRunning()},{"validated",IsValidated()}};}
QJsonObject EpubSourceEditor::Execute(const QJsonObject& Request) {
    if(!ready_)return {{"error","epub-edit-not-ready"}};const auto Action=Request["action"].toString();
    if(Action=="epub.edit.validate"){validate(false);return {{"accepted",true}};}
    if(Action=="epub.edit.replace") {
        if(!flush())return {{"error","epub-edit-draft-invalid"}};const auto Path=Request["resource"].toString();const auto Index=resources_->findText(Path);if(Index<0)return {{"error","epub-edit-resource"}};
        const bool Raster=QStringList{"image/png","image/jpeg","image/gif"}.contains(resources_->itemData(Index).toString());
        QByteArray Data;if(Raster){const auto Decoded=QByteArray::fromBase64Encoding(Request["base64"].toString().toLatin1(),QByteArray::AbortOnBase64DecodingErrors);if(!Decoded)return {{"error","epub-edit-image-encoding"}};Data=Decoded.decoded;}else Data=Request["text"].toString().toUtf8();
        const auto Error=model_.Replace(Path,Data);if(!Error.isEmpty())return {{"error",Error}};
        {const QSignalBlocker Block(resources_);resources_->setCurrentIndex(Index);}select();emit Changed();return {{"accepted",true}};
    }
    return {{"error","epub-edit-action"}};
}
storage::Result<QByteArray> EpubSourceEditor::Export(const QString& Destination)const {
    if(!ready_ || QFileInfo(Destination).suffix().compare("epub",Qt::CaseInsensitive)!=0)return storage::Error{storage::ErrorCategory::Unsupported,"epub-edit-save-as-epub-required"};
    if(protectedPaths_.contains(QFileInfo(Destination).absoluteFilePath().toCaseFolded()) || (!identity(Destination).isEmpty() && identities_.contains(identity(Destination))))return storage::Error{storage::ErrorCategory::AccessDenied,"epub-edit-source-overwrite-denied"};
    if(!IsValidated())return storage::Error{storage::ErrorCategory::InvalidRequest,"epub-edit-validation-required"};return validatedBytes_;
}
void EpubSourceEditor::validate(bool SaveAfter) {
    if(!ready_ || validation_.isRunning())return;validatedBytes_.clear();validationToken_=Token();saveAfter_=SaveAfter;status_->setText(tr("패키지·XML·참조·이미지를 검사하고 있습니다…"));auto Model=model_;const auto Draft=draft();auto Cancel=cancel_;
    validation_.setFuture(QtConcurrent::run([Model=std::move(Model),Draft,Cancel]{return Model.Export(Draft,[Cancel]{return Cancel->load();});}));emit StateChanged();
}
void EpubSourceEditor::Commit(const QString& Path,const QByteArray& Bytes){baseToken_=Token();protectedPaths_.append(QFileInfo(Path).absoluteFilePath().toCaseFolded());const auto Id=identity(Path);if(!Id.isEmpty())identities_.append(Id);load(Bytes);}
}
