#pragma once

#include <string>
#include <unordered_map>
#include <QVariant>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>

namespace WallpaperEngine::Scene {

/**
 * JavaScript Engine for evaluating Wallpaper Engine script properties.
 *
 * Supports:
 * - Simple property references: {"user": "propname", "value": true}
 * - Combo conditions: {"user": {"condition": "N", "name": "propname"}, "value": true}
 * - Script-based evaluation via inlined JS snippets
 *
 * Built on Qt's QJSEngine for zero external dependencies.
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
     *   {"script": "..."}                    → evaluates JS and returns result
     */
    bool evaluateVisibility(const QVariant& visibleVal);
    bool evaluateVisibility(const QJsonValue& visibleVal);

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

    // Project-level properties (from project.json)
    std::unordered_map<std::string, QVariant> m_properties;

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
