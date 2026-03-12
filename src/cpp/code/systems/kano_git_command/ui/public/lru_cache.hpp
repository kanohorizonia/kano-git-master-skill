#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <unordered_map>
#include <utility>

namespace kano::git::commands {

template <typename K, typename V>
class LruCache {
public:
    explicit LruCache(const std::size_t InCapacity) : capacity_(InCapacity) {}

    auto get(const K& key) -> V* {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return nullptr;
        }
        touch(it);
        return &it->second.value;
    }

    auto put(const K& key, V value) -> void {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second.value = std::move(value);
            touch(it);
            return;
        }

        if (capacity_ == 0) {
            return;
        }

        order_.push_front(key);
        map_.emplace(key, Node{std::move(value), order_.begin()});
        evict_if_needed();
    }

    auto contains(const K& key) const -> bool {
        return map_.find(key) != map_.end();
    }

    auto erase(const K& key) -> void {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return;
        }
        order_.erase(it->second.it);
        map_.erase(it);
    }

    auto clear() -> void {
        order_.clear();
        map_.clear();
    }

    auto size() const -> std::size_t {
        return map_.size();
    }

    auto erase_if(const std::function<bool(const K&, const V&)>& predicate) -> void {
        for (auto it = map_.begin(); it != map_.end(); ) {
            if (predicate(it->first, it->second.value)) {
                order_.erase(it->second.it);
                it = map_.erase(it);
                continue;
            }
            ++it;
        }
    }

private:
    struct Node {
        V value;
        typename std::list<K>::iterator it;
    };

    auto touch(typename std::unordered_map<K, Node>::iterator it) -> void {
        order_.splice(order_.begin(), order_, it->second.it);
        it->second.it = order_.begin();
    }

    auto evict_if_needed() -> void {
        while (map_.size() > capacity_) {
            const auto& key = order_.back();
            map_.erase(key);
            order_.pop_back();
        }
    }

    std::size_t capacity_ = 0;
    std::list<K> order_;
    std::unordered_map<K, Node> map_;
};

} // namespace kano::git::commands
