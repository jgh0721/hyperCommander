# Rebuild the packaged plugins from the sources under Licenses/AdditionalImageFormats.
cmake_minimum_required(VERSION 4.2)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(HyperCommanderAdditionalImageFormats LANGUAGES CXX)
    find_package(Qt6 6.12 REQUIRED COMPONENTS Gui)
    qt_standard_project_setup(REQUIRES 6.12)
endif()

set(CodecSources scanlineconverter.cpp scanlineconverter_p.h util_p.h
    microexif.cpp microexif_p.h fastmath_p.h packbits_p.h)
foreach(Format IN ITEMS qoi dds pcx psd)
    list(APPEND CodecSources ${Format}.cpp ${Format}_p.h ${Format}.json)
endforeach()

set(SourceDir "${CMAKE_CURRENT_SOURCE_DIR}/KImageFormats")
if(NOT EXISTS "${SourceDir}/qoi.cpp")
    include(FetchContent)
    FetchContent_GetProperties(hc_kimageformats SOURCE_DIR KImageFormatsRoot)
    set(SourceDir "${CMAKE_CURRENT_BINARY_DIR}/KImageFormats")
    foreach(Source IN LISTS CodecSources)
        if(NOT Source MATCHES "^psd[.](cpp|json)$")
            configure_file("${KImageFormatsRoot}/src/imageformats/${Source}" "${SourceDir}/${Source}" COPYONLY)
        endif()
    endforeach()
    # Only the two tested Photoshop document extensions are advertised.
    file(CONFIGURE OUTPUT "${SourceDir}/psd.json" CONTENT [=[{
    "Keys": ["psd", "psb"],
    "MimeTypes": ["image/vnd.adobe.photoshop", "image/vnd.adobe.photoshop"]
}
]=] @ONLY NEWLINE_STYLE CRLF)
    # A Size probe can parse the header and then fail to read the resources.
    # read() must wait for the complete metadata check before consuming pixels.
    file(READ "${KImageFormatsRoot}/src/imageformats/psd.cpp" PsdSource)
    set(InfoEnd [=[        return ok;
    }

    PSDHeader m_header;
]=])
    string(FIND "${PsdSource}" "${InfoEnd}" InfoEndAt)
    if(InfoEndAt EQUAL -1 OR NOT PsdSource MATCHES "return IsValid[(]m_header[)];")
        message(FATAL_ERROR "The pinned PSD decoder no longer matches its metadata validation change")
    endif()
    string(REPLACE "return IsValid(m_header);" "return m_infoReady;" PsdSource "${PsdSource}")
    string(REPLACE "${InfoEnd}" [=[        m_infoReady = true;
        return true;
    }

    bool m_infoReady = false;
    PSDHeader m_header;
]=] PsdSource "${PsdSource}")
    file(CONFIGURE OUTPUT "${SourceDir}/psd.cpp" CONTENT "${PsdSource}" @ONLY NEWLINE_STYLE CRLF)
endif()

function(hc_add_kimageformat Format PluginClass)
    set(Target "hc_${Format}_plugin")
    set(Sources ${Format}.cpp ${Format}_p.h ${Format}.json scanlineconverter.cpp scanlineconverter_p.h util_p.h ${ARGN})
    list(TRANSFORM Sources PREPEND "${SourceDir}/")
    qt_add_plugin(${Target} CLASS_NAME ${PluginClass} PLUGIN_TYPE imageformats ${Sources})
    target_link_libraries(${Target} PRIVATE Qt6::Gui)
    target_compile_options(${Target} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(${Target} PROPERTIES
        OUTPUT_NAME "kimg_${Format}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/Libs/imageformats"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/Libs/imageformats")
    if(TARGET hc_additional_image_notices)
        add_dependencies(${Target} hc_additional_image_notices)
    endif()
endfunction()

if(KImageFormatsRoot)
    set(NoticeDir "${CMAKE_BINARY_DIR}/$<CONFIG>/Licenses/AdditionalImageFormats")
    list(TRANSFORM CodecSources PREPEND "${SourceDir}/")
    add_custom_target(hc_additional_image_notices
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${NoticeDir}/KImageFormats"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
            "${KImageFormatsRoot}/LICENSES/BSD-3-Clause.txt"
            "${KImageFormatsRoot}/LICENSES/LGPL-2.0-or-later.txt"
            "${KImageFormatsRoot}/LICENSES/LGPL-2.1-or-later.txt"
            "${KImageFormatsRoot}/LICENSES/LGPL-2.1-only.txt"
            "${KImageFormatsRoot}/LICENSES/LGPL-3.0-only.txt"
            "${NoticeDir}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${CodecSources} "${NoticeDir}/KImageFormats"
        VERBATIM)
endif()

hc_add_kimageformat(qoi QOIPlugin)
hc_add_kimageformat(dds QDDSPlugin)
hc_add_kimageformat(pcx PCXPlugin)
hc_add_kimageformat(psd PSDPlugin microexif.cpp microexif_p.h fastmath_p.h packbits_p.h)
