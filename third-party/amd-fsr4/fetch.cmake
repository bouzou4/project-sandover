# AMD FSR 4 runtime (signed DLLs from the AMD FSR SDK), fetched at configure time: 27 MB of binary
# kept out of the repository. The headers are vendored (include/, MIT). See README.txt.
#
# Sets FSR4_SDK_FOUND, FSR4_SDK_INCLUDE_DIR and FSR4_SDK_RUNTIMES (the two DLLs).

set(FSR4_SDK_COMMIT "60f4ea81909200d8542eca14dccb2628b763a9a3")  # v2.3.0
set(FSR4_SDK_URL
  "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${FSR4_SDK_COMMIT}/Kits/FidelityFX/signedbin")
set(FSR4_SDK_FILES
  "amd_fidelityfx_loader_dx12.dll" e2d85aa05a9bd9ed8b38935fdf5199372cca6f74c12015143bb6f945ee1608aa
  "amd_fidelityfx_upscaler_dx12.dll" d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46
)

set(FSR4_SDK_FOUND OFF)
set(FSR4_SDK_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/include")
if(NOT SANDOVER_FSR4_SDK_DIR AND DEFINED ENV{SANDOVER_FSR4_SDK_DIR})
  set(SANDOVER_FSR4_SDK_DIR "$ENV{SANDOVER_FSR4_SDK_DIR}")
endif()
if(SANDOVER_FSR4_SDK_DIR)
  set(_fsr4_root "${SANDOVER_FSR4_SDK_DIR}")
else()
  set(_fsr4_root "${CMAKE_BINARY_DIR}/amd-fsr4-${FSR4_SDK_COMMIT}")
endif()

set(_fsr4_ok ON)
set(FSR4_SDK_RUNTIMES "")
list(LENGTH FSR4_SDK_FILES _fsr4_n)
math(EXPR _fsr4_last "${_fsr4_n} - 1")
foreach(_i RANGE 0 ${_fsr4_last} 2)
  math(EXPR _j "${_i} + 1")
  list(GET FSR4_SDK_FILES ${_i} _rel)
  list(GET FSR4_SDK_FILES ${_j} _sha)
  set(_dst "${_fsr4_root}/${_rel}")
  if(EXISTS "${_dst}")
    file(SHA256 "${_dst}" _have)
  else()
    set(_have "")
  endif()
  if(NOT _have STREQUAL _sha)
    if(SANDOVER_FSR4_SDK_DIR)
      message(WARNING "FSR 4: ${_dst} is missing or does not match ${FSR4_SDK_COMMIT}")
      set(_fsr4_ok OFF)
      break()
    endif()
    message(STATUS "FSR 4: fetching ${_rel}")
    file(DOWNLOAD "${FSR4_SDK_URL}/${_rel}" "${_dst}" EXPECTED_HASH SHA256=${_sha} STATUS _st
         TLS_VERIFY ON)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      list(GET _st 1 _msg)
      message(WARNING "FSR 4: download of ${_rel} failed (${_msg}); building without FSR 4")
      file(REMOVE "${_dst}")
      set(_fsr4_ok OFF)
      break()
    endif()
  endif()
  list(APPEND FSR4_SDK_RUNTIMES "${_dst}")
endforeach()

if(_fsr4_ok)
  set(FSR4_SDK_FOUND ON)
  message(STATUS "FSR 4 runtime (SDK ${FSR4_SDK_COMMIT}): ${_fsr4_root}")
endif()
