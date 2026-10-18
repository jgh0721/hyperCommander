#include "text/QTextView.h"
#include "text/ITextEditorBackend.h"
#include "text/ScintillaQtDirectBackend.h"
#include "pdf/QPDFView.h"
#include "image/QImageView.h"
#include <QJsonObject>
#include "core/AppSettings.h"
#include "ScintillaEditBase.h"
#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_text.h>
#include <QBuffer>
#include <QFileInfo>
#include <QColorSpace>
#include <QSignalBlocker>
namespace { constexpr qsizetype ArtifactLimit = 64*1024*1024; }
qintptr ScintillaQtDirectBackend::Send(unsigned int Message, quintptr W, qintptr L) const
{ return m_editor ? m_editor->send(Message,W,L) : 0; }
ITextEditorBackend* QTextView::Backend() const { return m_editor; }
QString QTextView::Content() const { return m_editor ? m_editor->text() : QString{}; }
void QTextView::LoadContent(const QString& Text, const QString& Path, const QString& Encoding)
{
    closeFile(); m_hotExitEnabled=false; m_filePath=Path; m_encoding=m_detectedEncoding=Encoding;
    m_document.setPreviewOnly(false);m_document.setTruncated(false);
    auto Ending = detectLineEnding(Text);
    if (!Text.contains('\r') && !Text.contains('\n')) {
        const auto Default = AppSettings().value("textView/defaultLineEnding", "CRLF").toString();
        Ending = Default == "LF" ? LineEnding::LF : Default == "CR" ? LineEnding::CR : LineEnding::CRLF;
    }
    m_document.setLineEnding(toDocumentLineEnding(Ending));
    ensureEditorBackend();m_editor->setReadOnly(false);m_editor->setText(Text);m_editor->setLineEnding(m_document.lineEnding(),false);m_editor->setModified(false);
    autoDetectLanguage(); updateMetrics();emit encodingChanged(Encoding);
}
void QTextView::ReplaceContent(const QString& Text)
{
    if(!m_editor || m_editor->isReadOnly()) return;
    m_editor->setSelectionByPos(0,m_editor->documentLength());m_editor->replace(Text);
}
void QTextView::CommitContent() { if(m_editor)m_editor->setModified(false); }
bool QPDFView::LoadContent(const QByteArray& Bytes, const QString& Path)
{
    closeFile();m_fileData=Bytes;m_filePath=Path;
    if(Bytes.size()>ArtifactLimit) return false;
    m_document=FPDF_LoadMemDocument64(m_fileData.constData(),m_fileData.size(),nullptr);
    if(!m_document) { m_fileData.clear();return false; }
    m_pageCount=FPDF_GetPageCount(m_document);
    if(m_pageCount<=0||m_pageCount>10000) { closeFile();return false; }
    m_currentPage=0;m_rotationAngle=Rotate0;m_annotationManager.setDocument(m_document);
    // Original form implementation never creates a valid form handle. Keep it
    // unavailable until a separate, validated form-fill lifecycle is implemented.
    m_formHandle=nullptr; m_nextThumbnailPage=0;
    m_pageCache.setMaxCost(32*1024);loadBookmarks();renderCurrentPage();startThumbnailGeneration();endLoading();
    emit pageChanged(1,m_pageCount);return true;
}
QByteArray QPDFView::SerializeContent() const
{
    if(!m_document)return {};
    struct Writer:FPDF_FILEWRITE {
        QByteArray bytes_;
        static int Write(FPDF_FILEWRITE* Self,const void* Data,unsigned long Size) {
            auto* W=static_cast<Writer*>(Self);
            if(Size>ArtifactLimit || W->bytes_.size()+Size>ArtifactLimit)return 0;
            W->bytes_.append(static_cast<const char*>(Data),Size);return 1;
        }
    } Writer;
    Writer.version=1;Writer.WriteBlock=Writer::Write;
    // Existing rotations are already in each page dictionary. Never replace all
    // page rotations with one viewport angle during serialization.
    return FPDF_SaveAsCopy(m_document,&Writer,0)?Writer.bytes_:QByteArray{};
}
int QPDFView::PageRotation(int Page) const
{
    if(!m_document)return -1;auto Handle=FPDF_LoadPage(m_document,Page);if(!Handle)return -1;
    const auto Rotation=FPDFPage_GetRotation(Handle);FPDF_ClosePage(Handle);return Rotation;
}
bool QPDFView::AddAnnotation(int Page, const QString& Kind,const QRectF& Rect,const QString& Text)
{
    const PDFAnnotationManager::PdfRect Bounds{Rect.left(),Rect.right(),Rect.top(),Rect.bottom()};
    bool Success=false;
    if(Kind=="ink")Success=m_annotationManager.addInkStroke(Page,{Rect.topLeft(),Rect.bottomRight()},Qt::red,2);
    else if(Kind=="freeText")Success=m_annotationManager.addFreeText(Page,Bounds,Text,QFont("Arial"),14,Qt::red);
    else if(Kind=="note")Success=m_annotationManager.addTextNote(Page,Rect.topLeft(),Text,Qt::yellow);
    else if(Kind=="highlight"||Kind=="underline"||Kind=="strikeOut")
        Success=m_annotationManager.addTextMarkup(Page,Kind=="highlight"?FPDF_ANNOT_HIGHLIGHT:Kind=="underline"?FPDF_ANNOT_UNDERLINE:FPDF_ANNOT_STRIKEOUT,Bounds,Qt::yellow,2);
    return commitAnnotationChange(Success);
}
void QPDFView::CommitContent() { m_modified=false; }

void InitializePortedViewResources() { Q_INIT_RESOURCE(HyperEditor); }

void InitializeDocumentPdfium(){static const bool Initialized=[] {FPDF_InitLibrary();return true;}();Q_UNUSED(Initialized);}

QJsonObject QPDFView::ClipboardContent() const {
    if (!m_selectedText.isEmpty()) return {{"mime","text/plain"},{"text",m_selectedText}};
    QByteArray Bytes;QBuffer Buffer(&Bytes);Buffer.open(QIODevice::WriteOnly);
    if (!m_selectedImage.image.isNull() && m_selectedImage.image.save(&Buffer,"PNG")) return {{"mime","image/png"},{"bytes",QString::fromLatin1(Bytes.toBase64())}};
    return {};
}
QJsonObject QImageView::ClipboardContent() const {
    QByteArray Bytes;QBuffer Buffer(&Bytes);Buffer.open(QIODevice::WriteOnly);
    if (!m_displayImage.isNull() && m_displayImage.save(&Buffer,"PNG")) return {{"mime","image/png"},{"bytes",QString::fromLatin1(Bytes.toBase64())}};
    return {};
}
