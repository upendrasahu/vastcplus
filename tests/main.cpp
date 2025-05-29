#include "../include/VASTClient.h"
#include <iostream>

int main() {
    try {
        VASTClient client("https://api.vastdata.com", "your_token_here");
        
        // Example GET request
        std::string response = client.get("/example_endpoint");
        std::cout << "GET Response: " << response << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    return 0;
}