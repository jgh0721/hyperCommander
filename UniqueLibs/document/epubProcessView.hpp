#pragma once
#include "documentWebProcess.hpp"
#include <QJsonObject>
#include <QWidget>
class QTimer;
namespace hc::document
{
    // GUI thread only. Drain lifecycle before QApplication destruction.
    // Explicit diagnostic or independently pinned read-only runtime; one document per view.
    class EpubProcessView final : public QWidget
    {
        Q_OBJECT
    public:
        EpubProcessView(WebProcessRuntime Runtime, WebProcessLimits Limits, QWidget* Parent = nullptr);
        ~EpubProcessView() override;
        // SourceLines: chapters carry source line attributes for scroll synchronisation (editing previews only).
        bool Load(QByteArray Bytes, qint64 Generation = 1,const QString& SettingsDirectory = {},bool InitiallyInteractive=true,bool SourceLines=false);
        void RequestClose();
        // Explicit activation request; hidden/closing views reject it.
        bool FocusInput();
        bool ChangeReadingSettings(const QString& Action, const QJsonObject& Profile = {}, bool Global = false);
        bool ReaderAction(const QString& Action,const QJsonValue& Value={});
        // Latest-wins scroll target {chapter, line, ratio, end, serial}; a newer request replaces one not yet sent.
        bool RequestSourceScroll(const QJsonObject& Value);
        // Editing previews: opens Bytes (a new export of the same book) in this helper and replaces the shown
        // reader once it reached Position {chapter, locator, scroll}. Returns the reload serial, 0 when refused.
        // ViewState()["reload"] = {serial, status: preparing|loading|ready|failed, error}.
        qint64 Reload(QByteArray Bytes, const QJsonObject& Position);
        QJsonObject ViewState() const;
        bool IsLoaded() const { return loaded_; }
        // Shortcut bindings last sent to the reader, for the key text of host menus.
        QJsonObject ShortcutBindings() const { return bindings_; }
        // Bindings the workspace applied from SettingsFile. New views using that file send them to
        // the reader instead of reading it on the GUI thread; other views read their file as before.
        static void SetDefaultBindings(const QString& SettingsFile, const QJsonObject& Bindings);
        static void InitializeLifecycle();
        static void RequestShutdown();
        static bool IsDrained();
        static void ReleaseLifecycle();
    signals:
        void Loaded();
        void EscapeRequested();
        void FullscreenRequested();
        // A window shortcut pressed in the reader, in order; Id is not checked here.
        void HostCommandRequested(const QString& Id);
        void Failed(const QString& Error);
        void Closed();
        void ViewStateChanged();
        // Once per new right click (or menu key) in the reader, after ViewStateChanged. Request: serial, x, y
        // (relative to this view), selection, link, locator, chapter, anchor, target {kind: text|image|margin}.
        void ContextMenuRequested(const QJsonObject& Request);
    protected:
        bool event(QEvent* Event) override;
    private:
        void collect();
        struct NativeBinding {
            quint64 owner_ = 0, generation_ = 0;
            bool operator==(const NativeBinding&) const = default;
        };
        struct Work;
        struct BindingState;
        class Registry;
        static void run(const std::shared_ptr<Work>& State, WebProcessRuntime Runtime, WebProcessLimits Limits);
        WebProcessRuntime runtime_;
        WebProcessLimits limits_;
        std::shared_ptr<Work> work_;
        std::shared_ptr<BindingState> native_;
        bool bound_ = false, loaded_ = false, failed_ = false, closed_ = false;
        QTimer* timer_ = nullptr;
        QJsonObject reported_, bindings_;
        qint64 contextSerial_ = 0, hostCommandSerial_ = 0;
    };
}
