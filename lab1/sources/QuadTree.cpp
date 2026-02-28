#include "QuadTree.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;

QuadTree::QuadTree()
    : terrainSize(0), maxDepth(0)
{
}

QuadTree::~QuadTree()
{
}

void QuadTree::Initialize(float size, int depth, const std::vector<float>& distances)
{
    terrainSize = size;
    maxDepth = depth;
    detailDistances = distances;
    
    rootNode = std::make_unique<TreeNode>();
    ConstructTree(rootNode.get(), 0, 0, size, 0);
}

void QuadTree::ConstructTree(TreeNode* node, float x, float z, float size, int depth)
{
    node->NodeSize = size;
    node->Level = depth;
    node->Position = XMFLOAT3(x + size * 0.5f, 0, z + size * 0.5f);
    
    float yMin = 0.0f;
    float yMax = 800.0f;
    node->BoundingVolume = BoundingBox(
        XMFLOAT3(x + size * 0.5f, (yMin + yMax) * 0.5f, z + size * 0.5f),
        XMFLOAT3(size * 0.5f, (yMax - yMin) * 0.5f, size * 0.5f)
    );
    
    node->UVMin = XMFLOAT2(x / terrainSize, 1.0f - (z + size) / terrainSize);
    node->UVMax = XMFLOAT2((x + size) / terrainSize, 1.0f - z / terrainSize);
    
    if (depth >= maxDepth)
    {
        node->Leaf = true;
        return;
    }
    
    node->Leaf = false;
    float half = size * 0.5f;
    
    node->Quadrants[0] = std::make_unique<TreeNode>();
    ConstructTree(node->Quadrants[0].get(), x, z, half, depth + 1);
    
    node->Quadrants[1] = std::make_unique<TreeNode>();
    ConstructTree(node->Quadrants[1].get(), x + half, z, half, depth + 1);
    
    node->Quadrants[2] = std::make_unique<TreeNode>();
    ConstructTree(node->Quadrants[2].get(), x, z + half, half, depth + 1);
    
    node->Quadrants[3] = std::make_unique<TreeNode>();
    ConstructTree(node->Quadrants[3].get(), x + half, z + half, half, depth + 1);
}

void QuadTree::Update(const XMFLOAT3& camPos, const BoundingFrustum& frustum)
{
    visibleNodes.clear();
    
    if (rootNode)
    {
        ProcessNode(rootNode.get(), camPos, frustum);
    }
}

void QuadTree::ProcessNode(TreeNode* node, const XMFLOAT3& camPos,
                            const BoundingFrustum& frustum)
{
    if (frustum.Contains(node->BoundingVolume) == DISJOINT)
    {
        return;
    }
    
    float dx = node->Position.x - camPos.x;
    float dy = node->Position.y - camPos.y;
    float dz = node->Position.z - camPos.z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    
    DetailLevel detail = ComputeDetail(dist);
    node->Detail = detail;
    
    int reqDepth = static_cast<int>(detail);
    bool subdivide = !node->Leaf && (maxDepth - node->Level) > reqDepth;
    
    if (subdivide)
    {
        for (int i = 0; i < 4; i++)
        {
            if (node->Quadrants[i])
            {
                ProcessNode(node->Quadrants[i].get(), camPos, frustum);
            }
        }
    }
    else
    {
        RenderableNode rn;
        rn.Node = node;
        rn.Detail = detail;
        rn.CameraDistance = dist;
        visibleNodes.push_back(rn);
    }
}

DetailLevel QuadTree::ComputeDetail(float dist) const
{
    for (size_t i = 0; i < detailDistances.size(); i++)
    {
        if (dist < detailDistances[i])
        {
            return static_cast<DetailLevel>(i);
        }
    }
    
    return static_cast<DetailLevel>(detailDistances.size());
}
