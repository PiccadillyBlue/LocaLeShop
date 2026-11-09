cmake_minimum_required(VERSION 3.16)

project(LocaLeShopClient VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(LocaLeShop
    src/main.c
    src/cJSON.c
)

target_include_directories(LocaLeShop PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    $ENV{DEVKITPRO}/wut/include
    $ENV{DEVKITPRO}/portlibs/wiiu/include
    $ENV{DEVKITPRO}/portlibs/ppc/include
    $ENV{DEVKITPRO}/portlibs/ppc/include/SDL2
)

target_link_directories(LocaLeShop PRIVATE
    $ENV{DEVKITPRO}/portlibs/wiiu/lib
    $ENV{DEVKITPRO}/portlibs/ppc/lib
    $ENV{DEVKITPRO}/wut/lib
)

target_compile_options(LocaLeShop PRIVATE
    -Wall
    -Wextra
    -O2
    -ffunction-sections
    -fdata-sections
)

# Static link order:
# 1. UI / Font rendering (SDL2_ttf, SDL2, freetype, harfbuzz, png16)
# 2. Network client (curl)
# 3. Compression & decoding (brotlidec, brotlicommon, bz2, z)
# 4. Cryptography (mbedtls, mbedx509, mbedcrypto)
# 5. Core Wii U runtime & math (wut, m)
target_link_libraries(LocaLeShop PRIVATE
    SDL2_ttf
    SDL2
    freetype
    harfbuzz
    png16
    curl
    brotlidec
    brotlicommon
    bz2
    z
    mbedtls
    mbedx509
    mbedcrypto
    wut
    m
)

wut_create_rpx(LocaLeShop)

set(WUHB_ARGS
    NAME "LocaLeShop"
    SHORTNAME "LocaLeShop"
    AUTHOR "LocaLeShop Contributors"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/content")
    list(APPEND WUHB_ARGS CONTENT "${CMAKE_CURRENT_SOURCE_DIR}/assets/content")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png")
    list(APPEND WUHB_ARGS ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/tv_splash.png")
    list(APPEND WUHB_ARGS TV_SPLASH "${CMAKE_CURRENT_SOURCE_DIR}/assets/tv_splash.png")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/drc_splash.png")
    list(APPEND WUHB_ARGS DRC_SPLASH "${CMAKE_CURRENT_SOURCE_DIR}/assets/drc_splash.png")
endif()

wut_create_wuhb(LocaLeShop ${WUHB_ARGS})
