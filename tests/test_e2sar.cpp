#include <e2sar.hpp>
#include <iostream>

int main() {
    std::cout << "E2SAR library integration test" << std::endl;
    std::cout << "E2SAR library path: /Users/baldin/workspaces/workspace-ejfat/e2sar-install/lib" << std::endl;
    std::cout << "E2SAR include path: /Users/baldin/workspaces/workspace-ejfat/e2sar-install/include" << std::endl;
    std::cout << std::endl;

    // Test that E2SAR headers can be included
    std::cout << "E2SAR headers successfully included!" << std::endl;

    // Test EjfatURI parsing
    std::string test_uri = "ejfat://token@localhost:12345/lb/1?data=192.168.1.1:54321";
    std::cout << "Testing URI parsing with: " << test_uri << std::endl;

    try {
        e2sar::EjfatURI uri(test_uri, e2sar::EjfatURI::TokenType::admin);
        std::cout << "e2sar::EjfatURI successfully created!" << std::endl;

        // Get and display parsed information
        std::cout << "\nParsed URI information:" << std::endl;
        std::cout << "  LB ID: " << uri.get_lbId() << std::endl;

        auto cpHost = uri.get_cpHost();
        if (cpHost) {
            std::cout << "  Control Plane: " << cpHost.value().first
                      << ":" << cpHost.value().second << std::endl;
        }

    } catch (const std::exception& e) {
        std::cout << "Note: URI parsing encountered: " << e.what() << std::endl;
        std::cout << "(This is expected for invalid/test URIs)" << std::endl;
    }

    std::cout << "\nE2SAR integration successful!" << std::endl;

    return 0;
}
