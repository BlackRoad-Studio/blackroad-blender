/* SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <functional>
#include <list>
#include <memory>
#include <mutex>

CCL_NAMESPACE_BEGIN

/* Cache limiter
 *
 * System to ensure a number of cached resources do not exceed a specified maximum number of
 * items. Cached handles hold a reference to the resources, and the limiter will delete
 * resources if the maximum is exceeded and there no current users of the resource. */

template<typename T> class CacheLimiter;
template<typename T> class CacheHandleUser;

/* Cache Handle
 *
 * Holds a reference to cached resource, which may get deleted if the maximum number
 * of items is exceeded. */
template<typename T> class CacheHandle {
 public:
  CacheHandleUser<T> acquire(CacheLimiter<T> &limiter,
                             const std::function<std::unique_ptr<T>()> &creator)
  {
    if (!internal_) {
      std::scoped_lock lock(internal_mutex_);
      if (!internal_) {
        internal_ = std::make_shared<Internal>(limiter);
      }
    }
    return internal_->acquire(creator);
  }

 private:
  friend class CacheLimiter<T>;
  friend class CacheHandleUser<T>;

  class Internal : public std::enable_shared_from_this<Internal> {
   public:
    Internal(CacheLimiter<T> &limiter) : limiter_(limiter) {}

    ~Internal()
    {
      limiter_.unregister(this);
      delete_resource();
    }

    CacheHandleUser<T> acquire(std::function<std::unique_ptr<T>()> creator)
    {
      limiter_.register_and_add_user(*this);

      std::lock_guard<std::mutex> lock(resource_mutex_);
      if (!resource_) {
        resource_ = creator();
      }
      return CacheHandleUser<T>(*this);
    }

    void delete_resource()
    {
      std::lock_guard<std::mutex> lock(resource_mutex_);
      resource_.reset();
    }

    CacheLimiter<T> &limiter_;
    std::unique_ptr<T> resource_;
    std::mutex resource_mutex_;

    int user_count_ = 0;
    bool in_lru_list_ = false;
    typename std::list<typename CacheHandle<T>::Internal *>::iterator lru_iterator_;
  };

  /* Shared pointer for lifetime extension in cache eviction. */
  std::mutex internal_mutex_;
  std::shared_ptr<Internal> internal_;
};

/* Cache Handle User
 *
 * Ensures a cache resource remains is not deleted as long as it exists. */
template<typename T> class CacheHandleUser {
 public:
  ~CacheHandleUser()
  {
    handle_.limiter_.remove_user(handle_);
  }

  CacheHandleUser(const CacheHandleUser &) = delete;
  CacheHandleUser &operator=(const CacheHandleUser &) = delete;

  const std::unique_ptr<T> &get() const
  {
    return handle_.resource_;
  }

 private:
  friend class CacheHandle<T>;
  CacheHandleUser(typename CacheHandle<T>::Internal &source) : handle_(source) {}

  typename CacheHandle<T>::Internal &handle_;
};

/* Cache limiter
 *
 * Cache handles are register with this global limiter. */
template<typename T> class CacheLimiter {
 public:
  explicit CacheLimiter(size_t max_items) : max_items_(max_items) {}

  CacheLimiter(const CacheLimiter &) = delete;
  CacheLimiter &operator=(const CacheLimiter &) = delete;

 private:
  friend class CacheHandle<T>;
  friend class CacheHandleUser<T>;

  /* When handle is deleted. */
  void unregister(typename CacheHandle<T>::Internal *handle)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle->in_lru_list_) {
      lru_list_.erase(handle->lru_iterator_);
      handle->in_lru_list_ = false;
    }
  }

  /* When handle user is acquired. */
  void register_and_add_user(typename CacheHandle<T>::Internal &handle)
  {
    std::unique_lock<std::mutex> lock(mutex_);

    /* (Re)insert handle at the front of the LRU list. */
    handle.user_count_++;
    if (handle.in_lru_list_) {
      lru_list_.erase(handle.lru_iterator_);
    }
    lru_list_.push_front(&handle);
    handle.lru_iterator_ = lru_list_.begin();
    handle.in_lru_list_ = true;

    /* Evict other handles if needed. */
    while (lru_list_.size() > max_items_) {
      typename CacheHandle<T>::Internal *handle_to_evict = nullptr;

      /* From the back of the LRU list, find an item with no users. */
      auto it = lru_list_.rbegin();
      while (it != lru_list_.rend()) {
        typename CacheHandle<T>::Internal *candidate = *it;
        if (candidate->user_count_ <= 0) {
          handle_to_evict = candidate;
          break;
        }
        ++it;
      }

      if (!handle_to_evict) {
        break;
      }

      lru_list_.erase(handle_to_evict->lru_iterator_);
      handle_to_evict->in_lru_list_ = false;

      /* Get a shared pointer to ensure this does not get deleted
       * before we have unloaded the resource. */
      auto handle_to_evict_ptr = handle_to_evict->shared_from_this();

      lock.unlock();

      /* Unload resource while unlocked, to avoid holding lock
       * too long if deletion is expensive. */
      handle_to_evict_ptr->delete_resource();
      handle_to_evict_ptr.reset();

      lock.lock();
    }
  }

  /* When handle user is deleted. */
  void remove_user(typename CacheHandle<T>::Internal &handle)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    handle.user_count_--;
  }

  size_t max_items_;
  std::mutex mutex_;
  std::list<typename CacheHandle<T>::Internal *> lru_list_;
};

CCL_NAMESPACE_END
