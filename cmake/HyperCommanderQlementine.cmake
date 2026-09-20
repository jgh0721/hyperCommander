include_guard(GLOBAL)

# Keep upstream cache options and project settings inside the dependency scope.
function(hc_add_qlementine)
    set(BUILD_SHARED_LIBS OFF)
    set(QLEMENTINE_BUILD_STATIC ON)
    set(QLEMENTINE_BUILD_STYLE_PLUGIN OFF)
    set(QLEMENTINE_SANDBOX OFF)
    set(QLEMENTINE_SHOWCASE OFF)
    add_subdirectory("${PROJECT_SOURCE_DIR}/externalLibs/qlementine"
        "${CMAKE_BINARY_DIR}/qlementine" EXCLUDE_FROM_ALL)
    target_compile_options(qlementine PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
endfunction()
hc_add_qlementine()

function(hc_deploy_qlementine_notices Target)
    add_custom_command(TARGET ${Target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DHC_QLEMENTINE_ROOT=${PROJECT_SOURCE_DIR}/externalLibs/qlementine"
            "-DHC_QLEMENTINE_DESTINATION=$<TARGET_FILE_DIR:${Target}>/licenses/qlementine"
            -P "${PROJECT_SOURCE_DIR}/cmake/DeployQlementine.cmake"
        VERBATIM)
endfunction()

install(FILES
    "${PROJECT_SOURCE_DIR}/externalLibs/qlementine/LICENSE"
    "${PROJECT_SOURCE_DIR}/externalLibs/qlementine/UPSTREAM.txt"
    "${PROJECT_SOURCE_DIR}/externalLibs/qlementine/licenses/ThirdPartyNotices.txt"
    "${PROJECT_SOURCE_DIR}/externalLibs/qlementine/licenses/Inter-OFL.txt"
    "${PROJECT_SOURCE_DIR}/externalLibs/qlementine/licenses/RobotoMono-Apache-2.0.txt"
    DESTINATION licenses/qlementine COMPONENT Runtime)

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    # Build the maintained .vcxproj, not the generated CMake project. Keep its
    # objects, output, and CMake-built dependencies separate from normal builds.
    add_custom_target(hc_native_vs_product
        COMMAND "${CMAKE_VS_MSBUILD_COMMAND}" "${PROJECT_SOURCE_DIR}/HyperCommander.sln"
            /t:Build "/p:Configuration=$<CONFIG>" /p:Platform=x64 /m:4 /p:CL_MPCount=4
            "/p:QtInstall=$<TARGET_FILE_DIR:Qt6::Core>/.."
            "/p:OutDir=${CMAKE_BINARY_DIR}/native-vs/$<CONFIG>/"
            "/p:IntDir=${CMAKE_BINARY_DIR}/native-vs-obj/$<CONFIG>/"
            "/p:HcNativePluginBuild=${CMAKE_BINARY_DIR}/native-vs-dependencies/$<CONFIG>"
        USES_TERMINAL VERBATIM)
endif()
