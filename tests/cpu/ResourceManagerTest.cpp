#include "ResourceManager.h"

#include <doctest/doctest.h>

#include <thread>
#include <vector>

namespace {

// Counts live instances so tests can observe when the manager frees a resource
struct Tracked {
    static inline int alive = 0;

    Tracked() { ++alive; }
    virtual ~Tracked() { --alive; }

    virtual int id() const { return 1; }
};

struct DerivedTracked : Tracked {
    explicit DerivedTracked(int value = 2) : value(value) {}
    int id() const override { return value; }
    int value;
};

// A resource that itself holds a handle, so freeing it re-enters the manager
struct Holder : Tracked {
    explicit Holder(Handle<Tracked> child) : child(std::move(child)) {}
    Handle<Tracked> child;
};

struct TrackedFixture {
    TrackedFixture() { Tracked::alive = 0; }
    ~TrackedFixture() { CHECK(Tracked::alive == 0); }
};

} // namespace

TEST_CASE_FIXTURE(TrackedFixture, "ResourceManager::create constructs a resource owned by the manager") {
    ResourceManager manager;

    auto handle = manager.create<DerivedTracked>(7);

    REQUIRE(handle);
    CHECK(handle->id() == 7);
    CHECK((*handle).value == 7);
    CHECK(handle.get()->value == 7);
    CHECK(handle.useCount() == 1);
    CHECK(manager.size() == 1);
    CHECK(Tracked::alive == 1);
}

TEST_CASE_FIXTURE(TrackedFixture, "ResourceManager::adopt takes ownership of an existing unique_ptr") {
    ResourceManager manager;

    auto owned = std::make_unique<DerivedTracked>(3);
    auto* raw = owned.get();
    auto handle = manager.adopt(std::move(owned));

    CHECK(handle.get() == raw);
    CHECK(manager.size() == 1);
}

TEST_CASE_FIXTURE(TrackedFixture, "Resources are freed when the last handle is destroyed") {
    ResourceManager manager;

    auto first = manager.create<Tracked>();
    {
        auto second = first;
        CHECK(first.useCount() == 2);
        CHECK(second.useCount() == 2);
        CHECK(second == first);
    }
    CHECK(first.useCount() == 1);
    CHECK(Tracked::alive == 1);

    first.reset();
    CHECK_FALSE(first);
    CHECK(first.useCount() == 0);
    CHECK(Tracked::alive == 0);
    CHECK(manager.size() == 0);
}

TEST_CASE_FIXTURE(TrackedFixture, "Handles are independent across resources") {
    ResourceManager manager;

    auto a = manager.create<Tracked>();
    auto b = manager.create<Tracked>();
    CHECK(manager.size() == 2);
    CHECK_FALSE(a == b);

    a.reset();
    CHECK(manager.size() == 1);
    CHECK(b.useCount() == 1);
}

TEST_CASE_FIXTURE(TrackedFixture, "Moving a handle transfers its reference without changing the count") {
    ResourceManager manager;

    auto source = manager.create<Tracked>();
    auto* raw = source.get();

    SUBCASE("move construction") {
        Handle<Tracked> destination = std::move(source);
        CHECK_FALSE(source);
        CHECK(destination.get() == raw);
        CHECK(destination.useCount() == 1);
        CHECK(manager.size() == 1);
    }
    SUBCASE("move assignment") {
        Handle<Tracked> destination;
        destination = std::move(source);
        CHECK_FALSE(source);
        CHECK(destination.get() == raw);
        CHECK(destination.useCount() == 1);
        CHECK(manager.size() == 1);
    }
}

TEST_CASE_FIXTURE(TrackedFixture, "Assigning over a handle releases its previous resource") {
    ResourceManager manager;

    auto a = manager.create<Tracked>();
    auto b = manager.create<Tracked>();

    a = b;
    CHECK(manager.size() == 1);
    CHECK(a == b);
    CHECK(b.useCount() == 2);

    a = nullptr;
    CHECK(a == nullptr);
    CHECK(b.useCount() == 1);

    // Self-assignment must not free the resource
    auto& alias = b;
    b = alias;
    CHECK(b.useCount() == 1);
    CHECK(Tracked::alive == 1);
}

TEST_CASE_FIXTURE(TrackedFixture, "Handle<Derived> converts to Handle<Base> and shares the reference count") {
    ResourceManager manager;

    auto derived = manager.create<DerivedTracked>(5);

    Handle<Tracked> base = derived;
    CHECK(base->id() == 5);
    CHECK(derived.useCount() == 2);

    Handle<Tracked> moved = std::move(derived);
    CHECK_FALSE(derived);
    CHECK(base.useCount() == 2);
}

TEST_CASE_FIXTURE(TrackedFixture, "static_handle_cast downcasts and shares the reference count") {
    ResourceManager manager;

    Handle<Tracked> base = manager.create<DerivedTracked>(9);
    auto derived = static_handle_cast<DerivedTracked>(base);

    CHECK(derived->value == 9);
    CHECK(base.useCount() == 2);

    base.reset();
    CHECK(Tracked::alive == 1);
    derived.reset();
    CHECK(Tracked::alive == 0);
}

TEST_CASE_FIXTURE(TrackedFixture, "Freeing a resource that holds handles releases them too") {
    ResourceManager manager;
    {
        auto holder = manager.create<Holder>(manager.create<Tracked>());
        CHECK(manager.size() == 2);
        CHECK(Tracked::alive == 2);
    }
    CHECK(manager.size() == 0);
    CHECK(Tracked::alive == 0);
}

TEST_CASE_FIXTURE(TrackedFixture, "Default and null handles are empty") {
    Handle<Tracked> empty;
    Handle<Tracked> null = nullptr;

    CHECK_FALSE(empty);
    CHECK(empty == nullptr);
    CHECK(null.get() == nullptr);
    CHECK(empty.useCount() == 0);

    // Copying and resetting empty handles is a no-op
    auto copy = empty;
    copy.reset();
    CHECK_FALSE(copy);
}

TEST_CASE_FIXTURE(TrackedFixture, "Handles can be copied and destroyed concurrently") {
    ResourceManager manager;
    auto handle = manager.create<Tracked>();

    constexpr int threadCount = 8;
    constexpr int iterations = 100000;

    std::vector<std::thread> threads;
    for (int t = 0; t < threadCount; ++t) {
        threads.emplace_back([&handle]() {
            for (int i = 0; i < iterations; ++i) {
                Handle<Tracked> copy = handle;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    CHECK(handle.useCount() == 1);
    CHECK(manager.size() == 1);
}

TEST_CASE_FIXTURE(TrackedFixture, "The last handle may be released on another thread") {
    ResourceManager manager;

    std::vector<Handle<Tracked>> handles;
    for (int i = 0; i < 64; ++i) {
        handles.push_back(manager.create<Tracked>());
    }

    std::vector<std::thread> threads;
    for (auto& handle : handles) {
        threads.emplace_back([moved = std::move(handle)]() mutable {
            moved.reset();
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    CHECK(manager.size() == 0);
}
