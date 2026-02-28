#include "Camera.h"

using namespace DirectX;

Camera::Camera() :
  m_position(0.0f, 0.0f, 0.0f),
  m_forwardVector(0.0f, 0.0f, 0.0f),
  m_rotation(0, 0, 0)
{
  // Position camera to see the terrain
  // Center is at (1024, 0, 1024), so position camera above and back
  SetPosition(1024, 300, 200);
}

Camera::~Camera()
{
}

void Camera::SetProjectionValues(float fovDegrees, float aspectRatio, float nearZ, float farZ)
{
  float fovRadians = (fovDegrees / 360.0f) * XM_2PI;

  XMStoreFloat4x4(&m_projectionMatrix, XMMatrixTranspose(XMMatrixPerspectiveFovLH(fovRadians, aspectRatio, nearZ, farZ)));
}

void Camera::SetOrthographicValues(float width, float height)
{
  XMStoreFloat4x4(&m_orthoMatrix, XMMatrixTranspose(XMMatrixOrthographicOffCenterLH(0, width, height, 0, 0.0f, 100.0f)));
}

void Camera::SetPosition(float x, float y, float z)
{
  m_position.x = x;
  m_position.y = y;
  m_position.z = z;

  UpdateViewMatrix();
}

void Camera::MoveForward()
{
  float forwardSpeed = 10.0f; // Increased speed

  XMStoreFloat3(&m_position, XMVector3Transform(XMLoadFloat3(&m_position), XMMatrixTranslation(m_forwardVector.x * forwardSpeed, m_forwardVector.y * forwardSpeed, m_forwardVector.z * forwardSpeed)));

  UpdateViewMatrix();
}

void Camera::MoveBackward()
{
  float forwardSpeed = 10.0f; // Increased speed

  XMStoreFloat3(&m_position, XMVector3Transform(XMLoadFloat3(&m_position), XMMatrixTranslation(-m_forwardVector.x * forwardSpeed, -m_forwardVector.y * forwardSpeed, -m_forwardVector.z * forwardSpeed)));

  UpdateViewMatrix();
}

void Camera::MoveLeft()
{
  float strafeSpeed = 10.0f;

  float pitch = XMConvertToRadians(m_rotation.x);
  float yaw = XMConvertToRadians(m_rotation.y);

  XMMATRIX camRotationMatrix = XMMatrixRotationRollPitchYaw(pitch, yaw, 0);
  XMVECTOR rightDir = XMVector3TransformCoord(defaultRightVector, camRotationMatrix);

  XMFLOAT3 rightVector;
  XMStoreFloat3(&rightVector, rightDir);

  XMStoreFloat3(&m_position, XMVector3Transform(XMLoadFloat3(&m_position), XMMatrixTranslation(-rightVector.x * strafeSpeed, -rightVector.y * strafeSpeed, -rightVector.z * strafeSpeed)));

  UpdateViewMatrix();
}

void Camera::MoveRight()
{
  float strafeSpeed = 10.0f;

  float pitch = XMConvertToRadians(m_rotation.x);
  float yaw = XMConvertToRadians(m_rotation.y);

  XMMATRIX camRotationMatrix = XMMatrixRotationRollPitchYaw(pitch, yaw, 0);
  XMVECTOR rightDir = XMVector3TransformCoord(defaultRightVector, camRotationMatrix);

  XMFLOAT3 rightVector;
  XMStoreFloat3(&rightVector, rightDir);

  XMStoreFloat3(&m_position, XMVector3Transform(XMLoadFloat3(&m_position), XMMatrixTranslation(rightVector.x * strafeSpeed, rightVector.y * strafeSpeed, rightVector.z * strafeSpeed)));

  UpdateViewMatrix();
}

void Camera::MoveUp()
{
  float verticalSpeed = 10.0f;
  m_position.y += verticalSpeed;
  UpdateViewMatrix();
}

void Camera::MoveDown()
{
  float verticalSpeed = 10.0f;
  m_position.y -= verticalSpeed;
  UpdateViewMatrix();
}

void Camera::TurnLeft()
{
  float leftTurnSpeed = 1.0f; // Reduced rotation speed

  // Update the rotation.
  m_rotation.y -= leftTurnSpeed;

  // Keep the rotation in the 0 to 360 range.
  if (m_rotation.y < 0.0f)
  {
    m_rotation.y += 360.0f;
  }

  UpdateViewMatrix();
}

void Camera::TurnRight()
{
  float rightTurnSpeed = 1.0f; // Reduced rotation speed

  // Update the rotation.
  m_rotation.y += rightTurnSpeed;

  // Keep the rotation in the 0 to 360 range.
  if (m_rotation.y > 360.0f)
  {
    m_rotation.y -= 360.0f;
  }

  UpdateViewMatrix();
}

void Camera::TurnUp()
{
  float upTurnSpeed = 1.0f; // Reduced rotation speed

  // Update the rotation.
  m_rotation.x -= upTurnSpeed;

  // Keep the rotation in the 0 to 360 range.
  if (m_rotation.x < 0.0f)
  {
    m_rotation.x += 360.0f;
  }

  UpdateViewMatrix();
}

void Camera::TurnDown()
{
  float downTurnSpeed = 1.0f; // Reduced rotation speed

  // Update the rotation.
  m_rotation.x += downTurnSpeed;

  // Keep the rotation in the 0 to 360 range.
  if (m_rotation.x > 360.0f)
  {
    m_rotation.x -= 360.0f;
  }

  UpdateViewMatrix();
}

void Camera::RotateYaw(float degrees)
{
  m_rotation.y += degrees;
  
  // Keep the rotation in the 0 to 360 range.
  while (m_rotation.y < 0.0f) m_rotation.y += 360.0f;
  while (m_rotation.y >= 360.0f) m_rotation.y -= 360.0f;
  
  UpdateViewMatrix();
}

void Camera::RotatePitch(float degrees)
{
  m_rotation.x += degrees;
  
  // Clamp pitch to avoid flipping (-89 to 89 degrees)
  if (m_rotation.x > 89.0f) m_rotation.x = 89.0f;
  if (m_rotation.x < -89.0f) m_rotation.x = -89.0f;
  
  UpdateViewMatrix();
}

void Camera::UpdateViewMatrix()
{
  float pitch = XMConvertToRadians(m_rotation.x);
  float yaw = XMConvertToRadians(m_rotation.y);

  XMMATRIX camRotationMatrix = XMMatrixRotationRollPitchYaw(pitch, yaw, 0); // I do not need roll

  XMVECTOR upDir = XMVector3TransformCoord(defaultUpVector, camRotationMatrix);
  XMVECTOR forwardDir = XMVector3TransformCoord(defaultForwardVector, camRotationMatrix);
  XMVECTOR rightDir = XMVector3TransformCoord(defaultRightVector, camRotationMatrix);

  XMStoreFloat3(&m_forwardVector, forwardDir);

  XMVECTOR positionVector = XMLoadFloat3(&m_position);
  forwardDir += positionVector;

  XMStoreFloat4x4(&m_viewMatrix, XMMatrixTranspose(XMMatrixLookAtLH(positionVector, forwardDir, upDir)));
}

DirectX::BoundingFrustum Camera::GetFrustum() const
{
  XMMATRIX proj = XMMatrixTranspose(XMLoadFloat4x4(&m_projectionMatrix));
  
  // Create frustum from projection matrix
  BoundingFrustum frustum(proj);
  
  // Transform frustum to world space using inverse view matrix
  XMMATRIX view = XMMatrixTranspose(XMLoadFloat4x4(&m_viewMatrix));
  XMMATRIX invView = XMMatrixInverse(nullptr, view);
  frustum.Transform(frustum, invView);
  
  return frustum;
}
