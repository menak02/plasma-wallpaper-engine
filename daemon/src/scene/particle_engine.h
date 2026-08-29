#pragma once

#include <vector>
#include <QImage>
#include <QPainter>
#include "scene_parser.h"

namespace WallpaperEngine::Scene {

struct Particle {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float size = 16.0f;
    float life = 1.0f;
    float maxLife = 1.0f;
    float alpha = 1.0f;
    float rotation = 0.0f;
    float rotSpeed = 0.0f;
    QColor color{255, 255, 255, 255};
    int emitterIndex = 0;
};

class ParticleEngine {
public:
    ParticleEngine();
    ~ParticleEngine() = default;

    void setEmitters(const std::vector<ParticleEmitterConfig>& emitters);
    void update(float dt, uint32_t screenWidth, uint32_t screenHeight);
    void render(QPainter& painter, uint32_t screenWidth, uint32_t screenHeight);
    void clear();

private:
    std::vector<ParticleEmitterConfig> m_emitters;
    std::vector<Particle> m_particles;
    std::vector<float> m_spawnAccumulators;

    void spawnParticle(size_t emitterIdx, uint32_t screenWidth, uint32_t screenHeight);
};

} // namespace WallpaperEngine::Scene
