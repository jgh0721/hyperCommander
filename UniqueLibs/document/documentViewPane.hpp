#pragma once
#include "viewUiContribution.hpp"
#include "documentStorageAdapter.hpp"
#include "hwpOpenWindow.hpp"
#include <QWidget>
#include <QJsonObject>
class QPlainTextEdit;
class QToolBar;
class QBaseView;
class QTextView;
class QPDFView;
class QImageView;
namespace hc::document
{
    class ImageReadingView;
    class ImageFramePlayer;
    class DocumentViewPane final : public QWidget
    {
        Q_OBJECT
    public:
        explicit DocumentViewPane(QWidget* Parent = nullptr);
        ~DocumentViewPane() override;
        QPlainTextEdit* Editor() const { return editor_; }
        ViewUiContribution Contribution() const { return contribution_; }
        bool Load(const TextInput& Input, bool Traverse, bool ImageEditing = true);
        void ConfigureHwp(WebProcessRuntime Runtime) { hwpRuntime_ = std::move(Runtime); }
        void SetUnlimitedBytes(bool Enabled) { unlimitedBytes_ = Enabled; }
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
        QJsonObject CaptureSelection() const;
        void ApplyClipboard(const QString& Mode,const QString& Text,const QJsonObject& Selection);
        QWidget* InputWidget() const;
        QBaseView* View() const { return view_; }
    signals:
        void ContentChanged();
        void ViewStateChanged();
        void OpenRequested(const QString& Path);
        void SaveRequested();
        void EncodingRequested(const QString& Encoding);
        void ClipboardRequested(const QString& Mode);
        void EditImageRequested(const QImage& Image);
        void ReadingSelected();
    private:
        void syncText();
        void applyTheme();
        QPlainTextEdit* editor_;
        QToolBar* toolbar_ = nullptr;
        QBaseView* view_ = nullptr;
        QTextView* text_ = nullptr;
        QPDFView* pdf_ = nullptr;
        QImageView* image_ = nullptr;
        ImageReadingView* reading_ = nullptr;
        ImageFramePlayer* animation_ = nullptr;
        HwpOpenWindow* hwp_ = nullptr;
        bool hwpFocusPending_ = false;
        WebProcessRuntime hwpRuntime_;
        QString viewId_ = "text", encoding_ = "UTF-8", eol_ = "LF";
        bool bom_ = false, syncing_ = false;
        bool unlimitedBytes_ = false;
        int quality_ = -1;
        QByteArray originalImageBytes_, originalImageHash_;
        QString originalImagePath_;
        QString readingPath_,readingEntry_;
        ViewUiContribution contribution_;
    };
}
