#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>

/*
 Overview of Structure:

 ResourceManager:
    - Owns every managed resource as a std::unique_ptr
    - Hands out reference counted Handles to those resources
    - Frees a resource once the last Handle to it is destroyed

 Handle<T>:
    - Behaves like a std::unique_ptr<T> for access (*, ->, get, bool)
    - Copyable: each copy holds a reference to the underlying resource
    - Reference count is atomic, so handles may be copied & destroyed across threads
 */

class ResourceManager;

template<typename T>
class Handle;

namespace detail {

// Type-erased control block that the ResourceManager stores for each resource
class ResourceEntryBase {
public:
    virtual ~ResourceEntryBase() = default;

protected:
    explicit ResourceEntryBase(ResourceManager* manager) : manager_(manager) {}

private:
    template<typename T> friend class ::Handle;
    friend class ::ResourceManager;

    ResourceManager* manager_;
    std::atomic<size_t> refCount_{0};
};

template<typename T>
class ResourceEntry final : public ResourceEntryBase {
public:
    ResourceEntry(ResourceManager* manager, std::unique_ptr<T>&& resource)
    : ResourceEntryBase(manager), resource_(std::move(resource)) {}

    T* get() {
        return resource_.get();
    }

private:
    std::unique_ptr<T> resource_;
};

} // namespace detail

template<typename T>
class Handle {
public:
    Handle() = default;
    Handle(std::nullptr_t) {}

    Handle(const Handle& other) : entry_(other.entry_), ptr_(other.ptr_) {
        acquire();
    }

    Handle(Handle&& other) noexcept : entry_(other.entry_), ptr_(other.ptr_) {
        other.entry_ = nullptr;
        other.ptr_ = nullptr;
    }

    // Allow implicit upcasts, e.g. Handle<TutorialMaterial> -> Handle<Material>
    template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    Handle(const Handle<U>& other) : entry_(other.entry_), ptr_(other.ptr_) {
        acquire();
    }

    template<typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
    Handle(Handle<U>&& other) noexcept : entry_(other.entry_), ptr_(other.ptr_) {
        other.entry_ = nullptr;
        other.ptr_ = nullptr;
    }

    ~Handle() {
        release();
    }

    Handle& operator=(Handle other) noexcept {
        swap(other);
        return *this;
    }

    void swap(Handle& other) noexcept {
        std::swap(entry_, other.entry_);
        std::swap(ptr_, other.ptr_);
    }

    void reset() {
        release();
        entry_ = nullptr;
        ptr_ = nullptr;
    }

    T* get() const {
        return ptr_;
    }

    T& operator*() const {
        assert(ptr_ != nullptr && "Dereferencing an empty Handle");
        return *ptr_;
    }

    T* operator->() const {
        assert(ptr_ != nullptr && "Dereferencing an empty Handle");
        return ptr_;
    }

    explicit operator bool() const {
        return ptr_ != nullptr;
    }

    // Number of live handles to this resource (0 for an empty handle)
    size_t useCount() const {
        return entry_ ? entry_->refCount_.load(std::memory_order_relaxed) : 0;
    }

    friend bool operator==(const Handle& lhs, const Handle& rhs) {
        return lhs.get() == rhs.get();
    }

    friend bool operator==(const Handle& lhs, std::nullptr_t) {
        return lhs.get() == nullptr;
    }

private:
    template<typename U> friend class Handle;
    friend class ResourceManager;

    template<typename U, typename V>
    friend Handle<U> static_handle_cast(const Handle<V>& handle);

    Handle(detail::ResourceEntryBase* entry, T* ptr) : entry_(entry), ptr_(ptr) {
        acquire();
    }

    void acquire() {
        if (entry_) {
            entry_->refCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Defined below ResourceManager since it calls back into the manager
    void release();

    detail::ResourceEntryBase* entry_ = nullptr;
    T* ptr_ = nullptr;
};

// Equivalent of std::static_pointer_cast for Handles (e.g. Handle<Descriptor> -> Handle<UniformBufferDescriptor>)
template<typename U, typename V>
Handle<U> static_handle_cast(const Handle<V>& handle) {
    return Handle<U>(handle.entry_, static_cast<U*>(handle.ptr_));
}

class ResourceManager {
public:
    ResourceManager() = default;

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    ~ResourceManager() {
        // Every handle must be destroyed before the manager that issued it
        assert(entries_.empty() && "ResourceManager destroyed while Handles are still alive");
    }

    // Construct a new resource in place and return the first handle to it
    template<typename T, typename... Args>
    Handle<T> create(Args&&... args) {
        return adopt(std::make_unique<T>(std::forward<Args>(args)...));
    }

    // Take ownership of an existing resource and return the first handle to it
    template<typename T>
    Handle<T> adopt(std::unique_ptr<T>&& resource) {
        assert(resource != nullptr && "Cannot adopt an empty resource");

        auto entry = std::make_unique<detail::ResourceEntry<T>>(this, std::move(resource));
        auto* rawEntry = entry.get();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entries_.emplace(rawEntry, std::move(entry));
        }
        return Handle<T>(rawEntry, rawEntry->get());
    }

    // Number of resources currently owned by the manager
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

private:
    template<typename T> friend class Handle;

    // Called by a Handle once the last reference to a resource is dropped
    void release(detail::ResourceEntryBase* entry) {
        std::unique_ptr<detail::ResourceEntryBase> owned;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = entries_.find(entry);
            assert(it != entries_.end() && "Releasing a resource not owned by this ResourceManager");
            owned = std::move(it->second);
            entries_.erase(it);
        }
        // Destroy outside of the lock: resources may themselves hold handles
        // whose release re-enters the manager
        owned.reset();
    }

    std::mutex mutex_;
    std::unordered_map<detail::ResourceEntryBase*, std::unique_ptr<detail::ResourceEntryBase>> entries_;
};

template<typename T>
void Handle<T>::release() {
    if (entry_ && entry_->refCount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        entry_->manager_->release(entry_);
    }
}
