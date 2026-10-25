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
        bool Load(QByteArray Bytes, qint64 Generation = 1);
        void RequestClose();
        // Explicit activation request; hidden/closing views reject it.
        bool FocusInput();
        bool ChangeReadingSettings(const QString& Action, const QJsonObject& Profile = {}, bool Global = false);
        bool ReaderAction(const QString& Action,const QJsonValue& Value={});
        QJsonObject ViewState() const;
        bool IsLoaded() const { return loaded_; }
        static void InitializeLifecycle();
        static void RequestShutdown();
        static bool IsDrained();
        static void ReleaseLifecycle();
    signals:
        void Loaded();
        void EscapeRequested();
        void FullscreenRequested();
        void Failed(const QString& Error);
        void Closed();
        void ViewStateChanged();
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
        QJsonObject reported_;
    };
}
