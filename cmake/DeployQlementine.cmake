foreach(HC_NOTICE IN ITEMS LICENSE UPSTREAM.txt licenses/ThirdPartyNotices.txt
        licenses/Inter-OFL.txt licenses/RobotoMono-Apache-2.0.txt)
    if(NOT EXISTS "${HC_QLEMENTINE_ROOT}/${HC_NOTICE}")
        message(FATAL_ERROR "Missing Qlementine distribution notice: ${HC_NOTICE}")
    endif()
    file(MAKE_DIRECTORY "${HC_QLEMENTINE_DESTINATION}")
    get_filename_component(HC_NOTICE_NAME "${HC_NOTICE}" NAME)
    file(COPY_FILE "${HC_QLEMENTINE_ROOT}/${HC_NOTICE}"
        "${HC_QLEMENTINE_DESTINATION}/${HC_NOTICE_NAME}" ONLY_IF_DIFFERENT)
endforeach()
