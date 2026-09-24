// Two terminal windows on one symbol share one localStorage blob. These pin
// the merge rules a flush and a pull apply so neither window erases the
// other's drawings: additions elsewhere survive, deletions here stick, the
// newer edit wins, and a drawing mid-drag is never replaced underneath.
#include "core/drawing_manager.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using drawing::Drawing;
using Items = std::vector<Drawing>;

static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); std::exit(1); }
}

static Drawing line(uint64_t id, double price, int64_t stamp) {
    Drawing d;
    d.id = id;
    d.tool = drawing::Tool::HLine;
    d.anchors = {drawing::Anchor{1'000, price}};
    d.modified_ms = stamp;
    return d;
}

static DrawingManager::SyncedItems synced_from(const Items& items) {
    DrawingManager::SyncedItems synced;
    for (const Drawing& d : items) synced[d.id] = DrawingManager::serialize_item(d, false);
    return synced;
}

static const Drawing* by_id(const Items& items, uint64_t id) {
    for (const Drawing& d : items) if (d.id == id) return &d;
    return nullptr;
}

int main() {
    // Both windows loaded {1, 2}. Window A (mine) deleted 2, moved 1, added 3.
    // Window B meanwhile added 4 and moved 1 later than A did.
    const Items loaded = {line(1, 100, 10), line(2, 200, 10)};
    const auto synced = synced_from(loaded);

    Items mine = {line(1, 101, 10), line(3, 300, 0)};          // 2 deleted here
    Items stored = {line(1, 100, 10), line(2, 200, 10), line(4, 400, 50)};
    Items merged = DrawingManager::merge_for_flush(stored, mine, synced, 0, 40);
    check(merged.size() == 3, "flush keeps mine plus their addition");
    check(by_id(merged, 2) == nullptr, "flush drops the id deleted here");
    check(by_id(merged, 4) != nullptr, "flush keeps the drawing only they have");
    check(by_id(merged, 1)->anchors[0].price == 101, "flush keeps my changed copy");
    check(by_id(merged, 1)->modified_ms == 40, "flush stamps my changed copy with now");
    check(by_id(merged, 3)->modified_ms == 40, "flush stamps my new drawing");
    check(merged[0].id == 1 && merged[1].id == 3 && merged[2].id == 4,
          "flush keeps my order first, arrivals after");

    // Their copy of 1 is newer than my unchanged one: theirs wins.
    Items mine_unchanged = {line(1, 100, 10)};
    Items stored_newer = {line(1, 105, 60)};
    merged = DrawingManager::merge_for_flush(stored_newer, mine_unchanged, synced, 0, 70);
    check(merged.size() == 1 && merged[0].anchors[0].price == 105, "flush takes their newer copy");

    // Same, but I am dragging 1 right now: mine stays until the drag ends.
    merged = DrawingManager::merge_for_flush(stored_newer, mine_unchanged, synced, 1, 70);
    check(merged[0].anchors[0].price == 100, "flush keeps the drawing being edited");

    // Pull: they deleted 2, added 4, moved 1 newer; I have an unsynced 3.
    Items mine_pull = {line(1, 100, 10), line(2, 200, 10), line(3, 300, 0)};
    Items stored_pull = {line(1, 105, 60), line(4, 400, 50)};
    Items pulled = DrawingManager::merge_for_pull(stored_pull, mine_pull, synced, 0);
    check(pulled.size() == 3, "pull yields theirs plus my unsynced drawing");
    check(by_id(pulled, 2) == nullptr, "pull drops what they deleted");
    check(by_id(pulled, 1)->anchors[0].price == 105, "pull takes their newer copy");
    check(by_id(pulled, 3) != nullptr, "pull keeps my unsynced drawing");
    check(by_id(pulled, 4) != nullptr, "pull adds their new drawing");

    // Pull while dragging 1: my copy stays.
    pulled = DrawingManager::merge_for_pull(stored_pull, mine_pull, synced, 1);
    check(by_id(pulled, 1)->anchors[0].price == 100, "pull keeps the drawing being edited");

    // A stored copy with an equal stamp is not "newer": my copy stays.
    Items stored_same = {line(1, 100, 10)};
    pulled = DrawingManager::merge_for_pull(stored_same, Items{line(1, 100, 10)}, synced, 0);
    check(pulled.size() == 1, "pull with equal stamps keeps one copy");

    // Round trip: the serialized form is stable, so an untouched item keeps its stamp.
    const std::string a = DrawingManager::serialize_item(line(7, 7, 1), false);
    const std::string b = DrawingManager::serialize_item(line(7, 7, 2), false);
    check(a == b, "unstamped serialization ignores the stamp");

    std::puts("drawing merge rules passed");
    return 0;
}
