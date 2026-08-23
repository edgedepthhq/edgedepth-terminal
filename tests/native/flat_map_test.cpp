// ═══════════════════════════════════════════════════════════════════════════════
// flat_map_test.cpp - native pin on the sorted container the order book is made of.
//
// Terminal::Orderbook is two FlatMaps: asks ascending, bids descending via
// std::greater. Every depth delta on the wire is an insert, an assign or an
// erase into one of them, and every widget read walks them in order. So the
// container's sort invariant IS the book's price ordering, and front() IS the
// best bid / best ask.
//
// The part that earns a test is the hand-rolled binary search. FlatMap ships two
// lower_bound overloads: the const one delegates to std::lower_bound, the
// mutable one is an open-coded branch-and-count search with prefetch hints. Every
// mutator routes through the open-coded one. A hand-rolled binary search that is
// off by one on some sizes does not crash: it silently inserts a duplicate key or
// misses an erase, and the book slowly fills with stale levels. The differential
// test below runs the two overloads against each other across every size and
// every probe position, which is the only way that class of bug shows up early.
//
// Determinism: the pseudo-random ordering is a fixed-seed LCG written out here,
// so a failure reproduces exactly rather than "sometimes on CI".
// ═══════════════════════════════════════════════════════════════════════════════

#include "types/flat_map.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect_true(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

void expect_size(std::size_t got, std::size_t want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got %zu, want %zu\n", what, got, want);
        ++failures;
    }
}

void expect_double(double got, double want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got %.9f, want %.9f\n", what, got, want);
        ++failures;
    }
}

// Fixed-seed LCG (Numerical Recipes constants). Deterministic across platforms
// because everything is explicitly 32-bit unsigned.
class Lcg {
public:
    explicit Lcg(std::uint32_t seed) : state_(seed) {}
    std::uint32_t next() {
        state_ = state_ * 1664525u + 1013904223u;
        return state_;
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }

private:
    std::uint32_t state_;
};

// ── The invariant every other assertion leans on ────────────────────────────
template <typename Map, typename Compare>
void expect_sorted_unique(const Map& m, Compare comp, const char* what) {
    for (std::size_t i = 1; i < m.data().size(); ++i) {
        const auto& prev = m.data()[i - 1].first;
        const auto& cur = m.data()[i].first;
        if (!comp(prev, cur)) {
            std::fprintf(stderr, "FAIL %s: keys out of order or duplicated at index %zu\n",
                         what, i);
            ++failures;
            return;
        }
    }
}

void test_mutable_lower_bound_matches_the_standard_one() {
    // Every size from empty up past the prefetch threshold (step > 8, i.e. more
    // than ~16 live keys), and for each size every possible probe: below the
    // range, exactly on each key, between each pair of keys, and above the range.
    for (std::size_t n = 0; n <= 40; ++n) {
        FlatMap<int, int> m;
        for (std::size_t i = 0; i < n; ++i) {
            m.insert_or_assign(static_cast<int>(i) * 2, static_cast<int>(i));
        }
        expect_size(m.size(), n, "differential fixture size");

        for (int probe = -3; probe <= static_cast<int>(n) * 2 + 3; ++probe) {
            const auto mutable_hit = m.lower_bound(probe);
            const auto& const_ref = m;
            const auto const_hit = const_ref.lower_bound(probe);
            const auto std_hit = std::lower_bound(
                m.data().begin(), m.data().end(), probe,
                [](const std::pair<int, int>& p, int k) { return p.first < k; });

            const auto mutable_idx = std::distance(m.data().begin(), mutable_hit);
            const auto const_idx = std::distance(const_ref.data().begin(), const_hit);
            const auto std_idx = std::distance(m.data().begin(), std_hit);

            if (mutable_idx != std_idx || const_idx != std_idx) {
                std::fprintf(stderr,
                             "FAIL lower_bound disagrees at size %zu probe %d: "
                             "hand-rolled %ld, const %ld, std %ld\n",
                             n, probe, static_cast<long>(mutable_idx),
                             static_cast<long>(const_idx), static_cast<long>(std_idx));
                ++failures;
                return;
            }
        }
    }
}

void test_mutable_lower_bound_matches_under_a_descending_comparator() {
    // The bid side sorts descending. The same open-coded search has to run
    // against std::greater, and getting the comparison sense wrong there turns
    // the best bid into the worst one.
    for (std::size_t n = 0; n <= 40; ++n) {
        FlatMap<int, int, std::greater<int>> m;
        for (std::size_t i = 0; i < n; ++i) {
            m.insert_or_assign(static_cast<int>(i) * 2, static_cast<int>(i));
        }
        for (int probe = -3; probe <= static_cast<int>(n) * 2 + 3; ++probe) {
            const auto hit = m.lower_bound(probe);
            const auto std_hit = std::lower_bound(
                m.data().begin(), m.data().end(), probe,
                [](const std::pair<int, int>& p, int k) { return p.first > k; });
            if (std::distance(m.data().begin(), hit) !=
                std::distance(m.data().begin(), std_hit)) {
                std::fprintf(stderr,
                             "FAIL descending lower_bound disagrees at size %zu probe %d\n",
                             n, probe);
                ++failures;
                return;
            }
        }
        expect_sorted_unique(m, std::greater<int>(), "descending fixture stays descending");
    }
}

void test_random_insert_order_still_sorts() {
    // The wire delivers price levels in whatever order the exchange sends them.
    // Whatever the arrival order, the container must end up sorted and unique,
    // and must agree with a plain std::vector reference model on every lookup.
    Lcg rng(0xC0FFEEu);
    FlatMap<int, int> m;
    std::vector<std::pair<int, int>> model;

    for (int round = 0; round < 4000; ++round) {
        const int key = static_cast<int>(rng.below(500));
        const int value = static_cast<int>(rng.next() & 0xFFFF);
        const std::uint32_t action = rng.next() % 10u;

        if (action < 6u) {  // insert or update
            m.insert_or_assign(key, value);
            auto it = std::find_if(model.begin(), model.end(),
                                   [key](const std::pair<int, int>& p) { return p.first == key; });
            if (it != model.end()) {
                it->second = value;
            } else {
                model.push_back({key, value});
            }
        } else if (action < 9u) {  // erase
            const bool erased = m.erase(key);
            auto it = std::find_if(model.begin(), model.end(),
                                   [key](const std::pair<int, int>& p) { return p.first == key; });
            const bool model_had = it != model.end();
            if (model_had) model.erase(it);
            if (erased != model_had) {
                std::fprintf(stderr, "FAIL erase(%d) returned %d, model says %d\n", key,
                             static_cast<int>(erased), static_cast<int>(model_had));
                ++failures;
                return;
            }
        } else {  // operator[] default-inserts
            const int seen = m[key];
            auto it = std::find_if(model.begin(), model.end(),
                                   [key](const std::pair<int, int>& p) { return p.first == key; });
            const int model_seen = (it != model.end()) ? it->second : 0;
            if (it == model.end()) model.push_back({key, 0});
            if (seen != model_seen) {
                std::fprintf(stderr, "FAIL operator[](%d) gave %d, model says %d\n", key, seen,
                             model_seen);
                ++failures;
                return;
            }
        }
    }

    expect_size(m.size(), model.size(), "size tracks the reference model");
    expect_sorted_unique(m, std::less<int>(), "randomised inserts stay sorted and unique");

    std::sort(model.begin(), model.end());
    for (std::size_t i = 0; i < model.size() && i < m.data().size(); ++i) {
        if (m.data()[i] != model[i]) {
            std::fprintf(stderr, "FAIL entry %zu is (%d,%d), model says (%d,%d)\n", i,
                         m.data()[i].first, m.data()[i].second, model[i].first, model[i].second);
            ++failures;
            return;
        }
    }

    // And the three read paths agree with each other on every key in range.
    const FlatMap<int, int>& cm = m;
    for (int key = -5; key < 505; ++key) {
        const bool in_model = std::binary_search(
            model.begin(), model.end(), std::pair<int, int>{key, 0},
            [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                return a.first < b.first;
            });
        const bool by_contains = cm.contains(key);
        const bool by_find = cm.find(key) != cm.end();
        const bool by_ptr = cm.find_ptr(key) != nullptr;
        if (by_contains != in_model || by_find != in_model || by_ptr != in_model) {
            std::fprintf(stderr, "FAIL lookup paths disagree on key %d\n", key);
            ++failures;
            return;
        }
    }
}

void test_insert_or_assign_updates_rather_than_duplicating() {
    FlatMap<double, double> asks;
    asks.insert_or_assign(100.5, 2.0);
    asks.insert_or_assign(100.5, 7.0);
    expect_size(asks.size(), 1, "reassigning a price does not add a second level");
    expect_double(*asks.find_ptr(100.5), 7.0, "the level carries the new size");

    // The rvalue overload has its own code path and must behave identically.
    FlatMap<int, std::string> named;
    named.insert_or_assign(1, std::string("first"));
    named.insert_or_assign(1, std::string("second"));
    expect_size(named.size(), 1, "the move overload also updates in place");
    expect_true(*named.find_ptr(1) == "second", "the move overload stores the new value");
}

void test_try_emplace_and_emplace_differ_on_an_existing_key() {
    FlatMap<int, int> m;
    auto fresh = m.try_emplace(7, 100);
    expect_true(fresh.second, "try_emplace reports a fresh insert");
    expect_double(fresh.first->second, 100, "try_emplace returns the new entry");

    auto existing = m.try_emplace(7, 999);
    expect_true(!existing.second, "try_emplace reports an existing key");
    expect_double(existing.first->second, 100, "try_emplace leaves the existing value alone");
    expect_size(m.size(), 1, "try_emplace on an existing key adds nothing");

    // NOTE: emplace here does NOT match std::map::emplace, which is a no-op on an
    // existing key. This one overwrites. Pinned deliberately, because the name
    // invites the opposite assumption at every call site.
    m.emplace(7, 555);
    expect_double(*m.find_ptr(7), 555, "emplace overwrites an existing key");
    expect_size(m.size(), 1, "emplace on an existing key adds nothing either");
}

void test_at_and_missing_key_behaviour() {
    FlatMap<int, int> m;
    m.insert_or_assign(3, 30);

    expect_double(m.at(3), 30, "at returns a present value");
    const FlatMap<int, int>& cm = m;
    expect_double(cm.at(3), 30, "const at returns a present value");

    bool threw = false;
    try {
        (void)m.at(4);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    expect_true(threw, "at throws out_of_range for a missing key");

    threw = false;
    try {
        (void)cm.at(4);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    expect_true(threw, "const at throws out_of_range for a missing key");

    // The non-throwing paths report absence rather than inventing a value.
    expect_true(cm.find_ptr(4) == nullptr, "find_ptr reports a missing key as null");
    expect_true(cm.find(4) == cm.end(), "find reports a missing key as end");
    expect_true(!cm.contains(4), "contains reports a missing key as false");
    expect_true(!m.erase(4), "erasing a missing key reports false");
    expect_size(m.size(), 1, "a failed erase changes nothing");
}

void test_snapshot_replace_and_prune() {
    // insert_sorted is the snapshot path: the backend already sorted the levels,
    // so this REPLACES the contents wholesale rather than merging into them. A
    // book that merged a snapshot into stale levels would keep phantom depth.
    FlatMap<double, double> book;
    book.insert_or_assign(99.0, 1.0);
    book.insert_or_assign(101.0, 1.0);

    const std::vector<std::pair<double, double>> snapshot{
        {100.0, 5.0}, {100.5, 3.0}, {101.0, 2.0}};
    book.insert_sorted(snapshot.begin(), snapshot.end());
    expect_size(book.size(), 3, "a snapshot replaces the book, it does not merge");
    expect_true(book.find_ptr(99.0) == nullptr, "the pre-snapshot level is gone");
    expect_double(*book.find_ptr(101.0), 2.0, "the snapshot value wins on a shared price");
    expect_sorted_unique(book, std::less<double>(), "the snapshot stays sorted");

    // remove_if is the zero-size sweep: a depth delta of size 0 retires a level.
    book.insert_or_assign(102.0, 0.0);
    book.remove_if([](const std::pair<double, double>& lvl) { return lvl.second == 0.0; });
    expect_size(book.size(), 3, "the zero-size level is swept out");
    expect_sorted_unique(book, std::less<double>(), "the sweep preserves order");

    // Erasing a tail range is how the book is capped to MAX_LEVELS.
    auto& raw = book.data();
    book.erase(raw.begin() + 1, raw.end());
    expect_size(book.size(), 1, "a range erase truncates the tail");
    expect_double(book.data()[0].first, 100.0, "and keeps the head");

    book.clear();
    expect_true(book.empty(), "clear empties the book");
    expect_size(book.size(), 0, "and zeroes the size");
}

void test_best_bid_and_ask_come_off_the_ends() {
    // The whole reason bids use std::greater: front() is the best price on both
    // sides, so the DOM and the spread read the same way for bids and asks.
    FlatMap<double, double> asks;
    FlatMap<double, double, std::greater<double>> bids;

    for (const double px : {100.5, 100.1, 100.9, 100.3}) asks.insert_or_assign(px, 1.0);
    for (const double px : {99.5, 99.9, 99.1, 99.3}) bids.insert_or_assign(px, 1.0);

    expect_double(asks.front().first, 100.1, "front() is the best (lowest) ask");
    expect_double(asks.back().first, 100.9, "back() is the worst ask");
    expect_double(bids.front().first, 99.9, "front() is the best (highest) bid");
    expect_double(bids.back().first, 99.1, "back() is the worst bid");
    expect_double(asks.front().first - bids.front().first, 100.1 - 99.9, "the spread reads off both fronts");

    // Reverse iteration walks away from the touch on both sides.
    expect_double(asks.rbegin()->first, 100.9, "reverse ask iteration starts at the far side");
    expect_double(bids.rbegin()->first, 99.1, "reverse bid iteration starts at the far side");

    // A better price arriving lands at the touch, not at the end.
    asks.insert_or_assign(100.0, 1.0);
    bids.insert_or_assign(99.95, 1.0);
    expect_double(asks.front().first, 100.0, "a new best ask takes the front");
    expect_double(bids.front().first, 99.95, "a new best bid takes the front");

    // And retiring the touch promotes the next level rather than leaving a hole.
    expect_true(asks.erase(100.0), "the best ask is erasable");
    expect_true(bids.erase(99.95), "the best bid is erasable");
    expect_double(asks.front().first, 100.1, "the next ask is promoted to the touch");
    expect_double(bids.front().first, 99.9, "the next bid is promoted to the touch");

    expect_sorted_unique(asks, std::less<double>(), "asks stay ascending");
    expect_sorted_unique(bids, std::greater<double>(), "bids stay descending");
}

}  // namespace

int main() {
    test_mutable_lower_bound_matches_the_standard_one();
    test_mutable_lower_bound_matches_under_a_descending_comparator();
    test_random_insert_order_still_sorts();
    test_insert_or_assign_updates_rather_than_duplicating();
    test_try_emplace_and_emplace_differ_on_an_existing_key();
    test_at_and_missing_key_behaviour();
    test_snapshot_replace_and_prune();
    test_best_bid_and_ask_come_off_the_ends();

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("flat_map_test: all assertions passed\n");
    return 0;
}
