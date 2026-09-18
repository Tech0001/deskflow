# SPDX-License-Identifier: MIT
add_executable(deskflow-file-transfer main.cpp)
target_link_libraries(deskflow-file-transfer PRIVATE platform arch base Qt6::Core Qt6::Network)
install(TARGETS deskflow-file-transfer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
