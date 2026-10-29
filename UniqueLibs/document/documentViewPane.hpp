#pragma once
#include "viewUiContribution.hpp"
#include "documentStatusBar.hpp"
#include "documentStorageAdapter.hpp"
#include "hwpOpenWindow.hpp"
#include <QWidget>
#include <QJsonObject>

class QToolBar;
class QBaseView;
class QTextView;
class QPDFView;
class QImageView;
namespace hc::document
{
    class EpubProcessView;
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
        bool CanEditEpub()const;bool CanSaveEpub()const;bool PrepareEpubSave();
        bool IsEpub() const { return viewId_ == "epub"; }
        bool IsProcessDocument() const { return IsHwp() || IsEpub(); }
        bool IsHwp() const { return viewId_ == "hwp" || viewId_ == "hwpx"; }
        bool RequestClose();
        void SetReadingSource(const QString& Path,const QString& Entry);
        QByteArray Content() const;
        QByteArray SessionContent() const;
        storage::Result<QByteArray> Export(const QString& Destination) const;
        QJsonObject Execute(const QJsonObject& Request);
        QString ViewId() const { return viewId_; }
        bool IsBusy() const;
        bool IsEditing() const;
        bool IsEpubEditing() const;
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
        EpubSourceEditor* EpubEditor() const { return epubEditor_; }
        // Image, PDF and EPUB editing commands dispatched by the workspace shortcut router.
        bool RunViewCommand(const QString& Id);
        bool CanRunViewCommand(const QString& Id) const;
        void SetShortcutBindings(const QJsonObject& Bindings);
    signals:
        void ContentChanged();
        void ViewStateChanged();
        void OpenRequested(const QString& Path);
        void SaveRequested();
        void EncodingRequested(const QString& Encoding);
        void ClipboardRequested(const QString& Mode);
        void EditImageRequested(const QImage& Image);
        void ReadingSelected();
        void EscapeRequested();
        void FullscreenRequested();
        void RetryRequested();
        void EditEpubRequested();
    protected:
        void resizeEvent(QResizeEvent* Event) override;
    private:
        QWidget* loadingPreview_ = nullptr;

        bool editEpub(const QString& Locator={},int Chapter=-1);
        EpubProcessView* currentEpub()const;
        void applyTheme();
        bool readOnly_ = true;
        QToolBar* toolbar_ = nullptr;
        QBaseView* view_ = nullptr;
        QTextView* text_ = nullptr;
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
        bool unlimitedBytes_ = false;
        int quality_ = -1;
        QByteArray originalImageBytes_, originalImageHash_;
        QString originalImagePath_;
        QString readingPath_,readingEntry_;
        ViewUiContribution contribution_;
    };
}
