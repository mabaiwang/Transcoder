cmake_minimum_required(VERSION 3.16)

project(EncodeStudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

add_executable(EncodeStudio
    main.cpp
    TranscoderWindow.h
    TranscoderWindow.cpp
    resources.qrc
    app.rc
)

target_link_libraries(EncodeStudio PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)

if(MSVC)
    # Required by Qt headers to expose a correct __cplusplus value on MSVC.
    target_compile_options(EncodeStudio PRIVATE /Zc:__cplusplus /utf-8)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    get_target_property(_qt_core_location Qt${QT_VERSION_MAJOR}::Core IMPORTED_LOCATION_RELEASE)
    if(NOT _qt_core_location)
        get_target_property(_qt_core_location Qt${QT_VERSION_MAJOR}::Core IMPORTED_LOCATION)
    endif()
    if(_qt_core_location)
        string(TOLOWER "${_qt_core_location}" _qt_core_location_lower)
        if(_qt_core_location_lower MATCHES "mingw")
            message(FATAL_ERROR
                "Detected MinGW Qt with MSVC generator:\n"
                "  Qt Core: ${_qt_core_location}\n\n"
                "Please use one of these combinations:\n"
                "  1) Visual Studio generator + Qt msvc kit (e.g. msvc2019_64)\n"
                "  2) MinGW compiler + Ninja/MinGW Makefiles + Qt mingw_64\n")
        endif()
    endif()
endif()

if(WIN32)
    set_target_properties(EncodeStudio PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
