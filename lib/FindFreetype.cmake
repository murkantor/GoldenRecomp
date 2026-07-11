set(FREETYPE_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/lib/freetype-windows-binaries/include)
set(FREETYPE_LIBRARIES "${CMAKE_SOURCE_DIR}/lib/freetype-windows-binaries/release static/vs2015-2022/win64/freetype.lib")

# RecompFrontend's bundled RmlUi requires the imported target form.
if (NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype STATIC IMPORTED)
    set_target_properties(Freetype::Freetype PROPERTIES
        IMPORTED_LOCATION "${FREETYPE_LIBRARIES}"
        INTERFACE_INCLUDE_DIRECTORIES "${FREETYPE_INCLUDE_DIRS}"
    )
endif()
set(FREETYPE_FOUND TRUE)
set(Freetype_FOUND TRUE)
