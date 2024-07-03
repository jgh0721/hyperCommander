include( ${CMAKE_CURRENT_LIST_DIR}/build.version.cmake )

add_compile_definitions( _WIN32_WINNT=0x0601 )
add_compile_definitions( NTDDI_VERSION=0x06010000 )
add_compile_definitions( _CRT_SECURE_NO_WARNINGS )
add_compile_definitions( _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING )