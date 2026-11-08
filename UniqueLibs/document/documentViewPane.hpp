#pragma once
#include "viewUiContribution.hpp"
#include "documentViewKind.hpp"
#include "documentStatusBar.hpp"
#include "documentStorageAdapter.hpp"
#include "commonLibs/document/documentRecovery.hpp"
#include "hwpOpenWindow.hpp"
#include <QWidget>
#include <QJsonObject>
#include <functional>
#include <optional>

class QToolBar;
class QMenu;
class QTimer;
class QBaseView;
class QTextView;
class QPDFView;
class QImageView;
namespace hc::document
{
    class EpubProcessView;
    class MarkdownDocumentView;
    class EpubSourceEditor;
    class ImageReadingView;
    class ImageFramePlayer;
    class DocumentViewPane final : public QWidget
    {
        Q_OBJECT
    public:
        explicit DocumentViewPane(QWidget* Parent = nullptr);
        ~DocumentViewPane() override;
        void ShowLoadingPreview(const QImage& Image,bool Partial=false);
        bool IsReadOnly() const { return readOnly_; }
        ViewUiContribution Contribution() const { return contribution_; }
        bool Load(const TextInput& Input, bool Traverse, bool ImageEditing = true);
        void ConfigureHwp(WebProcessRuntime Runtime) { hwpRuntime_ = std::move(Runtime); }
        void SetUnlimitedBytes(bool Enabled) { unlimitedBytes_ = Enabled; }
        void ConfigureEpub(WebProcessRuntime Runtime,bool Editing=false) { epubRuntime_ = std::move(Runtime);epubEditingAllowed_=Editing; }
        bool CanEditEpub()const;bool CanSaveEpub()const;bool CanPrepareEpubSave()const;bool PrepareEpubSave(bool SaveAs);
        bool IsMarkdown() const { return markdown_ != nullptr; }
        bool MarkdownSourceVisible() const;
        bool IsEpub() const { return viewId_ == "epub"; }
        bool IsProcessDocument() const { return IsHwp() || IsEpub(); }
        bool IsHwp() const { return viewId_ == "hwp" || viewId_ == "hwpx"; }
        bool RequestClose();
        void SetReadingSource(const QString& Path,const QString& Entry);
        QByteArray Content() const;
        QString SearchText() const;
        std::function<QJsonArray()> CaptureBookSearchSources(const QString& Document) const;
        bool ApplySearchText(const QString& Expected, const QString& Replacement);
        void SelectSearchRange(qint64 Start, qint64 Length);
        // Session bytes and, for text, why they cannot be written in the document's
        // encoding. Both come from one serialization so a save never pairs bytes
        // with another revision's verdict.
        struct SessionSerialization { QByteArray bytes_; std::optional<storage::Error> error_; };
        SessionSerialization SerializeSession() const;
        QByteArray SessionContent() const;
        std::optional<TextRecoveryState> CaptureTextRecovery(QByteArray& Bytes, bool IncludeText = true) const;
        bool RestoreTextRecovery(const RecoverySnapshot& Record);
        void RestoreTextPosition(const TextRecoveryState& State);
        storage::Result<QByteArray> Export(const QString& Destination,bool SaveEpubOriginal=false) const;
        DocumentEncoder CaptureSave(const QString& Destination, bool SaveEpubOriginal = false, std::optional<QByteArray> Content = std::nullopt,
            std::optional<storage::Error> ContentError = std::nullopt) const;
        void CommitSaved(const QString& Path, const QByteArray& Bytes, const QByteArray& Content, const QByteArray& Hash, bool Current);
        void ReportSaveFailure(const QByteArray& Content, const QString& Error);
        QJsonObject Execute(const QJsonObject& Request);
        QString ViewId() const { return viewId_; }
        bool IsBusy() const;
        bool HasLoadingPreview() const;
        bool ImagePresentationReady() const;
        bool IsEditing() const;
        bool IsEpubEditing() const;
        DocumentViewKind Kind() const;
        void DiscardEpubRecovery();
        void SetEpubRecoveryEnabled(bool Enabled);
        void CheckpointEpub();
        bool CanUndo() const;
        bool CanRedo() const;
        void Undo();
        void Redo();
        void Commit(const QString& Path = {}, const QByteArray& Bytes = {});
        void Cancel();
        void Focus();
        void SetReadOnly(bool Enabled);
        void SetClipboardBroker(bool Enabled);
        QJsonObject ViewState() const;
        QList<DocumentStatusField> StatusFields() const;
        void RunStatusCommand(const QString& Id);
        QJsonObject CaptureSelection() const;
        void ApplyClipboard(const QString& Mode,const QString& Text,const QJsonObject& Selection);
        QWidget* InputWidget() const;
        QBaseView* View() const { return view_; }
        // The view's own toolbar row (text, PDF, image, Markdown, EPUB); nullptr for HWP.
        QToolBar* PrimaryToolbar() const;
        EpubSourceEditor* EpubEditor() const { return epubEditor_; }
        // Image, PDF and EPUB editing commands dispatched by the workspace shortcut router.
        bool RunViewCommand(const QString& Id);
        bool CanRunViewCommand(const QString& Id) const;
        void SetShortcutBindings(const QJsonObject& Bindings);
        // Status-bar picker for a field ("encoding", "eol", "page", …) anchored at its button.
        void OpenStatusPicker(const QString& Id, QWidget* Anchor);
        // "변환하여 저장": checked before anything changes; ErrorCode receives the reason on failure.
        bool SetSaveEncoding(const QString& Encoding, bool Bom, QString* ErrorCode = nullptr);
        QString SaveEncoding() const { return encoding_; }
        bool SaveBom() const { return bom_; }
    signals:
        // "다시 읽기 (해석만)": the workspace re-decodes the loaded bytes.
        void ReinterpretRequested(const QString& Encoding);
        // A command such as Ctrl+G asks the workspace to open the picker of a status field.
        void StatusPickerRequested(const QString& Id);
        void ContentChanged();
        void SearchRequested(const QString& Query);
        void ViewStateChanged();
        void OpenRequested(const QString& Path);
        void SaveRequested();
        void EpubSaveRequested(bool SaveAs);
        void EncodingRequested(const QString& Encoding);
        void ClipboardRequested(const QString& Mode);
        void EditImageRequested(const QImage& Image);
        void ReadingSelected();
        void ReadingVolumeChanged(const QString& Path);
        void EscapeRequested();
        void FullscreenRequested();
        void RetryRequested();
        void EditEpubRequested();
    protected:
        void resizeEvent(QResizeEvent* Event) override;
    private:
        QWidget* loadingPreview_ = nullptr;

        bool editEpub(const QString& Locator={},int Chapter=-1);
        // Mode menu and View/Edit menu items per view kind (documentViewMenus.cpp).
        void buildTextChrome(QMenu* Menu, bool Language);
        void buildTextViewItems(bool Zoom);
        void buildMarkdownChrome();
        void syncMarkdownChrome();
        void buildPdfChrome();
        void buildImageChrome();
        // Image, comic and webtoon share one pane: titles and items follow Kind() (documentViewMenus.cpp).
        void syncImageChrome();
        // EPUB(B) menu, view items and the reader row of the EPUB toolbar (documentViewMenus.cpp).
        void buildEpubChrome(QToolBar* Bar);
        void resetChrome();
        // The encoding the loaded bytes were decoded with ("다시 읽기" check mark); encoding_ is the save encoding.
        QString readEncoding_;
        TextDetection readDetection_ = TextDetection::Explicit;
        void refreshChrome();
        QAction* chromeAction(const QString& Text, const QString& Name, std::function<void()> Run, const QString& Key = {});
        QMenu* modeMenu_ = nullptr;
        // Owns the per-load mode menu and menu actions so a reload replaces them all.
        QObject* chromeOwner_ = nullptr;
        EpubProcessView* currentEpub()const;
        void applyTheme();
        bool readOnly_ = true;
        QToolBar* toolbar_ = nullptr;
        QBaseView* view_ = nullptr;
        QTextView* text_ = nullptr;
        MarkdownDocumentView* markdown_ = nullptr;
        QPDFView* pdf_ = nullptr;
        QImageView* image_ = nullptr;
        ImageReadingView* reading_ = nullptr;
        ImageFramePlayer* animation_ = nullptr;
        HwpOpenWindow* hwp_ = nullptr;
        bool hwpFocusPending_ = false;
        WebProcessRuntime hwpRuntime_, epubRuntime_;
        EpubProcessView* epub_ = nullptr;
        QWidget* epubFailure_=nullptr;
        bool epubClosing_ = false,epubEditingAllowed_=false;
        EpubSourceEditor* epubEditor_=nullptr;QByteArray epubBytes_;QString epubPath_;QJsonObject shortcutBindings_;
        void updateEpubMode();
        QByteArray epubSourceHash_;
        QString viewId_ = "text", encoding_ = "UTF-8", eol_ = "LF";
        bool bom_ = false, syncing_ = false;
        mutable QString statusEol_;
        QByteArray serializeContent(std::optional<storage::Error>* Error) const;
        bool unlimitedBytes_ = false;
        int quality_ = -1;
        // The slideshow outlives the image view: stepping to the next image reloads the view (plan 8.4).
        QTimer* slideshow_ = nullptr;
        bool slideshowStep_ = false;
    public:
        void SetSlideshow(bool Running);
        bool SlideshowRunning() const;
    private:
        QByteArray originalImageBytes_, originalImageHash_;
        QString originalImagePath_;
        QString readingPath_,readingEntry_;
        ViewUiContribution contribution_;
    };
}
