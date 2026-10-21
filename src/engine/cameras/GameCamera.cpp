#include "port/LocalMultiplayer.h"
#include <libultraship.h>
#include <libultra/gbi.h>
#include "GameCamera.h"
#include "port/interpolation/FrameInterpolation.h"
#include "engine/Matrix.h"
#include "port/Game.h"
#include <algorithm>
#include <array>
#include <cmath>

extern "C" {
#include "main.h"
#include "code_800029B0.h"
#include "camera.h"
#include "racing/collision.h"
#include "racing/math_util.h"
}

size_t GameCamera::_count = 0;

GameCamera::GameCamera() {
    _camera = &cameras[_count];
    _camera->cameraId = _count;
    _camera->perspectiveMatrix = &PerspectiveMatrix;
    _camera->lookAtMatrix = &LookAtMatrix;

    _count += 1;
}

GameCamera::GameCamera(FVector pos, s16 rot, u32 mode) {
    _camera = &cameras[_count];
    _camera->cameraId = _count;
    _camera->perspectiveMatrix = &PerspectiveMatrix;
    _camera->lookAtMatrix = &LookAtMatrix;
    switch(gGamestate) {
        case RACING:
            _camera->renderMode = RENDER_TRACK_SECTIONS;
            break;
        case ENDING:
        case CREDITS_SEQUENCE:
            _camera->renderMode = RENDER_FULL_SCENE;
            break;
    }
    ProjMode = ProjectionMode::PERSPECTIVE;
    bActive = true;
    if (gGamestate != CREDITS_SEQUENCE) {
        Vec3f spawn = {pos.x, pos.y, pos.z};
        camera_init(spawn, rot, mode, _camera->cameraId);
    }

    _count += 1;
}

void GameCamera::Tick() {
    if (!bActive) { return; }
    if (nullptr == _camera) {
        bActive = false;
        return;
    }

    func_8001EE98(&gPlayers[_camera->playerId], _camera, _camera->cameraId);
    KeepInFrontOfWalls();
}

void GameCamera::KeepInFrontOfWalls() {
    if (!CVarGetInteger("gCameraStayInFrontOfWalls", 0) || gGamestate != RACING ||
        !_camera || _camera->playerId < 0 || _camera->playerId >= NUM_RACERS || !gCollisionMesh) return;
    const auto& player = gPlayers[_camera->playerId];
    if (!(player.type & PLAYER_EXISTS) || (player.type & PLAYER_CINEMATIC_MODE)) return;

    // Trace from the kart side of the wall to the desired camera position.
    // Testing only the camera's final point misses thin walls entirely.
    const FVector anchor(player.pos[0], player.pos[1] + std::max(10.f, player.boundingBoxSize), player.pos[2]);
    const FVector desired(_camera->pos[0], _camera->pos[1], _camera->pos[2]);
    const FVector ray = desired - anchor;
    const float length = std::sqrt(ray.Dot(ray));
    if (length < 1.f) return;
    const float horizontal = std::sqrt(ray.x * ray.x + ray.z * ray.z);
    const FVector right = horizontal > .001f ? FVector(ray.z / horizontal * 4.f, 0, -ray.x / horizontal * 4.f) : FVector(4, 0, 0);
    const std::array<FVector, 5> offsets = {FVector(0, 0, 0), right, right * -1.f, FVector(0, 4, 0), FVector(0, -4, 0)};
    float nearest = 1.f;
    for (u32 i = 0; i < gCollisionMeshCount; ++i) {
        const auto& triangle = gCollisionMesh[i];
        // Cheap segment bounds reject most of the map before triangle work.
        if (triangle.maxX < std::min(anchor.x, desired.x) - 4 || triangle.minX > std::max(anchor.x, desired.x) + 4 ||
            triangle.maxY < std::min(anchor.y, desired.y) - 4 || triangle.minY > std::max(anchor.y, desired.y) + 4 ||
            triangle.maxZ < std::min(anchor.z, desired.z) - 4 || triangle.minZ > std::max(anchor.z, desired.z) + 4 ||
            !triangle.vtx1 || !triangle.vtx2 || !triangle.vtx3) continue;
        const auto vertex = [](const Vtx* v) { return FVector(v->v.ob[0], v->v.ob[1], v->v.ob[2]); };
        const FVector a = vertex(triangle.vtx1);
        const FVector e1 = vertex(triangle.vtx2) - a;
        const FVector e2 = vertex(triangle.vtx3) - a;
        const FVector p = ray.Cross(e2);
        const float determinant = e1.Dot(p);
        if (std::abs(determinant) < .0001f) continue;
        const float inverse = 1.f / determinant;
        // Two-sided intersections also catch a camera already behind a wall.
        for (const auto& offset : offsets) {
            const FVector relative = anchor + offset - a;
            const float u = relative.Dot(p) * inverse;
            if (u < 0.f || u > 1.f) continue;
            const FVector q = relative.Cross(e1);
            const float v = ray.Dot(q) * inverse;
            if (v < 0.f || u + v > 1.f) continue;
            const float hit = e2.Dot(q) * inverse;
            if (hit >= 0.f && hit <= nearest) nearest = hit;
        }
    }
    if (nearest >= 1.f) return;
    const float clearance = std::max(6.f, CM_GetProps()->NearPersp + 2.f);
    const FVector safe = anchor + ray * std::max(0.f, nearest - clearance / length);
    _camera->pos[0] = safe.x;
    _camera->pos[1] = safe.y;
    _camera->pos[2] = safe.z;
    check_bounding_collision(&_camera->collision, 3.f, safe.x, safe.y, safe.z);
    const float dx = _camera->lookAt[0] - safe.x;
    const float dy = _camera->lookAt[1] - safe.y;
    const float dz = _camera->lookAt[2] - safe.z;
    _camera->rot[1] = atan2s(dx, dz);
    _camera->rot[0] = atan2s(std::sqrt(dx * dx + dz * dz), dy);
}

void GameCamera::SetActive(bool state) {
    bActive = state;
}

bool GameCamera::IsActive() {
    return bActive;
}

Camera* GameCamera::Get() {
    return _camera;
}

void GameCamera::SetProjectionMode(GameCamera::ProjectionMode mode) {
    ProjMode = mode;
}

Mtx* GameCamera::GetPerspMatrix() {
    return &PerspectiveMatrix;
}

Mtx* GameCamera::GetLookAtMatrix() {
    return &LookAtMatrix;
}

void GameCamera::SetViewProjection() {
    u16 perspNorm;

    // Tag the camera for the interpolation engine
    FrameInterpolation_RecordOpenChild("camera",
                                       (FrameInterpolation_GetCameraEpoch() | ((_camera->cameraId << 8))));

    // Calculate camera perspective (camera movement/location)
    guPerspective(&PerspectiveMatrix, &perspNorm, _camera->fieldOfView, gScreenAspect,
                  CM_GetProps()->NearPersp, RacerRenderDistance_FarPlane(CM_GetProps()->FarPersp), 1.0f);
    gSPPerspNormalize(gDisplayListHead++, perspNorm);
    gSPMatrix(gDisplayListHead++, &PerspectiveMatrix,
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);

    // Calculate the camera lookAt (camera rotation)
    guLookAt(&LookAtMatrix, _camera->pos[0], _camera->pos[1], _camera->pos[2], _camera->lookAt[0],
             _camera->lookAt[1], _camera->lookAt[2], _camera->up[0], _camera->up[1], _camera->up[2]);
    gSPMatrix(gDisplayListHead++, &LookAtMatrix,
              G_MTX_NOPUSH | G_MTX_MUL | G_MTX_PROJECTION);

    FrameInterpolation_RecordCloseChild();
}
