// BBB-AIRGAP: give C++ exceptions somewhere to land that clears keys first.
//
// The firmware routes every abort() through jade_abort() with a linker wrap
// (main/CMakeLists.txt:85, "-Wl,--wrap=abort"), which is how the ESP32 build guarantees that
// keychain_clear() and sensitive_clear_stack() run before the process dies.  That wrap cannot
// do the same job here and adding the flag would not fix it: libjade.so links libstdc++
// dynamically, and --wrap only redirects undefined abort references in our own link unit, not
// the call inside libstdc++'s own std::terminate.  On the ESP32 libstdc++ is linked statically,
// so the flag covers it there and the port silently lost the guarantee.
//
// The consequence is not theoretical.  The bc-ur decoder is C++ compiled with -fno-exceptions
// (libjade/CMakeLists.txt), while the standard library it calls into is not: a std::stoul on a
// malformed UR sequence throws, nothing can catch it, and the process dies with the keychain
// still in memory.  A terminate handler is independent of how libstdc++ is linked and also
// covers throws we have not found yet, which a per-call input check cannot.
//
// The handler is installed with a function pointer rather than calling jade_abort() directly
// because libjade.c includes main/amalgamated.c, so jade_abort() is local to that translation
// unit and not linkable from here.

#include <cstdlib>
#include <exception>

extern "C" {

static void (*s_on_terminate)(void) = nullptr;

// Runs instead of libstdc++'s default terminate handler.  s_on_terminate() is jade_abort(),
// which clears sensitive memory and then calls the real abort(); the std::abort() below is
// only the backstop for the case where installation raced ahead of the pointer being set,
// because a terminate handler must not return.
static void jade_terminate_handler(void)
{
    if (s_on_terminate) {
        s_on_terminate();
    }
    std::abort();
}

void jade_install_terminate_handler(void (*on_terminate)(void))
{
    s_on_terminate = on_terminate;
    std::set_terminate(jade_terminate_handler);
}
}
