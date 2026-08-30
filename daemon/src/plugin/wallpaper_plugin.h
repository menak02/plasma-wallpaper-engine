#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>

namespace WallpaperEngine::Plugin {

/**
 * Plugin API version. Increment when breaking the ABI.
 */
constexpr int WALLPAPER_PLUGIN_API_VERSION = 1;

/**
 * Hook identifiers available to plugins.
 */
enum class HookPoint {
    PreParse,      // Called before scene parsing begins
    PostParse,     // Called after scene is fully parsed
    PreRender,     // Called each frame before rendering
    PostRender,    // Called each frame after rendering
    OnLoad,        // Called when a wallpaper is loaded
    OnUnload       // Called when a wallpaper is unloaded
};

/**
 * Plugin context passed to each hook. Provides safe, read-only access to
 * scene metadata. Concrete implementations provide scene-specific context.
 */
struct PluginContext {
    std::string wallpaperId;
    std::string wallpaperTitle;
    float currentTime = 0.0f;
    float deltaTime = 0.0f;
};

/**
 * Base interface for wallpaper plugins.
 *
 * A plugin is a unit of composable behavior loaded at runtime. Plugins
 * register handlers for one or more HookPoints; the engine invokes them
 * at appropriate moments during scene lifecycle.
 */
class IWallpaperPlugin {
public:
    virtual ~IWallpaperPlugin() = default;

    /**
     * Plugin's human-readable name.
     */
    virtual std::string name() const = 0;

    /**
     * Plugin's version string (semver recommended).
     */
    virtual std::string version() const = 0;

    /**
     * Plugin's author/maintainer.
     */
    virtual std::string author() const = 0;

    /**
     * Called once when the plugin is loaded.
     */
    virtual void onLoad() {}

    /**
     * Called once when the plugin is unloaded.
     */
    virtual void onUnload() {}

    /**
     * Hook handler. Plugins override the hooks they need.
     * Default implementations are no-ops.
     */
    virtual void onPreParse(const PluginContext& /*ctx*/) {}
    virtual void onPostParse(const PluginContext& /*ctx*/) {}
    virtual void onPreRender(const PluginContext& /*ctx*/) {}
    virtual void onPostRender(const PluginContext& /*ctx*/) {}
    virtual void onWallpaperLoaded(const PluginContext& /*ctx*/) {}
    virtual void onWallpaperUnloaded(const PluginContext& /*ctx*/) {}
};

/**
 * Plugin registry. Tracks loaded plugins and dispatches hook events.
 *
 * Plugins are typically compiled into the binary (header-only API) for now;
 * the registry is extensible so dynamic loading via dlsym can be added later
 * without changing the public surface.
 */
class PluginRegistry {
public:
    static PluginRegistry& instance();

    /**
     * Register a plugin. The registry takes ownership.
     * Returns false if a plugin with the same name is already registered.
     */
    bool registerPlugin(std::unique_ptr<IWallpaperPlugin> plugin);

    /**
     * Unregister a plugin by name. Calls onUnload() on the plugin.
     */
    bool unregisterPlugin(const std::string& name);

    /**
     * Returns the number of registered plugins.
     */
    size_t pluginCount() const { return m_plugins.size(); }

    /**
     * Iterate all registered plugins (read-only).
     */
    const std::vector<std::unique_ptr<IWallpaperPlugin>>& plugins() const { return m_plugins; }

    /**
     * Dispatch a hook to all registered plugins.
     */
    void dispatch(HookPoint hook, const PluginContext& ctx);

    /**
     * Load all bundled plugins. Called once during engine init.
     */
    void loadBuiltinPlugins();

    /**
     * Unload all plugins. Called during engine shutdown.
     */
    void unloadAll();

private:
    PluginRegistry() = default;
    std::vector<std::unique_ptr<IWallpaperPlugin>> m_plugins;
    std::unordered_map<std::string, size_t> m_indexByName;
};

/**
 * Helper macro for static plugin registration.
 *
 * Usage:
 *   class MyPlugin : public IWallpaperPlugin { ... };
 *   REGISTER_WALLPAPER_PLUGIN(MyPlugin)
 */
template <typename T>
struct PluginRegistrar {
    PluginRegistrar() {
        PluginRegistry::instance().registerPlugin(std::make_unique<T>());
    }
};

#define REGISTER_WALLPAPER_PLUGIN(PluginClass) \
    static PluginRegistrar<PluginClass> _plugin_registrar_##PluginClass

} // namespace WallpaperEngine::Plugin
