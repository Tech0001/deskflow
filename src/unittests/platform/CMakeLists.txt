# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

if(UNIX)
  create_test(
    NAME FileTransferTests
    DEPENDS platform
    LIBS base arch Qt6::Network
    SOURCE FileTransferTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
  )
  add_dependencies(FileTransferTests deskflow-file-transfer)
endif()

if (WIN32)
  create_test(
    NAME MSWindowsClipboardTests
    DEPENDS platform
    LIBS base arch
    SOURCE MSWindowsClipboardTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
  )
elseif(APPLE)
  create_test(
    NAME OSXClipboardTests
    DEPENDS platform
    LIBS base arch
    SOURCE OSXClipboardTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
  )
  create_test(
    NAME OSXKeyStateTests
    DEPENDS platform
    LIBS base arch
    SOURCE OSXKeyStateTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
  )
elseif(UNIX)
  if(LIBEI_FOUND)
    create_test(
      NAME EiScrollStateTests
      DEPENDS platform
      LIBS base arch
      SOURCE EiScrollStateTests.cpp
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
    )
    create_test(
      NAME EiKeyStateTests
      DEPENDS platform
      LIBS base arch
      SOURCE EiKeyStateTests.cpp
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
    )
    if(HAVE_LP_CLIPBOARD)
      create_test(
        NAME LazyFileClipboardTests
        DEPENDS platform
        LIBS base arch Qt6::Network
        SOURCE LazyFileClipboardTests.cpp
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
      )
      create_test(
        NAME WaylandClipboardTests
        DEPENDS platform
        LIBS base arch Qt6::Gui
        SOURCE WaylandClipboardTests.cpp
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
      )
      create_test(
        NAME PortalClipboardTests
        DEPENDS platform
        LIBS base arch
        SOURCE PortalClipboardTests.cpp
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
      )
    endif()
  endif()

  if (BUILD_X11_SUPPORT)
    create_test(
      NAME XWindowsClipboardTests
      DEPENDS platform
      LIBS base arch
      SOURCE XWindowsClipboardTests.cpp
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/platform"
    )
  endif()
endif()
