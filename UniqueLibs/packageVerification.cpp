#include "stdafx.h"
#include "packageVerification.hpp"

/// 고유 라이브러리
#include "HyperCommanderUpdater/updateTrust.hpp"
#include "nativeFilePath.hpp"
#include "pluginHost/manifestFile.hpp"

/// 플랫폼/프레임워크 헤더파일
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include <mutex>
#include <stop_token>
#include <vector>

namespace hc::packages
{
    namespace
    {
        struct HandleCloser
        {
            void operator()(void* Handle) const { CloseHandle(Handle); }
        };
        using Handle = std::unique_ptr<void, HandleCloser>;
        constexpr auto HashLength = 64;
        constexpr auto RequestLength = 2 * HashLength;
        constexpr auto MaximumFiles = 512;

        Handle OpenLease(const QString& Path, bool Directory)
        {
            const auto Native = storage::native::FilePath(Path);
            const auto Raw = CreateFileW(reinterpret_cast<const wchar_t*>(Native.utf16()),
                Directory ? FILE_READ_ATTRIBUTES : GENERIC_READ,
                Directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | (Directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN), nullptr);
            return Handle(Raw == INVALID_HANDLE_VALUE ? nullptr : Raw);
        }

        bool FileIdentity(HANDLE File, QCryptographicHash& Identity)
        {
            FILE_ID_INFO Id{};
            if (GetFileInformationByHandleEx(File, FileIdInfo, &Id, sizeof(Id))) {
                Identity.addData(QByteArrayView(reinterpret_cast<const char*>(&Id.VolumeSerialNumber), sizeof(Id.VolumeSerialNumber)));
                Identity.addData(QByteArrayView(reinterpret_cast<const char*>(&Id.FileId.Identifier), sizeof(Id.FileId.Identifier)));
                return true;
            }
            // Older filesystems may not expose the 128-bit NTFS file identity.
            // They still support the local verifier, so leave cross-process reuse disabled.
            return false;
        }

        bool TrustedVerifier(QLocalSocket& Socket)
        {
            ULONG Pid = 0;
            if (!GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(Socket.socketDescriptor()), &Pid))
                return false;
            if (Pid == GetCurrentProcessId()) return true;
            Handle Process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid));
            wchar_t Image[32768];
            DWORD Length = std::size(Image);
            if (!Process || !QueryFullProcessImageNameW(Process.get(), 0, Image, &Length)) return false;
            const auto Product = QDir(QCoreApplication::applicationDirPath()).filePath("HyperCommander.exe");
            if (QDir::fromNativeSeparators(QString::fromWCharArray(Image, Length)).compare(Product, Qt::CaseInsensitive) != 0)
                return false;
            const auto Lease = OpenLease(Product, false);
            if (!Lease) return false;
            try {
                update::VerifyProductSignature(Product.toStdWString(), "HyperCommander.exe");
                return true;
            } catch (const std::exception&) {
                return false;
            }
        }
    }

    struct FileLease::State
    {
        std::vector<Handle> handles_;
        QByteArray key_, identity_;
    };

    FileLease::FileLease() : state_(std::make_unique<State>()) {}
    FileLease::~FileLease() = default;

    std::shared_ptr<FileLease> FileLease::Acquire(const VerificationRequest& Request)
    {
        if (Request.hashes_.isEmpty() || Request.hashes_.size() > MaximumFiles
            || Request.root_.isEmpty() || !QDir::isAbsolutePath(Request.root_)) return {};
        auto Lease = std::shared_ptr<FileLease>(new FileLease);
        auto& Files = *Lease->state_;
        QCryptographicHash Key(QCryptographicHash::Sha256), Identity(QCryptographicHash::Sha256);
        Key.addData(Request.root_.toCaseFolded().toUtf8());
        QSet<QString> Directories, Paths;
        bool HasIdentity = true;
        for (auto At = Request.hashes_.cbegin(); At != Request.hashes_.cend(); ++At) {
            const auto Parts = At.key().split('/');
            const auto Folded = At.key().toCaseFolded();
            if (At.key().contains('\\') || At.key().contains(':') || At.key().contains(QChar::Null)
                || Parts.size() > 32 || Parts.contains("") || Parts.contains(".") || Parts.contains("..")
                || Paths.contains(Folded) || At.value().size() != HashLength
                || QByteArray::fromHex(At.value()).toHex() != At.value())
                return {};
            Paths.insert(Folded);
            Key.addData(QByteArrayView("\0", 1));
            Key.addData(At.key().toUtf8());
            Key.addData(At.value());
            const auto Path = QDir(Request.root_).filePath(At.key());
            QStringList Ancestors;
            for (auto Directory = QFileInfo(Path).absolutePath(); !Directories.contains(Directory); ) {
                Ancestors.prepend(Directory);
                const auto Parent = QFileInfo(Directory).absolutePath();
                if (Parent == Directory) break;
                Directory = Parent;
            }
            for (const auto& Directory : Ancestors) {
                auto Lock = OpenLease(Directory, true);
                if (!Lock) return {};
                BY_HANDLE_FILE_INFORMATION Info{};
                if (!GetFileInformationByHandle(Lock.get(), &Info) || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    || !(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return {};
                Directories.insert(Directory);
                Files.handles_.push_back(std::move(Lock));
            }
            auto Lock = OpenLease(Path, false);
            if (!Lock) return {};
            BY_HANDLE_FILE_INFORMATION Info{};
            if (!GetFileInformationByHandle(Lock.get(), &Info)
                || (Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return {};
            HasIdentity &= FileIdentity(Lock.get(), Identity);
            Files.handles_.push_back(std::move(Lock));
        }
        Files.key_ = Key.result().toHex();
        if (HasIdentity) Files.identity_ = Identity.result().toHex();
        return Lease;
    }

    bool FileLease::HasStartupVerification() const
    {
        const auto Endpoint = qEnvironmentVariable(VerificationEndpoint);
        if (Endpoint.isEmpty() || state_->identity_.isEmpty() || qEnvironmentVariableIsSet(VerifyEachOpen)) return false;
        QLocalSocket Socket;
        Socket.connectToServer(Endpoint);
        if (!Socket.waitForConnected(50) || !TrustedVerifier(Socket)) return false;
        const auto Request = state_->key_ + state_->identity_;
        if (Socket.write(Request) != RequestLength) return false;
        Socket.flush();
        if (!Socket.bytesAvailable() && !Socket.waitForReadyRead(100)) return false;
        return Socket.read(1) == QByteArrayLiteral("1");
    }

    class VerificationService final : public QLocalServer
    {
    public:
        explicit VerificationService(QObject* Owner) : QLocalServer(Owner)
        {
            setSocketOptions(UserAccessOption);
            setMaxPendingConnections(16);
            connect(this, &QLocalServer::newConnection, this, [this] {
                while (auto* Socket = nextPendingConnection()) {
                    connect(Socket, &QLocalSocket::disconnected, Socket, &QObject::deleteLater);
                    Socket->setReadBufferSize(RequestLength);
                    QTimer::singleShot(1000, Socket, [Socket] { Socket->abort(); });
                    const auto Respond = [this, Socket] {
                        if (Socket->bytesAvailable() < RequestLength) return;
                        const auto Request = Socket->read(RequestLength);
                        bool Valid = false;
                        { std::lock_guard Lock(cache_->mutex_); Valid = cache_->leases_.contains(Request); }
                        Socket->write(Valid ? "1" : "0", 1);
                        Socket->disconnectFromServer();
                    };
                    connect(Socket, &QLocalSocket::readyRead, Socket, Respond);
                    Respond();
                }
            });
        }

        ~VerificationService() override { cache_->stop_.request_stop(); }

        void Warm(std::function<QList<VerificationRequest>()> Requests)
        {
            (void)QtConcurrent::run([Cache = cache_, Requests = std::move(Requests)] {
                QMap<QString, QByteArray> VerifiedFiles;
                const auto Packages = Requests();
                for (const auto& Request : Packages) {
                    if (Cache->stop_.stop_requested()) break;
                    const auto Lease = FileLease::Acquire(Request);
                    if (!Lease || Lease->state_->identity_.isEmpty()
                        || !verify(Request, VerifiedFiles, Cache->stop_.get_token())) continue;
                    std::lock_guard Lock(Cache->mutex_);
                    Cache->leases_.insert(Lease->state_->key_ + Lease->state_->identity_, Lease);
                }
            });
        }

    private:
        static bool verify(const VerificationRequest& Request, QMap<QString, QByteArray>& VerifiedFiles, std::stop_token Stop)
        {
            QMap<QString, QByteArray> NewFiles;
            try {
                const QDir Root(Request.root_);
                for (auto At = Request.hashes_.cbegin(); At != Request.hashes_.cend(); ++At) {
                    if (Stop.stop_requested()) return false;
                    const auto Path = Root.filePath(At.key());
                    if (VerifiedFiles.value(Path) == At.value()) continue;
                    if (update::HashFile(Path.toStdWString(), 512ULL * 1024 * 1024, Stop) != At.value().toStdString())
                        return false;
                    NewFiles.insert(Path, At.value());
                }
            } catch (const std::exception&) {
                return false;
            }
            // Earlier packages keep the shared Qt binaries locked for the lifetime of the cache.
            VerifiedFiles.insert(NewFiles);
            return true;
        }

        struct Cache
        {
            std::mutex mutex_;
            std::stop_source stop_;
            QMap<QByteArray, std::shared_ptr<FileLease>> leases_;
        };
        std::shared_ptr<Cache> cache_ = std::make_shared<Cache>();
    };

    void ConfigureVerification(QObject* Owner, bool Strict, std::function<QList<VerificationRequest>()> Requests)
    {
        qunsetenv(VerificationEndpoint);
        delete Owner->findChild<QLocalServer*>("hc.packageVerification", Qt::FindDirectChildrenOnly);
        if (Strict) {
            qputenv(VerifyEachOpen, "1");
            return;
        }
        qunsetenv(VerifyEachOpen);
        auto* Service = new VerificationService(Owner);
        Service->setObjectName("hc.packageVerification");
        if (!Service->listen(QStringLiteral("hypercommander-verify-") + QUuid::createUuid().toString(QUuid::Id128))) {
            delete Service;
            return;
        }
        qputenv(VerificationEndpoint, Service->serverName().toUtf8());
        Service->Warm(std::move(Requests));
    }

    VerificationRequest ForPlugin(const plugins::InspectedPlugin& Plugin)
    {
        VerificationRequest Request{Plugin.root_, {}};
        const auto& Manifest = Plugin.manifest_;
        Request.hashes_.insert(QString::fromStdString(Manifest.binary_), QByteArray::fromStdString(Manifest.sha256_));
        for (const auto& [Path, Hash] : Manifest.dependencies_)
            Request.hashes_.insert(QString::fromStdString(Path), QByteArray::fromStdString(Hash));
        for (const auto& [Path, Hash] : Manifest.helpers_)
            Request.hashes_.insert(QString::fromStdString(Path), QByteArray::fromStdString(Hash));
        return Request;
    }
}
