if(HC_IMAGE_CODEC_PREFIX AND NOT TARGET dav1d::dav1d)
    find_library(HC_DAV1D_IMPLIB NAMES dav1d libdav1d
        PATHS "${HC_IMAGE_CODEC_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
    add_library(dav1d::dav1d SHARED IMPORTED GLOBAL)
    set_target_properties(dav1d::dav1d PROPERTIES
        IMPORTED_IMPLIB "${HC_DAV1D_IMPLIB}"
        IMPORTED_LOCATION "${HC_IMAGE_CODEC_PREFIX}/bin/dav1d.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${HC_IMAGE_CODEC_PREFIX}/include")
endif()
