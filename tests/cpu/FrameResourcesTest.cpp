#include "core/FrameResources.h"

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace {

// Records creation and destruction of per-frame elements
struct Log {
    std::vector<std::string> events;
};

PerFrame<int> makeLogged(FrameRegistry& registry, Log& log, const std::string& name) {
    return PerFrame<int>(registry,
                         [&log, name](uint32_t frame) {
                             log.events.push_back("create " + name + std::to_string(frame));
                             return static_cast<int>(frame);
                         },
                         [&log, name](int& frame) {
                             log.events.push_back("destroy " + name + std::to_string(frame));
                         });
}

} // namespace

TEST_CASE("FrameRegistry tracks the current frame") {
    FrameRegistry registry(3);
    CHECK(registry.framesInFlight() == 3);
    CHECK(registry.currentFrame() == 0);
    registry.advance();
    registry.advance();
    CHECK(registry.currentFrame() == 2);
    registry.advance();
    CHECK(registry.currentFrame() == 0);

    registry.advance();
    registry.resize(2);
    CHECK(registry.currentFrame() == 0);

    CHECK_THROWS_AS(FrameRegistry(0), std::invalid_argument);
    CHECK_THROWS_AS(registry.resize(0), std::invalid_argument);
}

TEST_CASE("PerFrame creates an element per frame and destroys them last first") {
    FrameRegistry registry(2);
    Log log;
    {
        auto values = makeLogged(registry, log, "a");
        CHECK(values.size() == 2);
        CHECK(values[0] == 0);
        CHECK(values[1] == 1);
        CHECK_THROWS(values[2]);
        CHECK(registry.size() == 1);

        registry.advance();
        CHECK(values.current() == 1);
    }
    CHECK(registry.size() == 0);
    CHECK(log.events == std::vector<std::string>{"create a0", "create a1", "destroy a1", "destroy a0"});
}

TEST_CASE("Resizing the registry grows and shrinks every collection") {
    FrameRegistry registry(2);
    Log log;
    auto values = makeLogged(registry, log, "a");
    log.events.clear();

    registry.resize(4);
    CHECK(values.size() == 4);
    CHECK(values[3] == 3);
    CHECK(log.events == std::vector<std::string>{"create a2", "create a3"});

    log.events.clear();
    registry.resize(1);
    CHECK(values.size() == 1);
    CHECK(log.events == std::vector<std::string>{"destroy a3", "destroy a2", "destroy a1"});

    // Resizing to the same size changes nothing
    log.events.clear();
    registry.resize(1);
    CHECK(log.events.empty());
}

TEST_CASE("Collections grow in creation order, so factories can use earlier collections") {
    FrameRegistry registry(1);
    PerFrame<int> base(registry, [](uint32_t frame) { return static_cast<int>(frame) * 10; });
    PerFrame<int> derived(registry, [&base](uint32_t frame) { return base[frame] + 1; });

    registry.resize(3);
    CHECK(derived[2] == 21);
}

TEST_CASE("Collections shrink in reverse creation order, so elements are destroyed before what they use") {
    FrameRegistry registry(3);
    Log log;
    auto base = makeLogged(registry, log, "base");
    auto derived = makeLogged(registry, log, "derived");

    log.events.clear();
    registry.resize(2);
    CHECK(log.events == std::vector<std::string>{"destroy derived2", "destroy base2"});

    log.events.clear();
    registry.resize(3);
    CHECK(log.events == std::vector<std::string>{"create base2", "create derived2"});
}

TEST_CASE("Moving a collection keeps it registered in its original position") {
    FrameRegistry registry(1);
    Log log;
    auto first = makeLogged(registry, log, "a");
    auto second = makeLogged(registry, log, "b");

    // Move the first collection somewhere else; it must still be resized before the second
    auto moved = std::make_unique<PerFrame<int>>(std::move(first));
    CHECK(first.empty());
    CHECK(registry.size() == 2);

    log.events.clear();
    registry.resize(2);
    CHECK(moved->size() == 2);
    CHECK(first.empty());
    CHECK(log.events == std::vector<std::string>{"create a1", "create b1"});

    // Move assignment releases the target's previous elements
    log.events.clear();
    second = std::move(*moved);
    CHECK(registry.size() == 1);
    CHECK(log.events == std::vector<std::string>{"destroy b1", "destroy b0"});
    CHECK(second.size() == 2);
}

TEST_CASE("Factories may create and destroy collections while resizing") {
    FrameRegistry registry(1);
    std::vector<std::unique_ptr<PerFrame<int>>> nested;
    std::unique_ptr<PerFrame<int>> victim;

    // Created after `outer`, so it would be resized after it
    PerFrame<int> outer(registry, [&](uint32_t frame) {
        if (frame == 1) {
            // Created mid-resize: already sized for the new frame count
            nested.push_back(std::make_unique<PerFrame<int>>(registry, [](uint32_t f) { return int(f); }));
            // Destroyed mid-resize: must not be resized afterwards
            victim.reset();
        }
        return static_cast<int>(frame);
    });
    victim = std::make_unique<PerFrame<int>>(registry, [](uint32_t f) { return int(f); });

    registry.resize(3);
    CHECK(outer.size() == 3);
    REQUIRE(nested.size() == 1);
    CHECK(nested[0]->size() == 3);
    CHECK_FALSE(victim);
}

TEST_CASE("A failing factory leaves nothing registered") {
    FrameRegistry registry(2);
    Log log;
    CHECK_THROWS_AS(PerFrame<int>(registry,
                                  [](uint32_t frame) -> int {
                                      if (frame == 1) throw std::runtime_error("boom");
                                      return 0;
                                  },
                                  [&log](int&) { log.events.push_back("destroy"); }),
                    std::runtime_error);
    CHECK(registry.size() == 0);
    // The element created before the failure was destroyed
    CHECK(log.events == std::vector<std::string>{"destroy"});
}

TEST_CASE("Collections may be created and destroyed on other threads") {
    FrameRegistry registry(2);
    std::atomic<bool> stop{false};
    std::atomic<int> created{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            while (!stop) {
                // Only creation and destruction may race with resizing, not use of the elements
                PerFrame<std::vector<int>> values(registry, [](uint32_t frame) {
                    return std::vector<int>(frame + 1, 7);
                });
                ++created;
            }
        });
    }

    for (uint32_t i = 0; i < 200; ++i) {
        registry.resize(1 + i % 4);
    }
    while (created < 100) {
        std::this_thread::yield();
    }
    stop = true;
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(registry.size() == 0);
}

TEST_CASE("FrameBinding returns a shared handle for every frame") {
    ResourceManager resources;
    FrameBinding<int> unbound;
    CHECK_FALSE(unbound.get(0));
    CHECK_FALSE(unbound.isPerFrame());

    auto value = resources.create<int>(5);
    FrameBinding<int> shared(value);
    CHECK_FALSE(shared.isPerFrame());
    CHECK(shared.get(0) == value);
    CHECK(shared.get(7) == value);
}

TEST_CASE("FrameBinding follows a per-frame collection as it is resized") {
    ResourceManager resources;
    FrameRegistry registry(2);
    {
        auto perFrame = resources.create<PerFrameHandles<int>>(registry, [&resources](uint32_t frame) {
            return resources.create<int>(static_cast<int>(frame) * 100);
        });
        FrameBinding<int> binding(perFrame);
        CHECK(binding.isPerFrame());
        CHECK(*binding.get(1) == 100);
        CHECK_FALSE(binding.get(2));

        registry.resize(3);
        CHECK(*binding.get(2) == 200);

        // The binding keeps the collection, and so its handles, alive
        perFrame.reset();
        CHECK(*binding.get(0) == 0);
        CHECK(resources.size() == 4);
    }
    CHECK(resources.size() == 0);
    CHECK(registry.size() == 0);
}
