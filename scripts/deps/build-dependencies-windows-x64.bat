@echo off
setlocal enabledelayedexpansion

echo Setting environment...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="

if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "VCVARS=%%I"
)

if not defined VCVARS (
  echo Visual Studio with the C++ x64 toolchain was not found.
  goto error
)

echo Using Visual Studio environment: %VCVARS%
call "%VCVARS%"
if errorlevel 1 goto error

set SEVENZIP="C:\Program Files\7-Zip\7z.exe"
set PATCH="C:\Program Files\Git\usr\bin\patch.exe"

if defined DEBUG (
  echo DEBUG=%DEBUG%
) else (
  set DEBUG=1
)

pushd %~dp0
set "SCRIPTDIR=%CD%"
cd ..\..\dep\msvc
mkdir deps-build
cd deps-build || goto error
set "BUILDDIR=%CD%"
cd ..
mkdir deps-x64
cd deps-x64 || goto error
set "INSTALLDIR=%CD%"
popd

echo SCRIPTDIR=%SCRIPTDIR%
echo BUILDDIR=%BUILDDIR%
echo INSTALLDIR=%INSTALLDIR%

set "PATH=%PATH%;%INSTALLDIR%\bin"

cd "%BUILDDIR%"

set FREETYPE=2.14.3
set HARFBUZZ=14.3.0
set LIBJPEGTURBO=3.2.0
set LIBPNG=1658
set QT=6.7.2
set QTMINOR=6.7
set SDL2=2.32.10
set WEBP=1.6.0
set ZLIB=1.3.1
set ZSTD=1.5.7

set CPUINFO=7524ad504fdcfcf75a18a133da6abd75c5d48053
set DISCORD_RPC=cc59d26d1d628fbd6527aac0ac1d6301f4978b92
set LUNASVG=9af1ac7b90658a279b372add52d6f77a4ebb482c
set SHADERC=f60bb80e255144e71776e2ad570d89b78ea2ab4f
set SOUNDTOUCH=463ade388f3a51da078dc9ed062bf28e4ba29da7
set SPIRV_CROSS=vulkan-sdk-1.3.290.0

call :downloadfile "freetype-%FREETYPE%.tar.gz" https://downloads.sourceforge.net/project/freetype/freetype2/%FREETYPE%/freetype-%FREETYPE%.tar.gz e61b31ab26358b946e767ed7eb7f4bb2e507da1cfefeb7a8861ace7fd5c899a1 || goto error
call :downloadfile "harfbuzz-%HARFBUZZ%.zip" https://github.com/harfbuzz/harfbuzz/archive/refs/tags/%HARFBUZZ%.zip 1b660cc960e92ff191330f9d20be2de9e17b6233a9eba560ffb31750d47de32f || goto error
call :downloadfile "lpng%LIBPNG%.zip" https://download.sourceforge.net/libpng/lpng%LIBPNG%.zip b32f170855dbbe3e6d9e645af40b538137041773672c3ba3e02db5816c82d376 || goto error
call :downloadfile "libjpeg-turbo-%LIBJPEGTURBO%.tar.gz" "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/%LIBJPEGTURBO%/libjpeg-turbo-%LIBJPEGTURBO%.tar.gz" 6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e || goto error
call :downloadfile "SDL2-%SDL2%.zip" "https://github.com/libsdl-org/SDL/releases/download/release-%SDL2%/SDL2-%SDL2%.zip" 12b2dc2eb8f2836100a7916b5d394a0c82f1f7e32693f95f98305403af242f08 || goto error
call :downloadfile "qtbase-everywhere-src-%QT%.zip" "https://download.qt.io/archive/qt/%QTMINOR%/%QT%/submodules/qtbase-everywhere-src-%QT%.zip" 488119aad60719a085a1e45c31641ac2406ef86fc088a3c99885c18e9d6b4bb9 || goto error
call :downloadfile "qtimageformats-everywhere-src-%QT%.zip" "https://download.qt.io/archive/qt/%QTMINOR%/%QT%/submodules/qtimageformats-everywhere-src-%QT%.zip" 8e736b02db7dd67dbe834d56503b242344ce85d3532da692f1812b30ccf80997 || goto error
call :downloadfile "qtsvg-everywhere-src-%QT%.zip" "https://download.qt.io/archive/qt/%QTMINOR%/%QT%/submodules/qtsvg-everywhere-src-%QT%.zip" 85a22142270a92be0dd0ab5d27cc53617b2a2f1a45fc0a3890024164032f8475 || goto error
call :downloadfile "qttools-everywhere-src-%QT%.zip" "https://download.qt.io/archive/qt/%QTMINOR%/%QT%/submodules/qttools-everywhere-src-%QT%.zip" 9e15f1fdbd83e4123e733bff20aff1b45921c09056c3790fa42eb71d0a5cd01f || goto error
call :downloadfile "qttranslations-everywhere-src-%QT%.zip" "https://download.qt.io/archive/qt/%QTMINOR%/%QT%/submodules/qttranslations-everywhere-src-%QT%.zip" d1f25e0f68a1282feffdd5fe795a027ee5f16ad19e3b1fa2e04a51cea19110ec || goto error
call :downloadfile "libwebp-%WEBP%.tar.gz" "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-%WEBP%.tar.gz" e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564 || goto error
call :downloadfile "zlib-%ZLIB%.tar.gz" "https://zlib.net/fossils/zlib-%ZLIB%.tar.gz" 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 || goto error
call :downloadfile "zstd-%ZSTD%.zip" "https://github.com/facebook/zstd/archive/refs/tags/v%ZSTD%.zip" 7897bc5d620580d9b7cd3539c44b59d78f3657d33663fe97a145e07b4ebd69a4 || goto error

call :downloadfile "cpuinfo-%CPUINFO%.zip" "https://github.com/pytorch/cpuinfo/archive/%CPUINFO%.zip" 13146ae7983d767a678dd01b0d6af591e77cec82babd41264b9164ab808d7d41 || goto error
call :downloadfile "discord-rpc-%DISCORD_RPC%.zip" "https://github.com/stenzek/discord-rpc/archive/%DISCORD_RPC%.zip" 4492cbe690a16546da9a9d89f14340352cad3b0ac5484b969749d6ab6f1ee836 || goto error
call :downloadfile "lunasvg-%LUNASVG%.zip" "https://github.com/sammycage/lunasvg/archive/%LUNASVG%.zip" 1425ec2bda0228b73ffdc70b0dc666fc7d2b69c33eec75a35c4421157c0e220c || goto error
call :downloadfile "shaderc-%SHADERC%.zip" "https://github.com/stenzek/shaderc/archive/%SHADERC%.zip" d24760f3c20a0ca39dfa85b84b6cf65eee077cd168e7aa08502e60c168ef05f6 || goto error
call :downloadfile "soundtouch-%SOUNDTOUCH%.zip" "https://github.com/stenzek/soundtouch/archive/%SOUNDTOUCH%.zip" 107a1941181a69abe28018b9ad26fc0218625758ac193bc979abc9e26b7c0c3a || goto error

if not exist SPIRV-Cross\ (
  git clone https://github.com/KhronosGroup/SPIRV-Cross/ -b %SPIRV_CROSS% --depth 1 || goto error
)

if %DEBUG%==1 (
  echo Building debug and release libraries...
) else (
  echo Building release libraries...
)

set FORCEPDB=-DCMAKE_SHARED_LINKER_FLAGS_RELEASE="/DEBUG"

echo Building Zlib...
rmdir /S /Q "zlib-%ZLIB%"
tar -xf "zlib-%ZLIB%.tar.gz" || goto error
cd "zlib-%ZLIB%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DZLIB_BUILD_EXAMPLES=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building libpng...
rmdir /S /Q "lpng%LIBPNG%"
%SEVENZIP% x "lpng%LIBPNG%.zip" || goto error
cd "lpng%LIBPNG%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DBUILD_SHARED_LIBS=ON -DPNG_TESTS=OFF -DPNG_STATIC=OFF -DPNG_SHARED=ON -DPNG_TOOLS=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building libjpeg...
rmdir /S /Q "libjpeg-turbo-%LIBJPEGTURBO%"
tar -xf "libjpeg-turbo-%LIBJPEGTURBO%.tar.gz" || goto error
cd "libjpeg-turbo-%LIBJPEGTURBO%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DENABLE_STATIC=OFF -DENABLE_SHARED=ON -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building FreeType without HarfBuzz...
rmdir /S /Q "freetype-%FREETYPE%"
tar -xf "freetype-%FREETYPE%.tar.gz" || goto error
cd "freetype-%FREETYPE%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DFT_REQUIRE_ZLIB=TRUE -DFT_REQUIRE_PNG=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_DISABLE_HARFBUZZ=TRUE -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building HarfBuzz...
rmdir /S /Q "harfbuzz-%HARFBUZZ%"
%SEVENZIP% x "-x^!harfbuzz-%HARFBUZZ%\README" "harfbuzz-%HARFBUZZ%.zip" || goto error
cd "harfbuzz-%HARFBUZZ%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DHB_BUILD_UTILS=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building FreeType with HarfBuzz...
rmdir /S /Q "freetype-%FREETYPE%"
tar -xf "freetype-%FREETYPE%.tar.gz" || goto error
cd "freetype-%FREETYPE%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DFT_REQUIRE_ZLIB=TRUE -DFT_REQUIRE_PNG=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_BROTLI=TRUE -DFT_REQUIRE_HARFBUZZ=TRUE -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building Zstandard...
rmdir /S /Q "zstd-%ZSTD%"
%SEVENZIP% x "-x^!zstd-%ZSTD%\tests\cli-tests\bin" "zstd-%ZSTD%.zip" || goto error
cd "zstd-%ZSTD%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_STATIC=OFF -DZSTD_BUILD_PROGRAMS=OFF -B build -G Ninja build/cmake || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building WebP...
rmdir /S /Q "libwebp-%WEBP%"
tar -xf "libwebp-%WEBP%.tar.gz" || goto error
cd "libwebp-%WEBP%" || goto error
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DBUILD_SHARED_LIBS=ON -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building SDL...
rmdir /S /Q "SDL2-%SDL2%"
%SEVENZIP% x "SDL2-%SDL2%.zip" || goto error
cd "SDL2-%SDL2%" || goto error
cmake -B build -DCMAKE_BUILD_TYPE=Release %FORCEPDB% -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DSDL_SHARED=ON -DSDL_STATIC=OFF -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
copy build\SDL2.pdb "%INSTALLDIR%\bin" || goto error
cd .. || goto error

if %DEBUG%==1 (
  set QTBUILDSPEC=-DCMAKE_CONFIGURATION_TYPES="Release;Debug" -G "Ninja Multi-Config"
) else (
  set QTBUILDSPEC=-DCMAKE_BUILD_TYPE=Release -G Ninja
)

echo Building Qt base...
rmdir /S /Q "qtbase-everywhere-src-%QT%"
%SEVENZIP% x "qtbase-everywhere-src-%QT%.zip" || goto error
cd "qtbase-everywhere-src-%QT%" || goto error

rem Disable the PCRE2 JIT, it doesn't properly verify AVX2 support.
%PATCH% -p1 < "%SCRIPTDIR%\qtbase-disable-pcre2-jit.patch" || goto error

cmake -B build -DFEATURE_sql=OFF -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" %FORCEPDB% -DINPUT_gui=yes -DINPUT_widgets=yes -DINPUT_ssl=yes -DINPUT_openssl=no -DINPUT_schannel=yes -DFEATURE_system_png=ON -DFEATURE_system_jpeg=ON -DFEATURE_system_zlib=ON -DFEATURE_system_freetype=ON -DFEATURE_system_harfbuzz=ON %QTBUILDSPEC% || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building Qt SVG...
rmdir /S /Q "qtsvg-everywhere-src-%QT%"
%SEVENZIP% x "qtsvg-everywhere-src-%QT%.zip" || goto error
cd "qtsvg-everywhere-src-%QT%" || goto error
mkdir build || goto error
cd build || goto error
call "%INSTALLDIR%\bin\qt-configure-module.bat" .. -- %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" || goto error
cmake --build . --parallel || goto error
ninja install || goto error
cd ..\.. || goto error

echo Building Qt Image Formats...
rmdir /S /Q "qtimageformats-everywhere-src-%QT%"
%SEVENZIP% x "qtimageformats-everywhere-src-%QT%.zip" || goto error
cd "qtimageformats-everywhere-src-%QT%" || goto error
mkdir build || goto error
cd build || goto error
call "%INSTALLDIR%\bin\qt-configure-module.bat" .. -- %FORCEPDB% -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DFEATURE_system_webp=ON || goto error
cmake --build . --parallel || goto error
ninja install || goto error
cd ..\.. || goto error

echo Building Qt Tools...
rmdir /S /Q "qttools-everywhere-src-%QT%"
%SEVENZIP% x "qttools-everywhere-src-%QT%.zip" || goto error
cd "qttools-everywhere-src-%QT%" || goto error
mkdir build || goto error
cd build || goto error
call "%INSTALLDIR%\bin\qt-configure-module.bat" .. -- %FORCEPDB% -DFEATURE_assistant=OFF -DFEATURE_clang=OFF -DFEATURE_designer=ON -DFEATURE_kmap2qmap=OFF -DFEATURE_pixeltool=OFF -DFEATURE_pkg_config=OFF -DFEATURE_qev=OFF -DFEATURE_qtattributionsscanner=OFF -DFEATURE_qtdiag=OFF -DFEATURE_qtplugininfo=OFF || goto error
cmake --build . --parallel || goto error
ninja install || goto error
cd ..\.. || goto error

echo Building Qt Translations...
rmdir /S /Q "qttranslations-everywhere-src-%QT%"
%SEVENZIP% x "qttranslations-everywhere-src-%QT%.zip" || goto error
cd "qttranslations-everywhere-src-%QT%" || goto error
mkdir build || goto error
cd build || goto error
call "%INSTALLDIR%\bin\qt-configure-module.bat" .. -- %FORCEPDB% || goto error
cmake --build . --parallel || goto error
ninja install || goto error
cd ..\.. || goto error

echo Building shaderc...
rmdir /S /Q "shaderc-%SHADERC%"
%SEVENZIP% x "shaderc-%SHADERC%.zip" || goto error
cd "shaderc-%SHADERC%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSHADERC_ENABLE_SHARED_CRT=ON -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building SPIRV-Cross...
cd SPIRV-Cross || goto error
rmdir /S /Q "build"
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DSPIRV_CROSS_SHARED=ON -DSPIRV_CROSS_STATIC=OFF -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_GLSL=ON -DSPIRV_CROSS_ENABLE_HLSL=ON -DSPIRV_CROSS_ENABLE_MSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_C_API=ON -DSPIRV_CROSS_ENABLE_UTIL=ON -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building cpuinfo...
rmdir /S /Q "cpuinfo-%CPUINFO%"
%SEVENZIP% x "cpuinfo-%CPUINFO%.zip" || goto error
cd "cpuinfo-%CPUINFO%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DCPUINFO_LIBRARY_TYPE=shared -DCPUINFO_RUNTIME_TYPE=shared -DCPUINFO_LOG_LEVEL=error -DCPUINFO_LOG_TO_STDIO=ON -DCPUINFO_BUILD_TOOLS=OFF -DCPUINFO_BUILD_UNIT_TESTS=OFF -DCPUINFO_BUILD_MOCK_TESTS=OFF -DCPUINFO_BUILD_BENCHMARKS=OFF -DUSE_SYSTEM_LIBS=ON -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building discord-rpc...
rmdir /S /Q "discord-rpc-%DISCORD_RPC%"
%SEVENZIP% x "discord-rpc-%DISCORD_RPC%.zip" || goto error
cd "discord-rpc-%DISCORD_RPC%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building lunasvg...
rmdir /S /Q "lunasvg-%LUNASVG%"
%SEVENZIP% x "lunasvg-%LUNASVG%.zip" || goto error
cd "lunasvg-%LUNASVG%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DBUILD_SHARED_LIBS=ON -DLUNASVG_BUILD_EXAMPLES=OFF -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Building soundtouch...
rmdir /S /Q "soundtouch-%SOUNDTOUCH%"
%SEVENZIP% x "soundtouch-%SOUNDTOUCH%.zip" || goto error
cd "soundtouch-%SOUNDTOUCH%" || goto error
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%INSTALLDIR%" -DCMAKE_INSTALL_PREFIX="%INSTALLDIR%" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -B build -G Ninja || goto error
cmake --build build --parallel || goto error
ninja -C build install || goto error
cd .. || goto error

echo Cleaning up...
cd ..
rd /S /Q deps-build

echo Exiting with success.
exit 0

:error
set "EXITCODE=%errorlevel%"
echo Failed with error #%EXITCODE%.
if not defined GITHUB_ACTIONS pause
exit /B %EXITCODE%

:downloadfile
if exist "%~1" (
  call :validatefile "%~1" "%~3"
  if not errorlevel 1 exit /B 0
  echo Cached %~1 failed validation. Redownloading...
  del /Q "%~1" || exit /B 1
)

echo Downloading %~1 from %~2...
curl -L --fail --show-error --retry 10 --retry-all-errors --retry-delay 3 --connect-timeout 30 -o "%~1" "%~2" || exit /B 1
if not exist "%~1" (
  echo Required source file %~1 not found after download.
  exit /B 1
)
call :validatefile "%~1" "%~3"
exit /B %errorlevel%

:validatefile
rem based on https://gist.github.com/gsscoder/e22daefaff9b5d8ac16afb070f1a7971
set idx=0
for /f %%F in ('certutil -hashfile "%~1" SHA256') do (
    set "out!idx!=%%F"
    set /a idx += 1
)
set filechecksum=%out1%

if /i %~2==%filechecksum% (
    echo Validated %~1.
    exit /B 0
) else (
    echo Expected %~2 got %filechecksum%.
    exit /B 1
)
