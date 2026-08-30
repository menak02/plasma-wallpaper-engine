#include "wallpaper_plugin.h"

namespace WallpaperEngine::Plugin {

PluginRegistry& PluginRegistry::instance() {
    static PluginRegistry s_instance;
    return s_instance;
}

bool PluginRegistry::registerPlugin(std::unique_ptr<IWallpaperPlugin> plugin) {
    if (!plugin) {
        return false;
    }
    const std::string name = plugin->name();
    if (m_indexByName.find(name) != m_indexByName.end()) {
        return false;
    }
    m_indexByName[name] = m_plugins.size();
    m_plugins.push_back(std::move(plugin));
    if (m_plugins.back()) {
        m_plugins.back()->onLoad();
    }
    return true;
}

bool PluginRegistry::unregisterPlugin(const std::string& name) {
    auto it = m_indexByName.find(name);
    if (it == m_indexByName.end()) {
        return false;
    }
    const size_t idx = it->second;
    if (m_plugins[idx]) {
        m_plugins[idx]->onUnload();
    }
    // Erase and rebuild index
    m_plugins.erase(m_plugins.begin() + static_cast<ptrdiff_t>(idx));
    m_indexByName.clear();
    for (size_t i = 0; i < m_plugins.size(); ++i) {
        if (m_plugins[i]) {
            m_indexByName[m_plugins[i]->name()] = i;
        }
    }
    return true;
}

void PluginRegistry::dispatch(HookPoint hook, const PluginContext& ctx) {
    for (auto& plugin : m_plugins) {
        if (!plugin) continue;
        switch (hook) {
            case HookPoint::PreParse:       plugin->onPreParse(ctx); break;
            case HookPoint::PostParse:      plugin->onPostParse(ctx); break;
            case HookPoint::PreRender:      plugin->onPreRender(ctx); break;
            case HookPoint::PostRender:     plugin->onPostRender(ctx); break;
            case HookPoint::OnLoad:         plugin->onWallpaperLoaded(ctx); break;
            case HookPoint::OnUnload:       plugin->onWallpaperUnloaded(ctx); break;
        }
    }
}

void PluginRegistry::loadBuiltinPlugins() {
    // Builtin plugins are registered via REGISTER_WALLPAPER_PLUGIN macros in
    // their respective translation units. Their static initializers run
    // before this function is called, so no additional work is required here.
}

void PluginRegistry::unloadAll() {
    for (auto& plugin : m_plugins) {
        if (plugin) {
            plugin->onUnload();
        }
    }
    m_plugins.clear();
    m_indexByName.clear();
}

} // namespace WallpaperEngine::Plugin
