#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QWidget>
#include <QPointer>
#include "hwpLoadBridge.hpp"
#include <memory>

class QWebEngineView;
class QWebEngineProfile;
namespace hc::document
{
    // Pins come from native package approval, never a manifest inside an
    // untrusted bundle. This verifies bytes; it does not confer package trust.
    class HwpEditBridge;
    class HwpPrintBridge;
    class HwpBundle final
    {
    public:
        static std::shared_ptr<const HwpBundle> LoadPinned(const QString& Root,
            const QMap<QString, QByteArray>& Pins, QString& Error);
        bool Allows(const QUrl& Url) const;
        QByteArray Bytes(const QUrl& Url) const;
    private:
        QMap<QString, QByteArray> files_;
    };
    // Register before QApplication. No profile/page/renderer is created here.
    bool RegisterHwpScheme();
    bool HwpWebArgumentsAllowed(const QStringList& Arguments);
    bool HwpWebEnvironmentAllowed();

    // Optional helper-side surface; never linked into the ordinary viewer DLL.
    // Typed read-only bridge; no file picker, downloads or editing capability.
    class HwpBundleView final : public QWidget
    {
    public:
        static std::unique_ptr<HwpBundleView> Create(std::shared_ptr<const HwpBundle> Bundle, QString& Error, HwpEditBridge* Editor = nullptr, HwpPrintBridge* Print = nullptr);
        ~HwpBundleView() override;
        QWebEngineView* View() const { return view_; }
        bool IsOffTheRecord() const;
        HwpLoadBridge* LoadBridge() const { return bridge_; }
    private:
        explicit HwpBundleView(std::shared_ptr<const HwpBundle> Bundle, const QString& ChannelScript, HwpEditBridge* Editor, HwpPrintBridge* Print);
        QPointer<HwpLoadBridge> bridge_;
        QWebEngineProfile* profile_;
        QWebEngineView* view_;
    };
}
