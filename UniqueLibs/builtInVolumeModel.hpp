#pragma once

#include <QtCore>

#include "concurrent/cmnSingleton.hpp"

namespace nsW32API
{
    typedef struct _VOLUME_INFO
    {
        QString             sName;                  // Volume Label, Drive Name, 빈 문자열 가능함, 중복 가능
        QString             sGUID;

        QString             sDriveLetter;           // \\ 추가됨
        QVector< QString >  vecMountPoint;          // \\ 추가됨
        QString             sFileSystem;
        quint32             dwSerialNo = 0;
        quint32             dwFileSystemFlags = 0;
        quint32             dwMaxComponentLen = 0;

        // GetDriveType 반환값
        UINT                dwDriveType = 0;

        // TODO: https://stackoverflow.com/questions/2989713/get-drive-type-with-setupdigetdeviceregistryproperty/44029812#44029812 
        // 반영하여 USB 드라이브 여부 판별

    } VOLUME_INFO;
}

namespace nsHC
{
    class CVolumeListModel : public QAbstractItemModel
    {
        Q_OBJECT
    public:
        void Refresh();
        void Add();
        void Remove();


    private:
        

        
    };

    using TyStVolumeListModel = nsCmn::nsConcurrent::TSingleton<nsHC::CVolumeListModel>;

} // nsHC

