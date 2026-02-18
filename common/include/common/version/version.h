#pragma once

#ifndef TOSTRING
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#endif

// clang-format off

#define VERSION_TYPE_DEV      0
#define VERSION_TYPE_ALPHA    1
#define VERSION_TYPE_BETA     2
#define VERSION_TYPE_RC       3
#define VERSION_TYPE_RELEASE  4

// Variables to be modified during the release process
#define PRODUCT_NAME           "RF2 Community Patch (SOPOT)"
#define VERSION_MAJOR          0
#define VERSION_MINOR          1
#define VERSION_PATCH          0
#define VERSION_TYPE           VERSION_TYPE_DEV
#define VERSION_TYPE_REVISION  1

// Version values for RF2 Community Patch (SOPOT)-managed config files.
#define ADS_VERSION            2      // .toml dedicated server config files
#define AFS_VERSION            1      // rf2patch settings file format
#define AFCC_VERSION           1      // alpine_system.ini

// Placeholder for future RF2 map/version compatibility logic.
#define MAXIMUM_RFL_VERSION    295

// clang-format on

#if VERSION_TYPE == VERSION_TYPE_DEV
#define VERSION_SUFFIX        "-dev"
#elif VERSION_TYPE == VERSION_TYPE_ALPHA
#define VERSION_SUFFIX        "-alpha" TOSTRING(VERSION_TYPE_REVISION)
#elif VERSION_TYPE == VERSION_TYPE_BETA
#define VERSION_SUFFIX        "-beta" TOSTRING(VERSION_TYPE_REVISION)
#elif VERSION_TYPE == VERSION_TYPE_RC
#define VERSION_SUFFIX        "-rc" TOSTRING(VERSION_TYPE_REVISION)
#elif VERSION_TYPE == VERSION_TYPE_RELEASE
#define VERSION_SUFFIX        ""
#else
#error Unknown version type
#endif

#define VERSION_STR TOSTRING(VERSION_MAJOR) "." TOSTRING(VERSION_MINOR) "." TOSTRING(VERSION_PATCH) VERSION_SUFFIX
#define PRODUCT_NAME_VERSION PRODUCT_NAME " " VERSION_STR
#define AF_USER_AGENT_SUFFIX(suffix) PRODUCT_NAME " v" VERSION_STR " " suffix
