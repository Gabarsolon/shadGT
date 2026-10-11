// Entry for a shadGT HLE library built on its own for Ryty. The target defines RYTY_HLE_HEADER,
// RYTY_HLE_REGISTER (the library's RegisterLib) and RYTY_HLE_LOG (its log file name).

#include "core/loader/symbols_resolver.h"
#include "ryty_gpu/ryty_library.h"
#include RYTY_HLE_HEADER

void Ryty::RytyLibraryInitialize() {
    InitializeCommon(RYTY_HLE_LOG);
    Core::Loader::SymbolsResolver resolver;
    RYTY_HLE_REGISTER(&resolver);
    BindExports();
}
