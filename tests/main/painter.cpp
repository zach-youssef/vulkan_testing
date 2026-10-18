// What painter.zgal's app needs that its main can't give: the scale no option sets, the extern
// function and the graph's data
#include "painter.zgal.h"

#include <iostream>

Result<painter::MainInputs> painter::mainInputs(VulkanContext&) {
    return MainInputs{
        .scale = 0.5f,
        .note = [](float level) {
            std::cout << "level " << level << std::endl;
            return level;
        },
        .marks = {1, 2, 3},
    };
}
