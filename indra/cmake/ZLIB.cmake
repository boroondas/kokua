# -*- cmake -*-

set(ZLIB_FIND_QUIETLY ON)
set(ZLIB_FIND_REQUIRED ON)

include(Prebuilt)

if (STANDALONE)
  include(FindZLIB)
else (STANDALONE)
  use_prebuilt_binary(zlib)
  if (WINDOWS)
    set(ZLIB_LIBRARIES 
      debug zlibd
      optimized zlib)
  else (WINDOWS)
    set(ZLIB_LIBRARIES z)
  endif (WINDOWS)
  if (WINDOWS)
    set(ZLIB_INCLUDE_DIRS ${LIBS_PREBUILT_DIR}/include/zlib)
  endif (WINDOWS)
  if (LINUX)
    set(ZLIB_INCLUDE_DIRS ${LIBS_PREBUILT_DIR}/${LL_ARCH_DIR}/include
                          ${LIBS_PREBUILT_DIR}/include)
  endif (LINUX)
endif (STANDALONE)
