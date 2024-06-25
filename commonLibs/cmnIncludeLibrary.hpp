#pragma once

#include "cmnBase.hpp"

#include "cmnTypes.hpp"
#include "cmnUtils.hpp"
#include "cmnConcurrent.hpp"
#include "concurrent/cmnConditionVariable.hpp"
#include "concurrent/cmnMutex.hpp"
#include "concurrent/cmnSingleton.hpp"
#include "cmnDateTime.hpp"
#include "cmnPath.hpp"
#include "cmnTextUtils.hpp"
#include "cmnProcess.hpp"

#if defined(USE_QT_SUPPORT)
#include "Qt/cmnQtUtils.hpp"
#include "Qt/cmnQtDelegator.hpp"
#endif

#define DECLARE_NSCMN using namespace nsCmn;

#if defined(__HDR_CMN_CONCURRENT__)
#define DECLARE_NSCMN_CONCURRENT using namespace nsCmn::nsConcurrent;
#else
#define DECLARE_NSCMN_CONCURRENT
#endif

#if defined(__HDR_CMN_PATH__)
#define DECLARE_NSCMN_PATH using namespace nsCmn::nsCmnPath;
#else
#define DECLARE_NSCMN_PATH
#endif

#define DECLARE_CMNLIBSV2_NAMESPACE \
    DECLARE_NSCMN \
    using namespace nsCmn::nsOAL; \
    DECLARE_NSCMN_CONCURRENT \
    DECLARE_NSCMN_PATH

