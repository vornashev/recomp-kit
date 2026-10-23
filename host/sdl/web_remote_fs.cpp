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
//
// The game waits only for the chunks a read covers. What a sequential read
// will want next is fetched by background threads, and once the hot list is
// in, they fill in the rest of the game while the game is not reading
// (RECOMP_REMOTE_FILL=0 turns that off), so a later session streams nothing.
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
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
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
// The read-ahead is fetched in the background: the game waits only for the
// chunks it asked for.
// The archives are read both ways: a movie or a map front to back, the
// 677 MB resource archive a few kilobytes here and there.
constexpr uint64_t MAX_AHEAD = 16;
// The prefetcher's requests are kept short (1 MB), so one in flight is soon
// out of the way of a read the game is waiting for.
constexpr uint64_t PREFETCH_RUN = 4;

std::string g_base_url, g_cache_dir;

// What the page shows of the stream (recomp_web_stream_stats): plain doubles
// the main thread reads without a lock. A torn or stale read is a display
// glitch at worst, never a decision.
enum StatSlot {
    S_MOUNTED,    // 1 once the manifest is in
    S_PACK_TOTAL, // bytes of the small-file pack still to fetch (0: none needed)
    S_PACK_DONE,
    S_HOT_TOTAL, // bytes of the hot list not yet kept when the run started
    S_HOT_DONE,
    S_GAME_TOTAL, // the whole game, bytes
    S_KEPT,       // bytes kept in storage
    S_KEPT_KNOWN, // 1 once S_KEPT has been counted
    S_FETCHED,    // bytes fetched this run, by anyone
    S_WAIT_SINCE, // emscripten_get_now() when the game started waiting on the network, 0 if not
    S_WAIT_BYTES, // how much it is waiting for
    S_FILL_MODE,  // 0 off, 1 in quiet moments, 2 as fast as it goes
    S_FILL_DONE,  // 1 once every file is kept
    S_COUNT
};
std::atomic<double> g_stats[16];
void stat_set(int slot, double v) {
    g_stats[slot].store(v, std::memory_order_relaxed);
}
void stat_add(int slot, double v) {
    g_stats[slot].fetch_add(v, std::memory_order_relaxed);
}
// The fill's pace, S_FILL_MODE's meaning; the page changes it while running.
std::atomic<int> g_fill_mode{1};

// Writes one chunk file; says why the first few that fail did, since a cache
// that silently keeps nothing streams the whole game again on every visit.
bool write_chunk(const std::string &path, const uint8_t *data, size_t n) {
    static std::atomic<int> told{0};
    const char *step = "open";
    int err = 0;
    FILE *f = fopen(path.c_str(), "wb");
    bool ok = f != nullptr;
    if (!ok)
        err = errno;
    if (ok && fwrite(data, 1, n, f) != n) {
        ok = false, err = errno, step = "write";
    }
    if (f && fclose(f) != 0 && ok) {
        ok = false, err = errno, step = "close";
    }
    if (!ok && told++ < 3)
        fprintf(stderr, "[remote] keeping %s failed at %s: %s (%zu bytes)\n", path.c_str(), step,
                strerror(err), n);
    return ok;
}

void make_parents(const std::string &path) {
    for (size_t at = 1; (at = path.find('/', at)) != std::string::npos; ++at)
        mkdir(path.substr(0, at).c_str(), 0777);
}

class RemoteFile;
// Hands chunks a sequential read will want next to the background threads.
// Called under g_lock.
void post_ahead_for(RemoteFile *file, uint64_t first, uint64_t last);

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
// Chunks a background thread is fetching, under g_lock. A read that needs one
// waits for that request instead of making the same one again; g_landed is
// signalled whenever one finishes.
std::set<std::pair<const RemoteFile *, uint64_t>> g_inflight;
std::condition_variable g_landed;
bool inflight(const RemoteFile *file, uint64_t index) {
    return g_inflight.count({file, index}) != 0;
}
// Chunks fetched but not yet written to storage, under g_lock. One thread,
// the keeper, does every write to storage: the game's read returns as soon as
// its bytes arrive, and nobody holds g_lock while the browser's storage works.
struct Pending {
    RemoteFile *file;
    uint64_t index;
    std::vector<uint8_t> bytes;
};
std::deque<Pending> g_pending;
std::set<RemoteFile *> g_dirty_maps; // files whose chunk map has to be written again
std::condition_variable g_pending_cv;
uint64_t g_fetched_bytes = 0, g_fetches = 0;
// Reads waiting on the network. The prefetcher starts nothing while one is:
// the game's own request comes first.
std::atomic<int> g_readers_waiting{0};
// When the game last fetched something itself (emscripten_get_now ms). Its
// reads come in bursts - a dialog, a map - and a background request started
// inside a burst sits in front of the next read, so the prefetcher waits for
// a quiet moment.
std::atomic<double> g_last_read_fetch{0};
// How long the game has waited on the network, in all and at most at once.
double g_wait_ms = 0, g_wait_max_ms = 0; // under g_lock
// Read on first use, after the page has filled in the environment.
bool tracing() {
    static const bool on = getenv("RECOMP_REMOTE_TRACE") != nullptr;
    return on;
}
void note_wait(double ms, const char *what, const std::string &rel, uint64_t index) {
    g_wait_ms += ms;
    if (ms > g_wait_max_ms)
        g_wait_max_ms = ms;
    if (tracing() && ms >= 100)
        fprintf(stderr, "[remote] the game waited %.0f ms for %s chunk %llu (%s)\n", ms, rel.c_str(),
                (unsigned long long)index, what);
}
constexpr double QUIET_MS = 400;
// The fill waits longer: loading a map reads in bursts a second apart, and a
// fill request in the middle of one is bandwidth the next read waits for.
constexpr double FILL_QUIET_MS = 1500;

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
    uint64_t ahead_posted_ = UINT64_MAX; // last chunk handed to the background
    bool missed_ = false; // the read in progress waited on the network

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
        while (end + 1 <= until && end + 1 < have_.size() && !have_[end + 1] && !ram_find(this, end + 1) &&
               !inflight(this, end + 1))
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
        stat_add(S_FETCHED, (double)run.size());
        if (tracing())
            fprintf(stderr, "[remote] fetch %s @%llu +%zu (asked chunk %llu)\n", rel_.c_str(),
                    (unsigned long long)from, run.size(), (unsigned long long)index);
        if ((++g_fetches & 31) == 0)
            fprintf(stderr, "[remote] %llu requests, %.1f MB fetched; the game waited %.1f s in all, "
                            "%.0f ms at most\n",
                    (unsigned long long)g_fetches, g_fetched_bytes / 1e6, g_wait_ms / 1000.0, g_wait_max_ms);
        for (uint64_t k = index; k <= end; ++k) {
            const uint64_t at = (k - index) * CHUNK;
            std::vector<uint8_t> bytes(run.begin() + at, run.begin() + at + chunk_len(k));
            keep(k, bytes);
            ram_put(this, k, std::move(bytes));
        }
        return true;
    }
    // Under g_lock: hands a chunk to the keeper, unless it is kept already.
    void keep(uint64_t k, const std::vector<uint8_t> &bytes) {
        if (!cache_ok_ || have_[k])
            return;
        for (const Pending &p : g_pending)
            if (p.file == this && p.index == k)
                return;
        g_pending.push_back({this, k, bytes});
        g_pending_cv.notify_one();
    }
    // Under g_lock: a chunk on its way to storage, back into memory.
    const std::vector<uint8_t> *from_pending(uint64_t index) {
        for (const Pending &p : g_pending)
            if (p.file == this && p.index == index)
                return ram_put(this, index, p.bytes);
        return nullptr;
    }
    const std::vector<uint8_t> *chunk(uint64_t index, uint64_t until, std::unique_lock<std::mutex> &hold) {
        if (auto *hit = ram_find(this, index))
            return hit;
        if (inflight(this, index)) {
            ++g_readers_waiting;
            const double t0 = emscripten_get_now();
            stat_set(S_WAIT_BYTES, (double)chunk_len(index));
            stat_set(S_WAIT_SINCE, t0);
            g_landed.wait(hold, [&] { return !inflight(this, index); });
            g_last_read_fetch = emscripten_get_now();
            note_wait(g_last_read_fetch.load() - t0, "a background request", rel_, index);
            stat_set(S_WAIT_SINCE, 0);
            --g_readers_waiting;
            missed_ = true;
            if (auto *hit = ram_find(this, index))
                return hit;
        }
        if (auto *hit = from_pending(index))
            return hit;
        if (auto *hit = from_storage(index))
            return hit;
        ++g_readers_waiting;
        const double t0 = emscripten_get_now();
        stat_set(S_WAIT_BYTES, (double)run_bytes(index, until));
        stat_set(S_WAIT_SINCE, t0);
        const bool fetched = fetch_run(index, until);
        stat_set(S_WAIT_SINCE, 0);
        g_last_read_fetch = emscripten_get_now();
        note_wait(g_last_read_fetch.load() - t0, "its own request", rel_, index);
        --g_readers_waiting;
        missed_ = true;
        if (!fetched)
            return nullptr;
        return ram_find(this, index);
    }

    void post_ahead(uint64_t first, uint64_t last) {
        post_ahead_for(this, first, last);
    }

  public:
    // For the prefetcher, all under g_lock. The first chunk from `*index` up
    // to `last` that is neither stored nor in memory, and how far the missing
    // run after it goes (at most PREFETCH_RUN chunks); false when none is.
    bool next_missing(uint64_t *index, uint64_t last, uint64_t *end, uint64_t max_run = PREFETCH_RUN) {
        load_cache();
        last = std::min<uint64_t>(last, have_.size() - 1);
        const auto present = [&](uint64_t k) {
            if (have_[k] || inflight(this, k))
                return true;
            for (const auto &r : g_ram)
                if (r.file == this && r.index == k)
                    return true;
            for (const Pending &p : g_pending)
                if (p.file == this && p.index == k)
                    return true;
            return false;
        };
        while (*index <= last && present(*index))
            ++*index;
        if (*index > last)
            return false;
        *end = *index;
        while (*end + 1 <= last && !present(*end + 1) && *end + 2 - *index <= max_run)
            ++*end;
        return true;
    }
    uint64_t run_bytes(uint64_t index, uint64_t end) const {
        return std::min<uint64_t>((end + 1) * CHUNK, size_) - index * CHUNK;
    }
    const std::string &rel() const {
        return rel_;
    }
    // Under g_lock: every chunk already stored.
    bool complete() {
        load_cache();
        for (uint8_t b : have_)
            if (!b)
                return false;
        return cache_ok_;
    }
    // Under g_lock: whether chunks can still be kept in storage.
    bool storable() {
        load_cache();
        return cache_ok_;
    }
    uint64_t chunks() const {
        return have_.size();
    }
    uint64_t size() const {
        return size_;
    }
    // Keeps a run fetched outside the lock; chunks a reader stored meanwhile
    // are left as they are. `keep` also puts it in memory, for a run fetched
    // because the game is about to read it.
    void store(uint64_t index, const std::vector<uint8_t> &run, bool in_memory = false) {
        load_cache();
        for (uint64_t k = index, at = 0; at < run.size() && k < have_.size(); ++k, at += CHUNK) {
            const size_t n = (size_t)chunk_len(k);
            std::vector<uint8_t> bytes(run.begin() + at, run.begin() + at + n);
            keep(k, bytes);
            if (in_memory && !ram_find(this, k))
                ram_put(this, k, std::move(bytes));
        }
    }
    // For the keeper. Outside g_lock: writes one chunk.
    bool write_kept(uint64_t k, const std::vector<uint8_t> &bytes) const {
        return write_chunk(chunk_path(k), bytes.data(), bytes.size());
    }
    // Under g_lock: the write's outcome.
    void kept(uint64_t k, bool ok) {
        if (!ok) {
            cache_ok_ = false; // storage full: keep streaming, stop keeping
            return;
        }
        have_[k] = 1;
        g_dirty_maps.insert(this);
        if (g_stats[S_KEPT_KNOWN].load(std::memory_order_relaxed) != 0)
            stat_add(S_KEPT, (double)chunk_len(k));
    }
    // Under g_lock: bytes of this file already in storage.
    uint64_t kept_bytes() {
        load_cache();
        uint64_t n = 0;
        if (cache_ok_)
            for (uint64_t k = 0; k < have_.size(); ++k)
                if (have_[k])
                    n += chunk_len(k);
        return n;
    }
    // Under g_lock: bytes of chunks first..last not yet in storage.
    uint64_t missing_bytes(uint64_t first, uint64_t last) {
        load_cache();
        uint64_t n = 0;
        for (uint64_t k = first; k <= last && k < have_.size(); ++k)
            if (!have_[k])
                n += chunk_len(k);
        return n;
    }
    // Under g_lock: the map as it stands, for the keeper to write.
    std::vector<uint8_t> map_bytes() const {
        char head[96];
        const int n = snprintf(head, sizeof head, "%llu %llu %llu\n", (unsigned long long)size_,
                               (unsigned long long)stamp_, (unsigned long long)CHUNK);
        std::vector<uint8_t> out(head, head + n);
        out.insert(out.end(), have_.begin(), have_.end());
        return out;
    }
    std::string map_path() const {
        return cache_base() + ".chunks";
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
        std::unique_lock<std::mutex> hold(g_lock);
        load_cache();
        // Sequential reads widen the read-ahead; anything else resets it.
        const bool sequential = (uint64_t)offset == next_offset_;
        ahead_ = sequential ? std::min<uint64_t>(ahead_ * 2, MAX_AHEAD) : 1;
        next_offset_ = (uint64_t)offset + len;
        const uint64_t last = ((uint64_t)offset + len - 1) / CHUNK;
        if (!sequential)
            ahead_posted_ = UINT64_MAX;
        missed_ = false;
        size_t done = 0;
        while (done < len) {
            const uint64_t at = (uint64_t)offset + done, index = at / CHUNK;
            const std::vector<uint8_t> *bytes = chunk(index, last, hold);
            if (!bytes)
                return done ? (ssize_t)done : -EIO;
            const size_t in = (size_t)(at - index * CHUNK);
            const size_t n = std::min(len - done, bytes->size() - in);
            memcpy(buf + done, bytes->data() + in, n);
            done += n;
        }
        // Read-ahead starts with a sequential read that had to wait, as the
        // game's own reads of a stream do, and is topped up while the stream
        // goes on; a resource that is already here, read in pieces, asks for
        // nothing.
        const bool streaming = ahead_posted_ != UINT64_MAX;
        if (ahead_ > 1 && (missed_ || (streaming && last + ahead_ / 2 > ahead_posted_)) &&
            last + 1 < have_.size()) {
            const uint64_t first = streaming ? std::max(last + 1, ahead_posted_ + 1) : last + 1;
            const uint64_t to = std::min<uint64_t>(last + ahead_, have_.size() - 1);
            if (first <= to) {
                post_ahead(first, to);
                ahead_posted_ = to;
            }
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

// The pack (manifest "#pack" line): the small files back to back, fetched in
// one request and filed into the cache, ahead of the hot list.
struct PackItem {
    RemoteFile *file;
    uint64_t offset, size;
};
std::string g_pack_name;
uint64_t g_pack_size = 0;
std::vector<PackItem> g_pack;

void prefetch_worker();

void fetch_pack() {
    bool needed = false;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        for (const PackItem &p : g_pack)
            if (!p.file->complete()) {
                needed = true;
                break;
            }
    }
    if (!needed || !g_pack_size)
        return;
    stat_set(S_PACK_TOTAL, (double)g_pack_size);
    const double started = emscripten_get_now();
    std::vector<uint8_t> pack(g_pack_size);
    const std::string url = g_base_url + "/" + g_pack_name;
    if (remote_fetch_range(url.c_str(), 0, (double)g_pack_size - 1, pack.data()) != (double)g_pack_size) {
        fprintf(stderr, "[remote] %s could not be fetched\n", g_pack_name.c_str());
        stat_set(S_PACK_DONE, (double)g_pack_size); // the files come one by one instead
        return;
    }
    stat_add(S_FETCHED, (double)g_pack_size);
    std::vector<uint8_t> part;
    for (const PackItem &p : g_pack) {
        if (p.offset + p.size > pack.size())
            continue;
        part.assign(pack.begin() + p.offset, pack.begin() + p.offset + p.size);
        std::lock_guard<std::mutex> hold(g_lock);
        p.file->store(0, part);
    }
    stat_set(S_PACK_DONE, (double)g_pack_size);
    fprintf(stderr, "[remote] %zu small files (%.1f MB) fetched in one request in %.1f s\n", g_pack.size(),
            g_pack_size / 1e6, (emscripten_get_now() - started) / 1000.0);
}

// What the background threads fetch, most urgent first: read-ahead for the
// game, then the hot list, then (when filling) every chunk of every file.
// Each job is a short run (PREFETCH_RUN chunks at most), so an urgent one is
// never stuck behind a long background transfer.
struct Range {
    RemoteFile *file;
    uint64_t first, last;
};
std::deque<Range> g_ahead; // under g_lock
std::vector<RemoteFile *> g_fill; // every file, the order they are filled in
size_t g_fill_file = 0;           // under g_lock
uint64_t g_fill_chunk = 0;        // under g_lock
std::atomic<uint64_t> g_filled{0};
std::atomic<bool> g_hot_done{false}, g_fill_done{false};
// One fill request at a time: it shares the connection with any miss of the
// game's, and a second one would only make that miss slower.
int g_fill_active = 0; // under g_lock

void post_ahead_for(RemoteFile *file, uint64_t first, uint64_t last) {
    g_ahead.push_back({file, first, last});
}

struct Job {
    RemoteFile *file = nullptr;
    uint64_t at = 0, end = 0;
    int kind = 0; // 0 read-ahead, 1 hot list, 2 fill
};

// Under g_lock. Marks the job's chunks in flight.
bool take_from(std::deque<Range> &queue, Job *job, int kind) {
    while (!queue.empty()) {
        Range &r = queue.front();
        uint64_t at = r.first, end = 0;
        if (r.file->next_missing(&at, r.last, &end)) {
            *job = {r.file, at, end, kind};
            r.first = end + 1;
            if (r.first > r.last)
                queue.pop_front();
            return true;
        }
        queue.pop_front();
    }
    return false;
}

bool take_hot(Job *job) {
    for (size_t i; (i = g_hot_next.load()) < g_hot.size(); ++g_hot_next) {
        HotItem &h = g_hot[i];
        uint64_t at = h.first, end = 0;
        if (h.file->next_missing(&at, h.last, &end)) {
            *job = {h.file, at, end, 1};
            h.first = end + 1;
            if (h.first > h.last)
                ++g_hot_next;
            return true;
        }
    }
    return false;
}

bool take_fill(Job *job) {
    for (; g_fill_file < g_fill.size(); ++g_fill_file, g_fill_chunk = 0) {
        RemoteFile *f = g_fill[g_fill_file];
        if (!f->chunks() || !f->storable())
            continue;
        uint64_t at = g_fill_chunk, end = 0;
        // A megabyte at a time: against a server a hundred milliseconds or
        // more away, smaller requests are mostly waiting for the answer, and
        // a read the game starts meanwhile shares the connection with one at
        // most.
        if (f->next_missing(&at, f->chunks() - 1, &end, PREFETCH_RUN)) {
            *job = {f, at, end, 2};
            g_fill_chunk = end + 1;
            return true;
        }
    }
    return false;
}

void mark(const Job &job, bool on) {
    for (uint64_t k = job.at; k <= job.end; ++k)
        if (on)
            g_inflight.insert({job.file, k});
        else
            g_inflight.erase({job.file, k});
}

// The keeper: every write to storage, one at a time, never under g_lock. The
// chunk maps are written when the queue runs dry, so a burst of chunks costs
// one map write per file rather than one per chunk.
void keeper() {
    for (;;) {
        // Left in the queue until written, so a reader still finds it there.
        // Only this thread removes entries, and adding to a deque leaves the
        // existing ones where they are, so the pointer holds outside the lock.
        const Pending *job = nullptr;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> maps;
        {
            std::unique_lock<std::mutex> hold(g_lock);
            g_pending_cv.wait(hold, [] { return !g_pending.empty() || !g_dirty_maps.empty(); });
            if (!g_pending.empty()) {
                job = &g_pending.front();
            } else {
                for (RemoteFile *f : g_dirty_maps)
                    maps.emplace_back(f->map_path(), f->map_bytes());
                g_dirty_maps.clear();
            }
        }
        if (job) {
            const bool ok = job->file->write_kept(job->index, job->bytes);
            std::lock_guard<std::mutex> hold(g_lock);
            job->file->kept(job->index, ok);
            g_pending.pop_front();
            continue;
        }
        for (const auto &m : maps)
            write_chunk(m.first, m.second.data(), m.second.size());
    }
}

// Background fetching: the pack first, then two threads that serve the
// read-ahead, the hot list and the fill for the rest of the run.
void prefetch_main() {
    std::thread(keeper).detach();
    fetch_pack();
    {
        // What is already here, for the page's chest, and how much of the hot
        // list is missing. Every file's chunk map is read once here.
        std::lock_guard<std::mutex> hold(g_lock);
        uint64_t kept = 0, hot = 0;
        for (RemoteFile *f : g_fill)
            kept += f->kept_bytes();
        for (const HotItem &h : g_hot)
            hot += h.file->missing_bytes(h.first, h.last);
        stat_set(S_KEPT, (double)kept);
        stat_set(S_KEPT_KNOWN, 1);
        stat_set(S_HOT_TOTAL, (double)hot);
    }
    std::thread(prefetch_worker).detach();
    prefetch_worker();
}

void prefetch_worker() {
    std::vector<uint8_t> run;
    const double started = emscripten_get_now();
    for (;;) {
        // The game's own request comes first.
        while (g_readers_waiting.load() > 0)
            emscripten_thread_sleep(10);
        Job job;
        bool got = false;
        {
            std::lock_guard<std::mutex> hold(g_lock);
            got = take_from(g_ahead, &job, 0);
            // Background work only in a quiet moment: the game's reads come
            // in bursts, and a request started inside one sits in front of
            // the next read.
            const int fill_mode = g_fill_mode.load();
            if (!got && (fill_mode == 2 || emscripten_get_now() - g_last_read_fetch.load() >= QUIET_MS)) {
                got = take_hot(&job);
                if (!got && !g_hot_done.load())
                    stat_set(S_HOT_DONE, g_stats[S_HOT_TOTAL].load());
                if (!got && !g_hot_done.exchange(true) && !g_hot.empty())
                    fprintf(stderr, "[remote] hot list ready: %.1f MB fetched ahead in %.1f s; the "
                                    "game waited %.1f s on the network so far\n",
                            g_prefetched.load() / 1e6, (emscripten_get_now() - started) / 1000.0,
                            g_wait_ms / 1000.0);
                // Eager (the page's "fetch it all now") runs two at a time and
                // does not wait for the game to be quiet.
                if (!got && fill_mode && g_fill_active < (fill_mode == 2 ? 2 : 1) &&
                    (fill_mode == 2 || emscripten_get_now() - g_last_read_fetch.load() >= FILL_QUIET_MS)) {
                    got = take_fill(&job);
                    if (!got && !g_fill_active)
                        stat_set(S_FILL_DONE, 1);
                    if (!got && !g_fill_active && !g_fill_done.exchange(true))
                        fprintf(stderr, "[remote] the whole game is in this browser now (%.1f MB "
                                        "filled in the background)\n", g_filled.load() / 1e6);
                }
            }
            if (got) {
                mark(job, true);
                g_fill_active += job.kind == 2;
            }
        }
        if (!got) {
            emscripten_thread_sleep(20);
            continue;
        }
        const uint64_t bytes = job.file->run_bytes(job.at, job.end);
        run.resize(bytes);
        const std::string url = g_base_url + "/" + job.file->rel();
        const double from = (double)(job.at * CHUNK);
        const double fetch_started = emscripten_get_now();
        const bool ok = remote_fetch_range(url.c_str(), from, from + (double)bytes - 1, run.data()) == (double)bytes;
        const double fetch_ms = emscripten_get_now() - fetch_started;
        if (tracing())
            fprintf(stderr, "[remote] background %s %s chunks %llu-%llu: %.0f ms\n",
                    job.kind == 0 ? "read-ahead" : job.kind == 1 ? "hot" : "fill", job.file->rel().c_str(),
                    (unsigned long long)job.at, (unsigned long long)job.end, fetch_ms);
        {
            std::lock_guard<std::mutex> hold(g_lock);
            if (ok)
                job.file->store(job.at, run, job.kind == 0);
            mark(job, false);
            g_fill_active -= job.kind == 2;
        }
        g_landed.notify_all();
        if (!ok) {
            emscripten_thread_sleep(1000); // a failing server is not asked again at once
            continue;
        }
        (job.kind == 2 ? g_filled : g_prefetched) += bytes;
        stat_add(S_FETCHED, (double)bytes);
        if (job.kind == 1)
            stat_add(S_HOT_DONE, (double)bytes);
        static std::atomic<uint64_t> reported{0};
        const uint64_t filled = g_filled.load();
        if (job.kind == 2 && filled - reported.load() >= (64u << 20)) {
            reported = filled;
            fprintf(stderr, "[remote] background fill: %.0f MB kept so far\n", filled / 1e6);
        }
    }
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
        if (line[0] == '#') {
            // "#pack\t<name>\t<size>"
            char *name = strchr(line, '\t');
            char *size = name ? strchr(name + 1, '\t') : nullptr;
            if (!strncmp(line, "#pack\t", 6) && size) {
                g_pack_name.assign(name + 1, size - name - 1);
                g_pack_size = strtoull(size + 1, nullptr, 10);
            }
            continue;
        }
        char *tab1 = strchr(line, '\t');
        char *tab2 = tab1 ? strchr(tab1 + 1, '\t') : nullptr;
        if (!tab2)
            continue;
        const uint64_t size = strtoull(line, nullptr, 10), stamp = strtoull(tab1 + 1, nullptr, 10);
        std::string path = tab2 + 1;
        if (!path.empty() && path.back() == '\r')
            path.pop_back();
        // A fourth column: where the file sits in the pack.
        int64_t pack_offset = -1;
        const size_t tab3 = path.find('\t');
        if (tab3 != std::string::npos) {
            pack_offset = strtoll(path.c_str() + tab3 + 1, nullptr, 10);
            path.resize(tab3);
        }
        auto dir = backend->root;
        size_t at = 0;
        for (size_t slash; (slash = path.find('/', at)) != std::string::npos; at = slash + 1)
            dir = dir->subdir(path.substr(at, slash - at));
        auto file = std::make_shared<RemoteFile>(path, size, stamp, id);
        by_path[path] = file.get();
        if (pack_offset >= 0 && size)
            g_pack.push_back({file.get(), (uint64_t)pack_offset, size});
        dir->add(path.substr(at), file);
        ++files;
        bytes += size;
    }
    free(manifest);
    fprintf(stderr, "[remote] %s: %zu files, %.0f MB, streamed from %s\n", mount, files, bytes / 1e6,
            g_base_url.c_str());
    char *hot = remote_fetch_text((g_base_url + "/hot.txt").c_str());
    if (hot) {
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
        fprintf(stderr, "[remote] fetching %zu hot ranges ahead\n", g_hot.size());
    }
    // The fill order: the files read here and there first (a miss in them is
    // a wait in the middle of play), the movies, which play front to back
    // and are read ahead anyway, last.
    if (const char *fill = getenv("RECOMP_REMOTE_FILL"); fill && *fill)
        g_fill_mode = atoi(fill) < 0 ? 0 : atoi(fill) > 2 ? 2 : atoi(fill);
    stat_set(S_FILL_MODE, g_fill_mode.load());
    stat_set(S_GAME_TOTAL, (double)bytes);
    stat_set(S_MOUNTED, 1);
    for (auto &entry : by_path)
        g_fill.push_back(entry.second);
    std::stable_sort(g_fill.begin(), g_fill.end(), [](RemoteFile *a, RemoteFile *b) {
        const bool am = a->rel().find("movies") != std::string::npos;
        const bool bm = b->rel().find("movies") != std::string::npos;
        if (am != bm)
            return bm;
        return a->size() < b->size();
    });
    std::thread(prefetch_main).detach();
    return files > 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE double *recomp_web_stream_stats(void) {
    static_assert(sizeof(std::atomic<double>) == sizeof(double), "the page reads these as doubles");
    return reinterpret_cast<double *>(g_stats);
}

extern "C" EMSCRIPTEN_KEEPALIVE void recomp_web_stream_fill(int mode) {
    mode = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    g_fill_mode = mode;
    stat_set(S_FILL_MODE, mode);
}
#endif
