# NVIDIA DLSS SDK (NGX), fetched at configure time and never committed: its licence allows
# distribution only as part of the built application (README.txt). The release does not ship the
# runtime DLL; it is only copied beside gk in the build tree for local runs.
#
# Sets DLSS_SDK_FOUND, DLSS_SDK_INCLUDE_DIR, DLSS_SDK_LIBRARY, DLSS_SDK_RUNTIME (nvngx_dlss.dll)
# and DLSS_SDK_LICENSE. SANDOVER_DLSS_SDK_DIR (cache or environment) points at a folder that
# already holds the files below, for offline builds; it is checked against the same hashes. That
# folder needs the lib for each runtime (nvsdk_ngx_d.lib and nvsdk_ngx_s.lib).

set(DLSS_SDK_TAG "v310.9.1")
set(DLSS_SDK_URL "https://raw.githubusercontent.com/NVIDIA/DLSS/${DLSS_SDK_TAG}")
# nvsdk_ngx_s.lib is built for the static C++ runtime (/MT), nvsdk_ngx_d.lib for the DLL runtime (/MD).
if(CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreaded")
  set(DLSS_NGX_LIB "nvsdk_ngx_s.lib")
  set(DLSS_NGX_LIB_SHA 4e5d355086d2bc11e1a0842457d2519ea528ee1f3e112c45679a84960c07dff3)
else()
  set(DLSS_NGX_LIB "nvsdk_ngx_d.lib")
  set(DLSS_NGX_LIB_SHA 4b6cecad7f1906571c94010241f650e4a5457e64fad49ddaccb82de79f6c2999)
endif()
set(DLSS_SDK_FILES
  "include/nvsdk_ngx.h" dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848
  "include/nvsdk_ngx_defs.h" ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e
  "include/nvsdk_ngx_params.h" 943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc
  "include/nvsdk_ngx_helpers.h" 5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471
  "include/nvsdk_ngx_helpers_d3d.h" ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba
  "include/nvsdk_ngx_helpers_cuda.h" f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042
  "lib/Windows_x86_64/x64/${DLSS_NGX_LIB}" ${DLSS_NGX_LIB_SHA}
  "lib/Windows_x86_64/rel/nvngx_dlss.dll" 3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983
  "LICENSE.txt" d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0
)

set(DLSS_SDK_FOUND OFF)
if(NOT SANDOVER_DLSS_SDK_DIR AND DEFINED ENV{SANDOVER_DLSS_SDK_DIR})
  set(SANDOVER_DLSS_SDK_DIR "$ENV{SANDOVER_DLSS_SDK_DIR}")
endif()
if(SANDOVER_DLSS_SDK_DIR)
  set(_dlss_root "${SANDOVER_DLSS_SDK_DIR}")
else()
  set(_dlss_root "${CMAKE_BINARY_DIR}/nvidia-dlss-${DLSS_SDK_TAG}")
endif()

set(_dlss_ok ON)
list(LENGTH DLSS_SDK_FILES _dlss_n)
math(EXPR _dlss_last "${_dlss_n} - 1")
foreach(_i RANGE 0 ${_dlss_last} 2)
  math(EXPR _j "${_i} + 1")
  list(GET DLSS_SDK_FILES ${_i} _rel)
  list(GET DLSS_SDK_FILES ${_j} _sha)
  set(_dst "${_dlss_root}/${_rel}")
  if(EXISTS "${_dst}")
    file(SHA256 "${_dst}" _have)
  else()
    set(_have "")
  endif()
  if(NOT _have STREQUAL _sha)
    if(SANDOVER_DLSS_SDK_DIR)
      message(WARNING "DLSS SDK: ${_dst} is missing or does not match ${DLSS_SDK_TAG}")
      set(_dlss_ok OFF)
      break()
    endif()
    message(STATUS "DLSS SDK: fetching ${_rel}")
    string(REPLACE " " "%20" _url "${DLSS_SDK_URL}/${_rel}")
    file(DOWNLOAD "${_url}" "${_dst}" EXPECTED_HASH SHA256=${_sha} STATUS _st TLS_VERIFY ON)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      list(GET _st 1 _msg)
      message(WARNING "DLSS SDK: download of ${_rel} failed (${_msg}); building without DLSS")
      file(REMOVE "${_dst}")
      set(_dlss_ok OFF)
      break()
    endif()
  endif()
endforeach()

if(_dlss_ok)
  set(DLSS_SDK_FOUND ON)
  set(DLSS_SDK_INCLUDE_DIR "${_dlss_root}/include")
  set(DLSS_SDK_LIBRARY "${_dlss_root}/lib/Windows_x86_64/x64/${DLSS_NGX_LIB}")
  set(DLSS_SDK_RUNTIME "${_dlss_root}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
  set(DLSS_SDK_LICENSE "${_dlss_root}/LICENSE.txt")
  message(STATUS "DLSS SDK ${DLSS_SDK_TAG}: ${_dlss_root}")
endif()

# the package preset (static runtime) must not ship a build that silently lost DLSS
if(NOT _dlss_ok AND CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreaded")
  message(FATAL_ERROR "DLSS SDK ${DLSS_SDK_TAG} is required for the package preset; see SANDOVER_DLSS_SDK_DIR")
endif()
