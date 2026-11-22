#include "mermaid_renderer.hpp"

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <glib.h>

#if SLATE_HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <list>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace slate {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
constexpr std::size_t max_output = 2 * 1024 * 1024;
constexpr std::size_t cache_bytes = 4 * 1024 * 1024;
constexpr int max_width = 720;
constexpr int max_height = 540;

template<class T> using GObjectPtr = std::unique_ptr<T, decltype(&g_object_unref)>;
using GString = std::unique_ptr<char, decltype(&g_free)>;

std::string renderer_path() {
    if (const auto* override = g_getenv("SLATE_MERMAN"); override && *override) {
        const GString path(g_find_program_in_path(override), &g_free);
        return path ? std::string(path.get()) : std::string{};
    }
    const GString path(g_find_program_in_path("merman-cli"), &g_free);
    if (path) return path.get();
    for (auto candidate : {fs::path(g_get_home_dir()) / ".local/bin/merman-cli",
                           fs::path("/opt/homebrew/bin/merman-cli"), fs::path("/usr/local/bin/merman-cli")})
        if (access(candidate.c_str(), X_OK) == 0) return candidate.string();
    return {};
}

#if SLATE_HAVE_FONTCONFIG
std::string font_file(const char* family, int weight) {
    using PatternPtr = std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)>;
    PatternPtr query(FcNameParse(reinterpret_cast<const FcChar8*>(family)), &FcPatternDestroy);
    if (!query) return {};
    FcPatternAddInteger(query.get(), FC_WEIGHT, weight);
    FcConfigSubstitute(nullptr, query.get(), FcMatchPattern);
    FcDefaultSubstitute(query.get());
    FcResult result;
    PatternPtr match(FcFontMatch(nullptr, query.get(), &result), &FcPatternDestroy);
    FcChar8* file = nullptr;
    if (match && FcPatternGetString(match.get(), FC_FILE, 0, &file) == FcResultMatch)
        return reinterpret_cast<const char*>(file);
    return {};
}
#endif

std::vector<std::string> default_fonts() {
    std::vector<std::string> fonts;
#if SLATE_HAVE_FONTCONFIG
    // Explicit font files skip a system-wide discovery pass in each short-lived
    // render. Use the full system catalog for styled or non-ASCII diagrams.
    for (const auto& [family, weight] : std::array{
        std::pair{"Recursive Variable,Arial,sans-serif", FC_WEIGHT_REGULAR},
        std::pair{"Recursive Variable,Arial,sans-serif", FC_WEIGHT_BOLD},
        std::pair{"monospace", FC_WEIGHT_REGULAR},
        std::pair{"serif", FC_WEIGHT_REGULAR}}) {
        auto file = font_file(family, weight);
        if (!file.empty() && std::find(fonts.begin(), fonts.end(), file) == fonts.end())
            fonts.push_back(std::move(file));
    }
#endif
    return fonts;
}

bool uses_default_fonts(std::string_view source) {
    if (std::any_of(source.begin(), source.end(), [](unsigned char c) { return c > 127; })) return false;
    const GString lower(g_ascii_strdown(source.data(), source.size()), &g_free);
    const std::string_view text(lower.get());
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == text.npos ||
        (!text.substr(first).starts_with("---") &&
         text.find("%%{") == text.npos && text.find("font") == text.npos);
}

// The source is at most 16 KiB. Fill a local socket before launching the child
// to avoid an extra writer thread and to deliver EOF without a temp file.
int source_socket(std::string_view source) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return -1;
    auto fail = [&] { close(pair[0]); close(pair[1]); return -1; };
    if (fcntl(pair[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(pair[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(pair[0], F_SETFL, O_NONBLOCK) < 0)
        return fail();
    while (!source.empty()) {
        const auto count = send(pair[0], source.data(), source.size(), 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return fail();
        source.remove_prefix(count);
    }
    close(pair[0]);
    return pair[1];
}

struct Output {
    std::vector<guint8> png;
    MermaidStatus status = MermaidStatus::Unavailable;
};

Output run_renderer(const std::string& executable, std::string_view source,
                    const std::vector<std::string>* fonts, const std::function<bool()>& cancelled) {
    if (cancelled()) return {{}, MermaidStatus::Cancelled};
    const int input_fd = source_socket(source);
    if (input_fd < 0) return {};
    GObjectPtr<GSubprocessLauncher> launcher(
        g_subprocess_launcher_new(GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE)),
        &g_object_unref);
    g_subprocess_launcher_take_stdin_fd(launcher.get(), input_fd);
    std::vector<const char*> args{
        executable.c_str(), "render", "-", "-f", "png", "-o", "-", "-q", "-b", "white",
        "--font-system", fonts ? "off" : "platform",
        "--raster-fit-width", "720", "--raster-fit-height", "540",
        "--raster-max-width", "720", "--raster-max-height", "540",
        "--embedded-image-max-total-pixels", "1048576",
        "--resource-profile", "constrained", "--operation-timeout-ms", "1500"
    };
    if (fonts) for (const auto& font : *fonts) {
        args.push_back("--font-file");
        args.push_back(font.c_str());
    }
    args.push_back(nullptr);
    const GObjectPtr<GSubprocess> process(g_subprocess_launcher_spawnv(launcher.get(), args.data(), nullptr),
                                         &g_object_unref);
    launcher.reset();
    if (!process) return {};
    struct Stop {
        GSubprocess* process;
        ~Stop() {
            g_subprocess_force_exit(process);
            g_subprocess_wait(process, nullptr, nullptr);
        }
    } stop{process.get()};

    auto* stream = g_subprocess_get_stdout_pipe(process.get());
    pollfd descriptor{g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(stream)), POLLIN, 0};
    const auto deadline = Clock::now() + 2s;
    Output output;
    while (Clock::now() < deadline) {
        if (cancelled()) return {{}, MermaidStatus::Cancelled};
        const auto available = poll(&descriptor, 1, 20);
        if (available < 0 && errno == EINTR) continue;
        if (available < 0) return {};
        if (!available) continue;
        std::array<guint8, 8192> buffer;
        const auto count = read(descriptor.fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return {};
        if (!count) {
            output.status = output.png.empty() ? MermaidStatus::Invalid : MermaidStatus::Ready;
            return output;
        }
        if (output.png.size() + count > max_output) return {};
        output.png.insert(output.png.end(), buffer.data(), buffer.data() + count);
    }
    return {};
}

MermaidImage decode_png(const std::vector<guint8>& bytes) {
    if (bytes.size() < 24 || std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) != 0 ||
        std::memcmp(bytes.data() + 12, "IHDR", 4) != 0) return {};
    const auto be32 = [&](std::size_t i) {
        return (unsigned(bytes[i]) << 24) | (unsigned(bytes[i + 1]) << 16) |
               (unsigned(bytes[i + 2]) << 8) | bytes[i + 3];
    };
    const auto width = be32(16), height = be32(20);
    if (!width || !height || width > max_width || height > max_height) return {};
    const GObjectPtr<GInputStream> stream(g_memory_input_stream_new_from_data(bytes.data(), bytes.size(), nullptr),
                                           &g_object_unref);
    auto* pixbuf = gdk_pixbuf_new_from_stream(stream.get(), nullptr, nullptr);
    return MermaidImage(pixbuf, [](GdkPixbuf* image) { if (image) g_object_unref(image); });
}
} // namespace

bool mermaid_renderer_available() { return !renderer_path().empty(); }

struct MermaidRenderer::Impl {
    struct Entry { std::string source; MermaidResult result; std::size_t bytes; };
    std::list<Entry> cache;
    std::size_t total = 0;
    std::string executable = renderer_path();
    std::optional<std::vector<std::string>> font_files;
};

MermaidRenderer::MermaidRenderer() : impl_(std::make_unique<Impl>()) {}
MermaidRenderer::~MermaidRenderer() = default;

MermaidResult MermaidRenderer::render(std::string_view source, const std::function<bool()>& cancelled) {
    if (cancelled()) return {{}, MermaidStatus::Cancelled};
    if (source.empty() || source.size() > max_mermaid_source) return {{}, MermaidStatus::Invalid};
    auto& state = *impl_;
    const auto hit = std::find_if(state.cache.begin(), state.cache.end(), [&](const auto& entry) { return entry.source == source; });
    if (hit != state.cache.end()) {
        state.cache.splice(state.cache.begin(), state.cache, hit);
        return state.cache.front().result;
    }
    if (state.executable.empty()) return {};
    const std::vector<std::string>* fonts = nullptr;
    if (uses_default_fonts(source)) {
        if (!state.font_files) state.font_files = default_fonts();
        if (!state.font_files->empty()) fonts = &*state.font_files;
    }
    auto output = run_renderer(state.executable, source, fonts, cancelled);
    MermaidResult result{{}, output.status};
    if (output.status == MermaidStatus::Ready) {
        result.image = decode_png(output.png);
        if (!result.image) return {};
    } else if (output.status != MermaidStatus::Invalid) return result;
    auto size = source.size();
    if (result.image) size += gdk_pixbuf_get_byte_length(result.image.get());
    while (state.total + size > cache_bytes || state.cache.size() >= 24) {
        state.total -= state.cache.back().bytes;
        state.cache.pop_back();
    }
    state.cache.push_front({std::string(source), result, size});
    state.total += size;
    return result;
}

} // namespace slate
