#pragma once

#include <limits>

// Parses an unsigned integer advancing s to the end of the parsed input.
// This function assumes that the first character of s is a digit.
template <typename Char>
unsigned int parse_nonnegative_int( const Char*& s, bool* ok = nullptr )
{
    assert( '0' <= *s && *s <= '9' );
    if( '0' > *s || *s > '9' )
    {
        if( ok )
            *ok = false;
        return 0;
    }

    unsigned value = 0;
    do
    {
        unsigned new_value = value * 10 + ( *s++ - '0' );
        // Check if value wrapped around.
        if( new_value < value )
        {
            if( ok )
                *ok = true;

            value = ( std::numeric_limits<unsigned>::max )( );
            break;  
        }
        value = new_value;
    } while( '0' <= *s && *s <= '9' );
    // Convert to unsigned to prevent a warning.
    unsigned max_int = ( std::numeric_limits<int>::max )( );
    if( value > max_int )
        assert( false );    // number is too big
    return value;
}
