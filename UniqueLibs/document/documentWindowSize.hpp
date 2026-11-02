#pragma once
#include "UniqueLibs/settings/appSettings.hpp"
#include <QEvent>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <utility>

namespace hc::document
{
    // Shared by the document workspace and the standalone HWP editor window.
    class DocumentWindowSize final : public QObject
    {
    public:
        DocumentWindowSize(QWidget* Window, bool Viewer, std::function<void(const QString&)> ErrorHandler)
            : QObject(Window), window_(Window), viewer_(Viewer), errorHandler_(std::move(ErrorHandler))
        {
            if (!Window->isWindow()) return;
            const auto Override = QCoreApplication::instance()->property("documentViewsConfigDir");
            const auto Directory = Override.isValid() ? Override.toString() : settings::DocumentPaths().directory_;
            path_ = viewer_ ? QDir(Directory).filePath(defaults::edit::ViewerConfigFileName) : defaults::edit::SettingsFile(Directory);
            saveTimer_.setSingleShot(true); saveTimer_.setInterval(300);
            connect(&saveTimer_, &QTimer::timeout, this, [this] { Save(); });
            window_->installEventFilter(this);
            Restore();
        }
        ~DocumentWindowSize() override { Save(false); }
        void Restore()
        {
            if (path_.isEmpty()) return;
            settings::JsonStore Store(path_, defaults::edit::DefaultDocument(), nullptr, scope());
            const auto Window = Store.Object()["window"].toObject();
            const QSize Saved(Window["width"].toInt(-1), Window["height"].toInt(-1));
            auto Size = Saved.width() > 0 && Saved.height() > 0 ? Saved : defaults::edit::WindowSize;
            if (window_->screen()) Size = Size.boundedTo(window_->screen()->availableGeometry().size());
            saveTimer_.stop(); remembered_ = stored_ = Size;
            window_->resize(Size); remembered_ = stored_ = window_->size();
            if (!Store.Error().isEmpty()) errorHandler_(tr("창 크기를 읽지 못했습니다: %1").arg(Store.Error()));
        }
        void Save(bool ReportErrors = true)
        {
            saveTimer_.stop();
            if (path_.isEmpty() || remembered_ == stored_) return;
            // Fresh reads and the existing atomic merge preserve other windows' settings.
            settings::JsonStore Store(path_, defaults::edit::DefaultDocument(), nullptr, scope());
            auto Root = Store.Object(); auto Window = Root["window"].toObject();
            Window["width"] = remembered_.width(); Window["height"] = remembered_.height(); Root["window"] = Window;
            if (Store.Writable() && Store.Replace(Root)) stored_ = remembered_;
            else if (ReportErrors) {
                if (Store.Error() == "file-locked" || Store.Error() == "changed-on-disk") saveTimer_.start();
                else errorHandler_(tr("창 크기를 저장하지 못했습니다: %1").arg(Store.Error()));
            }
        }
    protected:
        bool eventFilter(QObject* Watched, QEvent* Event) override
        {
            if (Watched == window_ && window_->isWindow()) {
                if (Event->type() == QEvent::Resize && window_->isVisible()
                    && !window_->isMinimized() && !window_->isMaximized() && !window_->isFullScreen()) {
                    remembered_ = window_->size();
                    if (remembered_ != stored_) saveTimer_.start(); else saveTimer_.stop();
                } else if (Event->type() == QEvent::Hide && saveTimer_.isActive()) Save();
            }
            return QObject::eventFilter(Watched, Event);
        }
    private:
        settings::JsonStore::Scope scope() const
        { return viewer_ ? settings::JsonStore::Scope::Standalone : settings::JsonStore::Scope::Product; }
        QWidget* window_;
        bool viewer_;
        QString path_;
        QSize remembered_, stored_;
        QTimer saveTimer_;
        std::function<void(const QString&)> errorHandler_;
    };
}
