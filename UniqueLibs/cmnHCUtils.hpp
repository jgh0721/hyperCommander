#pragma once

class QFileIconGatherer
{
    
};

enum TyEnSizeStyle {
    HC_SIZE_STYLE_BYTES,
    HC_SIZE_STYLE_KBYTES,
    HC_SIZE_STYLE_BKMG4,        // ( x.xxx  k/M/G )
    HC_SIZE_STYLE_BKMG3,        // ( x.xx   k/M/G )
    HC_SIZE_STYLE_BKMG2,        // ( x.x    k/M/G )
    HC_SIZE_STYLE_BKMG,         // ( x      k/M/G )
    HC_SIZE_STYLE_BKMGT4,       // ( x.xxx  k/M/G/T )
    HC_SIZE_STYLE_BKMGT3,       // ( x.xx   k/M/G/T )
    HC_SIZE_STYLE_BKMGT2,       // ( x.x    k/M/G/T )
    HC_SIZE_STYLE_BKMGT,        // ( x      k/M/G/T )
    HC_SIZE_STYLE_BKM2,         // ( x.x    k/M )
    HC_SIZE_STYLE_BKM,          // ( x      k/M )
};

QString                                 GetFormattedSizeText( qint64 Size, TyEnSizeStyle Style );
QString                                 GetFormattedAttrText( quint32 Attributes, bool IsUpper );

