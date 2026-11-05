# Puts the default launcher.ini next to the launcher. One already there is that PC's own, and stays as it is.
if(NOT EXISTS "${TARGET}")
    configure_file("${SOURCE}" "${TARGET}" COPYONLY)
endif()
