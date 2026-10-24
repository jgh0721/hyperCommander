#include "documentViewPane.hpp"
#include "epubProcessView.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include "documentThemeAdapter.hpp"
#include "imageReadingView.hpp"
#include "imageFramePlayer.hpp"
#include "documentTextCodec.hpp"
#include "text/QTextView.h"
#include "text/ScintillaQtDirectBackend.h"
#include <Scintilla.h>
#include <QJsonArray>
#include <algorithm>
#include "pdf/QPDFView.h"
#include "image/QImageView.h"
#include "core/ThemeManager.h"
#include "core/ToolbarIcons.h"
#include "core/PrintManager.h"
#include "core/WatermarkDialog.h"
#include <fmstyle/ThemeManager.h>
#include <QApplication>
#include <QPlainTextEdit>
#include <QToolBar>
#include <QVBoxLayout>
#include <QComboBox>
#include <QSpinBox>
#include <QInputDialog>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QFileInfo>
#include <QMessageBox>
#include <QMenu>
#include <QToolButton>
#include <QFontDatabase>
#include <QBuffer>
#include <QCryptographicHash>
#include <QTemporaryFile>
#include <QPrinter>
#include <QTimer>
#include <QFile>
#include <QStandardPaths>
#include <qt_windows.h>
#include <Scintilla.h>
void InitializePortedViewResources();
namespace hc::document
{
    DocumentViewPane::DocumentViewPane(QWidget* Parent):QWidget(Parent)
    {
        epubRuntime_ = MakeProductEpubRuntime(QCoreApplication::applicationDirPath());
        InitializePortedViewResources();setObjectName("documentViewPane");
        auto* Layout=new QVBoxLayout(this);Layout->setContentsMargins(0,0,0,0);Layout->setSpacing(0);
        // Retain the P1 control adapter for callers; Scintilla owns visible text,
        // selection, byte positions, folds and history.
        editor_=new QPlainTextEdit(this);
        // This hidden P1 compatibility adapter never paints. Laying out its
        // duplicate of a long Scintilla line stalls the helper control channel.
        editor_->document()->setLayoutEnabled(false); editor_->setLineWrapMode(QPlainTextEdit::NoWrap);editor_->setObjectName("textDocument");editor_->setReadOnly(true);editor_->hide();DocumentThemeAdapter::Attach(editor_);
        connect(editor_,&QPlainTextEdit::textChanged,this,[this] {
            if(syncing_||!text_)return;QScopedValueRollback<bool> Guard(syncing_,true);
            const auto Text=editor_->document()->toRawText().replace(QChar(0x2029),'\n');
            editor_->setProperty("scalarText",Text);auto Normalized=Text;if(eol_=="CRLF")Normalized.replace("\r\n","\n").replace("\n","\r\n");text_->ReplaceContent(Normalized);emit ContentChanged();
        });
        connect(editor_,&QPlainTextEdit::selectionChanged,this,[this] {
            if(syncing_||!text_)return;const auto Cursor=editor_->textCursor();const auto Text=editor_->document()->toRawText();
            text_->Backend()->setSelectionByPos(Text.left(Cursor.selectionStart()).toUtf8().size(),Text.left(Cursor.selectionEnd()).toUtf8().size());
        });
        auto& Theme=fm::style::ThemeManager::instance();
        connect(&Theme,&fm::style::ThemeManager::changed,this,&DocumentViewPane::applyTheme);
        connect(&Theme,&fm::style::ThemeManager::fontsChanged,this,&DocumentViewPane::applyTheme);
    }
    void DocumentViewPane::SetReadingSource(const QString& Path,const QString& Entry){
        readingPath_=Path;readingEntry_=Entry;
        if(image_&&Entry.isEmpty())image_->SetNavigationSource(Path);
        if(reading_&&!reading_->isHidden()&&reading_->State()["pageCount"].toInt()>0&&!Path.isEmpty()&&reading_->State()["book"].toString()!=Path)
            reading_->OpenPath(Path,Entry,reading_->State()["mode"].toInt());
    }
    bool DocumentViewPane::Load(const TextInput& Input,bool Traverse,bool ImageEditing)
    {
        const QSignalBlocker Block(this),EditorBlock(editor_);
        viewId_=Input.viewId_;encoding_=Input.encoding_;bom_=Input.bom_;eol_=Input.eol_;
        originalImageBytes_ = viewId_ == "image" ? Input.bytes_ : QByteArray{};
        originalImageHash_ = viewId_ == "image" ? Input.hash_ : QByteArray{};
        originalImagePath_ = viewId_ == "image" ? Input.path_ : QString{};
        if(auto* EditBar=findChild<QToolBar*>("imageEditToolbar"))delete EditBar;
        if(animation_){delete animation_;animation_=nullptr;}
        if(reading_){delete reading_;reading_=nullptr;}
        if(view_) {delete view_;view_=nullptr;text_=nullptr;pdf_=nullptr;image_=nullptr;}
        if(toolbar_) {delete toolbar_;toolbar_=nullptr;}
        if(hwp_){delete hwp_;hwp_=nullptr;}
        if(epub_){delete epub_;epub_=nullptr;}
        epubClosing_=false;
        if(IsEpub()) {
            epubSourceHash_=Input.hash_;
            epub_=new EpubProcessView(epubRuntime_, {12, 1024ULL*1024*1024, 20000}, this);
            layout()->addWidget(epub_);epub_->show();
            contribution_={};contribution_.viewId_="epub";contribution_.menuTitle_=tr("EPUB");
            connect(epub_,&EpubProcessView::EscapeRequested,this,&DocumentViewPane::EscapeRequested);
            connect(epub_,&EpubProcessView::ViewStateChanged,this,&DocumentViewPane::ViewStateChanged);
            connect(epub_,&EpubProcessView::Loaded,this,[this]{if(isVisible())epub_->FocusInput();});
            return epub_->Load(Input.bytes_);
        }
        if(IsHwp()) {
            hwpFocusPending_=true;
            hwp_=new HwpOpenWindow(Input.path_,QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/rhwp",hwpRuntime_);
            hwp_->setParent(this,Qt::Widget);hwp_->setAttribute(Qt::WA_QuitOnClose,false);
            // The workspace owns the print command and its shortcut.
            if(auto* Print=hwp_->findChild<QAction*>("rhwp.nativePrint")){Print->setShortcut({});hwp_->removeAction(Print);}
            hwp_->setContextMenuPolicy(Qt::NoContextMenu);
            layout()->addWidget(hwp_);hwp_->show();
            contribution_={};contribution_.viewId_=viewId_;contribution_.menuTitle_=tr("한글 문서");
            connect(hwp_,&HwpOpenWindow::StateChanged,this,[this]{
                emit ViewStateChanged();
                if(hwpFocusPending_ && hwp_->Document() && hwp_->Document()->Snapshot().lifecycle_==DocumentLifecycle::Ready){
                    hwpFocusPending_=false;if(isVisible())Focus();
                }
            });
            connect(hwp_,&HwpOpenWindow::Closed,this,&DocumentViewPane::ViewStateChanged);
            QTimer::singleShot(0,hwp_,&HwpOpenWindow::Open);
            return true;
        }
        bool Success=true;
        if(viewId_=="pdf") {pdf_=new QPDFView(this);view_=pdf_;Success=pdf_->LoadContent(Input.bytes_,Input.path_);}
        else if(viewId_=="image") {image_=new QImageView(this);view_=image_;image_->LoadContent(Input.image_,Input.path_,Traverse);if(readingEntry_.isEmpty()&&!readingPath_.isEmpty())image_->SetNavigationSource(readingPath_);}
        else {text_=new QTextView(this);view_=text_;text_->LoadContent(Input.text_,Input.path_,Input.encoding_);editor_->setProperty("scalarText",Input.text_);editor_->setPlainText(Input.text_);editor_->setReadOnly(false);}
        if(!Success)return false;
        toolbar_=view_->createToolBar();
        if(!toolbar_)toolbar_=new QToolBar(this);
        toolbar_->setObjectName("viewToolbar");layout()->addWidget(toolbar_);
        connect(view_, &QBaseView::ClipboardRequested, this, &DocumentViewPane::ClipboardRequested);
        if(pdf_) {auto* Auxiliary=view_->createAuxiliaryToolBar();if(Auxiliary){layout()->addWidget(Auxiliary);pdf_->ShowAnnotationToolBar(true);}}
        layout()->addWidget(view_);
        contribution_.viewId_=viewId_;contribution_.menuTitle_=viewId_=="pdf"?tr("PDF"):viewId_=="image"?tr("이미지"):tr("텍스트");
        contribution_.menuActions_=toolbar_->actions();
        if(text_) {
            connect(text_,&QTextView::ContentChanged,this,[this]{syncText();emit ContentChanged();});
            connect(text_,&QTextView::EncodingRequested,this,&DocumentViewPane::EncodingRequested);
            auto* Encoding=new QComboBox(toolbar_);Encoding->setObjectName("documentEncoding");Encoding->setAccessibleName(tr("저장 인코딩"));Encoding->addItems({"UTF-8","UTF-16LE","UTF-16BE","CP949"});Encoding->setCurrentText(encoding_);
            toolbar_->addWidget(Encoding);
            connect(Encoding,&QComboBox::textActivated,this,[this](const QString& Value) {
                // Validate conversion before changing output settings. The user
                // keeps editing if a legacy encoding cannot represent a scalar.
                auto Bytes=EncodeDocumentText(text_->Content(),Value,bom_);
                if(Bytes.HasValue()){encoding_=Value;emit ContentChanged();}
                else EncodingRequested(Value);
            });
            auto* Bom = new QComboBox(toolbar_); Bom->setObjectName("documentBom"); Bom->setAccessibleName(tr("저장 BOM"));
            Bom->addItem(tr("BOM 제외"), false); Bom->addItem(tr("BOM 포함"), true); Bom->setCurrentIndex(bom_ ? 1 : 0); Bom->setEnabled(encoding_ != "CP949"); toolbar_->addWidget(Bom);
            connect(Bom, &QComboBox::activated, this, [this, Bom] { bom_ = Bom->currentData().toBool(); emit ContentChanged(); });
            connect(Encoding, &QComboBox::textActivated, this, [this, Bom] { Bom->setEnabled(encoding_ != "CP949"); });
            connect(text_,&QTextView::statusChanged,this,&DocumentViewPane::ViewStateChanged);
        }
        if(pdf_){connect(pdf_,&QPDFView::ContentChanged,this,&DocumentViewPane::ContentChanged);connect(pdf_,&QPDFView::viewStateChanged,this,&DocumentViewPane::ViewStateChanged);}
        if(image_) {
            const auto OriginalActions=toolbar_->actions();
            reading_=new ImageReadingView(this);reading_->hide();layout()->addWidget(reading_);
            animation_=new ImageFramePlayer(this);connect(animation_,&ImageFramePlayer::Frame,image_,&QImageView::DisplayAnimationFrame);
            connect(image_,&QImageView::ContentChanged,animation_,[this]{animation_->Play(false);});
            auto* Play=toolbar_->addAction(tr("재생/정지"));Play->setShortcut(QKeySequence("Alt+Pause"));Play->setShortcutContext(Qt::WidgetWithChildrenShortcut);addAction(Play);
            connect(Play,&QAction::triggered,animation_,[this]{if(!reading_->isHidden())reading_->ToggleAnimation();else animation_->Play(!animation_->Playing());});
            for(int Direction:{-1,1}){auto* Step=toolbar_->addAction(Direction<0?tr("이전 프레임"):tr("다음 프레임"));Step->setObjectName(Direction<0?"imageFramePrevious":"imageFrameNext");connect(Step,&QAction::triggered,animation_,[this,Direction]{if(!reading_->isHidden())reading_->StepAnimation(Direction);else animation_->Step(Direction);});}
            if(Input.bytes_.startsWith("GIF")||(Input.bytes_.startsWith("RIFF")&&Input.bytes_.mid(8,4)=="WEBP"))animation_->Load(Input.bytes_);
            auto* Mode=new QComboBox(toolbar_);Mode->setObjectName("imageReadingMode");Mode->setAccessibleName(tr("이미지 보기 모드"));Mode->addItems({tr("일반 이미지"),tr("코믹"),tr("웹툰")});toolbar_->insertWidget(toolbar_->actions().value(0),Mode);
            const auto Book=Input.book_;const auto Path=Input.path_;const auto First=Input.image_;
            connect(Mode,&QComboBox::activated,this,[this,Book,Path,First,Mode](int Value){
                if(image_->isModified()||image_->IsEditing()) {const QSignalBlocker Block(Mode);Mode->setCurrentIndex(0);QMessageBox::information(this,tr("읽기 모드"),tr("편집한 이미지를 저장한 후 읽기 모드로 전환하십시오."));return;}
                auto* Edit=findChild<QAction*>("imageEditMode");
                auto* EditBar=findChild<QToolBar*>("imageEditToolbar");
                auto* Fit=findChild<QComboBox*>("imageReadingFit");
                if(Value==0&&!Book&&reading_->State()["pageCount"].toInt()==0){reading_->hide();image_->show();if(Fit->count()==4)Fit->addItem(tr("늘림"));Edit->setEnabled(true);EditBar->setVisible(Edit->isChecked());return;}
                if(reading_->State()["pageCount"].toInt()==0){
                    if(Book)reading_->Load(Book,First);
                    else reading_->OpenPath(readingPath_.isEmpty()?Path:readingPath_,readingEntry_,Value);
                }
                animation_->Clear();image_->setEditTool(QImageView::None);image_->hide();EditBar->hide();Edit->setEnabled(false);reading_->show();Fit->removeItem(4);reading_->SetMode(Value==2?ImageReadingView::Webtoon:Value==1?ImageReadingView::Comic:ImageReadingView::Single);reading_->setFocus();emit ViewStateChanged();
            });
            connect(reading_,&ImageReadingView::Changed,this,[this]{if(!reading_->isHidden())emit ReadingSelected();emit ViewStateChanged();});
            const auto ReadingOptionsStart=toolbar_->actions().size();
            auto* Rtl=toolbar_->addAction(tr("우→좌"));Rtl->setCheckable(true);Rtl->setObjectName("imageRightToLeft");connect(Rtl,&QAction::toggled,reading_,&ImageReadingView::SetRightToLeft);
            auto* Cover=toolbar_->addAction(tr("표지 단독"));Cover->setObjectName("imageCover");Cover->setCheckable(true);Cover->setChecked(true);connect(Cover,&QAction::toggled,reading_,&ImageReadingView::SetCover);
            auto* Split=toolbar_->addAction(tr("펼침면 분할"));Split->setObjectName("imageSplit");Split->setCheckable(true);connect(Split,&QAction::toggled,reading_,&ImageReadingView::SetSplit);
            auto* Auto=toolbar_->addAction(tr("다음 권 자동"));Auto->setObjectName("imageAutoNext");Auto->setCheckable(true);Auto->setChecked(true);connect(Auto,&QAction::toggled,reading_,&ImageReadingView::SetAutomaticNext);
            auto* NextVolume=toolbar_->addAction(tr("다음 권"));connect(NextVolume,&QAction::triggered,reading_,[this]{reading_->OpenVolume(1);});
            auto* EditCurrent=toolbar_->addAction(tr("현재 이미지 사본 편집"));connect(EditCurrent,&QAction::triggered,reading_,[this]{const auto Image=!reading_->isHidden()?reading_->CurrentImage():image_->Content();if(!Image.isNull())emit EditImageRequested(Image);});
            for(const auto& Item:{std::pair{"grayscale",tr("회색조")},std::pair{"invert",tr("색 반전")}}){auto* A=toolbar_->addAction(Item.second);A->setCheckable(true);connect(A,&QAction::toggled,reading_,[this,Name=QString(Item.first)](bool On){reading_->SetAdjustment(Name,On?1:0);});}
            for(const auto& Item:{std::pair{"brightness",tr("밝기")},std::pair{"contrast",tr("대비")},std::pair{"gamma",tr("감마")},std::pair{"sharpness",tr("선명화")}}){auto* A=toolbar_->addAction(Item.second);connect(A,&QAction::triggered,reading_,[this,Name=QString(Item.first),Label=Item.second]{bool Ok=false;const double V=QInputDialog::getDouble(this,Label,Label,Name=="contrast"||Name=="gamma"?1:0,Name=="brightness"?-1:Name=="gamma"||Name=="contrast"?.1:0,Name=="gamma"?5:3,2,&Ok);if(Ok)reading_->SetAdjustment(Name,V);});}
            auto* ReadingOptions=new QMenu(tr("읽기 설정"),toolbar_);ReadingOptions->setObjectName("imageReadingOptions");
            for(auto* A:toolbar_->actions())if(A->objectName().startsWith("imageFrame")){toolbar_->removeAction(A);ReadingOptions->addAction(A);}
            const auto AllActions=toolbar_->actions();for(int I=int(ReadingOptionsStart)-2;I<AllActions.size();++I){toolbar_->removeAction(AllActions[I]);ReadingOptions->addAction(AllActions[I]);}
            auto* OptionsButton=new QToolButton(toolbar_);OptionsButton->setText(tr("읽기 설정"));OptionsButton->setPopupMode(QToolButton::InstantPopup);OptionsButton->setMenu(ReadingOptions);toolbar_->addWidget(OptionsButton);
            connect(image_,&QImageView::ContentChanged,this,&DocumentViewPane::ContentChanged);connect(image_,&QImageView::viewStateChanged,this,&DocumentViewPane::ViewStateChanged);
            auto* EditToolbar=new QToolBar(tr("이미지 편집"),this);EditToolbar->setObjectName("imageEditToolbar");
            auto* Edit=toolbar_->addAction(ToolbarIcons::iconByName("annotation-freetext"),tr("편집"));
            Edit->setObjectName("imageEditMode");Edit->setProperty("documentIconName","annotation-freetext");
            Edit->setToolTip(tr("이미지 편집 도구모음 표시/숨기기"));Edit->setCheckable(true);Edit->setChecked(ImageEditing);
            if(auto* Button=qobject_cast<QToolButton*>(toolbar_->widgetForAction(Edit)))Button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            connect(Edit,&QAction::toggled,this,[this,EditToolbar](bool Enabled){
                EditToolbar->setVisible(Enabled);
                // A hidden drawing toolbar must not leave an active canvas tool.
                if(!Enabled)image_->setEditTool(QImageView::None);
                emit ViewStateChanged();
            });
            for(auto* Action:OriginalActions){
                const auto Name=Action->objectName();
                if(Name=="image.previous"||Name=="image.next"){
                    disconnect(Action,nullptr,image_,nullptr);const auto Direction=Name=="image.next"?1:-1;
                    connect(Action,&QAction::triggered,this,[this,Direction]{if(!reading_->isHidden())reading_->Move(Direction);else if(Direction>0)image_->nextImage();else image_->previousImage();});
                } else {toolbar_->removeAction(Action);EditToolbar->addAction(Action);}
            }
            auto* Fit=new QComboBox(toolbar_);Fit->setObjectName("imageReadingFit");Fit->addItems({tr("원본 크기"),tr("창에 맞춤"),tr("너비 맞춤"),tr("높이 맞춤"),tr("늘림")});Fit->setCurrentIndex(1);toolbar_->addWidget(Fit);
            connect(Fit,&QComboBox::activated,this,[this](int Value){if(!reading_->isHidden())reading_->SetFit(Value);else image_->setFitMode(static_cast<QImageView::FitMode>(Value));});
            for(int Direction:{-1,1}){auto* Zoom=toolbar_->addAction(Direction>0?tr("확대"):tr("축소"));connect(Zoom,&QAction::triggered,this,[this,Direction]{if(!reading_->isHidden())reading_->Zoom(Direction>0?1.25:.8);else if(Direction>0)image_->setZoomLevel(image_->zoomLevel()*1.25);else image_->setZoomLevel(image_->zoomLevel()*.8);});}
            auto* Gap=ReadingOptions->addAction(tr("페이지 간격"));connect(Gap,&QAction::triggered,this,[this]{bool Ok=false;const auto Value=QInputDialog::getInt(this,tr("페이지 간격"),tr("간격 (DIP)"),reading_->State()["gap"].toInt(),0,200,1,&Ok);if(Ok)reading_->SetGap(Value);});
            auto* Bookmark=ReadingOptions->addAction(tr("책갈피 저장"));connect(Bookmark,&QAction::triggered,reading_,&ImageReadingView::Bookmark);
            auto* OpenBookmark=ReadingOptions->addAction(tr("책갈피 이동"));connect(OpenBookmark,&QAction::triggered,reading_,&ImageReadingView::OpenBookmark);
            auto* Private=ReadingOptions->addAction(tr("시크릿 읽기"));Private->setCheckable(true);connect(Private,&QAction::toggled,reading_,&ImageReadingView::SetPrivate);
            static_cast<QVBoxLayout*>(layout())->insertWidget(1,EditToolbar);
            EditToolbar->setVisible(ImageEditing);
            auto* Filters=new QComboBox(toolbar_);Filters->setObjectName("imageDisplayFilter");Filters->addItem(tr("자동"),"auto");
            for(const auto& Filter:ImageFilterRegistry::Filters())Filters->addItem(Filter.label_,Filter.id_);
            auto* Quality=new QSpinBox(toolbar_);Quality->setObjectName("imageExportQuality");Quality->setRange(0,100);Quality->setValue(95);Quality->setPrefix(tr("품질 "));EditToolbar->addWidget(Quality);
            connect(Quality,&QSpinBox::valueChanged,this,[this](int Value){quality_=Value;});
            auto* Resize=EditToolbar->addAction(tr("출력 크기"));
            connect(Resize,&QAction::triggered,this,[this] {
                bool Accepted=false;const auto Current=image_->Content().size();
                const int Width=QInputDialog::getInt(this,tr("출력 크기"),tr("너비"),Current.width(),1,16384,1,&Accepted);if(!Accepted)return;
                const int Height=QInputDialog::getInt(this,tr("출력 크기"),tr("높이"),Current.height(),1,16384,1,&Accepted);if(Accepted)image_->ApplyResize(QSize(Width,Height),image_->DisplayFilter());
            });
            toolbar_->insertWidget(toolbar_->actions().value(0),Filters);connect(Filters,&QComboBox::activated,image_,[this,Filters](int Index){image_->SetDisplayFilter(Filters->itemData(Index).toString());if(reading_)reading_->SetFilter(Filters->itemData(Index).toString());});
            connect(reading_,&ImageReadingView::Changed,this,[this,Mode,Rtl,Cover,Split,Auto,Filters,Fit]{
                if(!!reading_->isHidden()||!reading_->State()["pageCount"].toInt())return;const auto S=reading_->State();
                const QSignalBlocker B1(Mode),B2(Rtl),B3(Cover),B4(Split),B5(Auto),B6(Filters),B7(Fit);
                Mode->setCurrentIndex(S["mode"].toInt());Rtl->setChecked(S["rtl"].toBool());Cover->setChecked(S["cover"].toBool());Split->setChecked(S["split"].toBool());Auto->setChecked(S["automaticNext"].toBool());Filters->setCurrentIndex(Filters->findData(S["filter"].toString()));Fit->setCurrentIndex(S["fit"].toInt());
            });
            if(Book){reading_->Load(Book,First);const int Value=reading_->property("readingHistoryRestored").toBool()?reading_->State()["mode"].toInt():1;Mode->setCurrentIndex(Value);emit Mode->activated(Value);EditToolbar->hide();if(!readingPath_.isEmpty()&&readingPath_!=Book->path_)reading_->OpenPath(readingPath_,readingEntry_,Value);}
        }
        connect(view_,&QBaseView::OpenRequested,this,&DocumentViewPane::OpenRequested);
        connect(view_,&QBaseView::SaveRequested,this,&DocumentViewPane::SaveRequested);
        contribution_.menuActions_=toolbar_->actions();applyTheme();return true;
    }
    void DocumentViewPane::syncText()
    {
        if(syncing_||!text_)return;QScopedValueRollback<bool> Guard(syncing_,true);
        const QSignalBlocker Block(editor_);const auto Text=text_->Content();editor_->setProperty("scalarText",Text);editor_->setPlainText(Text);
    }
    QByteArray DocumentViewPane::Content() const
    {
        if(text_){auto Bytes=EncodeDocumentText(text_->Content(),encoding_,bom_);return Bytes.HasValue()?Bytes.Value():text_->Content().toUtf8();}
        return pdf_?pdf_->SerializeContent():image_?(image_->isModified()?image_->SerializeContent():originalImageBytes_):QByteArray{};
}
    QByteArray DocumentViewPane::SessionContent() const
    {
        if(epub_)return "epub:"+epubSourceHash_.toHex();
        if (!image_) return Content();
        if (!image_->isModified()) return "image:" + originalImageHash_.toHex();
        const auto Bytes = image_->SerializeContent();
        return Bytes.isEmpty() ? QByteArray{} : "image:" + QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex();
    }
    storage::Result<QByteArray> DocumentViewPane::Export(const QString& Destination) const
    {
        if(epub_)return storage::Error{storage::ErrorCategory::Unsupported,"epub-read-only"};
        if(reading_&&!reading_->isHidden()){
            const auto Image=reading_->CurrentImage();if(Image.isNull())return storage::Error{storage::ErrorCategory::Unavailable,"image-page-not-ready"};
            QByteArray Bytes;QBuffer Buffer(&Bytes);Buffer.open(QIODevice::WriteOnly);
            if(!Image.save(&Buffer,QFileInfo(Destination).suffix().toLatin1().constData(),quality_))return storage::Error{storage::ErrorCategory::Unsupported,"document-serialization-or-format"};return Bytes;
        }
        if(text_)return EncodeDocumentText(text_->Content(),encoding_,bom_);
        const auto Suffix=QFileInfo(Destination).suffix();
        auto Bytes=pdf_?pdf_->SerializeContent():image_?(!image_->isModified() && quality_ < 0
            && Suffix.compare(QFileInfo(originalImagePath_).suffix(),Qt::CaseInsensitive)==0
            ? originalImageBytes_ : image_->SerializeContent(Suffix.toLatin1(),quality_)):QByteArray{};
        if(Bytes.isEmpty())return storage::Error{storage::ErrorCategory::InvalidRequest,"document-serialization-or-format"};
        return Bytes;
    }
    bool DocumentViewPane::IsBusy() const {
        if(epub_){const auto State=epub_->ViewState();return !State["retired"].toBool()&&(epubClosing_||State["status"]!="ready");}
        if(hwp_&&!hwp_->IsClosed()) {
            const auto* Document=hwp_->Document();
            return hwp_->IsBusy()||(!Document&&hwp_->Error().isEmpty())
                ||(Document&&(Document->IsBusy()||Document->Snapshot().lifecycle_==DocumentLifecycle::Opening));
        }
        return (reading_&&!reading_->isHidden()&&reading_->IsBusy())||(image_&&image_->IsRendering())||(pdf_&&pdf_->IsRendering());
    }
    bool DocumentViewPane::RequestClose(){if(epub_){epubClosing_=true;epub_->RequestClose();return epub_->ViewState()["retired"].toBool();}if(!hwp_)return true;hwp_->RequestClose();return hwp_->IsClosed();}
    bool DocumentViewPane::IsEditing() const {return image_&&image_->IsEditing();}
    bool DocumentViewPane::CanUndo() const {return text_?static_cast<ScintillaQtDirectBackend*>(text_->Backend())->Send(SCI_CANUNDO):image_&&image_->CanUndo();}
    bool DocumentViewPane::CanRedo() const {return text_?static_cast<ScintillaQtDirectBackend*>(text_->Backend())->Send(SCI_CANREDO):image_&&image_->CanRedo();}
    void DocumentViewPane::Undo(){if(text_)static_cast<ScintillaQtDirectBackend*>(text_->Backend())->Send(SCI_UNDO);else if(image_)image_->undoEdit();}
    void DocumentViewPane::Redo(){if(text_)static_cast<ScintillaQtDirectBackend*>(text_->Backend())->Send(SCI_REDO);else if(image_)image_->redoEdit();}
    void DocumentViewPane::Commit(const QString& Path, const QByteArray& Bytes)
    {
        if(text_)text_->CommitContent();if(pdf_)pdf_->CommitContent();
        if(image_) {
            if (!Path.isEmpty() && !Bytes.isEmpty()) {
                originalImageHash_ = QByteArray::fromHex(SessionContent().mid(6));
                originalImageBytes_ = Bytes; originalImagePath_ = Path;
            }
            image_->CommitContent();
        }
    }
    void DocumentViewPane::Focus(){
        const auto Native=reinterpret_cast<HWND>(window()->internalWinId());
        if(isVisible() && Native && GetAncestor(GetForegroundWindow(),GA_ROOT)==GetAncestor(Native,GA_ROOT)
            && !QApplication::activeModalWidget() && !QApplication::activePopupWidget()){
            if(QApplication::activeWindow()!=window())QApplication::setActiveWindow(window());
        }
        if(epub_)epub_->FocusInput();else if(hwp_)hwp_->setFocus(Qt::OtherFocusReason);else if(reading_&&!reading_->isHidden())reading_->setFocus();else if(text_)text_->Backend()->widget()->setFocus();else if(image_){image_->show();image_->setFocus(Qt::OtherFocusReason);}else if(view_)view_->setFocus();
    }
    void DocumentViewPane::SetReadOnly(bool Enabled){editor_->setReadOnly(Enabled);if(text_)text_->setReadOnly(Enabled);if(view_&&!text_)view_->setEnabled(!Enabled);}
    void DocumentViewPane::SetClipboardBroker(bool Enabled)
    {
        if(view_)view_->RestrictClipboard(Enabled);
        if(!Enabled||!text_)return;
        // The workspace intercepts brokered commands. Remove native Scintilla
        // clipboard key bindings and popup/drop paths inside restricted hosts.
        auto* Backend=static_cast<ScintillaQtDirectBackend*>(text_->Backend());Backend->Send(SCI_USEPOPUP,SC_POPUP_NEVER);
        for(auto Key:{'C','X','V'})Backend->Send(SCI_CLEARCMDKEY,Key|(SCMOD_CTRL<<16));
        Backend->Send(SCI_CLEARCMDKEY,SCK_INSERT|(SCMOD_SHIFT<<16));Backend->Send(SCI_CLEARCMDKEY,SCK_INSERT|(SCMOD_CTRL<<16));
        Backend->widget()->setAcceptDrops(false);
    }
    void DocumentViewPane::applyTheme()
    {
        if(!view_)return;const auto& Theme=fm::style::ThemeManager::instance();
        // Palette-change delivery can lag the theme signal, especially for
        // inactive tabs. Resolve from the new theme before updating child views.
        setPalette(Theme.colors().toPalette());
        const auto Dark=fm::style::isDarkVariant(Theme.effectiveVariant());
        ThemeManager::instance().setTheme(Dark?ThemeManager::Dark:ThemeManager::Light);view_->setTheme(Dark?QBaseView::Dark:QBaseView::Light);
        // Actions are created before the workspace applies its palette. Refresh
        // normal/off glyphs too; a Light icon otherwise disappears on Dark.
        for(auto* Action:findChildren<QAction*>())ToolbarIcons::refreshAction(Action);
        if(text_){auto Font=QFontDatabase::systemFont(QFontDatabase::FixedFont);if(!Theme.monoFontFamily().isEmpty())Font.setFamily(Theme.monoFontFamily());Font.setPixelSize(Theme.monoFontPx());text_->setEditorFont(Font);text_->ReloadPreferences();}
    }
    QJsonObject DocumentViewPane::ViewState() const
    {
        auto* Input=InputWidget();
        QJsonObject State{{"viewId",viewId_},{"canUndo",CanUndo()},{"canRedo",CanRedo()},
            {"inputFocused",Input&&(Input->hasFocus()||Input->isAncestorOf(QApplication::focusWidget()))}};
        if(text_){State["encoding"]=encoding_;State["bom"]=bom_;State["language"]=text_->currentLanguage();State["readOnly"]=text_->isReadOnly();State["lineCount"]=text_->lineCount();}
        if(pdf_){State["page"]=pdf_->currentPage();State["pageCount"]=pdf_->totalPageCount();State["formsSupported"]=false;State["searchCount"]=pdf_->totalSearchResultCount();}
        if(image_){State["filter"]=image_->DisplayFilter();State["width"]=image_->Content().width();State["height"]=image_->Content().height();State["rendering"]=image_->IsRendering();State["renderError"]=image_->RenderError();}
        if(reading_&&!reading_->isHidden())State["reading"]=reading_->State();
        if(animation_)State["animationPlaying"]=animation_->Playing();
        if(epub_){
            const auto Epub=epub_->ViewState();State["epub"]=Epub;State["readOnly"]=true;
            State["closed"]=Epub["retired"].toBool();
            State["ready"]=Epub["status"]=="ready"&&!epubClosing_&&Epub["attached"].toBool();
            State["error"]=Epub["status"]=="error"?Epub["message"].toString():QString();
        }
        if(hwp_){
            State["readOnly"]=true;State["closed"]=hwp_->IsClosed();State["error"]=hwp_->Error();
            const auto* Document=hwp_->Document();
            State["ready"]=Document&&Document->Snapshot().lifecycle_==DocumentLifecycle::Ready&&!hwp_->IsClosing();
            if(Document&&Document->Snapshot().error_)State["error"]=QString::fromStdString(Document->Snapshot().error_->code_);
        }
        return State;
    }
    QJsonObject DocumentViewPane::Execute(const QJsonObject& Request)
    {
        const auto Action=Request["action"].toString();
        auto Error=[](const char* Code){return QJsonObject{{"error",Code}};};
        if(epub_) {
            if(Action.startsWith("epub.reader."))return epub_->ReaderAction(Action.mid(12),Request["value"])
                ? QJsonObject{{"accepted",true}} : Error("epub-reader-not-ready");
            if(Action.startsWith("epub.settings.")) return epub_->ChangeReadingSettings(Action.mid(14),Request["profile"].toObject(),Request["global"].toBool())
                ? QJsonObject{{"accepted",true}} : Error("epub-settings-not-ready");
            return Error("epub-read-only");
        }
        if(hwp_){
            if(Action!="print")return Error("document-view-command-unsupported");
            if(IsBusy()||!ViewState()["ready"].toBool())return Error("document-not-ready");
            QTimer::singleShot(0,hwp_,&HwpOpenWindow::Print);return {{"accepted",true}};
        }
        if(reading_&&!reading_->isHidden()&&(Action=="image.rotate"||Action=="image.flip"||Action=="image.crop"||Action=="image.resize"||Action=="image.composite"))return Error("image-copy-edit-required");
        if(Action.startsWith("reading.")&&reading_){
            if(Action=="reading.mode"&&(image_->isModified()||image_->IsEditing()))return Error("image-save-before-reading");
            if(Action=="reading.animation"){reading_->ToggleAnimation();return {{"accepted",true}};}
            if(Action=="reading.mode"){auto* Mode=findChild<QComboBox*>("imageReadingMode");const auto Value=qBound(0,Request["mode"].toInt(),2);Mode->setCurrentIndex(Value);emit Mode->activated(Value);}
            else if(Action=="reading.page")reading_->GoTo(Request["page"].toInt(),Request["relative"].toDouble());
            else if(Action=="reading.next")reading_->Move(1,Request["onePage"].toBool());else if(Action=="reading.previous")reading_->Move(-1,Request["onePage"].toBool());
            else if(Action=="reading.rtl")reading_->SetRightToLeft(Request["enabled"].toBool());else if(Action=="reading.cover")reading_->SetCover(Request["enabled"].toBool());
            else if(Action=="reading.split")reading_->SetSplit(Request["enabled"].toBool());else if(Action=="reading.adjust")reading_->SetAdjustment(Request["name"].toString(),Request["value"].toDouble());
            else if(Action=="reading.zoom")reading_->Zoom(Request["factor"].toDouble(1));else if(Action=="reading.volume")reading_->OpenVolume(Request["direction"].toInt(1));
            else if(Action=="reading.gap")reading_->SetGap(Request["value"].toInt());else if(Action=="reading.bookmark")reading_->Bookmark();else if(Action=="reading.openBookmark")reading_->OpenBookmark();else if(Action=="reading.private")reading_->SetPrivate(Request["enabled"].toBool());
            else if(Action=="reading.fit")reading_->SetFit(Request["mode"].toInt());else return Error("reading-command");return {{"accepted",true}};
        }
        if(text_&&text_->isReadOnly()&&(Action=="replaceText"||Action=="text.replaceAll"))return Error("document-edit-unavailable");
        if(Action=="replaceText"&&text_){auto Text=Request["text"].toString();if(eol_=="CRLF")Text.replace("\r\n","\n").replace("\n","\r\n");text_->ReplaceContent(Text);return {{"accepted",true}};}
        if(Action=="selectAll"&&text_){static_cast<ScintillaQtDirectBackend*>(text_->Backend())->Send(SCI_SELECTALL);return {{"accepted",true}};}
        if(Action=="text.find"&&text_)text_->findText(Request["text"].toString(),Request["regex"].toBool(),Request["caseSensitive"].toBool(),Request["wholeWords"].toBool());
        else if(Action=="text.replaceAll"&&text_)text_->replaceAll(Request["text"].toString(),Request["replacement"].toString(),Request["regex"].toBool(),Request["caseSensitive"].toBool(),Request["wholeWords"].toBool());
        else if(Action=="text.encoding"&&text_) {
            const auto Encoding=Request["encoding"].toString();const bool Bom=Request.contains("bom")?Request["bom"].toBool():bom_;
            auto Bytes=EncodeDocumentText(text_->Content(),Encoding,Bom);if(!Bytes.HasValue())return {{"error",QString::fromStdString(Bytes.Failure().code_)}};
            encoding_=Encoding;bom_=Bom;emit ContentChanged();
        }
        else if(Action=="pdf.page"&&pdf_)pdf_->goToPage(Request["page"].toInt());
        else if(Action=="pdf.search"&&pdf_)pdf_->searchText(Request["text"].toString());
        else if(Action=="pdf.rotate"&&pdf_)pdf_->setRotationAngle(static_cast<QPDFView::RotationAngle>(Request["angle"].toInt()));
        else if(Action=="pdf.annotate"&&pdf_){if(!pdf_->AddAnnotation(Request["page"].toInt(),Request["kind"].toString(),QRectF(Request["x"].toDouble(),Request["y"].toDouble(),Request["width"].toDouble(),Request["height"].toDouble()),Request["text"].toString()))return Error("document-pdf-annotation");}
        else if(Action=="image.fit"&&image_){if(reading_&&!reading_->isHidden())reading_->SetFit(Request["mode"].toInt());else image_->setFitMode(static_cast<QImageView::FitMode>(qBound(0,Request["mode"].toInt(),4)));}
        else if(Action=="image.filter"&&image_){image_->SetDisplayFilter(Request["filter"].toString());if(reading_)reading_->SetFilter(Request["filter"].toString());}
        else if(Action=="image.rotate"&&image_){if(Request["angle"].toInt()==90)image_->rotateClockwise();else if(Request["angle"].toInt()==-90)image_->rotateCounterClockwise();else return Error("document-image-rotation");}
        else if(Action=="image.flip"&&image_){if(Request["vertical"].toBool())image_->flipVertical();else image_->flipHorizontal();}
        else if(Action=="image.crop"&&image_){if(!image_->ApplyCrop(QRect(Request["x"].toInt(),Request["y"].toInt(),Request["width"].toInt(),Request["height"].toInt())))return Error("document-image-crop");}
        else if(Action=="image.resize"&&image_){if(!image_->ApplyResize(QSize(Request["width"].toInt(),Request["height"].toInt()),Request["filter"].toString("auto")))return Error("document-image-resize");}
        else if(Action=="image.exportQuality"&&image_)quality_=qBound(-1,Request["quality"].toInt(-1),100);
        else if(Action=="image.composite"&&image_){QImage Image;Image.loadFromData(QByteArray::fromBase64(Request["bytes"].toString().toLatin1()));if(!image_->ApplyComposite(Image,QRectF(Request["x"].toDouble(),Request["y"].toDouble(),Request["width"].toDouble(),Request["height"].toDouble()),Request["angle"].toDouble()))return Error("document-image-composite");}
        else if(Action=="printToPdf"&&view_){
            QTemporaryFile Artifact; if(!Artifact.open())return Error("document-print-artifact");
            const auto Path=Artifact.fileName();Artifact.close();
            QPrinter Printer(QPrinter::HighResolution);Printer.setOutputFormat(QPrinter::PdfFormat);Printer.setOutputFileName(Path);
            PrintManager Manager;QImageView Printed;
            if(reading_&&!reading_->isHidden()){const auto Image=reading_->CurrentImage();if(Image.isNull())return Error("image-page-not-ready");Printed.LoadContent(Image,reading_->CurrentName(),false);Manager.printView(&Printed,&Printer);}else Manager.printView(view_,&Printer);
            QFile File(Path);if(!File.open(QIODevice::ReadOnly))return Error("document-print-output");
            const auto Bytes=File.readAll();if(!Bytes.startsWith("%PDF-"))return Error("document-print-output");
            const auto Saved=SaveTextFile(Request["path"].toString(),Bytes,{},{});
            if(!Saved.HasValue())return Error(Saved.Failure().code_.c_str());
        }
        else if(Action=="print"&&view_){QTimer::singleShot(0,this,[this]{PrintManager Printer;QImageView Printed;if(reading_&&!reading_->isHidden()){if(reading_->CurrentImage().isNull())return;Printed.LoadContent(reading_->CurrentImage(),reading_->CurrentName(),false);Printer.showPrintDialog(&Printed,this);}else Printer.showPrintDialog(view_,this);});}
        else if(Action=="printPreview"&&view_)QTimer::singleShot(0,this,[this]{if(reading_&&!reading_->isHidden()){if(reading_->CurrentImage().isNull())return;QImageView Printed;Printed.LoadContent(reading_->CurrentImage(),reading_->CurrentName(),false);PrintManager Printer;Printer.showPreview(&Printed,this);}else view_->printPreview();});
        else if(Action=="watermark"&&view_){auto Config=view_->watermark();Config.enabled=Request["enabled"].toBool();Config.text=Request["text"].toString();view_->setWatermark(Config);}
        else if(Action=="captureProtection"&&view_){view_->setScreenCaptureProtection(Request["enabled"].toBool());if(ScreenProtect::isProtected(view_)!=Request["enabled"].toBool())return Error("document-capture-protection-unavailable");}
        else return Error("document-view-command-unsupported");
        return {{"accepted",true}};
    }
}

namespace hc::document {
QWidget* DocumentViewPane::InputWidget() const {return epub_?static_cast<QWidget*>(epub_):hwp_?static_cast<QWidget*>(hwp_):reading_&&!reading_->isHidden()?static_cast<QWidget*>(reading_):text_?text_->Backend()->widget():view_;}
QJsonObject DocumentViewPane::CaptureSelection() const
{
    if (pdf_) return pdf_->ClipboardContent();
    if(reading_&&!reading_->isHidden()){QByteArray Bytes;QBuffer B(&Bytes);if(!B.open(QIODevice::WriteOnly)||!reading_->CurrentImage().save(&B,"PNG"))return {};return {{"mime","image/png"},{"bytes",QString::fromLatin1(Bytes.toBase64())}};}
    if (image_) return image_->ClipboardContent();
    if (!text_) return {};
    auto* Backend = static_cast<ScintillaQtDirectBackend*>(text_->Backend());
    QJsonArray Ranges; QStringList Parts;
    const int Count = static_cast<int>(Backend->Send(SCI_GETSELECTIONS));
    for (int Index = 0; Index < Count; ++Index) {
        const auto Start = Backend->Send(SCI_GETSELECTIONNSTART, Index), End = Backend->Send(SCI_GETSELECTIONNEND, Index);
        const auto Virtual = qMin(Backend->Send(SCI_GETSELECTIONNCARETVIRTUALSPACE, Index), Backend->Send(SCI_GETSELECTIONNANCHORVIRTUALSPACE, Index));
        Ranges.append(QJsonObject{{"start", int(Start)}, {"end", int(End)}, {"virtual", int(Virtual)}});
        QByteArray Bytes(End - Start + 1, Qt::Uninitialized);
        Sci_TextRangeFull Range{}; Range.chrg.cpMin = Start; Range.chrg.cpMax = End; Range.lpstrText = Bytes.data();
        Backend->Send(SCI_GETTEXTRANGEFULL, 0, reinterpret_cast<qintptr>(&Range));
        Parts.append(QString::fromUtf8(Bytes.constData(), End - Start));
    }
    return {{"mime", "text/plain"}, {"start", Backend->selectionStartPos()}, {"end", Backend->selectionEndPos()},
        {"ranges", Ranges}, {"text", Parts.join("\r\n")}};
}
void DocumentViewPane::ApplyClipboard(const QString& Mode, const QString& Text, const QJsonObject& Selection)
{
    if (!text_ || text_->isReadOnly() || (Mode != "cut" && Mode != "paste")) return;
    auto* Backend = static_cast<ScintillaQtDirectBackend*>(text_->Backend());
    auto Ranges = Selection["ranges"].toArray();
    if (Ranges.isEmpty()) Ranges.append(Selection);
    QList<QJsonObject> Ordered; for (const auto& Range : Ranges) Ordered.append(Range.toObject());
    std::sort(Ordered.begin(), Ordered.end(), [](const auto& A, const auto& B) { return A["start"].toInt() < B["start"].toInt(); });
    const auto Bytes = Mode == "paste" ? Text.toUtf8() : QByteArray{};
    if (Bytes.contains('\0')) return;
    qint64 Projected = Backend->documentLength(); int PreviousEnd = -1;
    for (const auto& Range : Ordered) {
        const int Start = Range["start"].toInt(), End = Range["end"].toInt(), Virtual = qMax(0, Range["virtual"].toInt());
        if (Start < 0 || End < Start || End > Backend->documentLength() || Start < PreviousEnd) return;
        PreviousEnd = End; Projected += Bytes.size() + (Mode == "paste" ? Virtual : 0) - (End - Start);
    }
    if (!unlimitedBytes_ && Projected > TextByteLimit) return;
    Backend->Send(SCI_BEGINUNDOACTION);
    Backend->Send(SCI_CLEARSELECTIONS);
    for (int Index = 0; Index < Ordered.size(); ++Index) {
        const auto Range = Ordered[Index];
        Backend->Send(Index == 0 ? SCI_SETSELECTION : SCI_ADDSELECTION, Range["end"].toInt(), Range["start"].toInt());
    }
    // Back-to-front replacement keeps the snapshot's UTF-8 positions valid.
    for (int Index = Ordered.size() - 1; Index >= 0; --Index) {
        const auto Range = Ordered[Index]; const int Start = Range["start"].toInt();
        const auto Insert = QByteArray(Mode == "paste" ? qMax(0, Range["virtual"].toInt()) : 0, ' ') + Bytes;
        Backend->Send(SCI_SETTARGETSTART, Start); Backend->Send(SCI_SETTARGETEND, Range["end"].toInt());
        Backend->Send(SCI_REPLACETARGET, Insert.size(), reinterpret_cast<qintptr>(Insert.constData()));
        Backend->Send(SCI_SETSELECTIONNCARET, Index, Start + Insert.size());
        Backend->Send(SCI_SETSELECTIONNANCHOR, Index, Start + Insert.size());
    }
    Backend->Send(SCI_ENDUNDOACTION);
}
}

namespace hc::document {void DocumentViewPane::Cancel(){if(epub_){epubClosing_=true;epub_->RequestClose();}if(hwp_&&hwp_->Document())hwp_->Document()->Cancel();if(image_)image_->CancelWork();if(reading_)reading_->Cancel();if(animation_)animation_->Clear();}}

namespace hc::document {
DocumentViewPane::~DocumentViewPane(){
    if(reading_)reading_->Cancel();
    if(image_)image_->CancelWork();
    // QWidget deletes children after this derived vtable has gone away.
    // Their resource-close signals must not call a DocumentViewPane thunk.
    for(auto* Child:findChildren<QObject*>())QObject::disconnect(Child,nullptr,this,nullptr);
    QObject::disconnect(&fm::style::ThemeManager::instance(),nullptr,this,nullptr);
}
}
