set(_wx_toolkit "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    option(DEP_WX_GTK3 "Build wxWidgets for GTK3 instead of GTK2" OFF)

    set(_gtk_ver 2)
    if (DEP_WX_GTK3)
        set(_gtk_ver 3)
    endif ()
    set(_wx_toolkit "-DwxBUILD_TOOLKIT=gtk${_gtk_ver}")
endif()

set(_unicode_utf8 OFF)
if (UNIX AND NOT APPLE) # wxWidgets will not use char as the underlying type for wxString unless its forced to.
    set (_unicode_utf8 ON)
endif()

add_cmake_project(wxWidgets
    URL https://github.com/prusa3d/wxWidgets/archive/78aa2dc0ea7ce99dc19adc1140f74c3e2e3f3a26.zip
    URL_HASH SHA256=94b7d972373503e380e5a8b0ca63b1ccb956da4006402298dd89a0c5c7041b1e
    CMAKE_ARGS
        "-DCMAKE_DEBUG_POSTFIX:STRING="
        -DwxBUILD_PRECOMP=ON
        ${_wx_toolkit}
        -DwxUSE_MEDIACTRL=OFF
        -DwxUSE_DETECT_SM=OFF
        -DwxUSE_UNICODE=ON
        -DwxUSE_UNICODE_UTF8=${_unicode_utf8}
        -DwxUSE_OPENGL=ON
        -DwxUSE_LIBPNG=sys
        -DwxUSE_ZLIB=sys
        -DwxUSE_NANOSVG=sys
        -DwxUSE_NANOSVG_EXTERNAL=ON
        -DwxUSE_REGEX=OFF
        -DwxUSE_LIBXPM=builtin
        -DwxUSE_LIBJPEG=sys
        -DwxUSE_LIBTIFF=OFF
        -DwxUSE_EXPAT=sys
        -DwxUSE_LIBSDL=OFF
        -DwxUSE_XTEST=OFF
        -DwxUSE_GLCANVAS_EGL=OFF
        -DwxUSE_WEBREQUEST=OFF
)

set(DEP_wxWidgets_DEPENDS ZLIB PNG EXPAT JPEG NanoSVG)
