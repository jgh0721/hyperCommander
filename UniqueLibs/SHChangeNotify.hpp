#pragma once

// NOTE: ISO 파일의 경우 DRIVEADD, MEDIAREMOVED 가 전달된다. 

class QSHChangeNotify : public QWidget
{
    Q_OBJECT
public:
    QSHChangeNotify( QWidget* parent = nullptr, Qt::WindowFlags f = Qt::WindowFlags() );


    HRESULT                                 StartWatching();
    void                                    StopWatching();

signals:
    /* form X:\\ */
    void                                    OnDriveAdd( const QString& Root );
    /* form X:\\ */
    void                                    OnDriveRemoved( const QString& Root );
    /* from X:\\ or ::{GUID} */
    void                                    OnMediaInserted( const QString& ItemIDDisplayName );
    /* from X:\\ or ::{GUID} */
    void                                    OnMediaRemoved( const QString& ItemIDDisplayName );

private:
    bool                                    nativeEvent( const QByteArray& eventType, void* message, qintptr* result ) override;
    bool                                    isItemNotificationEvent( long lEvent ) const;

    WId                                     hWnd = NULL;
    unsigned int                            ulRegister = 0;

    // WM_USER ( 0x0400 : 1024 )
    static const int                        SHELL_NOTIFY_MESSAGE = 0x0400 + 199;
};
