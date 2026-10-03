#pragma once

#if defined(MAGPIE_STATIC_DEFINE)
#    define MAGPIE_EXPORT
#elif defined(_WIN32) || defined(__CYGWIN__)
#    if defined(MAGPIE_BUILDING_LIBRARY)
#        define MAGPIE_EXPORT __declspec(dllexport)
#    else
#        define MAGPIE_EXPORT __declspec(dllimport)
#    endif
#elif defined(__GNUC__) || defined(__clang__)
#    define MAGPIE_EXPORT __attribute__((visibility("default")))
#else
#    define MAGPIE_EXPORT
#endif
