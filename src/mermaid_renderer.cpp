#include "mermaid_renderer.hpp"

#include <gio/gio.h>
#include <glib.h>
#include <glib-unix.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <list>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace slate {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
constexpr std::size_t max_message = 4 * 1024 * 1024;
constexpr std::size_t cache_bytes = 4 * 1024 * 1024;
constexpr std::size_t max_source = 16 * 1024;

template<class T> using GObjectPtr = std::unique_ptr<T, decltype(&g_object_unref)>;
using GString = std::unique_ptr<char, decltype(&g_free)>;

struct SocketPair {
    int local = -1, child = -1;

    bool open() {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return false;
        local = pair[0];
        child = pair[1];
        if (fcntl(local, F_SETFD, FD_CLOEXEC) < 0 || fcntl(child, F_SETFD, FD_CLOEXEC) < 0) return false;
        if (!g_unix_set_fd_nonblocking(local, TRUE, nullptr)) return false;
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        if (setsockopt(local, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) return false;
#endif
        return true;
    }

    ~SocketPair() {
        if (local != -1) close(local);
        if (child != -1) close(child);
    }
};

std::size_t json_end(std::string_view value, std::size_t pos) {
    if (pos == value.size()) return pos;
    const auto first = value[pos++];
    if (first == '"' || first == '{' || first == '[') {
        unsigned depth = first != '"';
        bool quoted = first == '"';
        while (pos < value.size()) {
            const auto c = value[pos++];
            if (quoted) {
                if (c == '\\' && pos < value.size()) ++pos;
                else if (c == '"') {
                    if (!depth) return pos;
                    quoted = false;
                }
            } else if (c == '"') quoted = true;
            else if (c == '{' || c == '[') ++depth;
            else if ((c == '}' || c == ']') && --depth == 0) return pos;
        }
        return pos;
    }
    while (pos < value.size() && value[pos] != ',' && value[pos] != '}' && value[pos] != ']' &&
           !g_ascii_isspace(value[pos])) ++pos;
    return pos;
}

// The pipe only accepts Chromium's protocol replies. The few string fields
// we read are generated identifiers, dimensions, or Base64 and need no unescaping.
std::string_view json_field(std::string_view object, std::string_view key) {
    if (!object.starts_with('{')) return {};
    std::size_t pos = 1;
    auto skip_space = [&] { while (pos < object.size() && g_ascii_isspace(object[pos])) ++pos; };
    for (;;) {
        skip_space();
        if (pos >= object.size() || object[pos] != '"') return {};
        const auto key_end = json_end(object, pos);
        if (key_end - pos < 2) return {};
        const auto name = object.substr(pos + 1, key_end - pos - 2);
        pos = key_end;
        skip_space();
        if (pos >= object.size() || object[pos++] != ':') return {};
        skip_space();
        const auto value_end = json_end(object, pos);
        if (name == key) return object.substr(pos, value_end - pos);
        pos = value_end;
        skip_space();
        if (pos >= object.size() || object[pos++] != ',') return {};
    }
}

std::string_view json_string(std::string_view quoted) {
    return quoted.size() >= 2 && quoted.starts_with('"') && quoted.ends_with('"')
        ? quoted.substr(1, quoted.size() - 2) : std::string_view{};
}

std::string json_quote(std::string_view value) {
    std::string quoted = "\"";
    for (const auto ch : value) {
        if (ch == '\\' || ch == '"') quoted += '\\';
        quoted += ch;
    }
    quoted += '"';
    return quoted;
}

std::string find_browser() {
    if (const auto* override = g_getenv("SLATE_CHROME"); override && *override) return override;
    for (const auto* name : {"chrome-headless-shell", "chromium", "google-chrome", "chromium-browser"}) {
        const GString path(g_find_program_in_path(name), &g_free);
        if (path) return path.get();
    }
#ifdef __APPLE__
    for (const auto* candidate : {"/Applications/Chromium.app/Contents/MacOS/Chromium",
                                  "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"})
        if (access(candidate, X_OK) == 0) return candidate;
#endif
    return {};
}

fs::path installed_asset_dir() {
    std::error_code error;
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (!size) return {};
    std::string executable(size, '\0');
    if (_NSGetExecutablePath(executable.data(), &size) != 0) return {};
    const auto path = fs::canonical(executable.c_str(), error);
#else
    const auto path = fs::read_symlink("/proc/self/exe", error);
#endif
    if (error) return {};
    return path.parent_path().parent_path() / "share/slate/mermaid";
}

std::string host_uri() {
    for (const auto& dir : {installed_asset_dir(), fs::path(SLATE_MERMAID_BUILD_DATA),
                            fs::path(SLATE_MERMAID_INSTALL_DATA)}) {
        if (dir.empty()) continue;
        const auto path = dir / "preview.html";
        if (access(path.c_str(), R_OK) != 0) continue;
        const GString uri(g_filename_to_uri(path.c_str(), nullptr, nullptr), &g_free);
        if (uri) return uri.get();
    }
    return {};
}

class Browser {
public:
    ~Browser() {
        if (process_) {
            g_subprocess_force_exit(process_.get());
            g_subprocess_wait(process_.get(), nullptr, nullptr);
        }
        if (profile_) {
            std::error_code error;
            fs::remove_all(profile_.get(), error);
        }
    }

    bool start(const std::function<bool()>& cancelled) {
        const auto executable = find_browser();
        const auto host = host_uri();
        if (executable.empty() || host.empty() || !command_.open() || !reply_.open()) return false;
        profile_.reset(g_dir_make_tmp("slate-mermaid-XXXXXX", nullptr));
        if (!profile_) return false;
        const auto user_data = std::string("--user-data-dir=") + profile_.get();
        const char* args[]{
            executable.c_str(), "--headless", "--no-startup-window", "--remote-debugging-pipe",
            "--disable-background-networking", "--disable-component-update", "--disable-sync",
            "--disable-extensions", "--disable-crash-reporter", "--disable-gpu",
            "--no-first-run", "--no-default-browser-check", "--use-mock-keychain", "--password-store=basic",
            user_data.c_str(), nullptr
        };
        GObjectPtr<GSubprocessLauncher> launcher(
            g_subprocess_launcher_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE)),
            &g_object_unref);
        g_subprocess_launcher_take_fd(launcher.get(), std::exchange(command_.child, -1), 3);
        g_subprocess_launcher_take_fd(launcher.get(), std::exchange(reply_.child, -1), 4);
        process_.reset(g_subprocess_launcher_spawnv(launcher.get(), args, nullptr));
        // Close the parent copies of the child's descriptors before waiting for replies.
        launcher.reset();
        if (!process_) return false;
        const auto deadline = Clock::now() + 6s;
        auto response = call("Target.createTarget", "{\"url\":" + json_quote(host) + "}", deadline, cancelled);
        auto target = json_string(json_field(json_field(response, "result"), "targetId"));
        if (target.empty()) return false;
        response = call("Target.attachToTarget", "{\"flatten\":true,\"targetId\":" + json_quote(target) + "}",
                        deadline, cancelled);
        session_.assign(json_string(json_field(json_field(response, "result"), "sessionId")));
        if (session_.empty()) return false;
        response = call("Emulation.setDeviceMetricsOverride",
                        R"({"width":720,"height":540,"deviceScaleFactor":1,"mobile":false})", deadline, cancelled);
        if (json_field(response, "result").empty()) return false;
        while (Clock::now() < deadline && !cancelled()) {
            response = evaluate("typeof slateRender", deadline, cancelled);
            if (json_string(value(response)) == "function") return true;
            if (response.empty()) return false;
            g_usleep(15000);
        }
        return false;
    }

    MermaidResult render(std::string_view source, const std::function<bool()>& cancelled) {
        const GString encoded(g_base64_encode(reinterpret_cast<const guchar*>(source.data()), source.size()), &g_free);
        const std::string script = "slateRender(new TextDecoder().decode(Uint8Array.from(atob('" +
            std::string(encoded.get()) + "'), c => c.charCodeAt(0))))";
        const auto deadline = Clock::now() + 2500ms;
        const auto response = evaluate(script, deadline, cancelled);
        if (cancelled()) return {{}, MermaidStatus::Cancelled};
        if (response.empty() || value(response).empty()) return {};
        auto png = json_string(value(response));
        if (png.empty()) return {{}, MermaidStatus::Invalid};
        std::string shot;
        if (const auto comma = png.find(','); comma != png.npos) {
            int width = 0, height = 0;
            const auto first = png.data(), last = first + png.size();
            if (std::from_chars(first, first + comma, width).ptr != first + comma ||
                std::from_chars(first + comma + 1, last, height).ptr != last ||
                width < 1 || width > 720 || height < 1 || height > 540) return {};
            shot = call("Page.captureScreenshot",
                "{\"format\":\"png\",\"clip\":{\"x\":0,\"y\":0,\"width\":" + std::to_string(width) +
                ",\"height\":" + std::to_string(height) + ",\"scale\":1}}", deadline, cancelled);
            if (cancelled()) return {{}, MermaidStatus::Cancelled};
            png = json_string(json_field(json_field(shot, "result"), "data"));
        }
        if (png.empty()) return {};
        gsize size = 0;
        const std::unique_ptr<guchar, decltype(&g_free)> bytes(
            g_base64_decode(std::string(png).c_str(), &size), &g_free);
        const GObjectPtr<GInputStream> stream(g_memory_input_stream_new_from_data(bytes.get(), size, nullptr), &g_object_unref);
        auto* pixbuf = gdk_pixbuf_new_from_stream(stream.get(), nullptr, nullptr);
        if (!pixbuf) return {};
        return {MermaidImage(pixbuf, [](GdkPixbuf* image) { g_object_unref(image); }), MermaidStatus::Ready};
    }

private:
    using Deadline = Clock::time_point;
    SocketPair command_, reply_;
    GObjectPtr<GSubprocess> process_{nullptr, &g_object_unref};
    GString profile_{nullptr, &g_free};
    std::string session_, buffer_;
    unsigned id_ = 0;
    std::size_t consumed_ = 0;

    static bool wait_for(int fd, short events, Deadline deadline, const std::function<bool()>& cancelled) {
        pollfd descriptor{fd, events, 0};
        while (!cancelled() && Clock::now() < deadline) {
            auto status = poll(&descriptor, 1, 25);
            if (status > 0) return descriptor.revents & events;
            if (status < 0 && errno != EINTR) return false;
        }
        return false;
    }

    std::string call(std::string_view method, std::string params, Deadline deadline,
                     const std::function<bool()>& cancelled) {
        const auto id = std::to_string(++id_);
        std::string request = "{\"id\":" + id + ",\"method\":" + json_quote(method) + ",\"params\":" + params;
        if (!session_.empty()) request += ",\"sessionId\":" + json_quote(session_);
        request.append("}\0", 2);
        for (std::size_t written = 0; written < request.size();) {
            if (!wait_for(command_.local, POLLOUT, deadline, cancelled)) return {};
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            const auto sent = send(command_.local, request.data() + written, request.size() - written, flags);
            if (sent < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (sent <= 0) return {};
            written += sent;
        }
        std::size_t scanned = consumed_;
        while (!cancelled() && Clock::now() < deadline) {
            if (auto separator = buffer_.find('\0', scanned); separator != buffer_.npos) {
                const auto packet = std::string_view(buffer_).substr(consumed_, separator - consumed_);
                consumed_ = scanned = separator + 1;
                if (json_field(packet, "id") == id) return std::string(packet);
                continue;
            }
            if (consumed_) {
                buffer_.erase(0, consumed_);
                consumed_ = 0;
            }
            scanned = buffer_.size();
            if (scanned >= max_message || !wait_for(reply_.local, POLLIN, deadline, cancelled)) return {};
            std::array<char, 8192> chunk;
            const auto count = recv(reply_.local, chunk.data(), chunk.size(), 0);
            if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (count <= 0) return {};
            buffer_.append(chunk.data(), count);
        }
        return {};
    }

    std::string evaluate(std::string_view expression, Deadline deadline, const std::function<bool()>& cancelled) {
        return call("Runtime.evaluate", "{\"awaitPromise\":true,\"returnByValue\":true,\"expression\":" +
                    json_quote(expression) + "}", deadline, cancelled);
    }

    static std::string_view value(std::string_view response) {
        return json_field(json_field(json_field(response, "result"), "result"), "value");
    }
};
} // namespace

struct MermaidRenderer::Impl {
    struct Entry { std::string source; MermaidResult result; std::size_t bytes; };
    std::list<Entry> cache;
    std::size_t total = 0;
    std::unique_ptr<Browser> browser;
    Clock::time_point retry_after{};
};

MermaidRenderer::MermaidRenderer() : impl_(std::make_unique<Impl>()) {}
MermaidRenderer::~MermaidRenderer() = default;
bool MermaidRenderer::browser_running() const { return bool(impl_->browser); }
void MermaidRenderer::release_browser() { impl_->browser.reset(); }

MermaidResult MermaidRenderer::render(std::string_view source, const std::function<bool()>& cancelled) {
    if (cancelled()) return {{}, MermaidStatus::Cancelled};
    if (source.empty() || source.size() > max_source) return {{}, MermaidStatus::Invalid};
    auto& state = *impl_;
    const auto hit = std::find_if(state.cache.begin(), state.cache.end(), [&](const auto& entry) { return entry.source == source; });
    if (hit != state.cache.end()) {
        state.cache.splice(state.cache.begin(), state.cache, hit);
        return state.cache.front().result;
    }
    if (!state.browser) {
        if (Clock::now() < state.retry_after) return {};
        state.browser = std::make_unique<Browser>();
        if (!state.browser->start(cancelled)) {
            state.browser.reset();
            if (cancelled()) return {{}, MermaidStatus::Cancelled};
            state.retry_after = Clock::now() + 5s;
            return {};
        }
    }
    auto result = state.browser->render(source, cancelled);
    if (result.status == MermaidStatus::Unavailable || result.status == MermaidStatus::Cancelled) {
        state.browser.reset(); // A timed-out or superseded script may still be running.
        return result;
    }
    auto bytes = source.size();
    if (result.image) bytes += gdk_pixbuf_get_byte_length(result.image.get());
    while (state.total + bytes > cache_bytes || state.cache.size() >= 24) {
        state.total -= state.cache.back().bytes;
        state.cache.pop_back();
    }
    state.cache.push_front({std::string(source), result, bytes});
    state.total += bytes;
    return result;
}

} // namespace slate
