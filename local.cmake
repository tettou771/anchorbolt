# Project-specific CMake — loaded by trussc_app(); SURVIVES `trusscli update`
# (unlike CMakeLists.txt, which trusscli regenerates).
#
# anchorbolt is a console tool, not a GUI app. TrussC.h forces the Windows GUI
# subsystem (/subsystem:windows) in Release so a double-clicked app doesn't flash
# a console — but for a console tool that detaches it from its launcher: no
# stdout in cmd, `anchorbolt start` returns to the prompt immediately (as if run
# via `start`), and closing the console window sends no CTRL_CLOSE, so the
# supervisor never stops and keeps restarting the app in the background.
# TRUSSC_SHOW_CONSOLE keeps the console subsystem, exactly like trusscli's own
# build. No effect on macOS/Linux.
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE TRUSSC_SHOW_CONSOLE)
endif()
