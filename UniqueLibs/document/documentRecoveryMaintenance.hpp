#pragma once

/// 모듈
#include "documentRecoveryStore.hpp"

/// UIs

/// 기타 헤더파일

/// 고유 라이브러리

/// 공통 라이브러리

/// 외부 라이브러리

/// 선언 헤더파일들

/// 플랫폼/프레임워크 헤더파일( Windows, Boost, Qt, ... )
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <vector>

/// Recovery copies of HyperEditor and HyperViewer kept in <config>/recovery: snapshots and HotExit sessions,
/// EPUB edit drafts (epub-drafts) and edited copies of files inside archives (archive-drafts).
/// Everything here is plain file work for a worker thread; nothing touches widgets.
namespace hc::document::recovery
{
    struct Policy
    {
        QString startupAction_ = QStringLiteral( "ask" );   // after an abnormal exit: ask · restore · keep
        int retentionDays_ = 30;                             // 0: unlimited
        qint64 maxBytes_ = 1024LL * 1024 * 1024;             // 0: unlimited
    };
    /// documentViews/recovery of a configuration document; missing values use the product defaults.
    Policy PolicyFrom( const QJsonObject& Configuration );
    Policy LoadPolicy( const QString& ConfigDirectory );

    // ---- moving the pre-2026.11 locations -------------------------------------------------------------
    struct MigrationReport { int moved_ = 0; QStringList sources_; QStringList errors_; };
    /// Snapshot stores under %LOCALAPPDATA% (this profile's sessions and unowned copies) and pending archive
    /// copies in %TEMP% are copied, never moved: those folders give up the copied files 30 days later. The EPUB
    /// drafts beside the settings move (same installation). Records a notice for the settings page.
    /// Call only for the product's own configuration folder.
    MigrationReport MigrateLegacy( const QString& ConfigDirectory, const QString& ProfileId,
        const QStringList& LegacyRoots, const QString& TemporaryDirectory );
    /// {"count", "sources", "utc"} while the settings page has not dismissed it; empty otherwise.
    QJsonObject MigrationNotice( const QString& Root );
    bool DismissMigrationNotice( const QString& Root );

    // ---- what the recovery list shows ------------------------------------------------------------------
    enum class Kind { Edit, Untitled, Epub, Archive };
    enum class State { Kept, HotExit, InUse, Pending };
    struct Item
    {
        Kind kind_ = Kind::Edit;
        State state_ = State::Kept;
        QString key_;            // document id · EPUB draft file · archive draft folder
        QString name_, location_;
        QString path_;           // snapshot file · EPUB draft file · archive content file
        QString owner_;          // window id that wrote a snapshot
        qint64 bytes_ = 0;
        QDateTime saved_, expires_;   // expires_ invalid: kept until opened or removed
        bool expired_ = false;
        QString applyBlocker_;   // archive copies: why HyperEditor cannot write it back itself (empty: it can)
    };
    std::vector< Item > ListItems( const QString& Root, const Policy& Settings, const QDateTime& Now );
    /// Removes one listed copy and its entry in a session that would reopen it. Returns an error code.
    QString RemoveCopy( const QString& Root, const Item& Entry );

    struct UsageRow { int count_ = 0; qint64 bytes_ = 0; QDateTime expires_; int expired_ = 0; };
    struct Usage
    {
        UsageRow edits_, hotExit_, epub_, archive_;
        qint64 Bytes() const { return edits_.bytes_ + hotExit_.bytes_ + epub_.bytes_ + archive_.bytes_; }
        int Count() const { return edits_.count_ + hotExit_.count_ + epub_.count_ + archive_.count_; }
        int Expired() const { return edits_.expired_ + epub_.expired_ + archive_.expired_; }
    };
    /// File statistics only; cheap enough for the settings page.
    Usage Summarize( const QString& Root, const Policy& Settings, const QDateTime& Now );

    // ---- retention ----------------------------------------------------------------------------------
    enum class PurgeScope
    {
        Expired,    // only copies past the retention period (the settings page's button)
        Startup     // also the size cap, abandoned sessions and leftovers no reader uses
    };
    struct PurgeReport { int removed_ = 0; qint64 bytes_ = 0; QStringList errors_; };
    PurgeReport Purge( const QString& Root, const Policy& Settings, const QDateTime& Now, PurgeScope Scope );

    // ---- sessions left by an abnormal exit ----------------------------------------------------------
    struct PromptEntry
    {
        enum Type { Edit, Untitled, Epub, Archive };
        Type type_ = Edit;
        QString key_;            // document id, or the archive draft folder
        QString name_, location_;
    };
    struct PromptGroup
    {
        QString sessionId_;      // empty: archive copies whose viewer ended
        QDateTime saved_;
        std::vector< PromptEntry > entries_;
        int clean_ = 0;          // unmodified documents that a restore reopens
    };
    /// Abnormally ended sessions of ProfileId (no live window) and pending archive copies not yet kept.
    std::vector< PromptGroup > CrashGroups( const QString& Root, const QString& ProfileId );
    /// Ends a session without restoring it: the copies of Delete are removed, every other copy stays listed.
    bool ReleaseSession( const QString& Root, const QString& SessionId, const QStringList& Delete = {} );
    /// Pending archive copies the user chose to keep stop appearing in the restart prompt.
    bool KeepArchiveDrafts( const QStringList& Directories );
    bool RemoveArchiveDraft( const QString& Directory );
    QString ArchiveDraftContent( const QString& Directory );
    bool LockHeld( const QString& Path );

    // ---- writing an archive copy back ------------------------------------------------------------------
    /// Empty when HyperEditor can update the archive itself: a ZIP file (not nested in another archive)
    /// within the EPUB package limits. Other formats are written back by HyperCommander (later work).
    QString ArchiveApplyBlocker( const QJsonObject& Manifest );
    QJsonObject ReadArchiveManifest( const QString& Directory );
    /// Replaces the entry when it still has the content the edit started from. Returns an error code.
    QString ApplyArchiveDraft( const QString& Directory );
}
