#include "roc/vehicle/version.hpp"

#include <iostream>

int main() {
    std::cout << "ROC Vehicle Client " << roc::vehicle::library_version() << '\n'
              << "Protocol baseline "
              << roc::vehicle::protocol_compatibility_version() << '\n';
    return 0;
}
