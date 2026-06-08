#include "app.hpp"
#include <iostream>

int main(int, char*[]) {
    App app;
    if (!app.initialize()) return 1;
    app.run();
    return 0;
}
