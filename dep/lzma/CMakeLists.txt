add_library(lzma
  include/7z.h
  include/7zBuf.h
  include/7zCrc.h
  include/7zTypes.h
  include/Alloc.h
  include/Bcj2.h
  include/Bra.h
  include/Compiler.h
  include/CpuArch.h
  include/Delta.h
  include/LzFind.h
  include/LzHash.h
  include/Lzma86.h
  include/Lzma2Dec.h
  include/LzmaDec.h
  include/LzmaEnc.h
  include/LzmaLib.h
  include/Precomp.h
  include/Sort.h
  src/7zArcIn.c
  src/7zBuf.c
  src/7zCrc.c
  src/7zCrcOpt.c
  src/7zDec.c
  src/7zStream.c
  src/Alloc.c
  src/Bcj2.c
  src/Bra.c
  src/Bra86.c
  src/BraIA64.c
  src/CpuArch.c
  src/Delta.c
  src/LzFind.c
  src/Lzma86Dec.c
  src/Lzma86Enc.c
  src/Lzma2Dec.c
  src/LzmaDec.c
  src/LzmaEnc.c
  src/LzmaLib.c
  src/Sort.c
)

target_compile_definitions(lzma PRIVATE _7ZIP_ST)

target_include_directories(lzma PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(lzma INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

