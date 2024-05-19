#pragma once

#include "cmnSystemDetection.hpp"
#include "cmnCompilerDetection.hpp"

#ifndef FlagOn
    #define FlagOn(_F,_SF)        ((_F) & (_SF))
#endif

#ifndef BooleanFlagOn
    #define BooleanFlagOn(F,SF)   ((BOOLEAN)(((F) & (SF)) != 0))
#endif

#ifndef SetFlag
    #define SetFlag(_F,_SF)       ((_F) |= (_SF))
#endif

#ifndef ClearFlag
    #define ClearFlag(_F,_SF)     ((_F) &= ~(_SF))
#endif
