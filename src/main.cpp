#include "app.hpp"
#include "daemon.hpp"
#include <iostream>
#include <cstring>

int main(int argc, char* argv[]) {
    if (argc > 1 && std::strcmp(argv[1], "--daemon") == 0) {
        runDaemon();
        return 0;
    }
    App app;
    if (!app.initialize()) return 1;
    app.run();
    return 0;
}
