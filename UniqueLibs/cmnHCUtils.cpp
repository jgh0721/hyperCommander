#include "stdafx.h"
#include "cmnHCUtils.hpp"

#include "cmnTypeDefs.hpp"

QString GetFormattedSizeText( qint64 Size, TyEnSizeStyle Style )
{
    QString Ret;

    switch( Style )
    {
        case HC_SIZE_STYLE_BYTES: {
            Ret = QString::number( Size ) + " bytes";
        } break;
        case HC_SIZE_STYLE_KBYTES: {
            if( Size == 0 )
                Ret = QString::number( Size ) + " kbytes";
            else if( Size > 0 && Size <= 1024 )
                Ret = QString::number( 1 ) + " kbytes";
            else 
                Ret = QString::number( Size / KIBI_BYTE );
        } break;
        case HC_SIZE_STYLE_BKMG:
        case HC_SIZE_STYLE_BKMG2:
        case HC_SIZE_STYLE_BKMG3:
        case HC_SIZE_STYLE_BKMG4:
        case HC_SIZE_STYLE_BKMGT:
        case HC_SIZE_STYLE_BKMGT2:
        case HC_SIZE_STYLE_BKMGT3:
        case HC_SIZE_STYLE_BKMGT4: {
            const int Precision = Style >= HC_SIZE_STYLE_BKMGT4 ? HC_SIZE_STYLE_BKMGT - Style : HC_SIZE_STYLE_BKMG - Style;

            if( Style >= HC_SIZE_STYLE_BKMGT && Style <= HC_SIZE_STYLE_BKMGT4 && 
                Size >= TEBI_BYTE )
            {
                Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( TEBI_BYTE ), 'f', Precision ) + " T";
            }
            else if( Size >= GIBI_BYTE )
            {
                Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( GIBI_BYTE ), 'f', Precision ) + " G";
            }
            else if( Size >= MEBI_BYTE )
            {
                Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( MEBI_BYTE ), 'f', Precision ) + " M";
            }
            else if( Size >= KIBI_BYTE )
            {
                Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( KIBI_BYTE ), 'f', Precision ) + " k";
            }
            else if( Size > 0 && Size < KIBI_BYTE )
            {
                Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( KIBI_BYTE ), 'f', Precision ) + " k";
            }
            else
            {
                Ret = QString::number( 0 ) + " k";
            }
        } break;
        case HC_SIZE_STYLE_BKM:
        case HC_SIZE_STYLE_BKM2: {
            if( Size >= MEBI_BYTE )
            {
                if( Style == HC_SIZE_STYLE_BKM )
                    Ret = QString::number( Size / MEBI_BYTE ) + " M";
                else if( Style == HC_SIZE_STYLE_BKM2 )
                    Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( MEBI_BYTE ), 'f', 1 ) + " M";
            }
            else if( Size >= KIBI_BYTE )
            {
                if( Style == HC_SIZE_STYLE_BKM )
                    Ret = QString::number( Size / KIBI_BYTE ) + " k";
                else if( Style == HC_SIZE_STYLE_BKM2 )
                    Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( KIBI_BYTE ), 'f', 1 ) + " k";
            }
            else if( Size > 0 && Size < KIBI_BYTE )
            {
                if( Style == HC_SIZE_STYLE_BKM )
                    Ret = QString::number( 1 ) + " k";
                else if( Style == HC_SIZE_STYLE_BKM2 )
                    Ret = QString::number( static_cast< double >( Size ) / static_cast< double >( KIBI_BYTE ), 'f', 1 ) + " k";
            }
            else
            {
                Ret = QString::number( 0 ) + " k";
            }

        } break;
    }

    return Ret;
}

QString GetFormattedAttrText( quint32 Attributes, bool IsUpper )
{
    const char Lower = IsUpper ? 0 : 'a' - 'A';

    QString Ret( 4, '-' );

    Ret[0] = QChar::fromLatin1( FlagOn( Attributes, FILE_ATTRIBUTE_READONLY ) ? '-' : 'R' + Lower );
    Ret[1] = QChar::fromLatin1( FlagOn( Attributes, FILE_ATTRIBUTE_ARCHIVE ) ? '-' : 'A' + Lower );
    Ret[2] = QChar::fromLatin1( FlagOn( Attributes, FILE_ATTRIBUTE_HIDDEN ) ? '-' : 'H' + Lower );
    Ret[3] = QChar::fromLatin1( FlagOn( Attributes, FILE_ATTRIBUTE_SYSTEM ) ? '-' : 'S' + Lower );

    return Ret;
}
