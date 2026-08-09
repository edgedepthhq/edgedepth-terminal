#pragma once
#include <vector>
#include <algorithm>
#include <utility>
#include <functional>
#include <cstddef>

template<typename K, typename V, typename Compare = std::less<K>>
class FlatMap {
public:
    using value_type = std::pair<K, V>;
    using iterator = typename std::vector<value_type>::iterator;
    using const_iterator = typename std::vector<value_type>::const_iterator;

private:
    std::vector<value_type> data_;
    Compare comp_;

public:
    explicit FlatMap(Compare comp = Compare()) : comp_(comp) {
        data_.reserve(1000);  // ✅ Increased from 200 for orderbooks
    }

    iterator lower_bound(const K& key) {
        size_t count = data_.size();
        size_t first = 0;
        while (count > 0) {
            size_t step = count / 2;
            size_t mid = first + step;
            // Tell CPU to prefetch the next elements we'll check
            if (step > 8) {  // Only worth it for large searches
                __builtin_prefetch(&data_[first + step/2], 0, 1);
                __builtin_prefetch(&data_[mid + step/2], 0, 1);
            }
            if (comp_(data_[mid].first, key)) {
                first = mid + 1;
                count -= step + 1;
            } else {
                count = step;
            }
        }
        return data_.begin() + first;
    }

    const_iterator lower_bound(const K& key) const {
        return std::lower_bound(data_.begin(), data_.end(), key,
            [this](const value_type& p, const K& k) { return comp_(p.first, k); });
    }
    // iterator lower_bound(const K& key) {
    //     return std::lower_bound(data_.begin(), data_.end(), key,
    //         [this](const value_type& p, const K& k) {
    //             return comp_(p.first, k);
    //         });
    // }
    //
    // const_iterator lower_bound(const K& key) const {
    //     return std::lower_bound(data_.begin(), data_.end(), key,
    //         [this](const value_type& p, const K& k) {
    //             return comp_(p.first, k);
    //         });
    // }

    // Insert or update (most common operation)
    void insert_or_assign(const K& key, const V& value) {
        auto it = lower_bound(key);
        if (it != data_.end() && it->first == key) {
            it->second = value;  // Update existing
        } else {
            data_.insert(it, {key, value});  // Insert new
        }
    }

    // Move semantics version (avoid copy)
    void insert_or_assign(const K& key, V&& value) {
        auto it = lower_bound(key);
        if (it != data_.end() && it->first == key) {
            it->second = std::move(value);
        } else {
            data_.emplace(it, key, std::forward<V>(value));
        }
    }

    template<typename... Args>
    std::pair<iterator, bool> try_emplace(const K& key, Args&&... args) {
        auto it = lower_bound(key);
        if (it != data_.end() && it->first == key) {
            return {it, false};  // Already exists
        }
        // Insert new element
        V value(std::forward<Args>(args)...);
        auto result = data_.insert(it, {key, std::move(value)});
        return {result, true};
    }

    // Emplace (construct in-place)
    template<typename... Args>
    void emplace(const K& key, Args&&... args) {
        auto it = lower_bound(key);
        if (it != data_.end() && it->first == key) {
            it->second = V(std::forward<Args>(args)...);
        } else {
            data_.emplace(it, key, V(std::forward<Args>(args)...));
        }
    }

    template<typename InputIt>
    void insert_sorted(InputIt first, InputIt last) {
        data_.clear();
        data_.reserve(std::distance(first, last));
        data_.insert(data_.end(), first, last);
        // Data is already sorted from protobuf, no need to sort again!
    }

    // Erase by key
    bool erase(const K& key) {
        auto it = lower_bound(key);
        if (it != data_.end() && it->first == key) {
            data_.erase(it);
            return true;
        }
        return false;
    }

    // Erase by iterator (for range erasure)
    iterator erase(iterator it) {
        return data_.erase(it);
    }

    // Erase range
    iterator erase(iterator first, iterator last) {
        return data_.erase(first, last);
    }

    // Find (returns iterator, more flexible than pointer)
    iterator find(const K& key) {
        auto it = lower_bound(key);
        if (it != data_.end() && it->first == key) {
            return it;
        }
        return data_.end();
    }

    const_iterator find(const K& key) const {
        auto it = lower_bound(key);
        if (it != data_.end() && it->first == key) {
            return it;
        }
        return data_.end();
    }

    // Const find (returns pointer for backwards compatibility)
    const V* find_ptr(const K& key) const {
        auto it = lower_bound(key);
        if (it != data_.end() && it->first == key) {
            return &it->second;
        }
        return nullptr;
    }

    // Contains check
    bool contains(const K& key) const {
        auto it = lower_bound(key);
        return it != data_.end() && it->first == key;
    }

    // Operator[] (for map-like access)
    V& operator[](const K& key) {
        auto it = lower_bound(key);
        if (it != data_.end() && it->first == key) {
            return it->second;
        }
        // Insert with default-constructed value
        it = data_.insert(it, {key, V()});  // Use V() instead of V{}
        return it->second;
    }
    // At (throws if not found)
    V& at(const K& key) {
        auto it = lower_bound(key);
        if (it != data_.end() && it->first == key) {
            return it->second;
        }
        throw std::out_of_range("FlatMap::at: key not found");
    }

    const V& at(const K& key) const {
        auto it = lower_bound(key);
        if (it != data_.end() && it->first == key) {
            return it->second;
        }
        throw std::out_of_range("FlatMap::at: key not found");
    }

    // Remove if (with predicate)
    void remove_if(std::function<bool(const value_type&)> pred) {
        data_.erase(
            std::remove_if(data_.begin(), data_.end(), pred),
            data_.end()
        );
    }

    // Batch operations
    void reserve(size_t capacity) {
        data_.reserve(capacity);
    }

    void shrink_to_fit() {
        data_.shrink_to_fit();
    }

    void clear() {
        data_.clear();
    }

    // Size queries
    size_t size() const { return data_.size(); }
    bool empty() const { return data_.empty(); }
    size_t capacity() const { return data_.capacity(); }

    // Direct data access (for efficient iteration)
    std::vector<value_type>& data() { return data_; }
    const std::vector<value_type>& data() const { return data_; }

    // Iterators
    iterator begin() { return data_.begin(); }
    iterator end() { return data_.end(); }
    const_iterator begin() const { return data_.begin(); }
    const_iterator end() const { return data_.end(); }
    const_iterator cbegin() const { return data_.cbegin(); }
    const_iterator cend() const { return data_.cend(); }

    // Reverse iterators (useful for bids)
    auto rbegin() { return data_.rbegin(); }
    auto rend() { return data_.rend(); }
    auto rbegin() const { return data_.rbegin(); }
    auto rend() const { return data_.rend(); }

    // ✅ Front/back access (useful for best bid/ask)
    value_type& front() { return data_.front(); }
    const value_type& front() const { return data_.front(); }
    value_type& back() { return data_.back(); }
    const value_type& back() const { return data_.back(); }
};