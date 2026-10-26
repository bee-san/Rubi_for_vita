# Changing a definition, decoder, site-placement rule or selector invalidates persisted learning.
# Derived from content, so a table update cannot accidentally retain an old manual revision number.
# Not an input: core/hook_luna_psp.inc, Luna's PSP table, which only the debug build's read-only PSP probe
# reads (core/psp_probe.c); no Vita hook does, so editing it must not make every game learn again
# (audit-adrenaline-probe F4, API 35). Nor core/psp_text.{c,h}, PSP text's profiles: only the PSP emulator's
# process reads them, and it keeps no learned source (game_ocr/main.c hooks_cache_prepare).
function(vjo_hook_revision root)
  set(inputs
    core/hook_luna.c core/hook_luna.h core/hook_luna_table.inc
    core/hook_text.c core/hook_text.h core/hook_cp932.inc core/hook_re.c core/hook_re.h
    core/hook_place.c core/hook_place.h core/hook_select.c core/hook_select.h
    core/hook_stream.c core/hook_stream.h game_ocr/hooks.c game_ocr/hooks.h game_ocr/hooks_observer.S)
  set(revision "")
  foreach(path IN LISTS inputs)
    file(SHA256 "${root}/${path}" digest)
    string(APPEND revision "${path}:${digest}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/${path}")
  endforeach()
  string(SHA256 revision "${revision}")
  add_compile_definitions(VJO_HOOK_DEFINITION_SHA256="${revision}")
endfunction()
