#pragma once

#include <DirectXMath.h>
#include <DirectXCollision.h>

class Camera
{
public:
  explicit Camera();
  ~Camera();

  void SetProjectionValues(float fovDegrees, float aspectRatio, float nearZ, float farZ);
  void SetOrthographicValues(float width, float height);
  void SetPosition(float x, float y, float z);

  // Movement
  void MoveForward();
  void MoveBackward();
  void MoveLeft();
  void MoveRight();
  void MoveUp();
  void MoveDown();

  // Rotation
  void TurnLeft();
  void TurnRight();
  void TurnUp();
  void TurnDown();
  
  // Rotation with custom angle (in degrees)
  void RotateYaw(float degrees);
  void RotatePitch(float degrees);

  const DirectX::XMMATRIX GetViewMatrix() const { return XMLoadFloat4x4(&m_viewMatrix); }
  const DirectX::XMMATRIX GetProjectionMatrix() const { return XMLoadFloat4x4(&m_projectionMatrix); }
  
  DirectX::BoundingFrustum GetFrustum() const;
  DirectX::XMFLOAT3 GetPosition() const { return m_position; }

private:
  void UpdateViewMatrix();

  const DirectX::XMVECTOR defaultForwardVector = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
  const DirectX::XMVECTOR defaultRightVector = DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
  const DirectX::XMVECTOR defaultUpVector = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

  DirectX::XMFLOAT3 m_forwardVector;

  DirectX::XMFLOAT3 m_position;
  DirectX::XMFLOAT3 m_rotation;

  DirectX::XMFLOAT4X4 m_viewMatrix;
  DirectX::XMFLOAT4X4 m_projectionMatrix;
  DirectX::XMFLOAT4X4 m_orthoMatrix;

  //float m_pitch;
  //float m_yaw;
};
