// web_remote_fs.cpp - the game's files streamed from the server on demand.
//
// Importing a whole installation before the first frame means downloading
// gigabytes, while a session reads a few tens of megabytes of them. This
// WasmFS backend mounts the server's copy read-only: the tree and the sizes
// come from <base>/manifest.txt, and a read fetches only the 1 MB chunks it
// touches, by HTTP range request, on the calling (game) thread. Fetched
// chunks are written to a sparse copy in the browser's private storage with
// a chunk map beside it, so the next visit reads them locally and a file
// that has been read completely never touches the network again.
#ifdef __EMSCRIPTEN__
#include "web_remote_fs.h"

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

// WasmFS's own backend interface (system/lib/wasmfs).
#include "backend.h"
#include "file.h"
#include "memory_backend.h"
#include "wasmfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

// A synchronous range request from a worker. Returns the bytes written to
// `dst`, or -1. A server that ignores Range answers 200 with the whole file,
// from which the wanted span is cut.
EM_JS(double, remote_fetch_range, (const char *url, double start, double end, void *dst), {
    try {
        const xhr = new XMLHttpRequest();
        xhr.open("GET", UTF8ToString(url), false);
        xhr.responseType = "arraybuffer";
        xhr.setRequestHeader("Range", "bytes=" + start + "-" + end);
        xhr.send();
        if (xhr.status != 206 && xhr.status != 200)
            return -1;
        let bytes = new Uint8Array(xhr.response);
        if (xhr.status == 200)
            bytes = bytes.subarray(start, end + 1);
        HEAPU8.set(bytes, dst);
        return bytes.length;
    } catch (e) {
        console.warn("remote fs: " + e);
        return -1;
    }
});

// The whole of a small text file (the manifest), malloc'd; null on failure.
EM_JS(char *, remote_fetch_text, (const char *url), {
    try {
        const xhr = new XMLHttpRequest();
        xhr.open("GET", UTF8ToString(url), false);
        xhr.send();
        if (xhr.status != 200)
            return 0;
        return stringToNewUTF8(xhr.responseText);
    } catch (e) {
        console.warn("remote fs: " + e);
        return 0;
    }
});

namespace {
using wasmfs::DataFile;
using wasmfs::MemoryDirectory;
using wasmfs::Symlink;
using wasmfs::File;
using wasmfs::Directory;
using RemoteBackendId = wasmfs::backend_t;

constexpr uint64_t CHUNK = 256u << 10;
// Read-ahead, in chunks. A read that continues where the last one ended
// doubles it up to the maximum; a jump elsewhere fetches only what it reads.
// The archives are read both ways: a movie or a map front to back, the
// 677 MB resource archive a few kilobytes here and there.
constexpr uint64_t MAX_AHEAD = 16;
// The prefetcher's requests are kept short (1 MB), so one in flight is soon
// out of the way of a read the game is waiting for.
constexpr uint64_t PREFETCH_RUN = 4;

std::string g_base_url, g_cache_dir;

void make_parents(const std::string &path) {
    for (size_t at = 1; (at = path.find('/', at)) != std::string::npos; ++at)
        mkdir(path.substr(0, at).c_str(), 0777);
}

class RemoteFile;

// Recently used chunks stay in memory: the game reads its archives 16 KB at
// a time, and going to the browser's storage for each of those would cost
// more than the read. Shared by all files, least recently used out first.
constexpr size_t RAM_CHUNKS = 256;
struct RamChunk {
    RemoteFile *file;
    uint64_t index;
    std::vector<uint8_t> bytes;
};
std::vector<RamChunk> g_ram; // most recent last
// Reads may come from more than one thread; the chunk cache is shared.
std::mutex g_lock;
uint64_t g_fetched_bytes = 0, g_fetches = 0;
// Reads waiting on the network. The prefetcher starts nothing while one is:
// the game's own request comes first.
std::atomic<int> g_readers_waiting{0};

const std::vector<uint8_t> *ram_find(RemoteFile *file, uint64_t index) {
    for (size_t i = g_ram.size(); i-- > 0;)
        if (g_ram[i].file == file && g_ram[i].index == index) {
            if (i + 1 != g_ram.size()) {
                RamChunk hit = std::move(g_ram[i]);
                g_ram.erase(g_ram.begin() + i);
                g_ram.push_back(std::move(hit));
            }
            return &g_ram.back().bytes;
        }
    return nullptr;
}
const std::vector<uint8_t> *ram_put(RemoteFile *file, uint64_t index, std::vector<uint8_t> bytes) {
    if (g_ram.size() >= RAM_CHUNKS)
        g_ram.erase(g_ram.begin());
    g_ram.reserve(RAM_CHUNKS); // pointers handed out stay valid until the next put
    g_ram.push_back({file, index, std::move(bytes)});
    return &g_ram.back().bytes;
}

// Each chunk is its own file in the browser's storage, beside a map of the
// chunks present: storage files are not sparse, so one copy of a 677 MB
// archive with a few chunks in it would take all 677 MB.
class RemoteFile : public DataFile {
    std::string rel_;
    uint64_t size_, stamp_;
    std::vector<uint8_t> have_; // one byte per chunk kept in storage
    bool cache_ok_ = false, cache_tried_ = false;
    uint64_t next_offset_ = UINT64_MAX, ahead_ = 1;

    std::string cache_base() const {
        return g_cache_dir + "/" + rel_;
    }
    std::string chunk_path(uint64_t index) const {
        return cache_base() + ".d/" + std::to_string(index);
    }
    // The map file: "<size> <stamp> <chunk>\n" and then one byte per chunk.
    // A map for a different size, stamp or chunk size is stale, and its
    // chunks are fetched again.
    void load_cache() {
        if (cache_tried_)
            return;
        cache_tried_ = true;
        if (g_cache_dir.empty())
            return;
        make_parents(cache_base() + ".d/x");
        cache_ok_ = true;
        FILE *map = fopen((cache_base() + ".chunks").c_str(), "rb");
        if (!map)
            return;
        unsigned long long size = 0, stamp = 0, chunk = 0;
        const bool current = fscanf(map, "%llu %llu %llu", &size, &stamp, &chunk) == 3 &&
                             fgetc(map) == '\n' && size == size_ && stamp == stamp_ && chunk == CHUNK;
        if (!current || fread(have_.data(), 1, have_.size(), map) != have_.size())
            std::fill(have_.begin(), have_.end(), 0);
        fclose(map);
    }
    void save_map() {
        FILE *map = fopen((cache_base() + ".chunks").c_str(), "wb");
        if (!map)
            return;
        fprintf(map, "%llu %llu %llu\n", (unsigned long long)size_, (unsigned long long)stamp_,
                (unsigned long long)CHUNK);
        fwrite(have_.data(), 1, have_.size(), map);
        fclose(map);
    }
    uint64_t chunk_len(uint64_t index) const {
        return std::min<uint64_t>(CHUNK, size_ - index * CHUNK);
    }
    const std::vector<uint8_t> *from_storage(uint64_t index) {
        if (!cache_ok_ || !have_[index])
            return nullptr;
        std::vector<uint8_t> bytes(chunk_len(index));
        FILE *f = fopen(chunk_path(index).c_str(), "rb");
        const bool ok = f && fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
        if (f)
            fclose(f);
        if (!ok) {
            have_[index] = 0; // lost: fetch it again
            return nullptr;
        }
        return ram_put(this, index, std::move(bytes));
    }
    // Fetches chunk `index` and the missing ones after it up to `until`
    // (inclusive) in one request, and keeps each in memory and in storage.
    bool fetch_run(uint64_t index, uint64_t until) {
        uint64_t end = index;
        while (end + 1 <= until && end + 1 < have_.size() && !have_[end + 1] && !ram_find(this, end + 1))
            ++end;
        const uint64_t from = index * CHUNK, to = std::min<uint64_t>((end + 1) * CHUNK, size_) - 1;
        std::vector<uint8_t> run(to - from + 1);
        const std::string url = g_base_url + "/" + rel_;
        if (remote_fetch_range(url.c_str(), (double)from, (double)to, run.data()) != (double)run.size()) {
            fprintf(stderr, "[remote] %s: range %llu-%llu failed\n", rel_.c_str(),
                    (unsigned long long)from, (unsigned long long)to);
            return false;
        }
        g_fetched_bytes += run.size();
        static const bool trace = getenv("RECOMP_REMOTE_TRACE") != nullptr;
        if (trace)
            fprintf(stderr, "[remote] fetch %s @%llu +%zu (asked chunk %llu)\n", rel_.c_str(),
                    (unsigned long long)from, run.size(), (unsigned long long)index);
        if ((++g_fetches & 31) == 0)
            fprintf(stderr, "[remote] %llu requests, %.1f MB fetched\n", (unsigned long long)g_fetches,
                    g_fetched_bytes / 1e6);
        bool stored = false;
        for (uint64_t k = index; k <= end; ++k) {
            const uint64_t at = (k - index) * CHUNK;
            std::vector<uint8_t> bytes(run.begin() + at, run.begin() + at + chunk_len(k));
            if (cache_ok_) {
                FILE *f = fopen(chunk_path(k).c_str(), "wb");
                bool ok = f && fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
                if (f && fclose(f) != 0)
                    ok = false;
                if (ok) {
                    have_[k] = 1;
                    stored = true;
                } else {
                    cache_ok_ = false; // storage full: keep streaming, stop keeping
                }
            }
            ram_put(this, k, std::move(bytes));
        }
        if (stored)
            save_map();
        return true;
    }
    const std::vector<uint8_t> *chunk(uint64_t index, uint64_t until) {
        if (auto *hit = ram_find(this, index))
            return hit;
        if (auto *hit = from_storage(index))
            return hit;
        ++g_readers_waiting;
        const bool fetched = fetch_run(index, until);
        --g_readers_waiting;
        if (!fetched)
            return nullptr;
        return ram_find(this, index);
    }

  public:
    // For the prefetcher, all under g_lock. The first chunk from `*index` up
    // to `last` that is neither stored nor in memory, and how far the missing
    // run after it goes (at most PREFETCH_RUN chunks); false when none is.
    bool next_missing(uint64_t *index, uint64_t last, uint64_t *end) {
        load_cache();
        last = std::min<uint64_t>(last, have_.size() - 1);
        const auto present = [&](uint64_t k) {
            if (have_[k])
                return true;
            for (const auto &r : g_ram)
                if (r.file == this && r.index == k)
                    return true;
            return false;
        };
        while (*index <= last && present(*index))
            ++*index;
        if (*index > last)
            return false;
        *end = *index;
        while (*end + 1 <= last && !present(*end + 1) && *end + 2 - *index <= PREFETCH_RUN)
            ++*end;
        return true;
    }
    uint64_t run_bytes(uint64_t index, uint64_t end) const {
        return std::min<uint64_t>((end + 1) * CHUNK, size_) - index * CHUNK;
    }
    const std::string &rel() const {
        return rel_;
    }
    // Keeps a run fetched outside the lock; chunks a reader stored meanwhile
    // are left as they are.
    void store(uint64_t index, const std::vector<uint8_t> &run) {
        bool changed = false;
        for (uint64_t k = index, at = 0; at < run.size() && k < have_.size(); ++k, at += CHUNK) {
            if (have_[k] || !cache_ok_)
                continue;
            const size_t n = (size_t)chunk_len(k);
            FILE *f = fopen(chunk_path(k).c_str(), "wb");
            bool ok = f && fwrite(run.data() + at, 1, n, f) == n;
            if (f && fclose(f) != 0)
                ok = false;
            if (!ok) {
                cache_ok_ = false;
                break;
            }
            have_[k] = 1;
            changed = true;
        }
        if (changed)
            save_map();
    }

    RemoteFile(const std::string &rel, uint64_t size, uint64_t stamp, RemoteBackendId backend)
        : DataFile(S_IFREG | 0444, backend), rel_(rel), size_(size), stamp_(stamp),
          have_((size + CHUNK - 1) / CHUNK, 0) {}

    int open(wasmfs::oflags_t flags) override {
        return (flags & O_ACCMODE) == O_RDONLY ? 0 : -EACCES;
    }
    int close() override {
        return 0;
    }
    off_t getSize() override {
        return (off_t)size_;
    }
    int setSize(off_t) override {
        return -EACCES;
    }
    int flush() override {
        return 0;
    }
    ssize_t write(const uint8_t *, size_t, off_t) override {
        return -EACCES;
    }
    ssize_t read(uint8_t *buf, size_t len, off_t offset) override {
        if (offset < 0)
            return -EINVAL;
        if ((uint64_t)offset >= size_ || !len)
            return 0;
        len = (size_t)std::min<uint64_t>(len, size_ - (uint64_t)offset);
        std::lock_guard<std::mutex> hold(g_lock);
        load_cache();
        // Sequential reads widen the read-ahead; anything else resets it.
        ahead_ = (uint64_t)offset == next_offset_ ? std::min<uint64_t>(ahead_ * 2, MAX_AHEAD) : 1;
        next_offset_ = (uint64_t)offset + len;
        const uint64_t last = ((uint64_t)offset + len - 1) / CHUNK;
        const uint64_t until = last + (ahead_ > 1 ? ahead_ - 1 : 0);
        size_t done = 0;
        while (done < len) {
            const uint64_t at = (uint64_t)offset + done, index = at / CHUNK;
            const std::vector<uint8_t> *bytes = chunk(index, until);
            if (!bytes)
                return done ? (ssize_t)done : -EIO;
            const size_t in = (size_t)(at - index * CHUNK);
            const size_t n = std::min(len - done, bytes->size() - in);
            memcpy(buf + done, bytes->data() + in, n);
            done += n;
        }
        return (ssize_t)done;
    }
};

// The hot list (<base>/hot.txt, "<path>\t<first chunk>\t<last chunk>" per
// line): the chunks a session reads from the intro to the first map,
// recorded once. A few threads fetch them in the background while the intro
// plays, so the game finds them local instead of waiting on a request per
// chunk. Only what is still missing is fetched, so a later visit costs
// nothing.
struct HotItem {
    RemoteFile *file;
    uint64_t first, last;
};
std::vector<HotItem> g_hot;
std::atomic<size_t> g_hot_next{0};
std::atomic<uint64_t> g_prefetched{0};
std::atomic<int> g_prefetchers{0};

void prefetch_worker() {
    std::vector<uint8_t> run;
    const double started = emscripten_get_now();
    for (size_t i; (i = g_hot_next++) < g_hot.size();) {
        const HotItem &h = g_hot[i];
        for (uint64_t at = h.first;;) {
            uint64_t end = 0, bytes = 0;
            while (g_readers_waiting.load() > 0)
                emscripten_thread_sleep(10);
            {
                std::lock_guard<std::mutex> hold(g_lock);
                if (!h.file->next_missing(&at, h.last, &end))
                    break;
                bytes = h.file->run_bytes(at, end);
            }
            run.resize(bytes);
            const std::string url = g_base_url + "/" + h.file->rel();
            const double from = (double)(at * CHUNK);
            if (remote_fetch_range(url.c_str(), from, from + (double)bytes - 1, run.data()) != (double)bytes)
                break;
            {
                std::lock_guard<std::mutex> hold(g_lock);
                h.file->store(at, run);
            }
            g_prefetched += bytes;
            at = end + 1;
        }
    }
    if (--g_prefetchers == 0)
        fprintf(stderr, "[remote] hot list ready: %.1f MB fetched ahead in %.1f s\n",
                g_prefetched.load() / 1e6, (emscripten_get_now() - started) / 1000.0);
}

class RemoteDirectory : public MemoryDirectory {
  public:
    RemoteDirectory(RemoteBackendId backend) : MemoryDirectory(S_IFDIR | 0555, backend) {}
    // Read-only: the game writes through the profile overlay, never here.
    std::shared_ptr<DataFile> insertDataFile(const std::string &, mode_t) override {
        return nullptr;
    }
    std::shared_ptr<Directory> insertDirectory(const std::string &, mode_t) override {
        return nullptr;
    }
    std::shared_ptr<Symlink> insertSymlink(const std::string &, const std::string &) override {
        return nullptr;
    }
    int removeChild(const std::string &) override {
        return -EACCES;
    }
    void add(const std::string &name, std::shared_ptr<File> child) {
        insertChild(name, std::move(child));
    }
    std::shared_ptr<RemoteDirectory> subdir(const std::string &name) {
        auto existing = MemoryDirectory::getChild(name);
        if (existing)
            return std::static_pointer_cast<RemoteDirectory>(existing);
        auto dir = std::make_shared<RemoteDirectory>(getBackend());
        add(name, dir);
        return dir;
    }
};

class RemoteBackend : public wasmfs::Backend {
  public:
    std::shared_ptr<RemoteDirectory> root;
    std::shared_ptr<DataFile> createFile(mode_t) override {
        return nullptr;
    }
    std::shared_ptr<Directory> createDirectory(mode_t) override {
        // The first directory asked for is the mount point itself.
        auto dir = std::make_shared<RemoteDirectory>(this);
        if (!root)
            root = dir;
        return dir;
    }
    std::shared_ptr<Symlink> createSymlink(std::string) override {
        return nullptr;
    }
};

} // namespace

bool web_remote_mount(const char *base_url, const char *mount, const char *cache_dir) {
    g_base_url = base_url ? base_url : "";
    while (!g_base_url.empty() && g_base_url.back() == '/')
        g_base_url.pop_back();
    g_cache_dir = cache_dir ? cache_dir : "";
    char *manifest = remote_fetch_text((g_base_url + "/manifest.txt").c_str());
    if (!manifest) {
        fprintf(stderr, "[remote] no manifest at %s/manifest.txt\n", g_base_url.c_str());
        return false;
    }
    auto owned = std::make_unique<RemoteBackend>();
    RemoteBackend *backend = owned.get();
    RemoteBackendId id = wasmfs::wasmFS.addBackend(std::move(owned));
    if (wasmfs_create_directory(mount, 0555, reinterpret_cast<::backend_t>(id)) != 0 || !backend->root) {
        free(manifest);
        return false;
    }
    // "<size>\t<stamp>\t<path>" per line, '/'-separated paths under the root.
    std::map<std::string, RemoteFile *> by_path;
    size_t files = 0;
    unsigned long long bytes = 0;
    for (char *line = strtok(manifest, "\n"); line; line = strtok(nullptr, "\n")) {
        char *tab1 = strchr(line, '\t');
        char *tab2 = tab1 ? strchr(tab1 + 1, '\t') : nullptr;
        if (!tab2)
            continue;
        const uint64_t size = strtoull(line, nullptr, 10), stamp = strtoull(tab1 + 1, nullptr, 10);
        std::string path = tab2 + 1;
        if (!path.empty() && path.back() == '\r')
            path.pop_back();
        auto dir = backend->root;
        size_t at = 0;
        for (size_t slash; (slash = path.find('/', at)) != std::string::npos; at = slash + 1)
            dir = dir->subdir(path.substr(at, slash - at));
        auto file = std::make_shared<RemoteFile>(path, size, stamp, id);
        by_path[path] = file.get();
        dir->add(path.substr(at), file);
        ++files;
        bytes += size;
    }
    free(manifest);
    fprintf(stderr, "[remote] %s: %zu files, %.0f MB, streamed from %s\n", mount, files, bytes / 1e6,
            g_base_url.c_str());
    if (char *hot = remote_fetch_text((g_base_url + "/hot.txt").c_str())) {
        for (char *line = strtok(hot, "\n"); line; line = strtok(nullptr, "\n")) {
            char *tab1 = strchr(line, '\t');
            char *tab2 = tab1 ? strchr(tab1 + 1, '\t') : nullptr;
            if (!tab2)
                continue;
            *tab1 = 0;
            auto it = by_path.find(line);
            if (it != by_path.end())
                g_hot.push_back({it->second, strtoull(tab1 + 1, nullptr, 10), strtoull(tab2 + 1, nullptr, 10)});
        }
        free(hot);
        // Two requests in flight beside the game's own.
        constexpr int THREADS = 2;
        g_prefetchers = THREADS;
        for (int i = 0; i < THREADS; ++i)
            std::thread(prefetch_worker).detach();
        fprintf(stderr, "[remote] fetching %zu hot ranges ahead\n", g_hot.size());
    }
    return files > 0;
}
#endif
