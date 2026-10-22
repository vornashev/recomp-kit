// web_remote_fs.h - mount the game's files from the server, streamed and cached.
#pragma once

// Mounts <base_url>/manifest.txt's tree read-only at `mount`. A read fetches
// the 1 MB chunks it needs by range request (call from a worker thread) and
// keeps them under `cache_dir` (OPFS) for later visits; null keeps nothing.
bool web_remote_mount(const char *base_url, const char *mount, const char *cache_dir);
