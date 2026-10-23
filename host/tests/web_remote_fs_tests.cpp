// Exercise the actual browser cache/locking code in Wasm, replacing only HTTP.
// Including the implementation gives access to its private cache state; this
// executable never mounts a game or starts the perpetual background workers.
#include "../sdl/web_remote_fs.cpp"
#include <emscripten/atomic.h>
#include <future>
#include <chrono>
#include <functional>

static std::atomic<int> control[4]{}; // blocked fetch started, release, request count, fail
static int failures = 0;
#define CHECK(e)                                                                                   \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            ++failures;                                                                            \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #e);                                   \
        }                                                                                          \
    } while (0)

EM_JS(void, install_transport, (void *control), {
    const slots = control >> 2;
    globalThis.XMLHttpRequest = class {
        open(method, url) {
            this.url = url;
        }
        setRequestHeader(name, range) {
            this.range = range;
        }
        send() {
            const shared = HEAP32;
            if (this.url.endsWith("/blocked")) {
                Atomics.add(shared, slots + 2, 1);
                Atomics.store(shared, slots, 1);
                // Bounded even if a broken cache keeps the other reader locked.
                Atomics.wait(shared, slots + 1, 0, 3000);
                if (Atomics.load(shared, slots + 3)) {
                    this.status = 500;
                    return;
                }
            }
            const range = this.range.substring(6).split("-");
            const start = Number(range[0]), end = Number(range[1]);
            const data = new Uint8Array(end - start + 1);
            for (let i = 0; i < data.length; i++)
                data[i] = (start + i) & 255;
            this.response = data.buffer;
            this.status = 206;
        }
    };
});

static void reset_cache() {
    std::lock_guard lock(g_lock);
    g_ram.clear();
    g_pending.clear();
    g_pending_bytes = 0;
    g_inflight.clear();
    g_dirty_maps.clear();
    g_ahead.clear();
    g_base_url = "https://cache.test";
    g_cache_dir.clear();
    for (auto &v : control)
        v = 0;
}

static bool wait_for(const std::function<bool()> &ready) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!ready() && std::chrono::steady_clock::now() < end)
        emscripten_thread_sleep(1);
    return ready();
}

static std::future<ssize_t> read_async(RemoteFile &file, uint8_t *out) {
    return std::async(std::launch::async, [&file, out] {
        install_transport(control);
        return file.read(out, 1, 73);
    });
}

static void test_unrelated_read_progress() {
    reset_cache();
    RemoteFile slow("blocked", CHUNK, 0, nullptr), fast("fast", CHUNK, 0, nullptr);
    uint8_t a = 0, b = 0;
    auto first = read_async(slow, &a);
    CHECK(wait_for([] { return control[0].load() != 0; }));
    auto second = read_async(fast, &b);
    const bool progressed =
        second.wait_for(std::chrono::milliseconds(300)) == std::future_status::ready;
    control[1] = 1;
    emscripten_atomic_notify(control + 1, 1);
    CHECK(progressed); // A network stall must not hold the global cache lock.
    CHECK(first.get() == 1 && second.get() == 1);
    CHECK(a == 73 && b == 73);
}

static void test_same_chunk_waiters(bool fail) {
    reset_cache();
    control[3] = fail;
    RemoteFile file("blocked", CHUNK, 0, nullptr);
    uint8_t a = 0, b = 0;
    auto first = read_async(file, &a);
    CHECK(wait_for([] { return control[0].load() != 0; }));
    auto second = read_async(file, &b);
    CHECK(wait_for([] { return g_readers_waiting.load() == 2; }));
    control[1] = 1;
    emscripten_atomic_notify(control + 1, 1);
    CHECK(first.get() == (fail ? -EIO : 1));
    CHECK(second.get() == (fail ? -EIO : 1));
    if (!fail) {
        CHECK(a == 73 && b == 73);
        CHECK(control[2] == 1); // shared in-flight data is not fetched twice
    }
    CHECK(g_inflight.empty());
    CHECK(g_readers_waiting == 0);
}

static void test_bounded_pending_and_refill() {
    reset_cache();
    g_cache_dir = "/cache";
    const uint64_t chunks = MAX_PENDING_BYTES / CHUNK + 4;
    RemoteFile file("large", chunks * CHUNK, 0, nullptr);
    install_transport(control);
    for (uint64_t k = 0; k < chunks; ++k) {
        uint8_t byte = 0;
        CHECK(file.read(&byte, 1, off_t(k * CHUNK + 73)) == 1 && byte == 73);
        CHECK(g_pending_bytes <= MAX_PENDING_BYTES);
    }
    CHECK(g_pending_bytes == MAX_PENDING_BYTES);
    // The last read succeeded, though persistence was full. Background fill
    // must still discover that chunk after the keeper has room again.
    uint64_t at = chunks - 1, end = 0;
    CHECK(!file.next_missing(&at, chunks - 1, &end));
    at = chunks - 1;
    CHECK(file.next_missing(&at, chunks - 1, &end, PREFETCH_RUN, true));
    CHECK(at == chunks - 1);
    CHECK(!file.complete());
    file.kept(0, false); // storage failure is not a failure to read game data
    g_ram.clear();
    uint8_t byte = 0;
    CHECK(file.read(&byte, 1, off_t((chunks - 1) * CHUNK + 91)) == 1 && byte == 91);
}

static void test_cross_chunk_and_eof() {
    reset_cache();
    install_transport(control);
    RemoteFile file("short-tail", CHUNK + 17, 0, nullptr);
    uint8_t bytes[32]{};
    CHECK(file.read(bytes, sizeof bytes, CHUNK - 10) == 27);
    for (int i = 0; i < 27; ++i)
        CHECK(bytes[i] == uint8_t(CHUNK - 10 + i));
    CHECK(file.read(bytes, 1, CHUNK + 17) == 0);
    CHECK(g_inflight.empty());
}

int main() {
    test_unrelated_read_progress();
    test_same_chunk_waiters(false);
    test_same_chunk_waiters(true);
    test_bounded_pending_and_refill();
    test_cross_chunk_and_eof();
    printf("remote cache: %d failures\n", failures);
    return failures ? 1 : 0;
}
