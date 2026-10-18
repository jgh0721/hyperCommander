#pragma once
#include "hwpBundleHost.hpp"
#include "hwpInputTransfer.hpp"
#include "hwpEditBridge.hpp"
#include "hwpPrintBridge.hpp"
#include <QObject>
#include <QWindow>
#include <QJsonObject>
#include <cstdint>
namespace hc::document
{
    // GUI-thread native pipe controller; never exposed to WebChannel.
    // Bundle must already be approved by the caller. A null bundle denies all work.
    class HwpHelperDocument final : public QObject
    {
    public:
        explicit HwpHelperDocument(std::shared_ptr<const HwpBundle> Bundle,
            std::uintptr_t ParentWindow = 0, int LoadTimeoutMs = 30000, bool AllowEdit = false);
        ~HwpHelperDocument() override;
        QJsonObject Handle(const QJsonObject& Request);
        void Refresh();
        bool RendererCreated() const { return bool(view_); }
    private:
        QJsonObject status() const;
        void fail(const QString& Code);
        std::shared_ptr<const HwpBundle> bundle_;
        HwpInputTransfer input_;
        HwpEditBridge editor_;
        HwpPrintBridge print_;
        bool allowEdit_ = false, editing_ = false;
        QJsonObject identity_;
        QByteArray inputSha_;
        std::unique_ptr<QWindow> parentWindow_;
        std::unique_ptr<HwpBundleView> view_;
        std::uintptr_t parent_, child_ = 0;
        int timeoutMs_;
        QString state_ = "idle", error_;
    };
}
