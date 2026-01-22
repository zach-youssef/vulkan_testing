#include "VulkanApp.h"


const uint32_t WINDOW_WIDTH = 800;
const uint32_t WINDOW_HEIGHT = 600;
const int MAX_FRAMES_IN_FLIGHT = 2;

int main() {
    VulkanApp app{
        WINDOW_HEIGHT,
        WINDOW_WIDTH,
        MAX_FRAMES_IN_FLIGHT
    };

    try {
        app.init();
        app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
