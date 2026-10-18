#include "documentViewPane.hpp"
#include "commonLibs/document/documentHostProtocol.hpp"
#include "documentThemeAdapter.hpp"
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
#include <QFontDatabase>
#include <QBuffer>
#include <QCryptographicHash>
#include <QTemporaryFile>
#include <QPrinter>
#include <QTimer>
#include <QFile>
#include <Scintilla.h>
void InitializePortedViewResources();
namespace hc::document
{
    DocumentViewPane::DocumentViewPane(QWidget* Parent):QWidget(Parent)
    {
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
    bool DocumentViewPane::Load(const TextInput& Input,bool Traverse)
    {
        const QSignalBlocker Block(this),EditorBlock(editor_);
        viewId_=Input.viewId_;encoding_=Input.encoding_;bom_=Input.bom_;eol_=Input.eol_;
        originalImageBytes_ = viewId_ == "image" ? Input.bytes_ : QByteArray{};
        originalImageHash_ = viewId_ == "image" ? Input.hash_ : QByteArray{};
        originalImagePath_ = viewId_ == "image" ? Input.path_ : QString{};
        if(view_) {delete view_;view_=nullptr;text_=nullptr;pdf_=nullptr;image_=nullptr;}
        if(toolbar_) {delete toolbar_;toolbar_=nullptr;}
        bool Success=true;
        if(viewId_=="pdf") {pdf_=new QPDFView(this);view_=pdf_;Success=pdf_->LoadContent(Input.bytes_,Input.path_);}
        else if(viewId_=="image") {image_=new QImageView(this);view_=image_;image_->LoadContent(Input.image_,Input.path_,Traverse);}
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
            connect(image_,&QImageView::ContentChanged,this,&DocumentViewPane::ContentChanged);connect(image_,&QImageView::viewStateChanged,this,&DocumentViewPane::ViewStateChanged);
            auto* EditToolbar=new QToolBar(tr("이미지 편집"),this);EditToolbar->setObjectName("imageEditToolbar");
            bool Editing=false;for(auto* Action:toolbar_->actions()){Editing|=Action->isCheckable();if(Editing){toolbar_->removeAction(Action);EditToolbar->addAction(Action);}}
            static_cast<QVBoxLayout*>(layout())->insertWidget(1,EditToolbar);
            auto* Filters=new QComboBox(toolbar_);Filters->setObjectName("imageDisplayFilter");Filters->addItem(tr("자동"),"auto");
            for(const auto& Filter:ImageFilterRegistry::Filters())Filters->addItem(Filter.label_,Filter.id_);
            auto* Quality=new QSpinBox(toolbar_);Quality->setObjectName("imageExportQuality");Quality->setRange(0,100);Quality->setValue(95);Quality->setPrefix(tr("품질 "));toolbar_->addWidget(Quality);
            connect(Quality,&QSpinBox::valueChanged,this,[this](int Value){quality_=Value;});
            auto* Resize=toolbar_->addAction(tr("출력 크기"));
            connect(Resize,&QAction::triggered,this,[this] {
                bool Accepted=false;const auto Current=image_->Content().size();
                const int Width=QInputDialog::getInt(this,tr("출력 크기"),tr("너비"),Current.width(),1,16384,1,&Accepted);if(!Accepted)return;
                const int Height=QInputDialog::getInt(this,tr("출력 크기"),tr("높이"),Current.height(),1,16384,1,&Accepted);if(Accepted)image_->ApplyResize(QSize(Width,Height),image_->DisplayFilter());
            });
            toolbar_->insertWidget(toolbar_->actions().value(0),Filters);connect(Filters,&QComboBox::activated,image_,[this,Filters](int Index){image_->SetDisplayFilter(Filters->itemData(Index).toString());});
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
        if (!image_) return Content();
        if (!image_->isModified()) return "image:" + originalImageHash_.toHex();
        const auto Bytes = image_->SerializeContent();
        return Bytes.isEmpty() ? QByteArray{} : "image:" + QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex();
    }
    storage::Result<QByteArray> DocumentViewPane::Export(const QString& Destination) const
    {
        if(text_)return EncodeDocumentText(text_->Content(),encoding_,bom_);
        const auto Suffix=QFileInfo(Destination).suffix();
        auto Bytes=pdf_?pdf_->SerializeContent():image_?(!image_->isModified() && quality_ < 0
            && Suffix.compare(QFileInfo(originalImagePath_).suffix(),Qt::CaseInsensitive)==0
            ? originalImageBytes_ : image_->SerializeContent(Suffix.toLatin1(),quality_)):QByteArray{};
        if(Bytes.isEmpty())return storage::Error{storage::ErrorCategory::InvalidRequest,"document-serialization-or-format"};
        return Bytes;
    }
    bool DocumentViewPane::IsBusy() const {return (image_&&image_->IsRendering())||(pdf_&&pdf_->IsRendering());}
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
    void DocumentViewPane::Focus(){if(text_)text_->Backend()->widget()->setFocus();else if(view_)view_->setFocus();}
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
        const auto Dark=QApplication::palette().color(QPalette::Window).lightness()<128;
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
        return State;
    }
    QJsonObject DocumentViewPane::Execute(const QJsonObject& Request)
    {
        const auto Action=Request["action"].toString();
        auto Error=[](const char* Code){return QJsonObject{{"error",Code}};};
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
        else if(Action=="image.fit"&&image_)image_->setFitMode(static_cast<QImageView::FitMode>(qBound(0,Request["mode"].toInt(),4)));
        else if(Action=="image.filter"&&image_)image_->SetDisplayFilter(Request["filter"].toString());
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
            PrintManager Manager;Manager.printView(view_,&Printer);
            QFile File(Path);if(!File.open(QIODevice::ReadOnly)||File.size()>DocumentByteLimit)return Error("document-print-output");
            const auto Bytes=File.readAll();if(!Bytes.startsWith("%PDF-"))return Error("document-print-output");
            const auto Saved=SaveTextFile(Request["path"].toString(),Bytes,{},{});
            if(!Saved.HasValue())return Error(Saved.Failure().code_.c_str());
        }
        else if(Action=="print"&&view_){QTimer::singleShot(0,this,[this]{PrintManager Printer;Printer.showPrintDialog(view_,this);});}
        else if(Action=="printPreview"&&view_)QTimer::singleShot(0,this,[this]{view_->printPreview();});
        else if(Action=="watermark"&&view_){auto Config=view_->watermark();Config.enabled=Request["enabled"].toBool();Config.text=Request["text"].toString();view_->setWatermark(Config);}
        else if(Action=="captureProtection"&&view_){view_->setScreenCaptureProtection(Request["enabled"].toBool());if(ScreenProtect::isProtected(view_)!=Request["enabled"].toBool())return Error("document-capture-protection-unavailable");}
        else return Error("document-view-command-unsupported");
        return {{"accepted",true}};
    }
}

namespace hc::document {
QWidget* DocumentViewPane::InputWidget() const {return text_?text_->Backend()->widget():view_;}
QJsonObject DocumentViewPane::CaptureSelection() const
{
    if (pdf_) return pdf_->ClipboardContent();
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
    if (Projected > TextByteLimit) return;
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

namespace hc::document {void DocumentViewPane::Cancel(){if(image_)image_->CancelWork();}}

namespace hc::document {
DocumentViewPane::~DocumentViewPane(){
    if(image_)image_->CancelWork();
    // QWidget deletes children after this derived vtable has gone away.
    // Their resource-close signals must not call a DocumentViewPane thunk.
    for(auto* Child:findChildren<QObject*>())QObject::disconnect(Child,nullptr,this,nullptr);
    QObject::disconnect(&fm::style::ThemeManager::instance(),nullptr,this,nullptr);
}
}
