#include "app.hpp"
#include "daemon.hpp"
#include <iostream>
#include <cstring>

int main(int argc, char* argv[]) {
    // Load libmpv NOW, while we're still single-threaded.  libmpv pulls in
    // libgomp (static-model TLS); on older glibc a static-TLS library can only
    // be dlopen'd before any thread starts, otherwise it fails with
    // "libgomp.so.1: cannot allocate memory in static TLS block".  Both the app
    // and the daemon create worker threads during init, so preload up here
    // before either path runs.
    MpvPlayer::preloadLibrary();

    if (argc > 1 && std::strcmp(argv[1], "--daemon") == 0) {
        runDaemon();
        return 0;
    }
    App app;
    if (!app.initialize()) return 1;
    app.run();
    return 0;
}
