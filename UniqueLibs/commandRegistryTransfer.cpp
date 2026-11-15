#include "stdafx.h"
#include "commandUiBridge.hpp"

/// 모듈
/// UIs
#include "HyperCommander/UIs/cmpPanel.hpp"
/// 기타 헤더파일
/// 고유 라이브러리
#include "registry/registryCommands.hpp"
#include "registry/registryBatch.hpp"
#include "document/documentOpenController.hpp"
#include "storageProviders.hpp"
#include "nativeWritableStorage.hpp"
#include "nativeMaterializer.hpp"
#include "qtMessageDialog.hpp"
#include "builtInFsModel.hpp"
/// 공통 라이브러리
#include "commonLibs/storage/materializedContent.hpp"
/// 외부 라이브러리
/// 선언 헤더파일들
/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QDir>
#include <QFileInfo>

DECLARE_CMNLIBSV2_NAMESPACE
Q_LOGGING_CATEGORY(lcRegistryOpen, "hc.registry.open")
using namespace hc::storage;
namespace {
struct ImportInput
{
    QString path, error;
    std::shared_ptr<IMaterializer> store;
    std::shared_ptr<IMaterializedFileLease> lease;
};
}
void CommandUiBridge::OpenRegistrySourceEditor(const QString& Path)
{
    const auto NativePath = QDir::toNativeSeparators(QFileInfo(Path).absoluteFilePath());
    const auto Kind = NativePath.startsWith("\\\\") ? LocatorKind::Smb : LocatorKind::Local;
    auto Reader = MakeNativeReadProvider(Kind); auto* Watcher = new QFutureWatcher<Result<EntrySnapshot>>(this);
    connect(Watcher, &QFutureWatcherBase::finished, this, [this, Reader, Kind, Watcher] {
        const auto Entry = Watcher->result(); Watcher->deleteLater();
        if (!Entry.HasValue()) { hc::ui::MessageDialog::warning(main_, tr("HyperEditor로 열기"), QString::fromStdString(Entry.Failure().code_)); return; }
        hc::document::DocumentOpenRequest Request; Request.intent_ = hc::document::DocumentIntent::Edit;
        Request.documents_.push_back({std::make_shared<ProviderContentSource>(Reader, Entry.Value()), MakeNativeWritableStorage(Kind, Reader)});
        Request.stillCurrent_ = [Guard = QPointer<CommandUiBridge>(this)] { return !Guard.isNull(); };
        documents_->Open(documents_->Capture(std::move(Request)));
    });
    Watcher->setFuture(QtConcurrent::run([Reader, NativePath, Kind] { return Reader->Resolve({Kind, NativePath.toStdString()}, {}).get(); }));
}
void CommandUiBridge::OpenRegistryImport(CFSModel* Model, const QModelIndex& Index)
{
    if (!Model || !registryCommands_) return;
    const auto Info = Model->GetFileInfo(Index); if (!Info || (Info->Attributes_ & FILE_ATTRIBUTE_DIRECTORY)) return;
    if (!Info->storageEntry_) { registryCommands_->OpenImport(Model->GetFileFullPath(Index)); return; }
    const auto Entry = *Info->storageEntry_;
    if (Entry.size_ > hc::registry::RegFileParser::MaxFileBytes) { hc::ui::MessageDialog::warning(main_, tr("가져오기 미리 보기"), tr("파일 크기가 미리 보기 한도를 넘었습니다.")); return; }
    const auto Reader = Model->ProviderFor(Entry); if (!Reader) return;
    auto Source = std::make_shared<ProviderContentSource>(Reader, Entry);
    auto Writer = Entry.locator_.kind_ == LocatorKind::Local || Entry.locator_.kind_ == LocatorKind::Smb ? MakeNativeWritableStorage(Entry.locator_.kind_, Reader)
        : Entry.locator_.kind_ == LocatorKind::Plugin && Model->GetRoot() ? Model->GetRoot()->WriteProvider() : nullptr;
    hc::document::DocumentOpenRequest Request; Request.intent_ = hc::document::DocumentIntent::Edit;
    Request.documents_.push_back({Source, Writer});
    Request.stillCurrent_ = [Guard = QPointer<CommandUiBridge>(this)] { return !Guard.isNull(); };
    const auto Editor = [this, Request, Reader, Locator = Entry.locator_] {
        // Resolve a fresh revision whenever the source is reopened for editing.
        auto* Watcher = new QFutureWatcher<Result<EntrySnapshot>>(this);
        connect(Watcher, &QFutureWatcherBase::finished, this, [this, Watcher, Reader, Request]() mutable {
            const auto Fresh = Watcher->result(); Watcher->deleteLater();
            if (!Fresh.HasValue()) { hc::ui::MessageDialog::warning(main_, tr("HyperEditor로 열기"), QString::fromStdString(Fresh.Failure().code_)); return; }
            Request.documents_[0].source_ = std::make_shared<ProviderContentSource>(Reader, Fresh.Value());
            documents_->Open(documents_->Capture(std::move(Request)));
        });
        Watcher->setFuture(QtConcurrent::run([Reader, Locator] {
            OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(2);
            return Reader->Resolve(Locator, Context).get();
        }));
    };
    if (Entry.locator_.kind_ == LocatorKind::Local || Entry.locator_.kind_ == LocatorKind::Smb) {
        // The dialog hashes the freshly reloaded native file and checks the same
        // bytes at apply time, so do not retain the original directory revision.
        registryCommands_->OpenImport(QString::fromStdString(Entry.locator_.root_), {}, Editor); return;
    }
    const hc::registry::RegistryImportLoader Loader = [Reader, Locator = Entry.locator_](const std::atomic_bool* Cancel) {
        hc::registry::RegistryImportSource Result;
        if (Cancel && Cancel->load()) { Result.error = QObject::tr("취소했습니다."); return Result; }
        OperationContext Context; Context.deadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        const auto Fresh = Reader->Resolve(Locator, Context).get();
        if (!Fresh.HasValue()) { Result.error = QString::fromStdString(Fresh.Failure().code_); return Result; }
        auto Source = std::make_shared<ProviderContentSource>(Reader, Fresh.Value());
        auto Input = std::make_shared<ImportInput>(); MaterializationLimits Limits;
        Limits.maxFileBytes_ = hc::registry::RegFileParser::MaxFileBytes; Limits.maxTotalBytes_ = Limits.maxFileBytes_;
        auto Store = MakeNativeMaterializer(QDir::tempPath().toStdString(), Limits);
        if (!Store.HasValue()) { Result.error = QString::fromStdString(Store.Failure().code_); return Result; }
        Input->store = Store.Value();
        auto Lease = Input->store->Materialize({Source}, Context).get();
        if (!Lease.HasValue()) { Result.error = QString::fromStdString(Lease.Failure().code_); return Result; }
        if (Cancel && Cancel->load()) { Result.error = QObject::tr("취소했습니다."); return Result; }
        Input->lease = Lease.Value(); Result.path = QString::fromStdString(Input->lease->LocalPath()); Result.keepAlive = Input;
        Result.verify = [Source] {
            OperationContext Check; Check.deadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(2);
            return Source->Verify(Check).get().HasValue();
        };
        return Result;
    };
    // Reload replaces the materialized lease and its revision guard together.
    // The loader runs on the preview worker, never on the GUI thread.
    registryCommands_->OpenImport(QString::fromStdString(Entry.locator_.root_), {}, Editor, {}, Loader);
}