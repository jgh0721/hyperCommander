#pragma once
#include <initializer_list>

namespace hc::view
{
    // File-list icon size follows the record height like Total Commander's row-sized icons.
    // 1줄은 16·24·32·48·64, 2줄은 128까지 고르며 위아래 여백을 남긴다.
    inline constexpr int ListIconMargin = 2;
    inline constexpr int ListIconMinimum = 16;
    inline int ListIconSize( int RecordHeight, bool TwoLine )
    {
        const int Available = RecordHeight - 2 * ListIconMargin;
        int Result = ListIconMinimum;
        for( const int Size : { 16, 24, 32, 48, 64, 128 } )
        {
            if( Size == 128 && !TwoLine ) break;
            if( Size <= Available ) Result = Size;
        }
        return Result;
    }
    // 아이콘과 글자 사이 간격. 1줄은 6, 2줄은 8.
    inline int ListIconTextGap( bool TwoLine ) { return TwoLine ? 8 : 6; }
}
