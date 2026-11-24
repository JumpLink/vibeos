# Loaded by build-maplibre.sh as CMAKE_PROJECT_INCLUDE. At the end of MapLibre's own configure it adds one
# small shared library that links mbgl-core, so CMake works out everything mbgl-core needs: its include
# directories and definitions are written out here, and its link line is read from ninja by the script.
# The navi widget (built with meson) compiles and links with exactly those.
if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR OR NAVI_EXPORT_FLAGS_DONE)
    return()
endif()
set(NAVI_EXPORT_FLAGS_DONE TRUE)

function(navi_export_flags)
    file(WRITE "${CMAKE_BINARY_DIR}/navi-probe.cpp" "int navi_probe(void) { return 0; }\n")
    add_library(navi-probe SHARED EXCLUDE_FROM_ALL "${CMAKE_BINARY_DIR}/navi-probe.cpp")
    target_link_libraries(navi-probe PRIVATE mbgl-core)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/navi-includes.txt"
         CONTENT "$<JOIN:$<TARGET_PROPERTY:navi-probe,INCLUDE_DIRECTORIES>,\n>\n")
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/navi-defines.txt"
         CONTENT "$<JOIN:$<TARGET_PROPERTY:navi-probe,COMPILE_DEFINITIONS>,\n>\n")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL navi_export_flags)
