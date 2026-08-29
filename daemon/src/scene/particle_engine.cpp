#include "particle_engine.h"
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <QRadialGradient>

namespace WallpaperEngine::Scene {

ParticleEngine::ParticleEngine() {
    m_particles.reserve(1024);
}

void ParticleEngine::clear() {
    m_particles.clear();
    m_emitters.clear();
    m_spawnAccumulators.clear();
}

void ParticleEngine::setEmitters(const std::vector<ParticleEmitterConfig>& emitters) {
    clear();
    m_emitters = emitters;
    m_spawnAccumulators.resize(emitters.size(), 0.0f);
}

void ParticleEngine::spawnParticle(size_t emitterIdx, uint32_t screenWidth, uint32_t screenHeight) {
    if (emitterIdx >= m_emitters.size()) return;
    const auto& config = m_emitters[emitterIdx];

    Particle p;
    p.emitterIndex = static_cast<int>(emitterIdx);
    
    p.x = static_cast<float>(rand() % (screenWidth > 0 ? screenWidth : 1920));
    p.y = static_cast<float>(rand() % (screenHeight > 0 ? screenHeight : 1080));

    float angle = static_cast<float>(rand() % 360) * 3.14159f / 180.0f;
    float speed = config.minSpeed + static_cast<float>(rand() % 100) / 100.0f * (config.maxSpeed - config.minSpeed);

    p.vx = std::cos(angle) * speed * 0.4f + config.direction.x() * speed;
    p.vy = std::sin(angle) * speed * 0.4f + config.direction.y() * speed;

    p.maxLife = config.lifetime > 0.5f ? config.lifetime : 4.0f;
    p.life = p.maxLife;
    p.size = config.minSize + static_cast<float>(rand() % 100) / 100.0f * (config.maxSize - config.minSize);
    p.color = config.color;
    p.rotation = static_cast<float>(rand() % 360);
    p.rotSpeed = (static_cast<float>(rand() % 100) - 50.0f) * 0.2f;

    m_particles.push_back(p);
}

void ParticleEngine::update(float dt, uint32_t screenWidth, uint32_t screenHeight) {
    if (m_emitters.empty()) return;

    for (size_t i = 0; i < m_emitters.size(); ++i) {
        const auto& config = m_emitters[i];
        m_spawnAccumulators[i] += config.rate * dt;

        while (m_spawnAccumulators[i] >= 1.0f && m_particles.size() < 1024) {
            spawnParticle(i, screenWidth, screenHeight);
            m_spawnAccumulators[i] -= 1.0f;
        }
    }

    for (auto it = m_particles.begin(); it != m_particles.end();) {
        it->life -= dt;
        if (it->life <= 0.0f) {
            it = m_particles.erase(it);
            continue;
        }

        const auto& config = m_emitters[it->emitterIndex];
        it->vx += config.gravity.x() * dt;
        it->vy += config.gravity.y() * dt;

        it->x += it->vx * dt;
        it->y += it->vy * dt;
        it->rotation += it->rotSpeed * dt;

        // Smooth curved alpha fade (max 0.6 opacity to prevent harsh solid dots)
        float progress = it->life / it->maxLife;
        it->alpha = std::sin(progress * 3.14159f) * 0.55f;

        if (it->x < -50.0f) it->x = static_cast<float>(screenWidth) + 40.0f;
        if (it->x > static_cast<float>(screenWidth) + 50.0f) it->x = -40.0f;
        if (it->y < -50.0f) it->y = static_cast<float>(screenHeight) + 40.0f;
        if (it->y > static_cast<float>(screenHeight) + 50.0f) it->y = -40.0f;

        ++it;
    }
}

void ParticleEngine::render(QPainter& painter, uint32_t screenWidth, uint32_t screenHeight) {
    Q_UNUSED(screenWidth);
    Q_UNUSED(screenHeight);

    for (const auto& p : m_particles) {
        if (p.emitterIndex < 0 || p.emitterIndex >= static_cast<int>(m_emitters.size())) continue;
        const auto& config = m_emitters[p.emitterIndex];

        painter.save();
        if (config.blending == BlendMode::Additive) {
            painter.setCompositionMode(QPainter::CompositionMode_Plus);
        } else {
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        }

        painter.setOpacity(p.alpha);
        painter.translate(p.x, p.y);
        painter.rotate(p.rotation);

        if (!config.particleSprite.isNull()) {
            painter.drawImage(QRectF(-p.size / 2.0f, -p.size / 2.0f, p.size, p.size), config.particleSprite);
        } else {
            // Soft feathered radial glow (no harsh solid snow dots)
            QRadialGradient grad(0, 0, p.size / 2.0f);
            QColor centerColor = p.color;
            centerColor.setAlphaF(0.9f);
            QColor edgeColor = p.color;
            edgeColor.setAlphaF(0.0f);

            grad.setColorAt(0.0, centerColor);
            grad.setColorAt(0.5, centerColor);
            grad.setColorAt(1.0, edgeColor);

            painter.setBrush(QBrush(grad));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(QRectF(-p.size / 2.0f, -p.size / 2.0f, p.size, p.size));
        }

        painter.restore();
    }
}

} // namespace WallpaperEngine::Scene
