cmake_minimum_required(VERSION 3.16)
project(slate VERSION 1.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(PkgConfig REQUIRED)
include(GNUInstallDirs)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0>=3.24 pango>=1.50)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.66)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0>=2.66)
pkg_check_modules(FONTCONFIG QUIET IMPORTED_TARGET fontconfig>=2.13)

add_library(md4c STATIC third_party/md4c/md4c.c third_party/md4c/entity.c)
target_include_directories(md4c PUBLIC third_party/md4c)

add_library(slate_markdown src/markdown_model.cpp)
target_include_directories(slate_markdown PUBLIC src)
target_link_libraries(slate_markdown PRIVATE md4c PkgConfig::GLIB)

add_library(slate_core src/document.cpp src/search.cpp)
target_include_directories(slate_core PUBLIC src)
target_compile_definitions(slate_core PUBLIC _FILE_OFFSET_BITS=64)
target_link_libraries(slate_core PUBLIC Threads::Threads)

add_library(slate_widgets src/editor_view.cpp src/workspace.cpp src/markdown_preview.cpp src/mermaid_renderer.cpp src/theme.cpp)
target_link_libraries(slate_widgets PUBLIC slate_core slate_markdown PkgConfig::GTK)
target_link_libraries(slate_widgets PRIVATE PkgConfig::GIO_UNIX)
if(FONTCONFIG_FOUND)
  target_link_libraries(slate_widgets PRIVATE PkgConfig::FONTCONFIG)
  target_compile_definitions(slate_widgets PRIVATE SLATE_HAVE_FONTCONFIG=1)
endif()
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources)
if(NOT GLIB_COMPILE_RESOURCES)
  message(FATAL_ERROR "Building the Slate icon requires glib-compile-resources")
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/slate-icons.c"
  COMMAND "${GLIB_COMPILE_RESOURCES}" --generate-source --c-name=slate_icons
          "--sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/packaging"
          "--target=${CMAKE_CURRENT_BINARY_DIR}/slate-icons.c"
          "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons.gresource.xml"
  DEPENDS packaging/icons.gresource.xml packaging/slate-editor.png
  VERBATIM)
add_library(slate_history src/file_history.cpp)
target_include_directories(slate_history PUBLIC src)
target_link_libraries(slate_history PRIVATE SQLite::SQLite3)
add_executable(slate src/main.cpp "${CMAKE_CURRENT_BINARY_DIR}/slate-icons.c")
target_link_libraries(slate PRIVATE slate_widgets slate_history)
install(TARGETS slate RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  install(FILES packaging/slate.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES packaging/slate-editor.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
  install(FILES packaging/slate-editor.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
endif()
install(FILES third_party/md4c/LICENSE.md DESTINATION share/licenses/slate RENAME md4c-LICENSE.md)

include(CTest)
if(BUILD_TESTING)
  add_executable(file_history_test tests/file_history_test.cpp)
  target_link_libraries(file_history_test PRIVATE slate_history)
  add_test(NAME file_history COMMAND file_history_test)

  add_executable(mobile_document_test tests/mobile_document_test.cpp mobile/core/slate_document.cpp)
  target_include_directories(mobile_document_test PRIVATE mobile/core)
  target_link_libraries(mobile_document_test PRIVATE slate_core)
  add_test(NAME mobile_document COMMAND mobile_document_test)

  add_executable(appearance_test tests/appearance_test.cpp)
  target_link_libraries(appearance_test PRIVATE slate_widgets)

  add_executable(mermaid_renderer_test tests/mermaid_renderer_test.cpp)
  target_link_libraries(mermaid_renderer_test PRIVATE slate_widgets)
  add_test(NAME mermaid_renderer COMMAND mermaid_renderer_test)
  set_tests_properties(mermaid_renderer PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)

  add_executable(markdown_tables_test tests/markdown_tables_test.cpp)
  target_link_libraries(markdown_tables_test PRIVATE slate_widgets)

  add_executable(markdown_model_test tests/markdown_model_test.cpp)
  target_link_libraries(markdown_model_test PRIVATE slate_markdown)
  add_test(NAME markdown_model COMMAND markdown_model_test)

  add_executable(markdown_preview_test tests/markdown_preview_test.cpp)
  target_link_libraries(markdown_preview_test PRIVATE slate_widgets)

  add_executable(document_test tests/document_test.cpp)
  target_link_libraries(document_test PRIVATE slate_core)
  add_test(NAME document COMMAND document_test)

  add_executable(workspace_test tests/workspace_search.cpp)
  target_link_libraries(workspace_test PRIVATE slate_core)
  add_test(NAME workspace COMMAND workspace_test)

  add_executable(view_rows_test tests/view_rows.cpp)
  target_link_libraries(view_rows_test PRIVATE slate_core PkgConfig::GTK)
  add_test(NAME view_rows COMMAND view_rows_test)

  add_executable(view_input_test tests/view_input.cpp)
  target_link_libraries(view_input_test PRIVATE slate_widgets)
  if(APPLE)
    add_test(NAME appearance COMMAND appearance_test)
    set_tests_properties(appearance PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME markdown_tables COMMAND markdown_tables_test)
    set_tests_properties(markdown_tables PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME markdown_preview COMMAND markdown_preview_test)
    set_tests_properties(markdown_preview PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
    add_test(NAME view_input COMMAND view_input_test)
    set_tests_properties(view_input PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
  endif()
endif()

if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_program(XVFB_RUN xvfb-run)
  find_program(XDOTOOL xdotool)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(XVFB_RUN)
    add_test(NAME appearance COMMAND ${XVFB_RUN} -a $<TARGET_FILE:appearance_test>)
    set_tests_properties(appearance PROPERTIES TIMEOUT 30 ENVIRONMENT "GDK_BACKEND=x11;G_DEBUG=fatal-criticals")
    add_test(NAME markdown_tables COMMAND ${XVFB_RUN} -a $<TARGET_FILE:markdown_tables_test>)
    set_tests_properties(markdown_tables PROPERTIES TIMEOUT 40 ENVIRONMENT "GDK_BACKEND=x11;G_DEBUG=fatal-criticals")
    add_test(NAME markdown_preview COMMAND ${XVFB_RUN} -a $<TARGET_FILE:markdown_preview_test>)
    set_tests_properties(markdown_preview PROPERTIES TIMEOUT 40
      ENVIRONMENT "GDK_BACKEND=x11;G_DEBUG=fatal-criticals")
    add_test(NAME view_input COMMAND ${XVFB_RUN} -a $<TARGET_FILE:view_input_test>)
    set_tests_properties(view_input PROPERTIES TIMEOUT 30
      ENVIRONMENT "GDK_BACKEND=x11;G_DEBUG=fatal-criticals;GTK_IM_MODULE=gtk-im-context-simple")
  endif()
  if(XVFB_RUN AND XDOTOOL AND Python3_Interpreter_FOUND)
    add_test(NAME desktop COMMAND ${XVFB_RUN} -a ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/desktop_smoke.py $<TARGET_FILE:slate>)
    set_tests_properties(desktop PROPERTIES TIMEOUT 60
      ENVIRONMENT "GDK_BACKEND=x11;GTK_IM_MODULE=gtk-im-context-simple")
  endif()
endif()

add_executable(slate_bench EXCLUDE_FROM_ALL benchmarks/large_file.cpp)
target_link_libraries(slate_bench PRIVATE slate_core)

add_executable(slate_preview_bench EXCLUDE_FROM_ALL benchmarks/markdown_preview.cpp)
target_link_libraries(slate_preview_bench PRIVATE slate_widgets)

add_executable(slate_markdown_bench EXCLUDE_FROM_ALL benchmarks/markdown_parse.cpp)
target_link_libraries(slate_markdown_bench PRIVATE slate_markdown)

add_executable(slate_scroll_bench EXCLUDE_FROM_ALL benchmarks/editor_scroll.cpp)
target_link_libraries(slate_scroll_bench PRIVATE slate_widgets)
