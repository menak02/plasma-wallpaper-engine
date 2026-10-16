#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <QVariant>
#include <QDateTime>
#include <QJSEngine>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

namespace WallpaperEngine::Scene {

/**
 * JavaScript Engine for evaluating Wallpaper Engine SceneScript property
 * scripts (ES modules with init()/update(value) and shared/scriptProperties
 * state) as well as the simple binding patterns:
 * - Simple property references: {"user": "propname", "value": true}
 * - Combo conditions: {"user": {"condition": "N", "name": "propname"}, "value": true}
 * - Script-based evaluation: {"script": "...", "value": fallback}
 *
 * Built on Qt's QJSEngine for zero external dependencies. One JSEngine
 * instance represents one loaded scene: `shared` state persists across all
 * script evaluations in it, matching Wallpaper Engine's SceneScript model.
 */
class JSEngine {
public:
    JSEngine();
    ~JSEngine();

    /**
     * Initialize the JS engine with project properties and system state.
     * Called once when a wallpaper is loaded.
     */
    void init(const std::unordered_map<std::string, QVariant>& properties,
              float currentTime = 0.0f);

    /**
     * Evaluate a visibility expression and return whether the layer should be visible.
     *
     * Supports patterns:
     *   true/false                           → direct bool
     *   {"user": "propname", "value": true}  → returns properties["propname"]
     *   {"user": {"condition": "N", "name": "propname"}, "value": true}
     *                                      → returns combo index == N
     *   {"script": "..."}                    → runs the module's init/update
     */
    bool evaluateVisibility(const QVariant& visibleVal);
    bool evaluateVisibility(const QJsonValue& visibleVal);

    /**
     * Evaluate a numeric property (alpha, scale, ...) honoring plain values,
     * user bindings and {"script": "..."} modules (update(value) return).
     * settle=true lets easing animations converge (load time); settle=false
     * performs a single update() tick (per-frame runtime).
     */
    double evaluateScriptNumber(const QJsonValue& propVal, double fallback, bool settle = true);

    /**
     * Evaluate a string property (text objects) — script modules return the
     * display string from update().
     */
    QString evaluateScriptString(const QJsonValue& propVal, const QString& fallback, bool settle = true);

    /**
     * Run a script module's init() early (scene pass 1) so controller
     * scripts can seed shared state before consumers evaluate. Idempotent.
     */
    void registerScript(const QString& scriptCode);

    /**
     * Evaluate a numeric property (opacity, position, etc.)
     */
    QVariant evaluateProperty(const QVariant& propVal);

    /**
     * Update time-dependent state (call each frame).
     */
    void update(float currentTime, float deltaTime);

    /**
     * Get current hour (0-23) for time-based scripts.
     */
    int getCurrentHour() const { return m_currentHour; }

    /**
     * Get current minute (0-59).
     */
    int getCurrentMinute() const { return m_currentMinute; }

    /**
     * Get current second (0-59).
     */
    int getCurrentSecond() const { return m_currentSecond; }

    /**
     * Get current day of month (1-31).
     */
    int getCurrentDay() const { return m_currentDay; }

    /**
     * Get current month (1-12).
     */
    int getCurrentMonth() const { return m_currentMonth; }

    /**
     * Get current year.
     */
    int getCurrentYear() const { return m_currentYear; }

    /**
     * Get current day of week (0=Sunday, 6=Saturday).
     */
    int getCurrentDayOfWeek() const { return m_currentDayOfWeek; }

    /**
     * Get current day name (short).
     */
    QString getDayNameShort() const;

    /**
     * Get current month name (short).
     */
    QString getMonthNameShort() const;

    /**
     * Get current time formatted as HH:MM.
     */
    QString getTimeFormatted() const;

private:
    bool evaluateSimpleUser(const QString& propName, const QVariant& defaultValue);
    bool evaluateConditionUser(const QVariant& userObj, const QVariant& defaultValue);
    bool evaluateScript(const QString& scriptCode, const QVariant& defaultValue);

    // Prepare the persistent engine state (globals, shared, property mocks).
    void ensureEngine();
    // Evaluate one script module inside its own function scope (returns the
    // {init, update, visibility} handle object; invalid on failure).
    QJSValue loadModule(const QString& scriptCode);
    // Call module.init() once per module and module.update(value).
    QJSValue callModule(const QString& scriptCode, const QJSValue& valueArg);

    // Project-level properties (from project.json)
    std::unordered_map<std::string, QVariant> m_properties;

    // Persistent per-scene script runtime: shared state survives across all
    // module evaluations (the mechanism scripts use to hide/show layers).
    QJSEngine m_js;
    QJSValue m_shared;
    QJSValue m_engineObj;
    bool m_engineReady = false;
    std::unordered_map<std::string, QJSValue> m_modules;
    std::unordered_set<std::string> m_initedModules;

    // System time state
    int m_currentHour = 0;
    int m_currentMinute = 0;
    int m_currentSecond = 0;
    int m_currentDay = 0;
    int m_currentMonth = 0;
    int m_currentYear = 0;
    int m_currentDayOfWeek = 0;

    // Last update time for delta calculation
    float m_lastTime = 0.0f;

    // Cached JS engine (one per instance)
    // Uses raw pointer since QJSEngine is not moveable
};

} // namespace WallpaperEngine::Scene
