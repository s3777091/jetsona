#include "display/common/trash_store.h"

#include "display/common/backgrounds.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace jetson::ui::trash {
namespace {

bool IsSafeBasename(const std::string &name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos;
}

bool Exists(const std::string &path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

bool EnsureTrashDir() {
    const std::string dir = TrashDir();
    if (::mkdir(dir.c_str(), 0755) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat st {};
    return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string TombstoneDir() {
    return TrashDir() + "/.deleted";
}

std::string TombstonePath(const std::string &original_name) {
    return TombstoneDir() + "/" + original_name;
}

bool WriteTombstone(const std::string &original_name) {
    if (!EnsureTrashDir()) return false;
    const std::string dir = TombstoneDir();
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    FILE *marker = std::fopen(TombstonePath(original_name).c_str(), "wb");
    if (!marker) return false;
    std::fclose(marker);
    return true;
}

std::string StoredPath(const std::string &id) {
    return TrashDir() + "/" + id;
}

std::string OriginalNameFromId(const std::string &id) {
    const size_t separator = id.find("__");
    if (separator == std::string::npos) return {};
    const std::string name = id.substr(separator + 2);
    return IsSafeBasename(name) ? name : std::string{};
}

std::time_t DeletedTimeFromId(const std::string &id, std::time_t fallback) {
    const size_t underscore = id.find('_');
    if (underscore == std::string::npos) return fallback;
    char *end = nullptr;
    const long long millis = std::strtoll(id.substr(0, underscore).c_str(), &end, 10);
    return (end && *end == '\0' && millis > 0)
               ? static_cast<std::time_t>(millis / 1000)
               : fallback;
}

} // namespace

std::string TrashDir() {
    return backgrounds::BackgroundsDir() + "/.trash";
}

bool MoveBackgroundToTrash(const std::string &file) {
    if (!IsSafeBasename(file) || !EnsureTrashDir()) return false;

    const std::string source = backgrounds::BackgroundPath(file);
    if (!Exists(source)) return false;

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string id;
    std::string destination;
    for (int attempt = 0; attempt < 100; ++attempt) {
        id = std::to_string(now) + "_" + std::to_string(attempt) + "__" + file;
        destination = StoredPath(id);
        if (!Exists(destination) && !Exists(destination + ".thumb")) break;
        destination.clear();
    }
    if (destination.empty() || std::rename(source.c_str(), destination.c_str()) != 0) {
        return false;
    }

    const std::string source_thumb = backgrounds::ThumbPath(file);
    if (Exists(source_thumb) &&
        std::rename(source_thumb.c_str(), (destination + ".thumb").c_str()) != 0) {
        // Keep the operation atomic from the user's perspective when the
        // companion thumbnail cannot be moved.
        std::rename(destination.c_str(), source.c_str());
        return false;
    }
    return true;
}

std::vector<Item> ListBackgroundTrash() {
    std::vector<Item> items;
    const std::string dir = TrashDir();
    DIR *handle = opendir(dir.c_str());
    if (!handle) return items;

    while (dirent *entry = readdir(handle)) {
        const std::string id = entry->d_name;
        if (!IsSafeBasename(id) ||
            (id.size() > 6 && id.compare(id.size() - 6, 6, ".thumb") == 0)) {
            continue;
        }
        const std::string original_name = OriginalNameFromId(id);
        if (original_name.empty()) continue;

        const std::string stored_path = StoredPath(id);
        struct stat st {};
        if (::stat(stored_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;

        Item item;
        item.id = id;
        item.original_name = original_name;
        item.stored_path = stored_path;
        item.thumbnail_path = stored_path + ".thumb";
        item.size = static_cast<long>(st.st_size);
        item.deleted_at = DeletedTimeFromId(id, st.st_ctime);
        item.has_thumbnail = Exists(item.thumbnail_path);
        items.push_back(std::move(item));
    }
    closedir(handle);

    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
        if (a.deleted_at != b.deleted_at) return a.deleted_at > b.deleted_at;
        return a.id > b.id;
    });
    return items;
}

bool RestoreBackground(const Item &item) {
    if (!IsSafeBasename(item.id)) return false;
    const std::string original_name = OriginalNameFromId(item.id);
    if (original_name.empty()) return false;

    const std::string stored = StoredPath(item.id);
    const std::string stored_thumb = stored + ".thumb";
    const std::string destination = backgrounds::BackgroundPath(original_name);
    const std::string destination_thumb = backgrounds::ThumbPath(original_name);
    if (!Exists(stored) || Exists(destination) ||
        (Exists(stored_thumb) && Exists(destination_thumb))) {
        return false;
    }

    if (std::rename(stored.c_str(), destination.c_str()) != 0) return false;
    if (Exists(stored_thumb) &&
        std::rename(stored_thumb.c_str(), destination_thumb.c_str()) != 0) {
        std::rename(destination.c_str(), stored.c_str());
        return false;
    }
    // A recovered file is intentionally live again, so clear a marker left by
    // an older permanently deleted copy with the same basename.
    std::remove(TombstonePath(original_name).c_str());
    return true;
}

bool PermanentlyDeleteBackground(const Item &item) {
    if (!IsSafeBasename(item.id)) return false;
    const std::string original_name = OriginalNameFromId(item.id);
    if (original_name.empty() || !WriteTombstone(original_name)) return false;
    const std::string stored = StoredPath(item.id);
    const std::string stored_thumb = stored + ".thumb";
    if (Exists(stored_thumb) && std::remove(stored_thumb.c_str()) != 0) {
        std::remove(TombstonePath(original_name).c_str());
        return false;
    }
    if (Exists(stored) && std::remove(stored.c_str()) != 0) {
        std::remove(TombstonePath(original_name).c_str());
        return false;
    }
    return true;
}

size_t EmptyBackgroundTrash() {
    size_t removed = 0;
    for (const auto &item : ListBackgroundTrash()) {
        if (PermanentlyDeleteBackground(item)) ++removed;
    }
    return removed;
}

} // namespace jetson::ui::trash
