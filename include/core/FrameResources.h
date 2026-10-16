#pragma once

#include "core/Contract.h"
#include "core/ResourceManager.h"
#include "core/Result.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <variant>
#include <vector>

/*
 Overview of Structure:

 FrameRegistry:
    - Holds the number of frames in flight and the index of the current frame
    - Tracks every PerFrame collection created from it, and resizes them all when the number of
      frames in flight changes
    - Owned by the VulkanContext (see VulkanContext::framesInFlight / setFramesInFlight)

 PerFrame<T>:
    - One T per frame in flight, created on demand by a factory and destroyed when no longer
      needed (when the number of frames shrinks, or the collection is destroyed)
    - Factories may fail (returning a Result<T>): creating the collection then fails, and so does
      growing the number of frames in flight, which leaves every collection at its previous size

 FrameBinding<T>:
    - A Handle<T> that is either shared by every frame (for static data) or looked up in a
      PerFrame collection of handles (for data that changes from frame to frame)

 Thread safety:
    - Creating, moving and destroying PerFrame collections is thread safe: collections may be
      owned by resources whose last handle is released on any thread.
    - Resizing (FrameRegistry::resize) is serialized with the above, but must not race with use of
      the collections' elements. VulkanContext only resizes between frames, once the device is idle.
    - Accessing a collection's elements is not synchronized; its owner is responsible for that.
 */

class FrameRegistry;

namespace detail {

// Type-erased view of a PerFrame collection, as tracked by the FrameRegistry
class PerFrameBase {
public:
    virtual ~PerFrameBase() = default;

protected:
    friend class ::FrameRegistry;
    // Grows or shrinks to count elements. Shrinking can't fail; growing stops at the first failure.
    virtual Result<void> resizeFrames(uint32_t count) = 0;
};

} // namespace detail

class FrameRegistry {
public:
    explicit FrameRegistry(uint32_t framesInFlight = 2) : framesInFlight_(checked(framesInFlight)) {}

    FrameRegistry(const FrameRegistry&) = delete;
    FrameRegistry& operator=(const FrameRegistry&) = delete;

    ~FrameRegistry() {
        // Every PerFrame collection must be destroyed before the registry it was created from
        assert(collections_.empty() && "PerFrame collections outlived their FrameRegistry");
    }

    uint32_t framesInFlight() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return framesInFlight_;
    }

    uint32_t currentFrame() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return currentFrame_;
    }

    // Moves on to the next frame, wrapping around after the last frame in flight
    void advance() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        currentFrame_ = (currentFrame_ + 1) % framesInFlight_;
    }

    // Changes the number of frames in flight, resizing every registered collection. When growing,
    // collections are resized in the order they were created, so a factory can use the new
    // elements of collections created before it. When shrinking, the order is reversed, so
    // elements are destroyed before the elements of earlier collections they may depend on (just
    // as destroying the collections themselves would). The current frame restarts at 0.
    //
    // If a factory fails while growing, every collection is shrunk back, the number of frames and
    // the current frame are left as they were, and the factory's error is returned.
    //
    // Elements of the slots being created or destroyed must not be in use.
    Result<void> resize(uint32_t framesInFlight) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        const uint32_t previous = framesInFlight_;
        const uint32_t previousFrame = currentFrame_;
        const bool shrinking = checked(framesInFlight) < framesInFlight_;
        framesInFlight_ = framesInFlight;
        currentFrame_ = 0;

        auto result = resizeAll(shrinking);
        if (!result) {
            framesInFlight_ = previous;
            currentFrame_ = previousFrame;
            // Shrinking back can't fail
            resizeAll(true).value();
        }
        return result;
    }

    // Number of registered collections
    size_t size() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return collections_.size();
    }

private:
    template<typename T>
    friend class PerFrame;

    static uint32_t checked(uint32_t framesInFlight) {
        if (framesInFlight == 0) {
            contractViolation("At least one frame must be in flight");
        }
        return framesInFlight;
    }

    // Resizes every collection to framesInFlight_, last created first when shrinking. Stops at the
    // first failure.
    Result<void> resizeAll(bool shrinking) {
        // The lock is recursive since factories may create (and so register) collections of their
        // own, and destroyed elements may destroy collections. Collections registered meanwhile
        // are already the new size; collections destroyed meanwhile are skipped.
        std::vector<uint64_t> ids;
        ids.reserve(collections_.size());
        for (auto& [id, collection] : collections_) {
            ids.push_back(id);
        }
        if (shrinking) {
            std::reverse(ids.begin(), ids.end());
        }
        for (uint64_t id : ids) {
            auto it = collections_.find(id);
            if (it != collections_.end()) {
                TRY(it->second->resizeFrames(framesInFlight_));
            }
        }
        return {};
    }

    // Registers a collection and creates its initial elements, returning its id. Both happen
    // under the lock, so a concurrent resize never sees a partially created collection. If
    // creating the elements fails the collection is unregistered (the caller destroys any
    // elements created).
    Result<uint64_t> add(detail::PerFrameBase* collection) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        uint64_t id = nextId_++;
        collections_.emplace(id, collection);
        if (auto result = collection->resizeFrames(framesInFlight_); !result) {
            collections_.erase(id);
            return std::move(result).error();
        }
        return id;
    }

    // Moves a registered collection (by calling move) and points its registration at the
    // moved-to collection, keeping its place in the order
    template<typename Move>
    void move(uint64_t id, detail::PerFrameBase* to, Move&& move) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        move();
        collections_.at(id) = to;
    }

    void remove(uint64_t id) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        collections_.erase(id);
    }

    std::recursive_mutex mutex_;
    uint32_t framesInFlight_;
    uint32_t currentFrame_ = 0;
    uint64_t nextId_ = 0;
    // Ordered by id, i.e. by creation
    std::map<uint64_t, detail::PerFrameBase*> collections_;
};

class VulkanContext;

template<typename T>
class PerFrame;

// One T per frame in flight. Elements are created by a factory, called with the frame index,
// when the collection is created and whenever the number of frames in flight grows. They are
// destroyed (last first) when it shrinks and when the collection is destroyed, after calling the
// optional destroy function on each.
//
// Factories that can't fail return T and are passed to the constructors. Factories that can fail
// return Result<T> and are passed to create(), which fails if the factory does; when the number
// of frames in flight grows, their failures fail the resize (see FrameRegistry::resize).
//
// The factory may refer to collections created before this one, since those are resized first.
// It (and destroy) may capture pointers to the collection's owner, so owners that do so must not
// be moved.
template<typename T>
class PerFrame final : public detail::PerFrameBase {
public:
    using Create = std::function<T(uint32_t frameIndex)>;
    using TryCreate = std::function<Result<T>(uint32_t frameIndex)>;
    using Destroy = std::function<void(T&)>;

    // An empty collection, not registered with any FrameRegistry
    PerFrame() = default;

    // With a factory that can't fail
    PerFrame(FrameRegistry& registry, Create create, Destroy destroy = {})
    : PerFrame(PerFrame::create(registry, wrap(std::move(create)), std::move(destroy)).value()) {}

    // Registers with the context's FrameRegistry (defined in VulkanContext.h)
    PerFrame(VulkanContext& context, Create create, Destroy destroy = {});

    // With a factory that can fail: fails (destroying any elements created) if it does
    static Result<PerFrame> create(FrameRegistry& registry, TryCreate create, Destroy destroy = {}) {
        PerFrame collection;
        collection.create_ = std::move(create);
        collection.destroy_ = std::move(destroy);
        auto id = registry.add(&collection);
        if (!id) {
            collection.clear();
            return std::move(id).error();
        }
        // Registered at its address here; moving it out updates the registration
        collection.registry_ = &registry;
        collection.id_ = *id;
        return collection;
    }

    // Registers with the context's FrameRegistry (defined in VulkanContext.h)
    static Result<PerFrame> create(VulkanContext& context, TryCreate create, Destroy destroy = {});

    // A collection owned by the context's ResourceManager, e.g. for a FrameBinding (defined in
    // VulkanContext.h)
    static Result<Handle<PerFrame>> createManaged(VulkanContext& context, TryCreate create, Destroy destroy = {});

    PerFrame(const PerFrame&) = delete;
    PerFrame& operator=(const PerFrame&) = delete;

    PerFrame(PerFrame&& other) noexcept {
        takeFrom(other);
    }

    PerFrame& operator=(PerFrame&& other) noexcept {
        if (this != &other) {
            release();
            takeFrom(other);
        }
        return *this;
    }

    ~PerFrame() override {
        release();
    }

    T& operator[](uint32_t frameIndex) {
        return elements_.at(frameIndex);
    }

    // The element for the registry's current frame
    T& current() {
        assert(registry_ && "Empty PerFrame collections have no current element");
        return elements_.at(registry_->currentFrame());
    }

    uint32_t size() const {
        return static_cast<uint32_t>(elements_.size());
    }

    bool empty() const {
        return elements_.empty();
    }

    auto begin() { return elements_.begin(); }
    auto end() { return elements_.end(); }

protected:
    Result<void> resizeFrames(uint32_t count) override {
        while (elements_.size() > count) {
            destroyLast();
        }
        while (elements_.size() < count) {
            TRY_ASSIGN(T element, create_(static_cast<uint32_t>(elements_.size())));
            elements_.push_back(std::move(element));
        }
        return {};
    }

private:
    static TryCreate wrap(Create create) {
        return [create = std::move(create)](uint32_t frameIndex) -> Result<T> { return create(frameIndex); };
    }

    void destroyLast() {
        if (destroy_) {
            destroy_(elements_.back());
        }
        elements_.pop_back();
    }

    void clear() {
        while (!elements_.empty()) {
            destroyLast();
        }
    }

    void takeFrom(PerFrame& other) {
        auto move = [this, &other] {
            registry_ = std::exchange(other.registry_, nullptr);
            id_ = other.id_;
            create_ = std::move(other.create_);
            destroy_ = std::move(other.destroy_);
            elements_ = std::move(other.elements_);
            other.elements_.clear();
        };
        if (other.registry_) {
            other.registry_->move(other.id_, this, move);
        } else {
            move();
        }
    }

    void release() {
        if (registry_) {
            // Unregister first, so the registry never resizes a partially destroyed collection
            registry_->remove(id_);
            registry_ = nullptr;
        }
        clear();
    }

    FrameRegistry* registry_ = nullptr;
    uint64_t id_ = 0;
    TryCreate create_;
    Destroy destroy_;
    std::vector<T> elements_;
};

// A collection of handles, one per frame in flight
template<typename T>
using PerFrameHandles = PerFrame<Handle<T>>;

// A Handle<T> for each frame: either one handle shared by every frame, for data that does not
// change between frames, or a handle per frame looked up in a PerFrameHandles<T> collection.
// The binding keeps the handles (and collection) alive.
template<typename T>
class FrameBinding {
public:
    // Unbound: get() returns an empty handle
    FrameBinding() = default;
    FrameBinding(std::nullptr_t) {}

    // The same handle for every frame
    FrameBinding(Handle<T> shared) : source_(std::move(shared)) {}

    template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    FrameBinding(Handle<U> shared) : source_(Handle<T>(std::move(shared))) {}

    // A handle per frame, read from the collection when requested so the binding follows the
    // collection as it is resized
    FrameBinding(Handle<PerFrameHandles<T>> perFrame) : source_(std::move(perFrame)) {}

    bool isPerFrame() const {
        return std::holds_alternative<Handle<PerFrameHandles<T>>>(source_);
    }

    // The per-frame collection (empty unless isPerFrame())
    Handle<PerFrameHandles<T>> collection() const {
        auto* perFrame = std::get_if<Handle<PerFrameHandles<T>>>(&source_);
        return perFrame ? *perFrame : nullptr;
    }

    // The shared handle (empty if isPerFrame() or unbound)
    Handle<T> shared() const {
        auto* shared = std::get_if<Handle<T>>(&source_);
        return shared ? *shared : nullptr;
    }

    // The handle for a frame. Empty if unbound, or if a per-frame collection has no such frame.
    Handle<T> get(uint32_t frameIndex) const {
        if (auto* shared = std::get_if<Handle<T>>(&source_)) {
            return *shared;
        }
        auto& perFrame = std::get<Handle<PerFrameHandles<T>>>(source_);
        if (!perFrame || frameIndex >= perFrame->size()) {
            return nullptr;
        }
        return (*perFrame)[frameIndex];
    }

private:
    std::variant<Handle<T>, Handle<PerFrameHandles<T>>> source_;
};
