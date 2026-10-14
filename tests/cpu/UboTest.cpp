#include "Ubo.h"

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

TEST_CASE("UniformBufferObject::fromModelViewProjection composes projection * view * model") {
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f));
    const glm::mat4 view = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    const glm::mat4 projection = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));

    const auto ubo = UniformBufferObject::fromModelViewProjection(model, view, projection);

    CHECK(ubo.mvp == projection * view * model);
    // Order matters: the result must differ from model * view * projection
    CHECK(ubo.mvp != model * view * projection);
}
