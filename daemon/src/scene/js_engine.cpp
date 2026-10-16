#include "js_engine.h"
#include <QJSEngine>
#include <QJSValue>
#include <QDebug>

namespace WallpaperEngine::Scene {

JSEngine::JSEngine() {}

JSEngine::~JSEngine() {}

void JSEngine::init(const std::unordered_map<std::string, QVariant>& properties,
                    float currentTime) {
    // Unwrap project.json property objects to their "value" field
    // project.json stores { "type":"bool", "value":true, ... } but we need just value
    m_properties.clear();
    m_properties.reserve(properties.size());
    for (auto& [k, v] : properties) {
        if (v.typeId() == QMetaType::QVariantMap) {
            QVariantMap m = v.toMap();
            if (m.contains(QStringLiteral("value"))) {
                m_properties[k] = m.value(QStringLiteral("value"));
                continue;
            }
        } else if (v.canConvert<QJsonObject>()) {
            QJsonObject o = v.value<QJsonObject>();
            if (o.contains(QStringLiteral("value"))) {
                m_properties[k] = o.value(QStringLiteral("value")).toVariant();
                continue;
            }
        }
        m_properties[k] = v;
    }
    m_lastTime = currentTime;

    // Initialize time state from current system time
    QDateTime now = QDateTime::currentDateTime();
    m_currentHour = now.time().hour();
    m_currentMinute = now.time().minute();
    m_currentSecond = now.time().second();
    m_currentDay = now.date().day();
    m_currentMonth = now.date().month();
    m_currentYear = now.date().year();
    m_currentDayOfWeek = now.date().dayOfWeek(); // 1=Monday, 7=Sunday
}

void JSEngine::update(float currentTime, float deltaTime) {
    // Update time state if significant time has passed
    if (currentTime - m_lastTime >= 1.0f) {
        m_lastTime = currentTime;
        QDateTime now = QDateTime::currentDateTime();
        m_currentHour = now.time().hour();
        m_currentMinute = now.time().minute();
        m_currentSecond = now.time().second();
        m_currentDay = now.date().day();
        m_currentMonth = now.date().month();
        m_currentYear = now.date().year();
        m_currentDayOfWeek = now.date().dayOfWeek();
    }
    Q_UNUSED(deltaTime);
}

bool JSEngine::evaluateVisibility(const QVariant& visibleVal) {
    // Handle QJsonValue-wrapped objects stored as QVariant
    if (visibleVal.typeId() == QMetaType::QVariantMap) {
        QVariantMap m = visibleVal.toMap();
        QJsonObject obj = QJsonObject::fromVariantMap(m);
        // reuse QJsonValue path
        return evaluateVisibility(QJsonValue(obj));
    }
    if (visibleVal.canConvert<QJsonObject>()) {
        QJsonObject obj = visibleVal.value<QJsonObject>();
        return evaluateVisibility(QJsonValue(obj));
    }
    // Handle object stored as generic QVariant (e.g., from QJsonValue::toVariant)
    if (visibleVal.typeId() == QMetaType::QJsonValue) {
        return evaluateVisibility(visibleVal.value<QJsonValue>());
    }
    // Simple boolean / numeric
    if (visibleVal.isNull() || !visibleVal.isValid()) return true;
    // For QVariant bool/int/string
    if (visibleVal.typeId() == QMetaType::Bool) return visibleVal.toBool();
    if (visibleVal.typeId() == QMetaType::QString) {
        QString s = visibleVal.toString().toLower();
        if (s == QStringLiteral("true")) return true;
        if (s == QStringLiteral("false")) return false;
    }
    return visibleVal.toBool();
}

bool JSEngine::evaluateVisibility(const QJsonValue& visibleVal) {
    if (visibleVal.isBool()) return visibleVal.toBool();
    if (visibleVal.isString()) {
        QString s = visibleVal.toString().toLower();
        if (s == QStringLiteral("true")) return true;
        if (s == QStringLiteral("false")) return false;
        return true;
    }
    if (!visibleVal.isObject()) {
        // Number, null, undefined → visible
        if (visibleVal.isNull() || visibleVal.isUndefined()) return true;
        return visibleVal.toBool(true);
    }

    QJsonObject obj = visibleVal.toObject();

    // Pattern: {"user": "propname", "value": true/false}
    if (obj.contains(QStringLiteral("user")) && obj[QStringLiteral("user")].isString()) {
        QString propName = obj[QStringLiteral("user")].toString();
        QVariant defaultValue = obj.contains(QStringLiteral("value")) ? obj[QStringLiteral("value")].toVariant() : QVariant(true);
        return evaluateSimpleUser(propName, defaultValue);
    }

    // Pattern: {"user": {"condition": "N", "name": "propname"}, "value": true/false}
    if (obj.contains(QStringLiteral("user")) && obj[QStringLiteral("user")].isObject()) {
        QJsonObject userObj = obj[QStringLiteral("user")].toObject();
        // condition may be string "0" or int 0 — normalize
        QVariant condVar;
        if (userObj[QStringLiteral("condition")].isString())
            condVar = userObj[QStringLiteral("condition")].toString().toInt();
        else
            condVar = userObj[QStringLiteral("condition")].toInt(-1);
        QString propName = userObj[QStringLiteral("name")].toString();
        QVariant defaultValue = obj.contains(QStringLiteral("value")) ? obj[QStringLiteral("value")].toVariant() : QVariant(true);
        // Build a QJsonObject for evaluateConditionUser compat
        QJsonObject tmp;
        tmp[QStringLiteral("name")] = propName;
        tmp[QStringLiteral("condition")] = QJsonValue::fromVariant(condVar);
        return evaluateConditionUser(QVariant::fromValue(tmp), defaultValue);
    }

    // Pattern: {"script": "..."} or {"value":...} with script inside
    if (obj.contains(QStringLiteral("script"))) {
        return evaluateScript(obj[QStringLiteral("script")].toString(), QVariant(true));
    }

    // Fallback: object with bare "value" bool (e.g. {"value": false})
    if (obj.contains(QStringLiteral("value")) && obj.size() == 1) {
        return obj[QStringLiteral("value")].toBool(true);
    }

    // Unknown pattern, default to true
    return true;
}

QVariant JSEngine::evaluateProperty(const QVariant& propVal) {
    if (!propVal.canConvert<QJsonObject>()) {
        return propVal;
    }

    QJsonObject obj = propVal.value<QJsonObject>();

    // Pattern: {"user": "propname", "value": default}
    if (obj.contains("user") && obj["user"].isString()) {
        QString propName = obj["user"].toString();
        QVariant defaultValue = obj.contains("value") ? obj["value"].toVariant() : QVariant();
        auto it = m_properties.find(propName.toStdString());
        if (it != m_properties.end()) {
            return it->second;
        }
        return defaultValue;
    }

    // Pattern: {"user": {"condition": "N", "name": "propname"}, "value": default}
    if (obj.contains("user") && obj["user"].isObject()) {
        QVariant userObj = obj["user"].toVariant();
        QVariant defaultValue = obj.contains("value") ? obj["value"].toVariant() : QVariant();
        // For simplicity, return default for now
        // Full implementation would need combo logic
        Q_UNUSED(userObj);
        return defaultValue;
    }

    return propVal;
}

bool JSEngine::evaluateSimpleUser(const QString& propName, const QVariant& defaultValue) {
    auto it = m_properties.find(propName.toStdString());
    if (it != m_properties.end()) {
        return it->second.toBool();
    }
    return defaultValue.toBool();
}

bool JSEngine::evaluateConditionUser(const QVariant& userObj, const QVariant& defaultValue) {
    if (!userObj.canConvert<QJsonObject>()) {
        return defaultValue.toBool();
    }

    QJsonObject uObj = userObj.value<QJsonObject>();
    QString propName = uObj.value("name").toString();
    int condition = uObj.value("condition").toInt(-1);

    auto it = m_properties.find(propName.toStdString());
    if (it == m_properties.end()) {
        return defaultValue.toBool();
    }

    QVariant propVal = it->second;

    // Handle combo/condition logic
    // condition "0" = value == 0, "1" = value == 1, etc.
    if (propVal.canConvert<int>() || propVal.typeId() == QMetaType::Int) {
        int intValue = propVal.toInt();
        return (intValue == condition);
    }

    // For boolean properties, condition "1" means true
    if (propVal.canConvert<bool>()) {
        bool boolVal = propVal.toBool();
        return (condition == 1 && boolVal) || (condition == 0 && !boolVal);
    }

    return defaultValue.toBool();
}

void JSEngine::ensureEngine() {
    if (m_engineReady) return;
    m_engineReady = true;

    m_shared = m_js.newObject();
    m_js.globalObject().setProperty(QStringLiteral("shared"), m_shared);

    m_engineObj = m_js.newObject();
    m_js.globalObject().setProperty(QStringLiteral("engine"), m_engineObj);

    // Property bag: project.json user properties first; script-declared
    // createScriptProperties() defaults merge in via the builder mock below.
    QJSValue propsObj = m_js.newObject();
    for (auto& [k, v] : m_properties) {
        QString key = QString::fromStdString(k);
        if (v.typeId() == QMetaType::Bool) propsObj.setProperty(key, v.toBool());
        else if (v.typeId() == QMetaType::Int) propsObj.setProperty(key, v.toInt());
        else if (v.typeId() == QMetaType::Double) propsObj.setProperty(key, v.toDouble());
        else propsObj.setProperty(key, v.toString());
    }
    m_js.globalObject().setProperty(QStringLiteral("scriptProperties"), propsObj);
    m_js.globalObject().setProperty(QStringLiteral("userProperties"), propsObj);
    m_js.globalObject().setProperty(QStringLiteral("thisObject"), m_js.newObject());

    // SceneScript property-declaration builder. finish() merges declared
    // defaults under the existing (user-provided) property values.
    m_js.evaluate(QStringLiteral(
        "function createScriptProperties() {"
        "  var user = scriptProperties;"
        "  var decls = [];"
        "  var b = {};"
        "  var add = function(o) { decls.push(o); return b; };"
        "  b.addCheckbox = add; b.addSlider = add; b.addCombo = add;"
        "  b.addTextinput = add; b.addTextInput = add; b.addFile = add;"
        "  b.addDirectory = add; b.addColor = add;"
        "  b.finish = function() {"
        "    var out = {};"
        "    for (var i = 0; i < decls.length; i++) {"
        "      var d = decls[i];"
        "      out[d.name] = (user && user[d.name] !== undefined) ? user[d.name] : d.value;"
        "    }"
        "    return out;"
        "  };"
        "  return b;"
        "}"));
}

QJSValue JSEngine::loadModule(const QString& scriptCode) {
    ensureEngine();

    const std::string key = scriptCode.toStdString();
    auto it = m_modules.find(key);
    if (it != m_modules.end()) return it->second;

    QString cleaned = scriptCode;
    cleaned.remove(QStringLiteral("'use strict';"));
    cleaned.remove(QStringLiteral("\"use strict\";"));
    cleaned.replace(QRegularExpression(QStringLiteral("\\bexport\\s+")), QStringLiteral(""));
    cleaned.remove(QRegularExpression(QStringLiteral("(let|var|const)\\s+__workshopId[^;]*;")));

    // Each module runs in its own function scope so `let` names cannot
    // collide across scripts, while globals (shared/scriptProperties/engine)
    // stay common — exactly SceneScript's isolation model.
    const QString wrapped = QStringLiteral(
        "(function() {\n%1\n"
        "  return {"
        "    init: typeof init === 'function' ? init : null,"
        "    update: typeof update === 'function' ? update : null,"
        "    visibility: typeof visibility !== 'undefined' ? visibility : undefined"
        "  };\n})()").arg(cleaned);

    QJSValue mod = m_js.evaluate(wrapped);
    if (mod.isError() || !mod.isObject()) {
        m_modules[key] = QJSValue();
        return QJSValue();
    }
    m_modules[key] = mod;
    return mod;
}

QJSValue JSEngine::callModule(const QString& scriptCode, const QJSValue& valueArg) {
    QJSValue mod = loadModule(scriptCode);
    if (!mod.isObject()) return QJSValue();

    const std::string key = scriptCode.toStdString();
    if (m_initedModules.find(key) == m_initedModules.end()) {
        m_initedModules.insert(key);
        QJSValue initFn = mod.property(QStringLiteral("init"));
        if (initFn.isCallable()) initFn.call();
    }

    QJSValue updateFn = mod.property(QStringLiteral("update"));
    if (updateFn.isCallable()) {
        QJSValue result = valueArg.isUndefined() ? updateFn.call() : updateFn.call({valueArg});
        if (!result.isError()) return result;
    }
    return QJSValue();
}

void JSEngine::registerScript(const QString& scriptCode) {
    if (scriptCode.isEmpty()) return;
    // Fast path for literal declarations never needs the engine.
    if (scriptCode.contains(QStringLiteral("createScriptProperties"))) {
        loadModule(scriptCode);
    } else {
        loadModule(scriptCode);
    }
    QJSValue mod = loadModule(scriptCode);
    if (mod.isObject()) {
        const std::string key = scriptCode.toStdString();
        if (m_initedModules.find(key) == m_initedModules.end()) {
            m_initedModules.insert(key);
            QJSValue initFn = mod.property(QStringLiteral("init"));
            if (initFn.isCallable()) initFn.call();
        }
    }
}

double JSEngine::evaluateScriptNumber(const QJsonValue& propVal, double fallback) {
    if (propVal.isDouble()) return propVal.toDouble();
    if (!propVal.isObject()) return fallback;

    QJsonObject obj = propVal.toObject();

    if (obj.contains(QStringLiteral("user"))) {
        QString propName;
        if (obj[QStringLiteral("user")].isString()) {
            propName = obj[QStringLiteral("user")].toString();
        } else {
            propName = obj[QStringLiteral("user")].toObject().value(QStringLiteral("name")).toString();
        }
        auto it = m_properties.find(propName.toStdString());
        if (it != m_properties.end()) return it->second.toDouble();
        return obj.contains(QStringLiteral("value")) ? obj[QStringLiteral("value")].toDouble(fallback) : fallback;
    }

    if (obj.contains(QStringLiteral("script"))) {
        QJSValue result;
        // Let animated scripts settle: SceneScript update() implementations
        // ease toward a target over ticks; a few extra ticks give the state
        // a static render would see after a second of runtime.
        for (int i = 0; i < 8; ++i) {
            QJSValue r = callModule(obj[QStringLiteral("script")].toString(), QJSValue(fallback));
            if (r.isNumber() || r.isBool()) result = r;
        }
        if (result.isNumber()) return result.toNumber();
        if (result.isBool()) return result.toBool() ? 1.0 : 0.0;
        return obj.contains(QStringLiteral("value")) ? obj[QStringLiteral("value")].toDouble(fallback) : fallback;
    }

    return obj.contains(QStringLiteral("value")) ? obj[QStringLiteral("value")].toDouble(fallback) : fallback;
}

bool JSEngine::evaluateScript(const QString& scriptCode, const QVariant& defaultValue) {
    if (scriptCode.isEmpty()) return defaultValue.toBool();

    // Fast path: direct let visibility = true/false
    QRegularExpression visRegex(QStringLiteral("let\\s+visibility\\s*=\\s*(true|false)"));
    auto m = visRegex.match(scriptCode);
    if (m.hasMatch()) return m.captured(1) == QStringLiteral("true");

    QJSValue mod = loadModule(scriptCode);
    if (!mod.isObject()) return defaultValue.toBool();

    // Propagate time globals used by SceneScript modules.
    float timeOfDay = static_cast<float>(m_currentHour) / 24.0f + static_cast<float>(m_currentMinute) / 1440.0f;
    m_engineObj.setProperty(QStringLiteral("timeOfDay"), timeOfDay);
    m_engineObj.setProperty(QStringLiteral("time"), timeOfDay * 24.0f);
    m_engineObj.setProperty(QStringLiteral("currentTime"), timeOfDay);

    QJSValue result = callModule(scriptCode, QJSValue(defaultValue.toDouble()));
    if (result.isBool()) return result.toBool();
    if (result.isNumber()) return result.toNumber() != 0;

    // Fall back to a `visibility` binding the module may have set.
    QJSValue visVal = mod.property(QStringLiteral("visibility"));
    if (visVal.isBool()) return visVal.toBool();
    if (visVal.isNumber()) return visVal.toNumber() != 0;

    return defaultValue.toBool();
}

QString JSEngine::getDayNameShort() const {
    static const QStringList days = QStringList()
        << QStringLiteral("Sunday") << QStringLiteral("Monday")
        << QStringLiteral("Tuesday") << QStringLiteral("Wednesday")
        << QStringLiteral("Thursday") << QStringLiteral("Friday")
        << QStringLiteral("Saturday");
    // Convert from Qt's dayOfWeek (1=Monday) to our array index (0=Sunday)
    int idx = m_currentDayOfWeek % 7;
    return days.at(idx);
}

QString JSEngine::getMonthNameShort() const {
    static const QStringList months = QStringList()
        << QStringLiteral("January") << QStringLiteral("February") << QStringLiteral("March")
        << QStringLiteral("April") << QStringLiteral("May") << QStringLiteral("June")
        << QStringLiteral("July") << QStringLiteral("August") << QStringLiteral("September")
        << QStringLiteral("October") << QStringLiteral("November") << QStringLiteral("December");
    return months.at(m_currentMonth - 1);
}

QString JSEngine::getTimeFormatted() const {
    return QString("%1:%2").arg(m_currentHour, 2, 10, QChar('0'))
                           .arg(m_currentMinute, 2, 10, QChar('0'));
}

} // namespace WallpaperEngine::Scene
