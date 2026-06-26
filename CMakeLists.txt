cmake_minimum_required(VERSION 3.16)
project(BCDToolWindows CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Sources ──────────────────────────────────────────────────────────────────

set(SOURCES
    src/main.cpp
    src/privilege.cpp
    src/gpt_reader.cpp
    src/uefi_vars.cpp
    src/esp_scanner.cpp
    src/os_detector.cpp
    src/boot_editor.cpp
    src/backup.cpp
)

# compat.h is header-only — listed here for IDE visibility
set(HEADERS
    src/efi_types.h
    src/compat.h
    src/privilege.h
    src/gpt_reader.h
    src/uefi_vars.h
    src/esp_scanner.h
    src/os_detector.h
    src/boot_editor.h
    src/backup.h
)

add_executable(BCDToolWindows WIN32 ${SOURCES})

target_include_directories(BCDToolWindows PRIVATE src)

# ── Subsystem: console (we want a terminal window) ──────────────────────────

set_target_properties(BCDToolWindows PROPERTIES
    WIN32_EXECUTABLE FALSE   # Keep it a console app
)

# ── Linked libraries ─────────────────────────────────────────────────────────

target_link_libraries(BCDToolWindows PRIVATE
    advapi32     # Privilege management, SetFirmwareEnvironmentVariable
    setupapi     # Disk enumeration
    shell32      # ShellExecuteEx (UAC elevation)
    shlwapi      # Path utilities
)

# ── Require Administrator via embedded manifest (MSVC) ───────────────────────

if(MSVC)
    # Embed a manifest that demands administrator privileges
    set_target_properties(BCDToolWindows PROPERTIES
        LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\""
    )

    # Warnings as errors in debug, optimized release
    target_compile_options(BCDToolWindows PRIVATE
        $<$<CONFIG:Release>:/O2 /DNDEBUG>
        $<$<CONFIG:Debug>:/Od /Zi>
        /W3
        /utf-8
    )
endif()

# ── MinGW / GCC ──────────────────────────────────────────────────────────────

if(MINGW)
    # Embed the UAC manifest via a resource file
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest")
        file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest"
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">\n"
            "  <trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v3\">\n"
            "    <security>\n"
            "      <requestedPrivileges>\n"
            "        <requestedExecutionLevel level=\"requireAdministrator\" uiAccess=\"false\"/>\n"
            "      </requestedPrivileges>\n"
            "    </security>\n"
            "  </trustInfo>\n"
            "</assembly>\n"
        )
    endif()

    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/app.rc")
        file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/src/app.rc"
            "#include <winuser.h>\n"
            "1 RT_MANIFEST \"app.manifest\"\n"
        )
    endif()

    target_sources(BCDToolWindows PRIVATE src/app.rc)

    target_compile_options(BCDToolWindows PRIVATE
        -municode -Wall -Wextra
        $<$<CONFIG:Release>:-O2>
    )
    target_link_options(BCDToolWindows PRIVATE
        -municode -mwindows
        $<$<CONFIG:Release>:-s>
    )
endif()

# ── Install ───────────────────────────────────────────────────────────────────

install(TARGETS BCDToolWindows DESTINATION bin)
