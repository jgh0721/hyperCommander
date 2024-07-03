#pragma once

/*!
    컬럼 정의 규격

    [=모듈.분류.이름.형식]




    환경설정

    [ColumnView]


 */

struct Node;

struct ColumnParseResult
{
    QStringView                         Module;
    QStringView                         Cate;
    QStringView                         Name;
    QStringView                         Convert;
};

struct Column
{
    int                                 Index = 0;
    int                                 Row = 0;
    QString                             Name;
    Qt::Alignment                       Align;
    QString                             Content;

    // Internal Use Only.
    QVector< wchar_t >                  Content_CVT;
};

struct ColumnView
{
    QString                             Name;
    bool                                IsBuiltIn = false;

    QVector< Column >                   VecColumns;
    QVector< ColumnParseResult >        VecParseResult;
};

constexpr auto BUILTIN_COL_FS_NAME          = QLatin1String( "[=HC.Fs.Name]" );
constexpr auto BUILTIN_COL_FS_SIZE          = QLatin1String( "[=HC.Fs.Size]" );
constexpr auto BUILTIN_COL_FS_CREATED       = QLatin1String( "[=HC.Fs.Created]" );
constexpr auto BUILTIN_COL_FS_MODIFIED      = QLatin1String( "[=HC.Fs.Modified]" );
constexpr auto BUILTIN_COL_FS_ATTRIB        = QLatin1String( "[=HC.Fs.Attrib]" );
constexpr auto BUILTIN_COL_FS_ATTRIB_TEXT   = QLatin1String( "[=HC.Fs.AttribText]" );

constexpr auto BUILTIN_COL_NTFS_STREAM_COUNT = QLatin1String( "[=HC.Ntfs.StreamCount]" );
// tagURLZone( urlmon.h ), 존재하면 >= -1
constexpr auto BUILTIN_COL_NTFS_ZONE_ID = QLatin1String( "[=HC.Ntfs.ZoneId]" );

const ColumnView BUILTIN_COLVIEW_DETAILS = {
    QObject::tr( "자세히" ), true,
    QVector< Column >()
        << Column { 0, 0, QObject::tr( "이름" ),      Qt::AlignLeft,     BUILTIN_COL_FS_NAME }
        << Column { 1, 0, QObject::tr( "크기" ),      Qt::AlignCenter,   BUILTIN_COL_FS_SIZE }
        << Column { 2, 0, QObject::tr( "만든 날짜" ), Qt::AlignCenter,   BUILTIN_COL_FS_CREATED }
        << Column { 3, 0, QObject::tr( "속성" ),      Qt::AlignCenter,   BUILTIN_COL_FS_ATTRIB_TEXT }
};
