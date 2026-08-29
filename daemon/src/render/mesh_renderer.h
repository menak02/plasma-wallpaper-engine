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

class MeshDeformer {
public:
    MeshDeformer() = default;
    ~MeshDeformer() = default;

    static void generateDefaultGrid(uint32_t cols, uint32_t rows, float width, float height,
                                    std::vector<MeshVertex>& outVertices, std::vector<MeshTriangle>& outIndices);

    static void deformVertices(std::vector<MeshVertex>& vertices, float time, float speed, float strength, float direction);

    static void renderDeformedMesh(QPainter& painter, const QImage& texture,
                                   const std::vector<MeshVertex>& vertices,
                                   const std::vector<MeshTriangle>& indices,
                                   float originX, float originY, float spriteW, float spriteH);
};

} // namespace WallpaperEngine::Render
