#pragma once

#include <vector>
#include <string>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <QMatrix4x4>
#include <QImage>
#include <QPolygonF>

namespace WallpaperEngine::Render {

struct MeshVertex {
    QVector3D position;
    QVector2D uv;
    QVector4D boneWeights;
    QVector4D boneIndices;
};

struct MeshTriangle {
    uint32_t i0, i1, i2;
};

// Minimal bone definition matching SceneLayer::Bone (scene_parser.h).
// Kept separate to avoid a render->scene dependency.
struct DeformBone {
    std::string name;
    std::string parent;
    QVector3D pos;      // rest position (scene/sprite space)
    QVector3D angle;    // rest angle (degrees)
    float weight = 1.0f;

    // Animated state (filled by the caller each frame; identity = rest pose)
    QVector3D animatedPos = QVector3D(0.0f, 0.0f, 0.0f);
    QVector3D animatedAngle = QVector3D(0.0f, 0.0f, 0.0f);
};

class MeshDeformer {
public:
    MeshDeformer() = default;
    ~MeshDeformer() = default;

    static void generateDefaultGrid(uint32_t cols, uint32_t rows, float width, float height,
                                    std::vector<MeshVertex>& outVertices, std::vector<MeshTriangle>& outIndices);

    static void deformVertices(std::vector<MeshVertex>& vertices, float time, float speed, float strength, float direction);

    // Puppet-warp bone deformation stub. Builds world-space bone matrices
    // (parent chain composed), assigns each grid vertex to its nearest bone(s)
    // distance-based weights; identity transform in rest pose.
    // so output is bit-identical to an undeformed grid. No GPU/GL involved.
    // Returns false when there are no bones (caller should use the plain path).
    static bool boneWeightedDeform(std::vector<MeshVertex>& vertices,
                                   const std::vector<DeformBone>& bones,
                                   float spriteW, float spriteH);

    static void renderDeformedMesh(QPainter& painter, const QImage& texture,
                                   const std::vector<MeshVertex>& vertices,
                                   const std::vector<MeshTriangle>& indices,
                                   float originX, float originY, float spriteW, float spriteH);
};

} // namespace WallpaperEngine::Render
