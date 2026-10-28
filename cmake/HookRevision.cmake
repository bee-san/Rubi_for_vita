# Changing a definition, decoder, site-placement rule or selector invalidates persisted learning.
# Derived from content, so a table update cannot accidentally retain an old manual revision number.
function(vjo_hook_revision root)
  set(inputs
    tools/hook_profiles.c tools/hook_profiles.py
    core/hook_profile.c core/hook_profile.h core/hook_profile_file.c core/hook_profile_file.h core/hook_ops.h
    core/hook_luna.c core/hook_luna.h core/hook_luna_table.inc core/hook_luna_psp.inc
    core/hook_text.c core/hook_text.h core/hook_cp932.inc core/hook_re.c core/hook_re.h
    core/hook_place.c core/hook_place.h core/hook_select.c core/hook_select.h
    core/hook_stream.c core/hook_stream.h core/hook_sc3.c core/hook_sc3.h core/hook_sc3_pins.inc
    game_ocr/hooks.c game_ocr/hooks.h game_ocr/hooks_observer.S)
  set(revision "")
  foreach(path IN LISTS inputs)
    file(SHA256 "${root}/${path}" digest)
    string(APPEND revision "${path}:${digest}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/${path}")
  endforeach()
  string(SHA256 revision "${revision}")
  add_compile_definitions(VJO_HOOK_DEFINITION_SHA256="${revision}")
endfunction()
