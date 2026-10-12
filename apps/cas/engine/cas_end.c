/*
 * P4OS - CAS: the end of the constructor table.
 *
 * The loader does not run a .so's .init_array, so cas_rt.c runs it itself.
 * The start marker is in cas_rt.c, in .init_array.00000, which the linker
 * sorts first; this one is in plain .init_array and comes from the last
 * archive of the link, so it is placed after every other constructor.
 */
typedef void (*cas_ctor_t)(void);

__attribute__((used, section(".init_array")))
const cas_ctor_t cas_ctor_end[1] = { 0 };
