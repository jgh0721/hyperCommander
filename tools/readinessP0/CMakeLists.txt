find_package(Qt6 6.12 REQUIRED COMPONENTS Svg Test)
# Reuse the exact vendored target linked by the product after P1 integration.
set(HC_P0_QLEMENTINE_SOURCE "${PROJECT_SOURCE_DIR}/externalLibs/qlementine")
file(READ "${HC_P0_QLEMENTINE_SOURCE}/upstream.json" P0_LOCK)
string(JSON P0_COMMIT GET "${P0_LOCK}" commit)
# Compile the same licensed sources as the application, without building or linking the application.
get_target_property(P0_PRODUCT_SOURCES HyperCommander SOURCES)
get_target_property(P0_PRODUCT_DIRECTORY HyperCommander SOURCE_DIR)
get_target_property(P0_PRODUCT_INCLUDES HyperCommander INCLUDE_DIRECTORIES)
set(P0_QTITAN_SOURCES "")
foreach(P0_SOURCE IN LISTS P0_PRODUCT_SOURCES)
    if(P0_SOURCE MATCHES "externalLibs/QtitanDataGrid/")
        cmake_path(ABSOLUTE_PATH P0_SOURCE BASE_DIRECTORY "${P0_PRODUCT_DIRECTORY}" NORMALIZE OUTPUT_VARIABLE P0_ABSOLUTE)
        list(APPEND P0_QTITAN_SOURCES "${P0_ABSOLUTE}")
    endif()
endforeach()
if(NOT P0_QTITAN_SOURCES)
    message(FATAL_ERROR "No licensed Qtitan sources found in the product source inventory")
endif()
qt_add_library(hc_p0_qtitan STATIC ${P0_QTITAN_SOURCES})
target_include_directories(hc_p0_qtitan PUBLIC ${P0_PRODUCT_INCLUDES})
target_compile_definitions(hc_p0_qtitan PUBLIC QTN_NAMESPACE QTITAN_LIBRARY_STATIC QTN_PRIVATE_HEADERS QTN_DC_STYLE
    PRIVATE QTN_VERSION_DATAGRID=9.3.0 _CRT_SECURE_NO_WARNINGS QT_NO_DEPRECATED_WARNINGS)
target_compile_options(hc_p0_qtitan PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8> $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/bigobj>)
target_link_libraries(hc_p0_qtitan PUBLIC Qt6::Widgets Qt6::PrintSupport Qt6::Sql Qt6::Network
    PRIVATE Qt6::CorePrivate Qt6::GuiPrivate Qt6::WidgetsPrivate)

qt_add_executable(hc_readiness_p0 main.cpp)
set_target_properties(hc_readiness_p0 PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(hc_readiness_p0 PRIVATE hc_p0_qtitan qlementine::qlementine Qt6::Svg Qt6::Test)
target_compile_options(hc_readiness_p0 PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)
target_compile_definitions(hc_readiness_p0 PRIVATE HC_P0_QLEMENTINE_COMMIT="${P0_COMMIT}")
hc_enable_qt_deployment(hc_readiness_p0 TEST PLUGINS)
find_program(P0_POWERSHELL NAMES powershell REQUIRED)
if(BUILD_TESTING)
    foreach(P0_SCALE IN ITEMS 1 1.25 1.5 2)
        add_test(NAME hc.readiness_p0_${P0_SCALE}
            COMMAND "${P0_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/Run-Test.ps1" -Executable "$<TARGET_FILE:hc_readiness_p0>")
        set_tests_properties(hc.readiness_p0_${P0_SCALE} PROPERTIES
            LABELS readiness-p0 RUN_SERIAL TRUE TIMEOUT 90
            ENVIRONMENT "QT_SCALE_FACTOR=${P0_SCALE};HC_P0_EVIDENCE=${CMAKE_BINARY_DIR}/readiness-evidence/$<CONFIG>/${P0_SCALE}")
    endforeach()
endif()
add_custom_target(hc_readiness_p0_inventory
    COMMAND "${P0_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/Measure-Inputs.ps1"
        -Repository "${PROJECT_SOURCE_DIR}" -OutputDirectory "${CMAKE_BINARY_DIR}/readiness-evidence"
        -QlementineSource "${HC_P0_QLEMENTINE_SOURCE}"
    VERBATIM)
