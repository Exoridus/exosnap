#pragma once

#ifdef _WIN32
  #ifdef RECORDER_FACADE_BUILDING
    #define RECORDER_FACADE_API __declspec(dllexport)
  #else
    #define RECORDER_FACADE_API __declspec(dllimport)
  #endif
#else
  #define RECORDER_FACADE_API __attribute__((visibility("default")))
#endif
