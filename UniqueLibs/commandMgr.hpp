#pragma once

#include "cmnConcurrent.hpp"

#include "commandMgr_Defs.hpp"
#include "commandUiBridge.hpp"
#include "commandUiConfig.hpp"

#include <memory>
#include <QAbstractNativeEventFilter>

namespace Qtitan
{
    class GridViewBase;
}

class CCommandMgr : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
  public:
    CCommandMgr( QObject* parent = nullptr );
    ~CCommandMgr() override;
    bool GlobalShortcutEnabled(const QString& Id) const;
    void SetGlobalShortcutEnabled(const QString& Id, bool Enabled);
    QString GlobalShortcutError(const QString& Id) const;

    void                  Refresh();
    void                  SaveSettings();
    TyMapShortcutToCMDStr Retrieve() const;
    TyMapShortcutToCMDStr RetrieveForCommand(const QString& Id) const;
    TyHC_COMMAND          GetDefaultCMD( const QString& Command );

    void SetShortcutWithCMD( QKeyCombination Key, const QString& CMD );
    void DelShortcut( QKeyCombination Key, const QString& Id = {} );

    // 보기 입력 범위(processView·serviceView). 전역·명령행 범위는 기존 함수로 넘긴다.
    TyMapShortcutToCMDStr RetrieveForScope( hc::ui::InputScope Scope ) const;
    void SetShortcutInScope( QKeyCombination Key, const QString& CMD, hc::ui::InputScope Scope );
    void DelShortcutInScope( QKeyCombination Key, const QString& Id, hc::ui::InputScope Scope );
    void ResetScope( hc::ui::InputScope Scope );
    // 보기 범위 키만 처리한다. 처리하지 않았으면 false(호출자가 전역 키로 넘어간다).
    bool ProcessScopedKeyPress( QKeyEvent* KeyEvent, hc::ui::InputScope Scope, const QModelIndex& CursorIndex = {} );

    QWidget*         GetMainUI() const;
    void             SetMainUI( QWidget* MainUI );
    CommandUiBridge* Bridge() const
    {
        return bridge_;
    }

    bool ProcessKeyPressEvent( QKeyEvent* KeyEvent, const QModelIndex& CursorIndex );
    bool InvokeCommand( const QString& Id, const QModelIndex& CursorIndex );
    void ProcessFavoriteDir( const QString& Command, const QString& Path );

  signals:
    void ShortcutsChanged();

  public slots:
    ////////////////////////////////////////////////////////////////////////////
    /// 명령 핸들러

    DECLARE_HC_COMMAND( TabNew );
    DECLARE_HC_COMMAND( TabPrevious );
    DECLARE_HC_COMMAND( TabNext );
    DECLARE_HC_COMMAND( TabClose );

    DECLARE_HC_COMMAND( ViewerQuick );

    DECLARE_HC_COMMAND( FileCopy );
    DECLARE_HC_COMMAND( FileRename );
    DECLARE_HC_COMMAND( ViewerOpen );
    DECLARE_HC_COMMAND( NavigateOpen );
    DECLARE_HC_COMMAND( FileMkdir );
    DECLARE_HC_COMMAND( FileDelete );
    DECLARE_HC_COMMAND( FileNormalize );
    DECLARE_HC_COMMAND( FileAttributes );

    DECLARE_HC_COMMAND( SelectionInvert );

    DECLARE_HC_COMMAND( FileBatchRename );
    DECLARE_HC_COMMAND( FileNew );
    DECLARE_HC_COMMAND( ArchiveCreate );
    DECLARE_HC_COMMAND( ArchiveExtract );

    DECLARE_HC_COMMAND( ViewHidden );
    DECLARE_HC_COMMAND( NavigateRefresh );

    DECLARE_HC_COMMAND( NavigateFavorites );
    DECLARE_HC_COMMAND_EX( NavigateDrive );
    DECLARE_HC_COMMAND_EX( NavigatePath );

    DECLARE_HC_COMMAND( PanelSwitch );
    DECLARE_HC_COMMAND( FileContext );
    DECLARE_HC_COMMAND( FileEdit );

  private:
    using TySpMapKeyToCMDStr = std::shared_ptr<TyMapShortcutToCMDStr>;

    TySpMapKeyToCMDStr retrieve() const;
    bool nativeEventFilter(const QByteArray& Type, void* Message, qintptr* Result) override;
    void refreshGlobalShortcuts();
    void clearGlobalShortcuts();
    struct GlobalKey { QString command_; QKeyCombination key_; unsigned modifiers_; unsigned virtualKey_; };
    QHash<int, GlobalKey> globalKeys_;
    QHash<QString, QString> globalErrors_;

    TySpMapKeyToCMDStr SpMapKeyToCMDText_;

    QWidget*                  MainUI_ = nullptr;
    QPointer<CommandUiBridge> bridge_;
};

using TyStCommandMgr = nsCmn::nsConcurrent::TSingleton<CCommandMgr>;
