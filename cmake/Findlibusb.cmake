# SPDX-FileCopyrightText: 2022 Andrea Pappacoda <andrea@pappacoda.it>
# SPDX-License-Identifier: ISC

find_package(libusb CONFIG)
if (NOT libusb_FOUND AND WIN32)
    # A native Windows build need not launch an MSYS pkg-config process.
    find_path(libusb_NATIVE_INCLUDE libusb.h
            HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
            PATH_SUFFIXES libusb-1.0)
    find_library(libusb_NATIVE_RELEASE NAMES libusb-1.0 usb-1.0
            HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
    find_library(libusb_NATIVE_DEBUG NAMES libusb-1.0 usb-1.0
            HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
            NO_DEFAULT_PATH)
    if (libusb_NATIVE_INCLUDE AND libusb_NATIVE_RELEASE)
        if (NOT TARGET libusb::libusb)
            add_library(libusb::libusb UNKNOWN IMPORTED GLOBAL)
            set_target_properties(libusb::libusb PROPERTIES
                    IMPORTED_LOCATION "${libusb_NATIVE_RELEASE}"
                    IMPORTED_LOCATION_RELEASE "${libusb_NATIVE_RELEASE}"
                    INTERFACE_INCLUDE_DIRECTORIES "${libusb_NATIVE_INCLUDE}"
                    MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
                    MAP_IMPORTED_CONFIG_MINSIZEREL Release)
            if (libusb_NATIVE_DEBUG)
                set_property(TARGET libusb::libusb PROPERTY IMPORTED_LOCATION_DEBUG "${libusb_NATIVE_DEBUG}")
            endif ()
        endif ()
        set(libusb_LINK_LIBRARIES "${libusb_NATIVE_RELEASE}")
        set(libusb_FOUND TRUE)
    endif ()
endif ()
if (NOT libusb_FOUND)
    find_package(PkgConfig)
    if (PKG_CONFIG_FOUND)
        pkg_search_module(libusb IMPORTED_TARGET GLOBAL libusb-1.0 libusb)
        if (libusb_FOUND)
            add_library(libusb::libusb ALIAS PkgConfig::libusb)
        endif ()
    endif ()
endif ()

find_package_handle_standard_args(libusb
        REQUIRED_VARS
        libusb_LINK_LIBRARIES
        libusb_FOUND
        VERSION_VAR libusb_VERSION
)
