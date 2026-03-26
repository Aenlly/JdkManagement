#include <vector>

#include <windows.h>

#include "app/app.h"
#include "core/paths.h"

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::vector<std::string> args;
    args.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }

    jkm::Application app(jkm::DetectAppPaths());
    return app.Run(args);
}
