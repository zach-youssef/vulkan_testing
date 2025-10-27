#pragma once

#include <glm/glm.hpp>

struct UniformBufferObject {
    glm::mat4 mvp;
    
    static UniformBufferObject fromModelViewProjection(glm::mat4 model, glm::mat4 view, glm::mat4 projection) {
        return UniformBufferObject{model * view * projection};
    }
};
