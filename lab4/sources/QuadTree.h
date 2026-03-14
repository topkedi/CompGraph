#pragma once

#include <Windows.h>
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <vector>
#include <memory>
#include <cstdint>

enum class DetailLevel
{
    High = 0,
    Medium = 1,
    Low = 2,
    VeryLow = 3
};

struct TreeNode
{
    DirectX::BoundingBox BoundingVolume;
    DirectX::XMFLOAT3 Position;
    float NodeSize;
    int Level;
    DetailLevel Detail;
    bool Leaf;
    
    std::unique_ptr<TreeNode> Quadrants[4];
    
    uint32_t VtxOffset;
    uint32_t IdxOffset;
    uint32_t IdxCount;
    
    DirectX::XMFLOAT2 UVMin;
    DirectX::XMFLOAT2 UVMax;
    
    TreeNode() : NodeSize(0), Level(0), Detail(DetailLevel::High), 
                 Leaf(true), VtxOffset(0), IdxOffset(0), IdxCount(0) {}
};

struct RenderableNode
{
    TreeNode* Node;
    DetailLevel Detail;
    float CameraDistance;
};

class QuadTree
{
public:
    QuadTree();
    ~QuadTree();
    
    void Initialize(float size, int depth, const std::vector<float>& distances);
    void Update(const DirectX::XMFLOAT3& camPos, const DirectX::BoundingFrustum& frustum);
    
    const std::vector<RenderableNode>& GetVisibleNodes() const { return visibleNodes; }
    TreeNode* GetRoot() { return rootNode.get(); }
    int GetMaxDepth() const { return maxDepth; }
    float GetTerrainSize() const { return terrainSize; }

private:
    void ConstructTree(TreeNode* node, float x, float z, float size, int depth);
    void ProcessNode(TreeNode* node, const DirectX::XMFLOAT3& camPos,
                     const DirectX::BoundingFrustum& frustum);
    DetailLevel ComputeDetail(float dist) const;
    
    std::unique_ptr<TreeNode> rootNode;
    std::vector<RenderableNode> visibleNodes;
    std::vector<float> detailDistances;
    float terrainSize;
    int maxDepth;
};
