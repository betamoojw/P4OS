/* P4OS - CAS: what cas.so exports.
 *
 * Only the app's entry points. Everything else - Giac, GMP, libstdc++, the
 * shims - is local to the .so: the linker binds those references itself and
 * they become relative relocations. Without this they are named relocations,
 * and the loader looks every name up in the FIRMWARE's table first
 * (esp_elf.c), so a firmware symbol with the same name as one of the
 * engine's (operator new, floor...) would silently take its place.
 *
 * project_so() takes no linker flags, so this goes in as an input file with
 * a .o name: ld does not recognise it as an object and reads it as an
 * implicit linker script. */
VERSION {
    {
        global: aos_app_abi; aos_app_init;
        local: *;
    };
}
