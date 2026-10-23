// web_sink.cpp - the mixer's output in the browser, off the page's main thread.
//
// SDL's Emscripten backend plays through a ScriptProcessorNode, whose callback
// runs on the main thread: the whole mix rendered there, between the frames
// the page presents and the input it delivers. A slow mix delayed a frame, a
// busy main thread starved the sound, and the render lock the game's thread
// also takes was waited for by spinning, since the main thread cannot block.
//
// Here a thread of our own renders into a ring in the module's shared memory,
// and an AudioWorklet on the browser's audio thread copies from it. Neither
// waits for the other: an empty ring plays silence and is counted, a full one
// makes the renderer sleep.
#include "sink.h"

#include <emscripten.h>
#include <emscripten/threading.h>

#include <atomic>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <vector>

#include "../../platform/os.h"

namespace {

// Shared with the worklet, which reads it through the module's memory. The
// positions count frames since the start and wrap at 2^32; the ring index is
// the position modulo kCapacity.
constexpr uint32_t kCapacity = 16384; // frames: 340 ms at 48 kHz, room for any ahead setting
constexpr uint32_t kChunk = 256;      // frames rendered per mixer call
constexpr uint32_t kMaxAhead = 6144;  // the most the backlog grows to after underruns
struct Ring {
    std::atomic<uint32_t> write; // advanced by the renderer
    std::atomic<uint32_t> read;  // advanced by the worklet
    std::atomic<uint32_t> underruns; // worklet quanta that found too little
    std::atomic<uint32_t> quanta;    // worklet quanta played
    float frames[kCapacity * 2];     // interleaved stereo
};
static_assert(sizeof(std::atomic<uint32_t>) == 4, "the worklet reads these as Int32");

// The worklet, as a module the page loads from a blob. It reads the ring
// through the module's SharedArrayBuffer; growing the memory never moves what
// is already there, so the buffer it was handed keeps covering the ring.
EM_JS(void, web_audio_open, (double rate, void *ring, uint32_t capacity), {
    if (Module.recompAudio)
        return;
    const state = { ctx: null, node: null };
    Module.recompAudio = state;
    const source = `
class RecompRing extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions;
    this.i32 = new Int32Array(o.memory, o.ring, 4);
    this.f32 = new Float32Array(o.memory, o.ring + 16, o.capacity * 2);
    this.capacity = o.capacity;
  }
  process(inputs, outputs) {
    const out = outputs[0], l = out[0], r = out[1] || out[0], n = l.length;
    const write = Atomics.load(this.i32, 0), read = Atomics.load(this.i32, 1);
    const avail = (write - read) >>> 0;
    const take = avail < n ? avail : n;
    let at = read % this.capacity;
    for (let i = 0; i < take; ++i) {
      l[i] = this.f32[at * 2];
      r[i] = this.f32[at * 2 + 1];
      if (++at === this.capacity) at = 0;
    }
    for (let i = take; i < n; ++i) l[i] = r[i] = 0;
    Atomics.store(this.i32, 1, (read + take) | 0);
    Atomics.add(this.i32, 3, 1);
    if (take < n) Atomics.add(this.i32, 2, 1);
    return true;
  }
}
registerProcessor("recomp-ring", RecompRing);`;
    let ctx;
    try {
        ctx = new AudioContext({ sampleRate: rate, latencyHint: "interactive" });
    } catch (e) {
        ctx = new AudioContext({ latencyHint: "interactive" });
        console.warn("[host] audio: the browser would not run at " + rate + " Hz; it resamples from " +
                     ctx.sampleRate + " Hz");
    }
    state.ctx = ctx;
    // Browsers start a context suspended until the page has been interacted
    // with; any later gesture resumes it.
    const resume = () => { if (state.ctx && state.ctx.state === "suspended" && !state.paused) state.ctx.resume(); };
    for (const type of ["pointerdown", "keydown", "touchend"])
        window.addEventListener(type, resume, { capture: true });
    const url = URL.createObjectURL(new Blob([source], { type: "text/javascript" }));
    ctx.audioWorklet.addModule(url).then(() => {
        URL.revokeObjectURL(url);
        state.node = new AudioWorkletNode(ctx, "recomp-ring", {
            numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
            processorOptions: { memory: HEAPU8.buffer, ring: ring, capacity: capacity },
        });
        state.node.connect(ctx.destination);
        resume();
        console.log("[host] audio worklet running: " + ctx.sampleRate + " Hz, base latency " +
                    Math.round((ctx.baseLatency || 0) * 1000) + " ms");
    }).catch((e) => console.error("[host] audio: the worklet would not start: " + e));
});

EM_JS(void, web_audio_pause, (int paused), {
    const state = Module.recompAudio;
    if (!state || !state.ctx)
        return;
    state.paused = !!paused;
    if (paused)
        state.ctx.suspend();
    else
        state.ctx.resume();
});

EM_JS(void, web_audio_close, (), {
    const state = Module.recompAudio;
    if (!state)
        return;
    if (state.node)
        state.node.disconnect();
    if (state.ctx)
        state.ctx.close();
    Module.recompAudio = null;
});

// Plain functions for the main-thread queue, which takes a function pointer.
void open_on_main(int rate, void *ring, uint32_t capacity) {
    web_audio_open(rate, ring, capacity);
}
void pause_on_main(int paused) {
    web_audio_pause(paused);
}
void close_on_main() {
    web_audio_close();
}

class WebSink final : public AudioSink {
  public:
    ~WebSink() override {
        stop();
    }
    bool start(double rate, std::function<void(float *, float *, uint32_t)> render) override {
        if (running_.load())
            return true;
        if (!emscripten_has_threading_support())
            return false;
        render_ = std::move(render);
        rate_ = rate;
        // The backlog kept rendered ahead of the worklet, as in the SDL sink.
        ahead_ = 2048;
        if (const char *ahead = recomp_env("AUDIO_AHEAD"); ahead && *ahead)
            ahead_ = uint32_t(strtoul(ahead, nullptr, 10));
        if (ahead_ < kChunk)
            ahead_ = kChunk;
        if (ahead_ > kCapacity - kChunk)
            ahead_ = kCapacity - kChunk;
        ring_ = new Ring();
        ring_->write.store(0);
        ring_->read.store(0);
        ring_->underruns.store(0);
        ring_->quanta.store(0);
        running_.store(true);
        paused_.store(false);
        // Render the first backlog before the worklet asks for anything.
        fill();
        emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VIII, (void *)open_on_main, int(rate),
                                                    (void *)ring_, kCapacity);
        thread_ = std::thread([this] { loop(); });
        printf("[host] audio: mixer %.0f Hz on its own thread, %u frames ahead, AudioWorklet output\n",
               rate, ahead_);
        fflush(stdout);
        return true;
    }
    void pause(bool paused) override {
        if (!running_.load())
            return;
        paused_.store(paused);
        emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VI, (void *)pause_on_main,
                                                    paused ? 1 : 0);
    }
    void stop() override {
        if (!running_.exchange(false))
            return;
        if (thread_.joinable())
            thread_.join();
        emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_V, (void *)close_on_main);
        report();
        // The ring stays: the worklet may still be reading it until the
        // context closes, and a sink is stopped once per run.
    }
    bool running() const override {
        return running_.load();
    }

  private:
    // Top the ring up to the backlog. Returns whether anything was rendered.
    bool fill() {
        bool any = false;
        for (;;) {
            const uint32_t write = ring_->write.load(std::memory_order_relaxed);
            const uint32_t read = ring_->read.load(std::memory_order_acquire);
            if (write - read >= ahead_)
                return any;
            const auto t0 = std::chrono::steady_clock::now();
            left_.assign(kChunk, 0.0f);
            right_.assign(kChunk, 0.0f);
            render_(left_.data(), right_.data(), kChunk);
            uint32_t at = write % kCapacity;
            for (uint32_t i = 0; i < kChunk; ++i) {
                ring_->frames[at * 2] = left_[i];
                ring_->frames[at * 2 + 1] = right_[i];
                if (++at == kCapacity)
                    at = 0;
            }
            ring_->write.store(write + kChunk, std::memory_order_release);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            render_ms_ += ms;
            if (ms > max_render_ms_)
                max_render_ms_ = ms;
            ++renders_;
            any = true;
        }
    }
    void loop() {
        auto last_report = std::chrono::steady_clock::now();
        uint32_t underruns_seen = 0;
        while (running_.load()) {
            // A worklet that ran dry means this thread was held up longer
            // than the backlog lasts: keep more of it, up to 128 ms at 48 kHz.
            const uint32_t underruns = ring_->underruns.load(std::memory_order_relaxed);
            if (underruns != underruns_seen) {
                underruns_seen = underruns;
                if (ahead_ + 512 <= kMaxAhead) {
                    ahead_ += 512;
                    printf("[host] audio: the output ran dry; keeping %u frames ahead now\n", ahead_);
                    fflush(stdout);
                }
            }
            if (!paused_.load())
                fill();
            // A quantum is 128 frames (2.7 ms at 48 kHz); waking every 4 ms
            // keeps the ring near the backlog without spinning.
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= std::chrono::seconds(30)) {
                last_report = now;
                report();
            }
        }
    }
    void report() {
        const uint32_t quanta = ring_ ? ring_->quanta.load() : 0;
        const uint32_t underruns = ring_ ? ring_->underruns.load() : 0;
        printf("[host] audio sink: %u quanta played, %u short (underrun), %llu renders of %u frames, "
               "max render %.1f ms, mean render %.2f ms, ahead %u frames\n",
               quanta, underruns, (unsigned long long)renders_, kChunk, max_render_ms_,
               renders_ ? render_ms_ / double(renders_) : 0.0, ahead_);
        fflush(stdout);
    }

    Ring *ring_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false}, paused_{false};
    std::function<void(float *, float *, uint32_t)> render_;
    std::vector<float> left_, right_;
    double rate_ = 48000, render_ms_ = 0, max_render_ms_ = 0;
    uint64_t renders_ = 0;
    uint32_t ahead_ = 2048;
};

} // namespace

std::unique_ptr<AudioSink> make_web_sink() {
    return std::make_unique<WebSink>();
}
