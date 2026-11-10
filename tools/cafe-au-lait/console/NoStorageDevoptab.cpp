// This app uses only its explicit read-only FSA client. Do not initialize WUT's
// general filesystem devoptab, which otherwise mounts SD and links write APIs.
extern "C" void __init_wut_devoptab() {}
extern "C" void __fini_wut_devoptab() {}
