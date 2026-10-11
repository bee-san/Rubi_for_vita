# Evaluated source pin. Archive checkouts work without .git metadata.
set(VJO_MEIKI_SOURCE_REVISION "39a3a9ae069180675c145f6df74c843dc2061967")
set(_meiki_source_pins
  "model_reader.c|11ed21326fe045b833e94027d76e7df9a1efe2a0b268ca1e85ccd4ba808f73e6"
  "model_reader.h|92dc4e561717cb81d9f8723efc8c7d13e17bb8733db87295a84285675c7056c3"
  "arena.c|30f3b7b6c95dd2310959ed4bfdc7f80240368b31e1572d9142b71d568561d1d1"
  "arena.h|510699fbd3778cfdb9de33b6280f6f1d415be2499ba63c121204979c0c2f0e7b"
  "decode.c|7a2e654b207b260306982a6e266ac0240cc38c20b437a370320aef6ef5e54a79"
  "decode.h|27ad16a639f5d0f77db1f0eb4fe86a4a2272c9987eb5e4faf44ff54d8ffc204c"
  "runtime.cpp|50dfeb27107af965b167830ada18f7f27a13e4d9dd91e10bffb8568b3e42a6c4"
  "runtime.h|31426ee1ac2c5bf2db77ef010b12a84ec2c0d63cb82fe69ec07ee1a0c11f59cc"
  "preprocess.c|02793943ccfcaf374eb4484e07fc8e892215a947b2654748a06b5007935e1c53"
  "preprocess.h|88f772d141a4c7db4c8622578fa2b28642114fd8161e2b16c59b32007a82be50"
  "detect_runtime.cpp|1278b79db2e7e84061ff9d0fa5c0835a2b5b4190cfb86a0510e5fe6f64319e3c"
  "detect_runtime.h|67188a2fb1dd2db82bc968420be52b483c6fd58aec42b48c3689cfddbbc65210"
  "detect_preprocess.c|694871a751bf63a5f11fe4234efaeac760131f996c8fca06f1b2db96289eae02"
  "detect_preprocess.h|f6a8899c2df44ee4e2479fd26549f7ca0c7b1c6268e11f477793e301efcb3f47"
  "module/CMakeLists.txt|180fe2fd5ca9a6d987d4275681a6e05fdad131b6a0cb8f94c7ee085234cc2bb8"
  "module/api.h|2b537d411d0d65768ac79640a8fc27ce9199913e34330a21efdc2a23c41d6cef"
  "module/bridge.c|6928efa4a3b6637942f1825e2b62d75261ad118a167a573d7add2b0c7f7966c9"
  "module/metadata.c|5d015b078996a3c0e11f13f0531bcea04da642089364777055c14537809e9a00"
  "module/engine.cpp|7c3e6c07970b0ea911d13b25631b99c4bdf9823dd7fdf2d7b864918a5656c84d"
  "module/exports.yml|9c4b3ff5388eefea166a94988ead82c50c9cae335ca1a75dbe9a51ea594c51f4"
  "module/internal.h|ce6c66f00ef23504348ecaabd32210c1365d389cbc35236f67eb37093fef2e36"
)
foreach(_pin IN LISTS _meiki_source_pins)
  string(REPLACE "|" ";" _parts "${_pin}")
  list(GET _parts 0 _file)
  list(GET _parts 1 _expected)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${VJO_MEIKI_SOURCE_DIR}/${_file}")
  if(NOT EXISTS "${VJO_MEIKI_SOURCE_DIR}/${_file}")
    message(FATAL_ERROR "Missing pinned Meiki source: ${_file}")
  endif()
  file(SHA256 "${VJO_MEIKI_SOURCE_DIR}/${_file}" _actual)
  if(NOT _actual STREQUAL _expected)
    message(FATAL_ERROR "Meiki source differs: ${_file}. Use vita-vn-ocr ${VJO_MEIKI_SOURCE_REVISION}")
  endif()
endforeach()
