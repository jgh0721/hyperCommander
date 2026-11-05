#pragma once

/// 고유 라이브러리
#include "solTCPluginMgr.hpp"
#include "wcxStorage.hpp"
#include "pluginHost/approvedWcxBackend.hpp"
#include "commonLibs/storage/writableStorage.hpp"

namespace hc::navigation
{
    struct ContainerCandidate
    {
        TyPlugInWCX wcx_;
        hc::plugins::ApprovedWcxOptions options_;
        QJsonValue application_;
        QStringList extensions_;
        bool dopus_ = false;
        bool content_ = false;
    };
    struct ContainerRequest
    {
        std::shared_ptr<hc::storage::IStorageProvider> source_;
        hc::storage::EntrySnapshot entry_;
        std::vector<ContainerCandidate> candidates_;
        hc::storage::WcxBackends ancestors_;
    };
    struct OpenedContainer
    {
        ContainerCandidate choice_;
        hc::storage::Locator location_;
        std::shared_ptr<hc::storage::IStorageProvider> reader_;
        std::shared_ptr<hc::storage::IWritableStorage> writer_;
        std::shared_ptr<hc::plugins::IArchiveBackend> backend_;
        hc::storage::WcxBackends ancestors_;
        QString native_;
    };
    // Capture approved configuration on the GUI thread; all content work stays on the caller's worker.
    std::vector<ContainerCandidate> FileContainerCandidates();
    hc::storage::Result<OpenedContainer> OpenFileContainer(ContainerRequest Request, hc::storage::OperationContext Context);
}
