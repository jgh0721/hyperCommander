#pragma once

#include <QtWidgets>
#include <functional>
#include <fmfilelist/RecordPainting.h>
#include "filePanelSettings.hpp"

#include "cmnTypeDefs.hpp"
#include "Modules/mdlFileSystem.hpp"

// Panel 에 표시되는 뷰 형태의 기초 클래스
// 또는 Lister 에 표시되는 WLX 등을 호스트한다.

class CFsModelT;
struct ColumnView;
class QtitanColumnAdapter;
class QtViewerHost;
class QtViewerSurface;
class ThumbnailModel;
namespace fm::filelist { class ThumbnailView; }
namespace hc::storage { class IContentSource; }

class CViewT : public QWidget
{
    Q_OBJECT
public:
    enum TyEnViewMode { VM_NONE, VM_GRID, VM_QUICK, VM_LISTER };
    CViewT( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

    TyEnViewMode                    GetViewMode() const { return viewMode_; }
    virtual void                    ApplyColorScheme( const TyColorScheme& ColorScheme );
    virtual bool                    IsEditing() const { return false; };
    virtual void                    AdjustSizeToContents() const {};
    virtual void                    EnsureKeyboardFocusOnView() const {};
    virtual void                    CloseView() {};

signals:
    void                            sig_NotifyViewActivated();

protected:
    bool                            eventFilter( QObject* watched, QEvent* Event ) override;

    TyEnViewMode                    viewMode_ = VM_NONE;
};

namespace Qtitan
{
    class Grid;
    class GridViewBase;
    class GridSelection;
    class GridRecordPainter;
    class GridBandedTableView;
    class ContextMenuEventArgs;
    class CellClickEventArgs;
    class RowClickEventArgs;
    class EditorEventArgs;
} // Qtitan

class CGridView : public CViewT
{
    Q_OBJECT
public:
    CGridView( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );
    ~CGridView() override;
    bool SetListMode(const QString& Mode);
    QString ListMode() const;
    QString ListModeOverride() const { return listModeOverride_.isEmpty() ? "inherit" : listModeOverride_; }
    const fm::filelist::ListAppearance& ListAppearance() const { return listAppearance_; }
    const fm::filelist::detail::RecordGeometry& RecordGeometry() const { return recordGeometry_; }
    const fm::style::ThemeColors& RecordColors() const { return recordColors_; }
    // 레코드 블록 높이에 맞춘 아이콘 크기(논리 px)와 2줄 보기의 글자 들여쓰기.
    int ListIconSize() const;
    int TwoLineTextIndent() const;
    bool IsPanelActive() const { return panelActive_; }
    bool IsTwoLine() const;
    bool IsThumbnailMode() const;
    using RenameCompletion = std::function<void(bool)>;
    using RenameHandler = std::function<void(const QModelIndex&, const QString&, RenameCompletion)>;
    bool BeginRename(const QModelIndex& Index, bool SelectExtension, RenameHandler Handler);
    void CancelRename();
    void SyncThumbnailCursor();
    void ClearThumbnailCache();
    ThumbnailModel* Thumbnails() const { return thumbnailModel_; }


    void                            SetModel( CFsModelT* Source, QAbstractProxyModel* Proxy, ColumnView* Columns );
    void                            ApplyColorScheme( const TyColorScheme& ColorScheme ) override;
    void                            ApplyDisplaySettings();
    void                            SetPanelActive(bool Active);
    bool                            IsEditing() const override;
    void                            AdjustSizeToContents() const override;
    void                            EnsureKeyboardFocusOnView() const override;

    nsHC::TySpFileSource            SelectRowOnCurrentTab( const QModelIndex& SrcIndex, bool IsMoveDown, bool& IsSelected );
    void                            ContextMenuOnCurrentTab( const QModelIndex& SrcIndex );
    QStringList ShellMenuPaths(const QModelIndex& Index = {}, bool UseSelection = true) const;
    bool ShowShellMenu(const QModelIndex& Index = {}, bool AtCursor = false);

    Qtitan::GridViewBase*           BaseView() const;
    CFsModelT*                      BaseModel() const;
    QtitanColumnAdapter*            ColumnWidths() const { return columnAdapter_; }
    void                           ConfigureColumns();
    bool                           ApplyColumnProfile(const hc::columns::ColumnProfile& Profile);
    hc::columns::ColumnProfile      ColumnProfile() const;
    bool AutomaticColumns() const { return !manualColumns_; }
    void SetAutomaticColumns(bool Automatic);
    QJsonObject                    CaptureWorkspaceView() const;
    void                           RestoreWorkspaceView(const QJsonObject& View);
    QStringList                    CaptureSelection() const;
    QString                        CaptureFocus() const;
    void                           RestoreSelection(const QStringList& Selection, const QString& Focus);

signals:
    void                           sigViewSettingsChanged();
    void                           sigListPresentationChanged();
    void sigThumbnailSelectionChanged();

protected slots:
    void                            oo_grid_contextMenu( Qtitan::ContextMenuEventArgs* Args );
    void                            oo_grid_cellClicked( Qtitan::CellClickEventArgs* Args );
    void                            oo_grid_rowDblClicked( Qtitan::RowClickEventArgs* Args );
    void                            oo_grid_rowFocusChanged( int OldRowIndex, int NewRowIndex );
    void                            oo_grid_editorStarting( Qtitan::EditorEventArgs* Args );
    void                            oo_grid_editorStarted( Qtitan::EditorEventArgs* Args );
    void                            oo_grid_editorPosting( Qtitan::EditorEventArgs* Args );
    void                            oo_grid_selectionChanged( Qtitan::GridSelection* NewSelection, Qtitan::GridSelection* OldSelection );

protected:
    bool                            eventFilter( QObject* watched, QEvent* Event ) override;
    void hideEvent(QHideEvent* Event) override;
    void showEvent(QShowEvent* Event) override;
    void                            resizeEvent( QResizeEvent* event ) override;
    bool                            nativeEvent( const QByteArray& eventType, void* message, qintptr* result ) override;

private:
    void resetRenameClicks();
    void selectRenameText(bool IncludeExtension);
    void positionRenameEditor();
    void commitRename(int Direction);
    void finishRename(bool Success, quint64 Generation);
    void continueRename();
    QPointer<QLineEdit> renameEditor_;
    QPersistentModelIndex renameIndex_;
    RenameHandler renameHandler_;
    QTimer renameTimer_;
    QString renameLocation_, renameOriginal_, renameNext_;
    bool renameSelectExtension_ = false, renameSubmitting_ = false, renameAwaitingRefresh_ = false, renameRetry_ = false;
    int renameDirection_ = 0;
    quint64 renameGeneration_ = 0;
    QModelIndex                     retrieveCursorIndex() const;
    void syncViewportBackground();
    void requestPresentation();
    void requestAutomaticColumns();
    bool manualColumns_ = false, applyingAutoColumns_ = false;
    QByteArray automaticColumnsSignature_;
    quint64 automaticColumnsGeneration_ = 0;
    std::stop_source automaticColumnsStop_;
    std::optional<hc::columns::ColumnProfile> automaticColumnsBase_;
    void applyThumbnailPresentation();
    fm::filelist::ThumbnailView* thumbnails_ = nullptr;
    ThumbnailModel* thumbnailModel_ = nullptr;
    QStackedWidget* presentationStack_ = nullptr;
    std::optional<int> thumbnailSizeOverride_;
    bool syncingThumbnailCursor_ = false;
    void applyPresentation();
    QJsonObject capturePresentation() const;
    void restorePresentation(const QJsonObject& View);
    QString listModeOverride_;
    std::optional<bool> nameBelowOverride_;
    QJsonObject unsupportedPresentation_;
    fm::filelist::ListAppearance listAppearance_;
    fm::filelist::detail::RecordGeometry recordGeometry_;
    fm::style::ThemeColors recordColors_;
    QTimer presentationTimer_;
    QTimer selectionSummaryTimer_;
    bool panelActive_ = false, applyingPresentation_ = false, restoringPresentation_ = false;
    bool autoInitialized_ = false, autoTwoLine_ = false;
    QString autoLocation_;
    Qtitan::GridRecordPainter* recordPainter_ = nullptr;


    QVBoxLayout*                    vbox_ = nullptr;
    Qtitan::Grid*                   grid_ = nullptr;
    Qtitan::GridBandedTableView*    view_ = nullptr;

    QJsonObject                     appliedDisplaySettings_;
    CFsModelT*                      model_ = nullptr;
    QAbstractProxyModel*            proxy_ = nullptr;
    QtitanColumnAdapter*            columnAdapter_ = nullptr;
    QMetaObject::Connection         selectionReset_;
    QMetaObject::Connection         selectionRestore_;
    QStringList                    resetSelection_;
    QString                        resetFocus_, resetPath_;
    
    // DataGrid 의 클릭 상태 추적을 위한 타이머
    QTimer                          viewClickTimer_;
    QPersistentModelIndex           mouseRenameIndex_;
    QPersistentModelIndex           lastNameClickIndex_;
    QElapsedTimer                   lastNameClickTime_;
    QPoint                          renamePressPosition_;
    bool                            plainLeftClick_ = false;
    QTimer                          columnSortTimer_;
    QLabel*                         columnStatus_ = nullptr;
    QWidget*                        pageBar_ = nullptr;
    QStringList                     querySelection_;
    QString                         queryFocus_;
    int                             fileNameColumnIndex = -1;       // 추후 사용.
};

class CCardView : public CViewT
{
    Q_OBJECT
public:

private:
    QVBoxLayout*                    vbox_ = nullptr;
    Qtitan::Grid*                   grid_ = nullptr;

};

class CViewBase : public CViewT
{
    Q_OBJECT
public:
    CViewBase( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() )
        : CViewT( Parent, f )
    {}

    void                            SetFilePath( const QString& FilePath ) { filePath_ = FilePath; }
    QString                         GetFilePath() const { return filePath_; }

protected:

    QString                         filePath_;

private:
};

class CInternalViewer : public CViewBase
{
    Q_OBJECT
public:
    CInternalViewer( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

};

class CExternalViewer : public CViewBase
{
    Q_OBJECT
public:
    CExternalViewer( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::FramelessWindowHint  );
    ~CExternalViewer();

    void                            SetWLX( TySpWLX Viewer );
    bool                            Initialize();

    ///////////////////////////////////////////////////////////////////////////
    ///

    HWND                            ListLoad( HWND ParentWin, const QString& FilePath, int ShowFlags );

private:
    void                            setDefaultParams();

    static LRESULT CALLBACK         cbWndProc( _In_ HWND hWnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam );
    static LRESULT CALLBACK         cbWndProcChild( _In_ HWND hWnd, _In_ UINT uMsg, _In_ WPARAM wParam, _In_ LPARAM lParam );

protected:

    TySpWLX                         viewer_;
    ATOM                            atom;
    QWidget*                        wParent = nullptr;
    HWND                            hParent = nullptr;

    QWidget*                        wChild = nullptr;
    HWND                            hChild = nullptr;

    QVBoxLayout*                    vbox_ = nullptr;
};

class CQuickView : public CViewT
{
    Q_OBJECT
public:
    CQuickView( QWidget* Parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );

    void                            SetFilePath( const QString& FilePath );
    void                            SetContent(QtViewerHost& Host, std::shared_ptr<hc::storage::IContentSource> Source);
    void                            CloseView() override;
    void                            EnsureKeyboardFocusOnView() const override;

signals:
    void                            CloseRequested();

protected:
    void                            showEvent( QShowEvent* event ) override;

private:

    bool                            isInitOnce_ = false;
    QtViewerSurface*                surface_ = nullptr;
    QString                         filePath_;
    CInternalViewer*                internalViewer_ = nullptr;
    CExternalViewer*                externalViewer_ = nullptr;

    QVBoxLayout*                    vbox_ = nullptr;
};
