/**
 * @file        ui/rex_app.cpp
 * @brief       ReXApp implementation — compiled as part of the consumer executable
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/rex_app.h>

#include <rex/cvar.h>
#include <rex/diag/crash_handler.h>
#include <rex/ui/flags.h>
#include <rex/kernel/crt/heap.h>
#include <rex/filesystem.h>
#include <rex/logging/sink.h>
#include <rex/logging.h>
#include <rex/ui/overlay/console_overlay.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/graphics/graphics_system.h>
#if REX_HAS_VULKAN
#include <rex/graphics/vulkan/graphics_system.h>
#endif
#if REX_HAS_D3D12
#include <rex/graphics/d3d12/graphics_system.h>
#endif
#include <native/audio/audio_system.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <native/ui/graphics_provider.h>
#include <rex/ui/keybinds.h>
#include <rex/version.h>

#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/system/util/xex2_info.h>

#include <fmt/format.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#if REX_PLATFORM_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <filesystem>

REXCVAR_DEFINE_STRING(user_data_root, "", "Runtime", "Override user data path");
REXCVAR_DEFINE_STRING(update_data_root, "", "Runtime", "Override update data path");

REXCVAR_DEFINE_BOOL(iso_direct, true, "Runtime",
                    "Allow mounting the game directly from a disc image (.iso). The loose "
                    "assets folder always takes priority when present; set false to ignore "
                    "disc images entirely.");
REXCVAR_DEFINE_STRING(game_iso, "", "Runtime",
                      "Path to the game's disc image (.iso). Empty = automatic: use the "
                      "assets folder if present, else scan for a matching .iso next to the "
                      "executable.");

REXCVAR_DEFINE_BOOL(dlc_containers, true, "Runtime",
                    "Mount DLC content containers (STFS/LIVE packages) directly from the dlc "
                    "folder and the content root, no extraction needed. Containers take "
                    "priority over extracted package folders of the same name.");
REXCVAR_DEFINE_STRING(dlc_dir, "", "Runtime",
                      "Override the DLC container folder. Empty = 'dlc' next to the "
                      "executable.");

REXCVAR_DEFINE_BOOL(use_shader_disk_cache, true, "GPU",
                    "Pre-compile the game's GPU pipelines from a persistent on-disk cache at "
                    "launch (during load) and record new ones, so pipelines do not compile "
                    "on-demand mid-gameplay - the first-encounter stutter (e.g. at mission "
                    "start). First run of a scene records the pipelines; later runs load them. "
                    "Cache lives in ./cache/shaders/.");

namespace rex {

namespace {

// A fatal setup problem the user must fix (missing/wrong game data). The log
// carries the details; the box exists so a double-clicked exe does not just
// silently vanish.
void ShowFatalMessageBox(const std::string& title, const std::string& message) {
#if REX_PLATFORM_WIN32
  auto wtitle = rex::string::to_utf16(title);
  auto wmessage = rex::string::to_utf16(message);
  MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(wmessage.c_str()),
              reinterpret_cast<const wchar_t*>(wtitle.c_str()), MB_OK | MB_ICONERROR);
#else
  (void)title;
  (void)message;
#endif
}

// Result of probing a candidate disc image: does it parse as a disc, does it
// contain a readable default.xex, and which title is it for.
struct ImageProbe {
  bool readable = false;
  uint32_t title_id = 0;
  std::string reason;
};

// Reads the title id out of a xex2 image's execution-info optional header.
// All offsets are validated against the buffer; a malformed header simply
// fails the probe instead of crashing.
bool ReadXexTitleId(const std::vector<uint8_t>& buf, uint32_t* out_title_id) {
  if (buf.size() < sizeof(xex2_header)) {
    return false;
  }
  const auto* header = reinterpret_cast<const xex2_header*>(buf.data());
  if (header->magic != 0x58455832) {  // 'XEX2'
    return false;
  }
  const uint32_t header_count = header->header_count;
  if (header_count > 1024 || 0x18 + size_t(header_count) * 8 > buf.size()) {
    return false;
  }
  for (uint32_t i = 0; i < header_count; i++) {
    const xex2_opt_header& opt = header->headers[i];
    if (opt.key != XEX_HEADER_EXECUTION_INFO) {
      continue;
    }
    const uint32_t offset = opt.offset;
    if (offset + sizeof(xex2_opt_execution_info) > buf.size()) {
      return false;
    }
    const auto* info = reinterpret_cast<const xex2_opt_execution_info*>(buf.data() + offset);
    *out_title_id = info->title_id;
    return true;
  }
  return false;
}

// Mounts a candidate image standalone (no VFS registration) and reads the
// title id from its default.xex.
ImageProbe ProbeGameImage(const std::filesystem::path& path) {
  ImageProbe probe;
  rex::filesystem::DiscImageDevice device("", path);
  if (!device.Initialize()) {
    probe.reason = "not a readable Xbox 360 disc image (or a damaged dump)";
    return probe;
  }
  auto* entry = device.ResolvePath("default.xex");
  if (!entry) {
    probe.reason = "image contains no default.xex";
    return probe;
  }
  rex::filesystem::File* file = nullptr;
  if (XFAILED(entry->Open(rex::filesystem::FileAccess::kFileReadData, &file)) || !file) {
    probe.reason = "could not open default.xex inside the image";
    return probe;
  }
  std::vector<uint8_t> buf(std::min<size_t>(entry->size(), 64 * 1024));
  size_t bytes_read = 0;
  auto status = file->ReadSync(std::span<uint8_t>(buf.data(), buf.size()), 0, &bytes_read);
  file->Destroy();
  if (XFAILED(status) || bytes_read != buf.size()) {
    probe.reason = "could not read default.xex inside the image";
    return probe;
  }
  uint32_t title_id = 0;
  if (!ReadXexTitleId(buf, &title_id)) {
    probe.reason = "default.xex inside the image has no readable title id";
    return probe;
  }
  probe.readable = true;
  probe.title_id = title_id;
  return probe;
}

// Scans a directory (non-recursive) for *.iso files, probes each in name
// order, and returns the first whose title id matches (any title if
// expected_title_id is 0). Rejects are logged, not fatal.
std::filesystem::path ScanForGameImage(const std::filesystem::path& dir,
                                       uint32_t expected_title_id) {
  std::vector<std::filesystem::path> candidates;
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(dir, ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) {
      continue;
    }
    auto ext = rex::path_to_utf8(it->path().extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".iso") {
      candidates.push_back(it->path());
    }
  }
  std::sort(candidates.begin(), candidates.end());
  for (const auto& candidate : candidates) {
    auto probe = ProbeGameImage(candidate);
    if (!probe.readable) {
      REXLOG_INFO("Game source: ignoring {}: {}", rex::path_to_utf8(candidate.filename()),
                  probe.reason);
      continue;
    }
    if (expected_title_id && probe.title_id != expected_title_id) {
      REXLOG_INFO("Game source: ignoring {}: title id {:08X} (expected {:08X})",
                  rex::path_to_utf8(candidate.filename()), probe.title_id, expected_title_id);
      continue;
    }
    return candidate;
  }
  return {};
}

}  // namespace

// --- ReXApp ---

ReXApp::~ReXApp() = default;

ReXApp::ReXApp(ui::WindowedAppContext& ctx, std::string_view name, PPCImageInfo ppc_info,
               std::string_view usage)
    : WindowedApp(ctx, name, usage), ppc_info_(ppc_info) {
  AddPositionalOption("game_directory");
}

bool ReXApp::ResolveGameSource(const std::filesystem::path& exe_dir,
                               std::filesystem::path& out_image) {
  out_image.clear();
  const uint32_t expected_title_id = OnGetExpectedTitleId();
  const bool iso_direct_enabled = REXCVAR_GET(iso_direct);

  const std::string app_name(GetName());
  // path_to_utf8 throughout: path::string() converts through the ANSI
  // codepage on Windows, which garbles non-ASCII file names in the log and
  // in the (UTF-8-consuming) message box.
  auto reject_image = [&](const std::filesystem::path& path, const std::string& reason) {
    REXLOG_ERROR("Game source: rejected image {}: {}", rex::path_to_utf8(path), reason);
    auto message = fmt::format(
        "The disc image was rejected:\n\n{}\n\n{}\n\n"
        "Provide your own legally obtained copy of the game.",
        rex::path_to_utf8(path), reason);
    ShowFatalMessageBox(app_name, message);
    return false;
  };
  auto check_expected_title = [&](const ImageProbe& probe, const std::filesystem::path& path,
                                  std::string& reason_out) {
    if (!probe.readable) {
      reason_out = probe.reason;
      return false;
    }
    if (expected_title_id && probe.title_id != expected_title_id) {
      reason_out = fmt::format("image is for title id {:08X}, this game expects {:08X}",
                               probe.title_id, expected_title_id);
      return false;
    }
    return true;
  };

  // The game_directory argument may point straight at a disc image file.
  if (!game_data_root_.empty() && std::filesystem::is_regular_file(game_data_root_)) {
    auto image_path = game_data_root_;
    std::string reason;
    if (!iso_direct_enabled) {
      return reject_image(image_path, "iso_direct is disabled in the config");
    }
    if (!check_expected_title(ProbeGameImage(image_path), image_path, reason)) {
      return reject_image(image_path, reason);
    }
    game_data_root_.clear();
    out_image = image_path;
    REXLOG_INFO("Game source: iso: {}", rex::path_to_utf8(image_path));
    return true;
  }

  const bool have_loose = std::filesystem::is_directory(game_data_root_) &&
                          std::filesystem::exists(game_data_root_ / "default.xex");

  if (iso_direct_enabled) {
    const std::string iso_cvar = REXCVAR_GET(game_iso);
    if (!iso_cvar.empty()) {
      // An explicit config path must be honoured or fail loudly, never
      // silently fall back. to_path: the toml string is UTF-8, not the ANSI
      // codepage a bare path construction would assume on Windows.
      std::filesystem::path image_path = rex::to_path(iso_cvar);
      std::string reason;
      if (!std::filesystem::is_regular_file(image_path)) {
        return reject_image(image_path, "game_iso does not point at a file");
      }
      if (!check_expected_title(ProbeGameImage(image_path), image_path, reason)) {
        return reject_image(image_path, reason);
      }
      out_image = image_path;
    } else {
      out_image = ScanForGameImage(exe_dir, expected_title_id);
    }
  }

  if (have_loose) {
    // Loose assets are the primary source, exactly as before; an image below
    // only fills per-file gaps.
    REXLOG_INFO("Game source: assets: {}",
                rex::path_to_utf8(std::filesystem::absolute(game_data_root_)));
    if (!out_image.empty()) {
      REXLOG_INFO("Game source: iso underlay: {} (loose files override the image)",
                  rex::path_to_utf8(out_image));
    }
    return true;
  }

  if (!out_image.empty()) {
    REXLOG_INFO("Game source: iso: {}", rex::path_to_utf8(std::filesystem::absolute(out_image)));
    std::error_code ec;
    if (std::filesystem::is_directory(game_data_root_) &&
        !std::filesystem::is_empty(game_data_root_, ec) && !ec) {
      // A partial assets folder on top of an image: the modding path.
      REXLOG_INFO("Game source: loose overlay: {} (loose files override the image)",
                  rex::path_to_utf8(std::filesystem::absolute(game_data_root_)));
    } else {
      game_data_root_.clear();
    }
    return true;
  }

  // Neither an assets folder nor a usable image: name both options clearly.
  auto assets_hint = game_data_root_.empty() ? (exe_dir / "assets") : game_data_root_;
  REXLOG_ERROR("Game source: none found - expected game files at {} or a .iso next to {}",
               rex::path_to_utf8(assets_hint), rex::path_to_utf8(exe_dir));
  auto message = fmt::format(
      "No game data was found.\n\n"
      "Provide your own legally obtained copy of the game in one of two ways:\n\n"
      "1. Extract the game's files into:\n    {}\n\n"
      "2. Place the game's disc image (.iso) next to the executable:\n    {}\n\n"
      "Advanced: set game_iso = \"path/to/image.iso\" in {}.toml.",
      rex::path_to_utf8(assets_hint), rex::path_to_utf8(exe_dir), app_name);
  ShowFatalMessageBox(app_name, message);
  return false;
}

bool ReXApp::OnInitialize() {
  auto exe_dir = rex::filesystem::GetExecutableFolder();
#if !defined(_WIN32)
  // Packaged as an AppImage, the executable runs from a read-only temporary
  // mount, so the config, logs and the game files (assets/ or .iso) cannot
  // live next to it. The AppImage runtime exports APPIMAGE as the path of the
  // .AppImage file itself: use the folder containing that instead, so an
  // AppImage behaves like the plain build. Bundled resources (e.g. the
  // gamecontrollerdb.txt the input driver loads) still resolve through
  // GetExecutableFolder, inside the image.
  if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) {
    const auto appimage_dir = std::filesystem::path(appimage).parent_path();
    if (!appimage_dir.empty()) {
      exe_dir = appimage_dir;
    }
  }
#endif

  auto config_path = exe_dir / (std::string(GetName()) + ".toml");

  // Load saved config (CVARs) before anything reads them
  if (std::filesystem::exists(config_path)) {
    rex::cvar::LoadConfig(config_path);
  }

  // App-level config presets / session defaults: cvars hold their final
  // user-config values and nothing has consumed them yet.
  OnConfigLoaded();

  // Logging setup from CVARs
  std::string log_file_cvar = REXCVAR_GET(log_file);
  std::string log_level_str = REXCVAR_GET(log_level);
  if (REXCVAR_GET(log_verbose) && log_level_str == "info") {
    log_level_str = "trace";
  }
  // Crash reporting, installed before anything heavy runs so a fault during
  // setup still produces a file. Crash files live beside the log, which is
  // where a user already looks and what a bug report already asks for.
  {
    std::filesystem::path crash_dir;
    if (log_file_cvar.empty()) {
      crash_dir = exe_dir / "logs";
    } else {
      std::error_code crash_dir_ec;
      // to_path: the cvar string is UTF-8 (see user_data_root above).
      crash_dir = std::filesystem::absolute(rex::to_path(log_file_cvar), crash_dir_ec)
                      .parent_path();
      if (crash_dir.empty())
        crash_dir = std::filesystem::current_path();
    }
    std::error_code crash_dir_ec;
    std::filesystem::create_directories(crash_dir, crash_dir_ec);

    rex::diag::crash::InstallOptions crash_options;
    crash_options.directory = crash_dir;
    crash_options.app_name = std::string(GetName());
    crash_options.build_title = REXGLUE_BUILD_TITLE;
    crash_options.build_commit = REXGLUE_GIT_HASH;
    crash_options.build_timestamp = REXGLUE_BUILD_TIMESTAMP;
    rex::diag::crash::Install(crash_options);
  }

  auto category_levels = rex::ParseCategoryLevelsFromConfig(config_path);
  auto log_config = rex::BuildLogConfig(log_file_cvar.empty() ? nullptr : log_file_cvar.c_str(),
                                        log_level_str, category_levels);
  if (log_file_cvar.empty()) {
    log_config.app_name = std::string(GetName());
    // path_to_utf8, not .string(): a non-ASCII exe dir narrowed through the
    // ANSI code page here made the log land in a mangled directory - or
    // threw during startup - exactly when a user on such a path most needs
    // their diagnostics. InitLogging converts back with rex::to_path.
    log_config.log_dir = rex::path_to_utf8(exe_dir / "logs");
  }
  rex::InitLogging(log_config);
  rex::RegisterLogLevelCallback();

  // Attach log capture sink to all loggers for the console overlay
  log_sink_ = std::make_shared<rex::LogCaptureSink>();
  rex::AddSink(log_sink_);
  // The crash file embeds this ring buffer's tail: the log itself is replaced
  // on the next launch, so the lines around a fault would otherwise be gone
  // by the time anyone looks.
  rex::diag::crash::SetLogTailSink(log_sink_.get());
  if (std::filesystem::exists(config_path)) {
    REXLOG_INFO("Loaded config: {}", config_path.filename().string());
  }

  REXLOG_INFO("{} starting", GetName());

  // Data-root resolution. Runs after LoadConfig, next to the game-data
  // resolution below and for the same reason: these cvars used to be
  // consumed at the top of this function, BEFORE the toml was parsed, so a
  // user_data_root/update_data_root set in the config file silently did
  // nothing - only the command line ever worked. Nothing between
  // LoadConfig and this point derives a path from the user root (log and
  // crash paths come from log_file/exe_dir).

  // Game directory: positional arg or default to exe_dir/assets
  std::filesystem::path game_dir;
  if (auto arg = GetArgument("game_directory")) {
    game_dir = *arg;
  } else {
    game_dir = exe_dir / "assets";
  }

  // User data: cvar override, or platform user directory
  std::filesystem::path user_dir;
  std::string user_data_cvar = REXCVAR_GET(user_data_root);
  if (!user_data_cvar.empty()) {
    // to_path: the toml string is UTF-8, not the ANSI codepage a bare path
    // construction would assume on Windows (same rule as game_iso/dlc_dir).
    user_dir = rex::to_path(user_data_cvar);
  } else {
    user_dir = rex::filesystem::GetUserFolder() / GetName();
  }

  // Update data: cvar override, or empty (opt-in)
  std::filesystem::path update_dir;
  std::string update_data_cvar = REXCVAR_GET(update_data_root);
  if (!update_data_cvar.empty()) {
    update_dir = rex::to_path(update_data_cvar);
  }

  // Allow subclass to override path defaults
  PathConfig path_config{game_dir, user_dir, update_dir};
  OnConfigurePaths(path_config);
  game_data_root_ = std::move(path_config.game_data_root);
  user_data_root_ = std::move(path_config.user_data_root);
  update_data_root_ = std::move(path_config.update_data_root);

  // Resolve where the game's data comes from. Runs after LoadConfig so the
  // game_iso / iso_direct toml overrides apply.
  std::filesystem::path game_image;
  if (!ResolveGameSource(exe_dir, game_image)) {
    return false;
  }
  if (!user_data_root_.empty()) {
    REXLOG_INFO("  User data:      {}", rex::path_to_utf8(user_data_root_));
  }
  if (!update_data_root_.empty()) {
    REXLOG_INFO("  Update data:    {}", rex::path_to_utf8(update_data_root_));
  }

  // Create runtime
  runtime_ = std::make_unique<rex::Runtime>(game_data_root_, user_data_root_, update_data_root_);
  if (!game_image.empty()) {
    runtime_->set_game_image_path(game_image);
  }
  runtime_->set_app_context(&app_context());

  // Build runtime config with default platform backends
  rex::RuntimeConfig config;
#if REX_HAS_D3D12
  config.graphics = REX_GRAPHICS_BACKEND(rex::graphics::d3d12::D3D12GraphicsSystem);
#elif REX_HAS_VULKAN
  config.graphics = REX_GRAPHICS_BACKEND(rex::graphics::vulkan::VulkanGraphicsSystem);
#endif
  config.audio_factory = REX_AUDIO_BACKEND(rex::audio::AudioSystem);
  config.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
  config.kernel_init = rex::kernel::InitializeKernel;

  // Allow subclass to customize config
  OnPreSetup(config);

  auto status = runtime_->Setup(ppc_info_.code_base, ppc_info_.code_size, ppc_info_.image_base,
                                ppc_info_.image_size, ppc_info_.func_mappings, std::move(config));
  if (XFAILED(status)) {
    REXLOG_ERROR("Runtime setup failed: {:08X}", status);
    return false;
  }

  // Guest-side crash reporting: the memory bounds classify a faulting address
  // as guest vs host, and the generated function table lets a crash name
  // rex_sub_* frames at runtime with no symbols shipped.
  if (runtime_->memory()) {
    rex::diag::crash::SetGuestMemoryBounds(runtime_->memory()->virtual_membase(), 0x11FFFFFFFull);
  }
  rex::diag::crash::SetGuestFunctionTable(ppc_info_.func_mappings);

  std::string xex_image = "game:\\default.xex";

  // Allow subclass to override xex image
  OnLoadXexImage(xex_image);

  // Load XEX image
  status = runtime_->LoadXexImage(xex_image);
  if (XFAILED(status)) {
    REXLOG_ERROR("Failed to load XEX: {:08X}", status);
    return false;
  }

  // Discover raw DLC containers now that the title id is known. Logs one
  // line per package found; extracted folders keep priority.
  if (REXCVAR_GET(dlc_containers)) {
    const std::string dlc_dir_cvar = REXCVAR_GET(dlc_dir);
    const std::filesystem::path dlc_dir =
        dlc_dir_cvar.empty() ? (exe_dir / "dlc") : rex::to_path(dlc_dir_cvar);
    runtime_->kernel_state()->content_manager()->DiscoverContainers(dlc_dir);
  }

  // Initialize rexcrt heap after LoadXexImage to avoid guest memory writes
  // corrupting the heap region. rexcrt_heap is set by codegen (REXCRT_HEAP)
  // when [rexcrt] contains heap functions -- originals are stripped so init
  // is required. Size is controlled by the rexcrt_heap_size_mb CVAR.
  if (ppc_info_.rexcrt_heap) {
    if (!rex::kernel::crt::InitHeap(REXCVAR_GET(rexcrt_heap_size_mb), runtime_->memory())) {
      REXLOG_ERROR("Failed to initialize rexcrt heap");
      return false;
    }
  }

  // Notify subclass
  OnPostSetup();

  // Create window
  window_ = rex::ui::Window::Create(app_context(), GetName(), 1280, 720);
  if (!window_) {
    REXLOG_ERROR("Failed to create window");
    return false;
  }

  // Set window title with SDK build stamp
  std::string title = std::string(GetName()) + " " + REXGLUE_BUILD_TITLE;
  window_->SetTitle(title);

  window_->AddListener(this);
  window_->AddInputListener(this, 0);

  // F11 toggles borderless fullscreen (rebindable via the bind_fullscreen
  // cvar). The change is recorded through the same path as a settings-menu
  // edit (SetFlagByName -> user_value), so "Save to config" persists it and
  // the next launch starts in the chosen mode.
  rex::ui::RegisterBind("bind_fullscreen", "F11", "Toggle fullscreen", [this] {
    if (!window_) {
      return;
    }
    bool new_fullscreen = !window_->IsFullscreen();
    window_->SetFullscreen(new_fullscreen);
    rex::cvar::SetFlagByName("fullscreen", new_fullscreen ? "true" : "false");
  });

  // Attach window to input system so deferred drivers (e.g. MnK) can register
  if (runtime_ && runtime_->input_system()) {
    static_cast<rex::input::InputSystem*>(runtime_->input_system())->AttachWindow(window_.get());
  }

  if (REXCVAR_GET(fullscreen)) {
    window_->SetFullscreen(true);
  }
  window_->Open();

  // Setup graphics presenter and ImGui
  auto* graphics_system = runtime_->graphics_system();
  if (graphics_system && graphics_system->presenter()) {
    auto* presenter = graphics_system->presenter();
    auto* provider = graphics_system->provider();
    if (provider) {
      immediate_drawer_ = provider->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(window_.get(), 64);
        imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, immediate_drawer_.get());
        // Built-in overlays
        debug_overlay_ = std::make_unique<ui::DebugOverlayDialog>(imgui_drawer_.get());
        build_stamp_overlay_ = std::make_unique<ui::BuildStampOverlay>(imgui_drawer_.get());
        console_overlay_ = std::make_unique<ui::ConsoleDialog>(imgui_drawer_.get(), log_sink_);
        settings_overlay_ = std::make_unique<ui::SettingsDialog>(
            imgui_drawer_.get(), config_path);

        // Allow subclass to add custom dialogs
        OnCreateDialogs(imgui_drawer_.get());

        runtime_->set_display_window(window_.get());
        runtime_->set_imgui_drawer(imgui_drawer_.get());

        // Pause the MnK virtual pad while a visible dialog owns the mouse.
        // Uses the drawer's published per-frame flag: visibility-conjoined
        // (a closed dialog can never latch it) and safe to read from the
        // input threads, unlike ImGui IO. Real controller drivers never
        // receive this callback (see InputSystem::SetActiveCallback) - a
        // physical pad keeps working with a dialog open.
        auto* input_sys = static_cast<rex::input::InputSystem*>(runtime_->input_system());
        if (input_sys) {
          input_sys->SetActiveCallback(
              []() { return !rex::ui::ImGuiDrawer::DialogsCaptureMouse(); });
        }
      }
    }
    window_->SetPresenter(presenter);
  }

  // Launch module in background
  app_context().CallInUIThreadDeferred([this]() {
    auto main_thread = runtime_->LaunchModule();
    if (!main_thread) {
      REXLOG_ERROR("Failed to launch module");
      app_context().QuitFromUIThread();
      return;
    }

    // Wire up the GPU pipeline disk cache. The backend has the whole shader
    // storage system (record + pre-create), but nothing triggers it otherwise,
    // so every run compiles pipelines on demand during gameplay - the
    // first-encounter stutter (mission start). Init here, now that the title is
    // loaded (title_id known) and the GPU is set up: blocking, so previously
    // recorded pipelines are pre-created during the initial load rather than
    // mid-play, and new ones are recorded for next time.
    if (REXCVAR_GET(use_shader_disk_cache) && runtime_->graphics_system() &&
        runtime_->kernel_state()) {
      uint32_t shader_cache_title_id = runtime_->kernel_state()->title_id();
      if (shader_cache_title_id != 0) {
        std::filesystem::path cache_root = std::filesystem::current_path() / "cache";
        REXLOG_INFO("Shader disk cache: initializing for title {:08X} at {}",
                    shader_cache_title_id, rex::path_to_utf8(cache_root));
        // graphics_system() returns the IGraphicsSystem interface; InitializeShaderStorage
        // lives on the concrete GraphicsSystem (always a rex::graphics::GraphicsSystem here).
        static_cast<rex::graphics::GraphicsSystem*>(runtime_->graphics_system())
            ->InitializeShaderStorage(cache_root, shader_cache_title_id, true);
      } else {
        REXLOG_WARN("Shader disk cache: title_id unavailable, skipping");
      }
    }

    module_thread_ = std::thread([this, main_thread = std::move(main_thread)]() mutable {
      main_thread->Wait(0, 0, 0, nullptr);
      REXLOG_INFO("Execution complete");
      if (!shutting_down_.load(std::memory_order_acquire)) {
        app_context().CallInUIThread([this]() { app_context().QuitFromUIThread(); });
      }
    });
  });

  return true;
}

void ReXApp::OnKeyDown(ui::KeyEvent& e) {
  rex::ui::ProcessKeyEvent(e);
}

void ReXApp::OnClosing(ui::UIEvent& e) {
  (void)e;
  REXLOG_INFO("Window closing, shutting down...");
  shutting_down_.store(true, std::memory_order_release);
  if (runtime_ && runtime_->kernel_state()) {
    runtime_->kernel_state()->TerminateTitle();
  }
  app_context().QuitFromUIThread();
}

void ReXApp::OnDestroy() {
  // The shutdown path is running, so the atexit hook must not report this
  // exit as unexpected.
  rex::diag::crash::NotifyOrderlyShutdown();

  // Notify subclass before cleanup
  OnShutdown();

  // The fullscreen bind's callback captures this - drop it before teardown.
  rex::ui::UnregisterBind("bind_fullscreen");

  // ImGui cleanup (reverse of setup)
  settings_overlay_.reset();
  console_overlay_.reset();
  build_stamp_overlay_.reset();
  debug_overlay_.reset();
  if (imgui_drawer_) {
    imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    imgui_drawer_.reset();
  }
  if (immediate_drawer_) {
    immediate_drawer_->SetPresenter(nullptr);
    immediate_drawer_.reset();
  }
  if (runtime_) {
    runtime_->set_display_window(nullptr);
    runtime_->set_imgui_drawer(nullptr);
  }
  // Window/runtime cleanup
  if (window_) {
    window_->SetPresenter(nullptr);
  }
  if (module_thread_.joinable()) {
    module_thread_.join();
  }
  if (window_) {
    window_->RemoveInputListener(this);
    window_->RemoveListener(this);
  }
  window_.reset();
  runtime_.reset();
}

void ReXApp::SetGuestFrameStats(ui::DebugOverlayDialog::FrameStatsProvider provider) {
  if (debug_overlay_) {
    debug_overlay_->SetStatsProvider(std::move(provider));
  }
}

}  // namespace rex
