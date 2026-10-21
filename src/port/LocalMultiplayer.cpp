#include "LocalMultiplayer.h"
#include "MultiplayerDisplays.h"
#include <libultraship.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <limits>
#include <numeric>
#include <queue>
#include <utility>
#include <spdlog/spdlog.h>
#include "port/Game.h"
#include "port/Engine.h"
#include "port/audio/HMAS.h"
#include "engine/World.h"
#include "engine/Cup.h"
#include "engine/TrackBrowser.h"
#include "engine/editor/Editor.h"
#include "engine/Matrix.h"
#include <assets/models/data_segment2.h>

extern "C" {
#include "main.h"
#include "menus.h"
#include "menu_items.h"
#include "sounds.h"
#include "save.h"
#include "defines.h"
#include "code_800029B0.h"
#include "code_80005FD0.h"
#include "code_80057C60.h"
#include "racing/race_logic.h"
#include "racing/skybox_and_splitscreen.h"
#include "audio/external.h"
#include "objects.h"
#include "render_objects.h"
#include "replays.h"
#include "racing/math_util.h"
#include "racing/collision.h"
#include "actor_types.h"
extern f32 gTimePlayerLastTouchedFinishLine[];
}

namespace {
bool active = false;
bool lobby = false;
bool optionsInitialized = false;
bool results = false;
bool paused = false;
int pausePlayer = 0;
int pauseChoice = 0;
MenuItem pauseCursor{};
int battleResultChoice = 0;
MenuItem battleResultCursor{};
bool raceResultStandings = false;
int raceResultChoice = 0;
MenuItem raceResultCursor{};
int battleSetupRequest = 0; // Deferred until the native main menu has initialized.
bool resumeBattleSession = false;
bool gameMenu = false;
bool startedFromGameMenu = false;
enum class GameMenuStep { Players, Mode, Class, Confirm, Characters, Course };
GameMenuStep gameMenuStep = GameMenuStep::Players;
int characterEditor = 0;
int courseField = 0;
int coursePreviewTrack = 0;
int coursePreviewTimer = 0;
int coursePreviewCup = -1;
int participants = 8;
// Keep the game-select cursor separate from the active race snapshot.
int menuPlayerCount = 8;
int racers = 8; // Race snapshot: humans have screens; bots occupy the remaining grid slots.
int mode = 0; // 0: Grand Prix, 1: VS, 2: Battle, 3: Time Trials with bots.
int engineClass = 1;
int selectedCup = 0;
int selectedTrack = 0;
int selectedArena = 0;
int battleRemaining = 0;
int battleWinner = -1;
int raceInCup = 0;
int request = 0; // 1: new session, 2: next race, 3: rematch, 4: lobby, 5: refresh menu, 6: ceremony, 9: finale cheat
bool ceremonyPending = false;
int ceremonyRacers = 4;
bool finalLapMusicStarted = false;
int ceremonyCharacter = 0;
int ceremonyRank = 0;
std::array<int, NUM_RACERS> characters = {0, 1, 2, 3, 4, 5, 6, 7};
std::array<bool, MAX_HUMAN_PLAYERS> ready = {};
// Native portrait order differs from the racing character IDs.
constexpr std::array<int, 8> characterGrid = {0, 1, 6, 3, 2, 4, 5, 7};
std::array<bool, NUM_RACERS> finished = {};
std::array<float, NUM_RACERS> finishTimes = {};
std::array<int, NUM_RACERS> finishingPlace = {};
std::array<bool, NUM_RACERS> estimatedTime = {};
std::array<int, NUM_RACERS> points = {};
bool progressDisplay = false; // C-right switches the one shared map/progress display.
int sharedHUDPlayer = 0; // Highlight the player who last changed the shared display.
struct AfkState {
    double idleSeconds = 0;
    double botSeconds = 0;
    double stuckSeconds = 0;
    bool driving = false;
    bool inputReplaced = false;
    u16 previousButtons = 0;
    double battlePlanAt = 0;
    double battleDebugAt = 0;
    double battleFireAt = 0;
    double battleRecoverUntil = 0;
    bool battleBomb = false;
    int battleTarget = -1;
    int battleItem = -1;
    int battlePatrol = -1;
    double battlePatrolUntil = 0;
    int battleBlockedPatrol = -1;
    double battleBlockedPatrolUntil = 0;
    FVector battleMotionGoal{};
    double battleMotionAt = 0;
    bool battleMotionSafe = false;
    double battleRouteBlockedSeconds = 0;
    bool battleReconnecting = false;
    FVector battleReconnectGoal{};
    double battleReconnectUntil = 0;
    int battleBlockedItem = -1;
    double battleBlockedItemUntil = 0;
    int battleLastTurnError = 32768;
    double battleTraceAt = 0;
    bool battlePivoting = false;
    s16 battlePivotYaw = 0;
    int battleProgressIntent = -1000;
    double battleGoalProgressAt = 0;
    float battleBestRemaining = std::numeric_limits<float>::infinity();
    float battleTravelSinceProgress = 0;
    FVector battleLastPosition{};
    double battleDefensiveSeconds = 0;
    int battleBlockedTarget = -1;
    double battleBlockedTargetUntil = 0;
    int battleRecoveries = 0;
    FVector battleProgress{};
    FVector battleGoal{};
    std::vector<int> battleRoute;
    size_t battleWaypoint = 0;
    struct Controller input{};
};
std::array<AfkState, MAX_HUMAN_PLAYERS> afkPlayers{};

void ReleaseAfkPlayer(int id) {
    auto& state = afkPlayers[id];
    auto& player = gPlayers[id];
    if (state.driving && (player.type & PLAYER_HUMAN) &&
        !(player.type & PLAYER_CINEMATIC_MODE) && gModeSelection != BATTLE) {
        player.type &= ~PLAYER_CPU;
        player.effects &= ~DRIFTING_EFFECT;
    }
    state = {};
}

// Battle navigation uses the arena's collision surfaces, including separate
// floors at the same X/Z. The graph is shared by all takeover bots in a match.
struct BattleNode {
    FVector pos;
    std::vector<int> neighbours;
};
struct BattleNavigation {
    bool built = false;
    std::vector<BattleNode> nodes;
    std::vector<float> distance;
    std::vector<int> parent;
};
BattleNavigation battleNavigation;

FVector BattlePosition(const Player& player) {
    return FVector(player.pos[0], player.pos[1], player.pos[2]);
}

float BattleDistance(const FVector& a, const FVector& b) {
    const auto delta = a - b;
    return std::sqrt(delta.Dot(delta));
}

bool VerticalBattleAI() {
    return gModeSelection == BATTLE && (IsBlockFort() || IsDoubleDeck());
}

bool FlatBattleAI() {
    return gModeSelection == BATTLE && (IsBigDonut() || IsSkyscraper());
}

// Unlike a highest-floor-below query, this identifies the surface on the
// requested deck. Ramps remain floors even when native collision classifies
// their dominant axis as X or Z rather than Y.
bool BattleTriangleHeight(const CollisionTriangle& triangle, float x, float z, float& height) {
    if (!triangle.vtx1 || !triangle.vtx2 || !triangle.vtx3 ||
        x < triangle.minX - .1f || x > triangle.maxX + .1f ||
        z < triangle.minZ - .1f || z > triangle.maxZ + .1f) return false;
    const auto* a = triangle.vtx1->v.ob;
    const auto* b = triangle.vtx2->v.ob;
    const auto* c = triangle.vtx3->v.ob;
    const float denominator = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
    if (std::abs(denominator) < .001f) return false;
    const float u = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / denominator;
    const float v = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / denominator;
    if (u < -.001f || v < -.001f || u + v > 1.001f) return false;
    height = u * a[1] + v * b[1] + (1.f - u - v) * c[1];
    return true;
}

int BattleCollisionCell(float x, float z) {
    const int cellX = (gTrackMaxX - gTrackMinX) / GRID_SIZE;
    const int cellZ = (gTrackMaxZ - gTrackMinZ) / GRID_SIZE;
    if (cellX <= 0 || cellZ <= 0 || x < gTrackMinX || z < gTrackMinZ) return -1;
    const int ix = static_cast<int>((x - gTrackMinX) / cellX);
    const int iz = static_cast<int>((z - gTrackMinZ) / cellZ);
    return ix >= 0 && iz >= 0 && ix < GRID_SIZE && iz < GRID_SIZE ? iz * GRID_SIZE + ix : -1;
}

bool BattleDeckFloor(float x, float z, float reference, float& floor) {
    const int cell = BattleCollisionCell(x, z);
    if (cell < 0 || !gCollisionIndices) return false;
    const auto& grid = gCollisionGrid[cell];
    float best = 12.f;
    bool found = false;
    for (u32 i = 0; i < grid.numTriangles; ++i) {
        const u16 index = gCollisionIndices[grid.triangle + i];
        if (index >= gCollisionMeshCount) continue;
        const auto& triangle = gCollisionMesh[index];
        if (triangle.normalY < .55f || (triangle.flags & 0x1000)) continue;
        float height;
        if (!BattleTriangleHeight(triangle, x, z, height)) continue;
        const float difference = std::abs(height - reference);
        if (difference < best) { best = difference; floor = height; found = true; }
    }
    return found;
}

bool BattleCanDriveDecks(const FVector& from, const FVector& to) {
    const auto delta = to - from;
    const float length = std::sqrt(delta.x * delta.x + delta.z * delta.z);
    if (std::abs(delta.y) > length * 1.45f + 3.f) return false;
    const int samples = std::max(1, static_cast<int>(std::ceil(length / 6.f)));
    const FVector side = length > .01f ? FVector(-delta.z / length * 7.f, 0, delta.x / length * 7.f) : FVector(7, 0, 0);
    float previousFloor = from.y - 6.f;
    float previousClearance = 0;
    bool leavingWall = false;
    bool corridorEstablished = false;
    for (int sample = 0; sample <= samples; ++sample) {
        auto point = from + delta * (static_cast<float>(sample) / samples);
        float floor;
        if (!BattleDeckFloor(point.x, point.z, point.y - 6.f, floor) ||
            (sample > 0 && std::abs(floor - previousFloor) > 12.f)) return false;
        previousFloor = floor;
        point.y = floor + 6.f;
        bool supportedSides = true;
        for (int offset : {-1, 1}) {
            float sideFloor;
            const auto probe = point + side * static_cast<float>(offset);
            if (!BattleDeckFloor(probe.x, probe.z, floor, sideFloor) || std::abs(sideFloor - floor) > 9.f) supportedSides = false;
        }
        // A real kart can be closer to a wall than the planning corridor's
        // width. Its first side probe may lie inside the fort. Allow only the
        // initial 18 units to leave that margin, keeping center-floor support
        // throughout; once clear, never permit a new unsupported side.
        if (!supportedSides && (corridorEstablished || sample * length / samples > 18.f || sample == samples)) return false;
        if (supportedSides) corridorEstablished = true;
        const int cell = BattleCollisionCell(point.x, point.z);
        if (cell < 0) return false;
        Collision collision{};
        collision.meshIndexZX = collision.meshIndexYX = collision.meshIndexZY = 0xFFFF;
        collision.surfaceDistance[0] = collision.surfaceDistance[1] = 1000.f;
        const auto& grid = gCollisionGrid[cell];
        for (u32 i = 0; i < grid.numTriangles; ++i) {
            const u16 index = gCollisionIndices[grid.triangle + i];
            if (index >= gCollisionMeshCount) continue;
            const auto& triangle = gCollisionMesh[index];
            // Do not mistake a drivable slope for a wall. Still reject low
            // overhead surfaces so floors never connect through a ceiling.
            if (triangle.normalY >= .55f) continue;
            if (triangle.normalY <= -.55f) {
                float ceiling;
                if (BattleTriangleHeight(triangle, point.x, point.z, ceiling) &&
                    ceiling > floor + 2.f && ceiling < floor + 16.f) return false;
            } else if (triangle.flags & FACING_X_AXIS) {
                check_collision_zy(&collision, 8.f, point.x, point.y, point.z, index);
            } else if (triangle.flags & FACING_Z_AXIS) {
                check_collision_yx(&collision, 8.f, point.x, point.y, point.z, index);
            }
        }
        const float clearance = std::min(collision.surfaceDistance[0], collision.surfaceDistance[1]);
        if (sample == 0 && clearance < 0) leavingWall = true;
        if (clearance < 0 && (!leavingWall || (sample > 0 && clearance < previousClearance - .1f))) return false;
        if (clearance >= 0) leavingWall = false;
        if (sample == samples && clearance < 0) return false;
        previousClearance = clearance;
    }
    return true;
}

// Sample the full kart corridor, not just endpoints: no cutting through thin
// walls, diagonal corners, the donut hole or gaps between elevated platforms.
bool BattleCanDrive(const FVector& from, const FVector& to) {
    if (!gCollisionMesh || !gCollisionMeshCount) return false;
    if (VerticalBattleAI()) return BattleCanDriveDecks(from, to);
    const auto delta = to - from;
    const float length = std::sqrt(delta.x * delta.x + delta.z * delta.z);
    const int samples = std::max(1, static_cast<int>(std::ceil(length / 14.f)));
    const FVector side = length > .01f ? FVector(-delta.z / length * 8.f, 0, delta.x / length * 8.f) : FVector(8, 0, 0);
    float previousWallClearance = 0.f;
    bool leavingWall = false;
    for (int sample = 0; sample <= samples; ++sample) {
        const auto point = from + delta * (static_cast<float>(sample) / samples);
        for (int offset = -1; offset <= 1; ++offset) {
            const auto probe = point + side * static_cast<float>(offset);
            const float floor = spawn_actor_on_surface(probe.x, probe.y + 16.f, probe.z);
            if (std::abs(floor + 6.f - point.y) > 18.f) return false;
        }
        Collision collision{};
        collision.meshIndexZX = collision.meshIndexYX = collision.meshIndexZY = 0xFFFF;
        check_bounding_collision(&collision, 9.f, point.x, point.y, point.z);
        const float wallClearance = std::min(collision.surfaceDistance[0], collision.surfaceDistance[1]);
        // The real kart is smaller than the planning corridor. A kart resting
        // against a wall may start inside this margin; permit motion out of
        // it, never deeper through the wall, and require a clear destination.
        if (sample == 0 && wallClearance < 0.f) leavingWall = true;
        if (wallClearance < 0.f) {
            if (!leavingWall || (sample > 0 && wallClearance < previousWallClearance - .1f)) return false;
        } else leavingWall = false;
        if (sample == samples && wallClearance < 0.f) return false;
        previousWallClearance = wallClearance;
        if (collision.meshIndexZX < gCollisionMeshCount &&
            (gCollisionMesh[collision.meshIndexZX].flags & 0x1000)) return false;
    }
    return true;
}

void BuildBattleNavigation() {
    auto& nav = battleNavigation;
    if (nav.built) return;
    nav.built = true;
    if (!gCollisionMesh || !gCollisionMeshCount) return;
    float minX = 32767, maxX = -32768, minZ = 32767, maxZ = -32768;
    for (u32 i = 0; i < gCollisionMeshCount; ++i) {
        const auto& triangle = gCollisionMesh[i];
        if (triangle.normalY < .55f || (triangle.flags & 0x1000)) continue;
        minX = std::min(minX, static_cast<float>(triangle.minX));
        maxX = std::max(maxX, static_cast<float>(triangle.maxX));
        minZ = std::min(minZ, static_cast<float>(triangle.minZ));
        maxZ = std::max(maxZ, static_cast<float>(triangle.maxZ));
    }
    if (minX >= maxX || minZ >= maxZ) return;
    const bool vertical = VerticalBattleAI();
    const float step = vertical ? std::max(24.f, std::max(maxX - minX, maxZ - minZ) / 180.f) :
                                 std::max(48.f, std::max(maxX - minX, maxZ - minZ) / 120.f);
    const int width = static_cast<int>((maxX - minX) / step) + 1;
    const int depth = static_cast<int>((maxZ - minZ) / step) + 1;
    std::vector<std::vector<int>> cells(width * depth);
    // Rasterize upward-facing collision triangles into a bounded layered grid.
    for (u32 i = 0; i < gCollisionMeshCount; ++i) {
        const auto& triangle = gCollisionMesh[i];
        if (triangle.normalY < .55f || (triangle.flags & 0x1000) ||
            !triangle.vtx1 || !triangle.vtx2 || !triangle.vtx3) continue;
        const auto vertex = [](const Vtx* v) { return FVector(v->v.ob[0], v->v.ob[1], v->v.ob[2]); };
        const auto a = vertex(triangle.vtx1), b = vertex(triangle.vtx2), c = vertex(triangle.vtx3);
        const float denominator = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        if (std::abs(denominator) < .001f) continue;
        const int left = std::clamp(static_cast<int>((triangle.minX - minX) / step), 0, width - 1);
        const int right = std::clamp(static_cast<int>((triangle.maxX - minX) / step) + 1, 0, width - 1);
        const int top = std::clamp(static_cast<int>((triangle.minZ - minZ) / step), 0, depth - 1);
        const int bottom = std::clamp(static_cast<int>((triangle.maxZ - minZ) / step) + 1, 0, depth - 1);
        for (int z = top; z <= bottom; ++z) for (int x = left; x <= right; ++x) {
            const float px = minX + x * step, pz = minZ + z * step;
            const float u = ((b.z - c.z) * (px - c.x) + (c.x - b.x) * (pz - c.z)) / denominator;
            const float v = ((c.z - a.z) * (px - c.x) + (a.x - c.x) * (pz - c.z)) / denominator;
            if (u < -.001f || v < -.001f || u + v > 1.001f) continue;
            const FVector point(px, u * a.y + v * b.y + (1 - u - v) * c.y + 6.f, pz);
            auto& cell = cells[z * width + x];
            if (cell.size() >= 12 || std::any_of(cell.begin(), cell.end(), [&](int n) {
                return std::abs(nav.nodes[n].pos.y - point.y) < 4.f;
            }) || !BattleCanDrive(point, point)) continue;
            cell.push_back(static_cast<int>(nav.nodes.size()));
            nav.nodes.push_back({point, {}});
        }
    }
    constexpr int offsets[4][2] = {{1, 0}, {0, 1}, {1, 1}, {-1, 1}};
    for (int z = 0; z < depth; ++z) for (int x = 0; x < width; ++x) {
        for (const auto& offset : offsets) {
            const int nx = x + offset[0], nz = z + offset[1];
            if (nx < 0 || nx >= width || nz >= depth) continue;
            for (int from : cells[z * width + x]) for (int to : cells[nz * width + nx]) {
                const auto& a = nav.nodes[from].pos;
                const auto& b = nav.nodes[to].pos;
                const float dx = a.x - b.x, dz = a.z - b.z;
                const float allowedRise = vertical ? std::sqrt(dx * dx + dz * dz) * 1.45f + 3.f : step * .8f;
                if (std::abs(a.y - b.y) > allowedRise || !BattleCanDrive(a, b)) continue;
                nav.nodes[from].neighbours.push_back(to);
                nav.nodes[to].neighbours.push_back(from);
            }
        }
    }
    SPDLOG_INFO("Battle AI: built {} navigation nodes from arena collision surfaces; vertical routing={}", nav.nodes.size(), vertical);
    if (vertical) {
        size_t links = 0, rampLinks = 0, isolated = 0;
        for (const auto& node : nav.nodes) {
            if (node.neighbours.empty()) ++isolated;
            for (int neighbour : node.neighbours) {
                ++links;
                if (std::abs(node.pos.y - nav.nodes[neighbour].pos.y) > 3.f) ++rampLinks;
            }
        }
        SPDLOG_INFO("Vertical battle graph: links={} ramp links={} isolated nodes={}", links / 2, rampLinks / 2, isolated);
    }
}

int BattleNearestNode(const FVector& position, bool reachableOnly = false) {
    const auto& nodes = battleNavigation.nodes;
    float best = 140.f;
    int nearest = -1;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
        if (VerticalBattleAI() && nodes[i].neighbours.empty()) continue;
        if (reachableOnly && !std::isfinite(battleNavigation.distance[i])) continue;
        const float distance = BattleDistance(nodes[i].pos, position);
        if (distance < best && std::abs(nodes[i].pos.y - position.y) < 40.f &&
            BattleCanDrive(position, nodes[i].pos)) { best = distance; nearest = i; }
    }
    return nearest;
}

bool BattleRivalAlive(int id, int other) {
    const auto& rival = gPlayers[other];
    return other != id && (rival.type & PLAYER_EXISTS) && gPlayerBalloonCount[other] >= 0 &&
           !(rival.type & (PLAYER_INVISIBLE_OR_BOMB | PLAYER_CINEMATIC_MODE));
}

void PlanBattleRoute(int id) {
    auto& state = afkPlayers[id];
    auto& nav = battleNavigation;
    const auto position = BattlePosition(gPlayers[id]);
    const bool vertical = VerticalBattleAI();
    const bool flat = FlatBattleAI();
    // Finish a checked two-leg connection before selecting a new one. Repeated
    // replanning halfway around a corner otherwise alternates escape headings.
    if (vertical && state.battleReconnecting && state.botSeconds < state.battleReconnectUntil &&
        BattleDistance(position, state.battleReconnectGoal) > 12.f &&
        BattleCanDrive(position, state.battleReconnectGoal)) return;
    state.battleReconnecting = false;
    state.battleRouteBlockedSeconds = 0;
    // A failed plan must not leave an obsolete route active. Local recovery
    // below can reconnect without continuing to steer into the old obstacle.
    state.battleRoute.clear();
    state.battleWaypoint = 0;
    state.battleGoal = position;
    nav.distance.assign(nav.nodes.size(), std::numeric_limits<float>::infinity());
    nav.parent.assign(nav.nodes.size(), -1);
    using Entry = std::pair<float, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
    int start = -1;
    if (vertical) {
        // Join every nearby, directly reachable part of the graph, rather
        // than letting one nearest node choose a disconnected ramp fragment.
        std::vector<Entry> candidates;
        for (int node = 0; node < static_cast<int>(nav.nodes.size()); ++node) {
            const float distance = BattleDistance(position, nav.nodes[node].pos);
            if (distance < 140.f && std::abs(nav.nodes[node].pos.y - position.y) < 40.f &&
                !nav.nodes[node].neighbours.empty()) candidates.emplace_back(distance, node);
        }
        std::sort(candidates.begin(), candidates.end());
        int seeds = 0;
        for (const auto& [distance, node] : candidates) {
            if (!BattleCanDrive(position, nav.nodes[node].pos)) continue;
            if (start < 0) start = node;
            nav.distance[node] = distance;
            pending.emplace(distance, node);
            if (++seeds >= 12) break;
        }
        if (start < 0) {
            // The coarse graph need not contain a node on the kart's side of
            // a corner. Find a supported intermediate step, then connect that
            // step to the graph. Both legs use the same wall/deck checks.
            float bestConnection = std::numeric_limits<float>::infinity();
            FVector connection{};
            for (float reach : {24.f, 48.f}) for (int heading = 0; heading < 16; ++heading) {
                const float radians = heading * (3.14159265359f / 8.f);
                auto step = position + FVector(-std::sin(radians), 0, std::cos(radians)) * reach;
                float floor;
                if (!BattleDeckFloor(step.x, step.z, position.y - 6.f, floor)) continue;
                step.y = floor + 6.f;
                if (!BattleCanDrive(position, step)) continue;
                const int node = BattleNearestNode(step);
                if (node < 0) continue;
                const float cost = BattleDistance(position, step) + BattleDistance(step, nav.nodes[node].pos);
                if (cost < bestConnection) { bestConnection = cost; start = node; connection = step; }
            }
            if (start >= 0) {
                nav.distance[start] = bestConnection;
                pending.emplace(bestConnection, start);
                state.battleReconnecting = true;
                state.battleReconnectGoal = connection;
                state.battleReconnectUntil = state.botSeconds + 2.5;
            }
        }
    } else {
        start = BattleNearestNode(position);
        if (start >= 0) {
            nav.distance[start] = 0;
            pending.emplace(0.f, start);
        }
    }
    if (start < 0) {
        if (vertical) {
            state.battleItem = state.battleTarget = -1;
            state.battleMotionAt = 0;
            state.battlePivoting = false;
        }
        if (state.botSeconds >= state.battleDebugAt) {
            SPDLOG_INFO("Battle AI route unavailable: slot {} has no reachable start node at ({}, {}, {}), graph={}",
                        id + 1, position.x, position.y, position.z, nav.nodes.size());
            state.battleDebugAt = state.botSeconds + 5;
        }
        return;
    }
    while (!pending.empty()) {
        const auto [cost, node] = pending.top(); pending.pop();
        if (cost > nav.distance[node]) continue;
        for (int next : nav.nodes[node].neighbours) {
            const float climbing = VerticalBattleAI() ? std::abs(nav.nodes[node].pos.y - nav.nodes[next].pos.y) * 1.5f : 0.f;
            const float nextCost = cost + BattleDistance(nav.nodes[node].pos, nav.nodes[next].pos) + climbing;
            if (nextCost >= nav.distance[next]) continue;
            nav.distance[next] = nextCost;
            nav.parent[next] = node;
            pending.emplace(nextCost, next);
        }
    }
    int goal = -1, target = -1, itemTarget = -1;
    float best = std::numeric_limits<float>::infinity();
    const int heldItem = gPlayers[id].currentItemCopy;
    const bool offensiveItem = heldItem == ITEM_GREEN_SHELL || heldItem == ITEM_TRIPLE_GREEN_SHELL ||
                               heldItem == ITEM_RED_SHELL || heldItem == ITEM_TRIPLE_RED_SHELL || heldItem == ITEM_BLUE_SPINY_SHELL;
    const bool armed = vertical ? state.battleBomb || offensiveItem || (gPlayers[id].effects & STAR_EFFECT) :
                                 state.battleBomb || heldItem != ITEM_NONE || (gPlayers[id].triggers & DRAG_ITEM_EFFECT);
    const bool needsItem = vertical ? !armed && heldItem == ITEM_NONE : !armed;
    // Unarmed bots seek real, currently collectible boxes. Fake boxes are hazards.
    if (needsItem) for (size_t itemIndex = 0; itemIndex < GetWorld()->Actors.size(); ++itemIndex) {
        const auto& actor = GetWorld()->Actors[itemIndex];
        if (!actor || actor->bPendingDestroy || actor->Type != ACTOR_ITEM_BOX || actor->State != 2) continue;
        if ((vertical || flat) && static_cast<int>(itemIndex) == state.battleBlockedItem &&
            state.botSeconds < state.battleBlockedItemUntil) continue;
        // Native item boxes store their support-floor height in Unk_08. Route
        // to that deck, not the floating visual center above the platform.
        const FVector box(actor->Pos[0], (vertical || flat) ? actor->Unk_08 + 6.f : actor->Pos[1], actor->Pos[2]);
        const int node = BattleNearestNode(box, vertical);
        if (node < 0 || !std::isfinite(nav.distance[node]) || !BattleCanDrive(nav.nodes[node].pos, box)) continue;
        float score = nav.distance[node];
        // On multi-level arenas retain a reachable box long enough to climb
        // its ramp rather than switching floors whenever another box is closer.
        if (VerticalBattleAI() && static_cast<int>(itemIndex) == state.battleItem) score -= 180.f;
        if (flat && static_cast<int>(itemIndex) == state.battleItem) score -= 100.f;
        // Prefer boxes with less competition instead of all bots queuing at one.
        for (int other = 0; other < LocalMP_Count(); ++other)
            if (BattleRivalAlive(id, other) && BattleDistance(BattlePosition(gPlayers[other]), box) < 100.f) score += 100.f;
        if (score < best) { best = score; goal = node; state.battleGoal = box; itemTarget = static_cast<int>(itemIndex); }
    }
    state.battleItem = itemTarget;
    // Keep an opponent for aiming even while heading for ammunition.
    float rivalScore = std::numeric_limits<float>::infinity();
    FVector rivalGoal = position;
    int rivalNode = -1;
    for (int other = 0; other < LocalMP_Count(); ++other) {
        if (!BattleRivalAlive(id, other)) continue;
        if ((vertical || flat) && other == state.battleBlockedTarget && state.botSeconds < state.battleBlockedTargetUntil) continue;
        const auto& rival = gPlayers[other];
        const auto predicted = BattlePosition(rival) + FVector(rival.velocity[0], 0, rival.velocity[2]) * 8.f;
        const int node = BattleNearestNode(BattlePosition(rival), vertical);
        if (node < 0 || !std::isfinite(nav.distance[node])) continue;
        const float score = nav.distance[node] + (other == state.battleTarget ? -120.f : 0.f) +
                            gPlayerBalloonCount[other] * 25.f +
                            ((rival.effects & STAR_EFFECT) ? 500.f : 0.f);
        if (score < rivalScore) {
            rivalScore = score; target = other; rivalNode = node;
            rivalGoal = BattleCanDrive(BattlePosition(rival), predicted) ? predicted : BattlePosition(rival);
        }
    }
    state.battleTarget = target;
    if (armed || (!vertical && goal < 0)) { goal = rivalNode; state.battleGoal = rivalGoal; }
    if (vertical && !armed) state.battleTarget = -1;
    if (goal < 0 && vertical) {
        // Reachable targets can temporarily disappear (picked-up boxes, rivals
        // behind walls/on other decks). Explore a connected ramp/landing instead
        // of parking. Keep the patrol goal for several seconds to make progress.
        if (state.battlePatrol >= 0 && state.battlePatrol < static_cast<int>(nav.nodes.size()) &&
            state.botSeconds < state.battlePatrolUntil &&
            std::isfinite(nav.distance[state.battlePatrol]) && nav.distance[state.battlePatrol] > 30.f) {
            goal = state.battlePatrol;
        } else {
            float patrolScore = std::numeric_limits<float>::infinity();
            for (int node = 0; node < static_cast<int>(nav.nodes.size()); ++node) {
                const float distance = nav.distance[node];
                if (!std::isfinite(distance) || distance < 35.f || nav.nodes[node].neighbours.empty()) continue;
                if (state.battleBlockedPatrol >= 0 && state.botSeconds < state.battleBlockedPatrolUntil &&
                    BattleDistance(nav.nodes[node].pos, nav.nodes[state.battleBlockedPatrol].pos) < 120.f) continue;
                const float height = std::abs(nav.nodes[node].pos.y - position.y);
                const float score = std::abs(distance - 320.f) - std::min(height, 120.f) * .5f +
                                    ((node + id * 43) % 23) * 2.f + (node == state.battlePatrol ? 200.f : 0.f);
                if (score < patrolScore) { patrolScore = score; goal = node; }
            }
            state.battlePatrol = goal;
            state.battlePatrolUntil = state.botSeconds + 6;
        }
        if (goal >= 0) state.battleGoal = nav.nodes[goal].pos;
    }
    if (goal < 0) {
        if (state.botSeconds >= state.battleDebugAt) {
            SPDLOG_INFO("Battle AI route unavailable: slot {} start={} has no reachable box or survivor, bomb={}",
                        id + 1, start, state.battleBomb);
            state.battleDebugAt = state.botSeconds + 5;
        }
        return;
    }
    for (int n = goal; n >= 0; n = nav.parent[n]) state.battleRoute.push_back(n);
    std::reverse(state.battleRoute.begin(), state.battleRoute.end());
    state.battleWaypoint = !vertical && !flat && state.battleRoute.size() > 1 ? 1 : 0;
    if (vertical || flat) {
        const int intent = state.battleItem >= 0 ? 10000 + state.battleItem :
                           state.battleTarget >= 0 ? 20000 + state.battleTarget : 30000 + state.battlePatrol;
        const float remaining = nav.distance[goal] + (flat ? BattleDistance(position, nav.nodes[start].pos) : 0.f);
        if (intent != state.battleProgressIntent || remaining < state.battleBestRemaining - 20.f) {
            state.battleProgressIntent = intent;
            state.battleBestRemaining = remaining;
            state.battleGoalProgressAt = state.botSeconds;
            state.battleTravelSinceProgress = 0;
        }
    }
}

FVector SelectVerticalBattleMotion(int id, FVector goal) {
    auto& state = afkPlayers[id];
    const auto& player = gPlayers[id];
    const auto position = BattlePosition(player);
    if (state.botSeconds < state.battleMotionAt && BattleDistance(position, state.battleMotionGoal) > 14.f)
        return state.battleMotionGoal;
    state.battleMotionAt = state.botSeconds + .12;
    state.battleMotionSafe = false;
    if (state.battleRoute.empty()) {
        // Reconnect to the nearby navigation network instead of braking on a
        // failed start-node query. Motion still requires a safe floor corridor.
        float nearest = 200.f;
        for (const auto& node : battleNavigation.nodes) {
            const float distance = BattleDistance(position, node.pos);
            if (node.neighbours.size() >= 2 && std::abs(node.pos.y - position.y) < 20.f && distance < nearest) {
                nearest = distance; goal = node.pos;
            }
        }
    }
    Vec3f destination = {goal.x, goal.y, goal.z};
    const float goalDistance = BattleDistance(position, goal);
    const s16 desiredYaw = goalDistance > 5.f ? -get_angle_between_two_vectors(gPlayers[id].pos, destination) : player.rotation[1];
    const float reach = goalDistance > 5.f ? std::min(std::clamp(32.f + player.speed * 10.f, 32.f, 65.f), goalDistance) : 40.f;
    float best = std::numeric_limits<float>::infinity();
    const auto tryHeading = [&](s16 yaw, float probeLength) {
        const float radians = yaw * (3.14159265359f / 32768.f);
        FVector point = position + FVector(-std::sin(radians), 0, std::cos(radians)) * probeLength;
        const auto toGoal = goal - position;
        const float horizontalSquared = toGoal.x * toGoal.x + toGoal.z * toGoal.z;
        const float projected = horizontalSquared > 1.f ?
            ((point.x - position.x) * toGoal.x + (point.z - position.z) * toGoal.z) / horizontalSquared : 0.f;
        float floor;
        const float reference = position.y - 6.f + std::clamp(projected, -1.f, 1.f) * (goal.y - position.y);
        if (!BattleDeckFloor(point.x, point.z, reference, floor) || std::abs(floor + 6.f - position.y) > probeLength * 1.45f + 3.f) return;
        point.y = floor + 6.f;
        if (!BattleCanDrive(position, point)) return;
        const int goalTurn = std::abs(static_cast<int>(static_cast<s16>(yaw - desiredYaw)));
        const int bodyTurn = std::abs(static_cast<int>(static_cast<s16>(yaw - player.rotation[1])));
        const float score = BattleDistance(point, goal) - goalDistance + goalTurn * .0012f + bodyTurn * .0007f;
        if (score < best) { best = score; state.battleMotionGoal = point; state.battleMotionSafe = true; }
    };
    tryHeading(desiredYaw, reach);
    // Score real steering alternatives around the kart. This makes a blocked
    // approach turn along an open corridor rather than keep accelerating at it.
    for (int direction = -8; direction < 8; ++direction)
        tryHeading(static_cast<s16>(player.rotation[1] + direction * 0x1000), reach);
    // At the foot/crest of a ramp a long straight probe can fail even though
    // the first part is drivable. Take a short supported step, then re-evaluate
    // from its new floor instead of alternating reverse and wall-facing turns.
    if (!state.battleMotionSafe) for (float shortReach : {24.f, 12.f}) {
        tryHeading(desiredYaw, shortReach);
        for (int direction = -8; direction < 8; ++direction)
            tryHeading(static_cast<s16>(player.rotation[1] + direction * 0x1000), shortReach);
        if (state.battleMotionSafe) break;
    }
    if (!state.battleMotionSafe) state.battleMotionGoal = position;
    return state.battleMotionGoal;
}

void DriveAfkBattlePlayer(int id) {
    auto& state = afkPlayers[id];
    auto& player = gPlayers[id];
    auto& controller = gControllers[id];
    state.input = controller;
    state.inputReplaced = true;
    state.botSeconds += TRACK_TIMER_ITER;
    const auto position = BattlePosition(player);
    const bool vertical = VerticalBattleAI();
    const bool flat = FlatBattleAI();
    if (vertical || flat) {
        state.battleTravelSinceProgress += BattleDistance(position, state.battleLastPosition);
        state.battleLastPosition = position;
        const int item = player.currentItemCopy;
        const bool defensive = item == ITEM_BANANA || item == ITEM_BANANA_BUNCH || item == ITEM_FAKE_ITEM_BOX ||
                               (player.triggers & DRAG_ITEM_EFFECT);
        state.battleDefensiveSeconds = defensive ? state.battleDefensiveSeconds + TRACK_TIMER_ITER : 0;
    }
    const bool bomb = gPlayerBalloonCount[id] < 0;
    if (state.battleBomb != bomb) {
        state.battleBomb = bomb;
        state.battlePlanAt = 0;
        state.battleRoute.clear();
        state.battleWaypoint = 0;
        state.battleTarget = -1;
        state.stuckSeconds = 0;
        state.battleRecoverUntil = 0;
        state.battleMotionAt = 0;
        SPDLOG_INFO("Battle AI: slot {} switched to {} strategy", id + 1, bomb ? "bomb pursuit" : "balloon combat");
    }
    BuildBattleNavigation();
    if (state.botSeconds >= state.battlePlanAt) {
        PlanBattleRoute(id);
        state.battlePlanAt = state.botSeconds + .65 + (id % 4) * .07;
    }
    if ((vertical || flat) && !state.battleRoute.empty()) {
        // Track the route actually being followed even between successful
        // replans. A moving target or a failed start query cannot hide a loop.
        float remaining = BattleDistance(battleNavigation.nodes[state.battleRoute.back()].pos, state.battleGoal);
        FVector previous = position;
        if (state.battleReconnecting) {
            remaining += BattleDistance(previous, state.battleReconnectGoal);
            previous = state.battleReconnectGoal;
        }
        for (size_t waypoint = state.battleWaypoint; waypoint < state.battleRoute.size(); ++waypoint) {
            const auto& point = battleNavigation.nodes[state.battleRoute[waypoint]].pos;
            remaining += BattleDistance(previous, point);
            previous = point;
        }
        if (remaining < state.battleBestRemaining - 20.f) {
            state.battleBestRemaining = remaining;
            state.battleGoalProgressAt = state.botSeconds;
            state.battleTravelSinceProgress = 0;
        }
    }
    if ((vertical || flat) && state.battleProgressIntent != -1000 &&
        state.botSeconds - state.battleGoalProgressAt > 5 && state.battleTravelSinceProgress > 180.f) {
        SPDLOG_INFO("{} battle AI circling: slot={} travel={:.1f} remaining={:.1f} box={} target={}; changing goal",
                    vertical ? "Vertical" : "Flat", id + 1, state.battleTravelSinceProgress,
                    state.battleBestRemaining, state.battleItem, state.battleTarget + 1);
        if (state.battleItem >= 0) {
            state.battleBlockedItem = state.battleItem;
            state.battleBlockedItemUntil = state.botSeconds + 8;
        }
        if (state.battleTarget >= 0) {
            state.battleBlockedTarget = state.battleTarget;
            state.battleBlockedTargetUntil = state.botSeconds + 8;
        }
        if (state.battlePatrol >= 0) {
            state.battleBlockedPatrol = state.battlePatrol;
            state.battleBlockedPatrolUntil = state.botSeconds + 8;
        }
        state.battlePatrolUntil = 0;
        state.battleProgressIntent = -1000;
        state.battleMotionAt = 0;
        state.battlePivoting = false;
        state.battleReconnecting = false;
        PlanBattleRoute(id);
    }
    FVector goal = state.battleGoal;
    if (vertical && state.battleReconnecting && BattleDistance(position, state.battleReconnectGoal) < 12.f)
        state.battleReconnecting = false;
    if (vertical || flat) {
        while (!state.battleReconnecting && state.battleWaypoint < state.battleRoute.size() &&
               BattleDistance(position, battleNavigation.nodes[state.battleRoute[state.battleWaypoint]].pos) < (flat ? 26.f : 12.f))
            ++state.battleWaypoint;
    }
    if (!state.battleReconnecting && state.battleWaypoint < state.battleRoute.size()) {
        // Skip intermediate points only when the entire corridor is walkable.
        const size_t lookaheadEnd = state.battleWaypoint + (vertical ? 3 : 5);
        for (size_t next = state.battleWaypoint + 1; next < state.battleRoute.size() &&
             next < (vertical ? lookaheadEnd : state.battleWaypoint + 5); ++next) {
            const auto& point = battleNavigation.nodes[state.battleRoute[next]].pos;
            if (!BattleCanDrive(position, point)) break;
            state.battleWaypoint = next;
        }
        goal = battleNavigation.nodes[state.battleRoute[state.battleWaypoint]].pos;
        if (BattleDistance(position, goal) < (vertical ? 12.f : 26.f)) ++state.battleWaypoint;
        if (state.battleWaypoint >= state.battleRoute.size() && BattleCanDrive(position, state.battleGoal)) goal = state.battleGoal;
    }
    // A target may move against a wall after the route was planned. Keep the
    // final safe waypoint instead of repeatedly braking toward an invalid goal.
    if (!state.battleRoute.empty() && state.battleWaypoint >= state.battleRoute.size() &&
        !BattleCanDrive(position, state.battleGoal)) {
        goal = battleNavigation.nodes[state.battleRoute.back()].pos;
        state.battlePlanAt = 0;
    }
    if (vertical && state.battleReconnecting) goal = state.battleReconnectGoal;
    if (vertical) {
        // Do not keep a valid-looking but now unreachable waypoint after a
        // collision pushes the kart around a corner or onto another deck.
        state.battleRouteBlockedSeconds = !state.battleRoute.empty() && !BattleCanDrive(position, goal) ?
            state.battleRouteBlockedSeconds + TRACK_TIMER_ITER : 0;
        if (state.battleRouteBlockedSeconds > .6) {
            state.battleRoute.clear();
            state.battleWaypoint = 0;
            state.battleGoal = goal = position;
            state.battleMotionAt = state.battlePlanAt = 0;
            state.battlePivoting = false;
            state.battleReconnecting = false;
            state.battleRouteBlockedSeconds = 0;
        }
    }
    // Bombs attack by touching a survivor. At close range leave the coarse
    // navigation waypoint and steer for a short, collision-checked intercept.
    if (state.battleBomb && state.battleTarget >= 0 && BattleRivalAlive(id, state.battleTarget)) {
        const auto& rival = gPlayers[state.battleTarget];
        const auto target = BattlePosition(rival);
        const float distance = BattleDistance(position, target);
        if (distance < 200.f && std::abs(target.y - position.y) < 25.f) {
            const float lead = distance < 35.f ? 0.f : std::min(18.f, distance / 7.f);
            const auto intercept = target + FVector(rival.velocity[0], 0, rival.velocity[2]) * lead;
            if (BattleCanDrive(position, intercept)) goal = intercept;
            else if (BattleCanDrive(position, target)) goal = target;
        }
    }
    const auto angleTo = [&](const FVector& point) {
        Vec3f destination = {point.x, point.y, point.z};
        return static_cast<s16>(-get_angle_between_two_vectors(player.pos, destination) - player.rotation[1]);
    };
    const FVector driveGoal = vertical ? SelectVerticalBattleMotion(id, goal) : goal;
    const s16 driveTurn = (vertical || flat) && state.battlePivoting ? static_cast<s16>(state.battlePivotYaw - player.rotation[1]) : angleTo(driveGoal);
    const int turnError = std::abs(static_cast<int>(driveTurn));
    int steering = std::clamp(static_cast<int>(driveTurn) / 80, -80, 80);
    u16 buttons = state.battleRoute.empty() && !(vertical && state.battleMotionSafe) ? B_BUTTON : A_BUTTON;
    if (std::abs(steering) > 55 && player.speed > 1.5f) buttons |= B_BUTTON;
    if (vertical) {
        const bool ramp = std::abs(goal.y - position.y) > 6.f || player.collision.orientationVector[1] < .96f;
        const float safeSpeed = ramp ? 2.1f : std::abs(steering) > 35 ? 2.5f : 4.f;
        if (player.speed > safeSpeed) buttons = B_BUTTON;
    }

    // Real displacement catches wall pushing even if wheel speed stays high.
    if (BattleDistance(position, state.battleProgress) > 35.f ||
        ((vertical || flat) && turnError > 0x1800 && turnError + 64 < state.battleLastTurnError)) {
        state.battleProgress = position; state.stuckSeconds = 0;
    } else state.stuckSeconds += TRACK_TIMER_ITER;
    state.battleLastTurnError = turnError;
    if (state.stuckSeconds > 1.8 && state.botSeconds >= state.battleRecoverUntil) {
        ++state.battleRecoveries;
        state.battleRecoverUntil = state.botSeconds + .85;
        state.stuckSeconds = 0;
        state.battlePlanAt = 0;
        if (vertical || flat) {
            state.battleMotionAt = 0;
            state.battlePivoting = false;
            if (state.battleItem >= 0 && state.battleRecoveries > 1) {
                state.battleBlockedItem = state.battleItem;
                state.battleBlockedItemUntil = state.botSeconds + 8;
            }
        }
    }
    const bool reversing = state.botSeconds < state.battleRecoverUntil;
    if (reversing) {
        buttons = B_BUTTON;
        // Native reverse needs B AND stick Y below -49 after deadzone/clamp.
        // Strong sideways input reduces Y, so use a modest recovery turn.
        steering = ((id + state.battleRecoveries) & 1) ? 35 : -35;
        if (vertical && state.battleMotionSafe) {
            const s16 reverseTurn = static_cast<s16>(angleTo(driveGoal) + 0x8000);
            steering = -std::clamp(static_cast<int>(reverseTurn) / 256, -35, 35);
        }
    } else {
        // Short local lookahead handles changed circumstances between plans.
        const auto delta = driveGoal - position;
        const float length = std::sqrt(delta.x * delta.x + delta.z * delta.z);
        if (length > 5.f && !BattleCanDrive(position, position + delta * (std::min(length, 38.f) / length))) {
            buttons = B_BUTTON;
            state.battlePlanAt = 0;
        }
        if (vertical && state.battleMotionSafe) {
            // Probe where momentum/body heading will actually carry the kart,
            // not just the line to its desired waypoint. Brake before a wall,
            // then use the game's normal A+B pivot to face the open corridor.
            FVector forward(-std::sin(player.rotation[1] * (3.14159265359f / 32768.f)), 0,
                             std::cos(player.rotation[1] * (3.14159265359f / 32768.f)));
            const float travel = std::sqrt(player.velocity[0] * player.velocity[0] + player.velocity[2] * player.velocity[2]);
            if (travel > .6f && !(player.kartProps & BACK_UP))
                forward = FVector(player.velocity[0] / travel, 0, player.velocity[2] / travel);
            const float lookahead = std::clamp(15.f + player.speed * 8.f, 15.f, 45.f);
            auto ahead = position + forward * lookahead;
            const float goalHorizontal = delta.x * delta.x + delta.z * delta.z;
            const float rampReference = goalHorizontal > 1.f ?
                std::clamp((forward.x * delta.x + forward.z * delta.z) * lookahead / goalHorizontal, -1.5f, 1.5f) * delta.y : 0.f;
            float floor;
            const bool aheadSupported = BattleDeckFloor(ahead.x, ahead.z, position.y - 6.f + rampReference, floor);
            if (aheadSupported) ahead.y = floor + 6.f;
            const bool blockedAhead = !aheadSupported || !BattleCanDrive(position, ahead);
            if (blockedAhead && player.speed > .6f) buttons = B_BUTTON;
            else if ((turnError > 0x2400 || (blockedAhead && turnError > 0x400)) && player.speed < .7f) buttons = A_BUTTON | B_BUTTON;
            else if (blockedAhead) buttons = B_BUTTON;
        }
        // Dodge nearby ground traps and moving shells when there is safe floor
        // on the escape side. Do not dodge orbiting/held items as projectiles.
        if (length > 5.f) for (const auto& actor : GetWorld()->Actors) {
            if (!actor || actor->bPendingDestroy || actor->Flags == 0) continue;
            const bool shell = (actor->Type == ACTOR_GREEN_SHELL || actor->Type == ACTOR_RED_SHELL) &&
                               (actor->State == MOVING_SHELL || actor->State == RED_SHELL_LOCK_ON);
            const bool trap = (actor->Type == ACTOR_BANANA && actor->State == BANANA_ON_GROUND) ||
                              (actor->Type == ACTOR_FAKE_ITEM_BOX && actor->State == FAKE_ITEM_BOX_ON_GROUND);
            if (!shell && !trap) continue;
            const FVector hazard(actor->Pos[0], actor->Pos[1], actor->Pos[2]);
            if (std::abs(hazard.y - position.y) > 22.f || BattleDistance(position, hazard) > (shell ? 100.f : 65.f)) continue;
            if (std::abs(static_cast<int>(angleTo(hazard))) > 0x1800) continue;
            const int side = angleTo(hazard) >= 0 ? -1 : 1;
            const auto forward = delta * (1.f / length);
            // Verify either side geometrically; steering follows the engine's
            // angle convention rather than assuming world-axis handedness.
            const FVector lateral(-forward.z * 28.f, 0, forward.x * 28.f);
            FVector escape = position + forward * 40.f + lateral * static_cast<float>(side);
            if (!BattleCanDrive(position, escape)) escape = position + forward * 40.f - lateral * static_cast<float>(side);
            if (BattleCanDrive(position, escape)) steering = std::clamp(static_cast<int>(angleTo(escape)) / 65, -80, 80);
            else buttons = B_BUTTON;
            break;
        }
    }

    const bool safeApproach = vertical ? state.battleMotionSafe :
        flat && !state.battleRoute.empty() && BattleDistance(position, driveGoal) > 5.f && BattleCanDrive(position, driveGoal);
    if ((vertical || flat) && !reversing) {
        if (!safeApproach) {
            // A failed motion query has no steering destination. Accelerating
            // toward the kart's own position previously created aimless loops.
            buttons = B_BUTTON;
            state.battlePivoting = false;
        } else {
            const bool sharpApproach = turnError > 0x3800 ||
                (BattleDistance(position, driveGoal) < 90.f && turnError > 0x2000);
            if (!state.battlePivoting && sharpApproach) {
                state.battlePivoting = true;
                state.battlePivotYaw = static_cast<s16>(player.rotation[1] + angleTo(driveGoal));
            }
            if (state.battlePivoting) {
                const s16 error = static_cast<s16>(state.battlePivotYaw - player.rotation[1]);
                steering = std::clamp(static_cast<int>(error) / 65, -80, 80);
                if (std::abs(static_cast<int>(error)) < 0x700) {
                    state.battlePivoting = false;
                    state.battleMotionAt = 0;
                    buttons = A_BUTTON;
                } else {
                    // Brake first, then turn with normal stationary A+B input.
                    // Holding a constant turn while accelerating makes an orbit
                    // whose radius can be larger than the distance to the box.
                    buttons = player.speed > .35f ? B_BUTTON : A_BUTTON | B_BUTTON;
                }
            }
        }
    }
    int stickY = reversing ? -80 : 0;
    if (state.stuckSeconds > 1.f && state.botSeconds >= state.battleDebugAt) {
        SPDLOG_INFO("Battle AI stalled: slot {} route={} target={} bomb={} speed={} balloons={} recovering={}",
                    id + 1, state.battleRoute.size(), state.battleTarget + 1, state.battleBomb,
                    player.speed, gPlayerBalloonCount[id], reversing);
        state.battleDebugAt = state.botSeconds + 5;
    }
    if (state.botSeconds >= state.battleFireAt && state.botSeconds >= state.battleRecoverUntil) {
        bool fire = false;
        const int item = player.currentItemCopy;
        const bool dragging = (player.triggers & DRAG_ITEM_EFFECT) != 0;
        if ((vertical || flat) && !state.battleBomb && state.battleDefensiveSeconds > 2.5 &&
            (item == ITEM_BANANA || item == ITEM_BANANA_BUNCH || item == ITEM_FAKE_ITEM_BOX || dragging) &&
            safeApproach && !state.battlePivoting) fire = true;
        if (!state.battleBomb && (item == ITEM_STAR || item == ITEM_BOO || item == ITEM_THUNDERBOLT)) fire = true;
        if (item >= ITEM_MUSHROOM && item <= ITEM_SUPER_MUSHROOM) {
            const auto delta = ((vertical || flat) ? state.battleGoal : goal) - position;
            const float distance = std::sqrt(delta.Dot(delta));
            const float boostDistance = vertical ? 120.f : 180.f;
            fire = distance > boostDistance && std::abs(steering) < 20 && !state.battlePivoting &&
                   BattleCanDrive(position, position + delta * (boostDistance / distance));
        }
        if (!state.battleBomb && state.battleTarget >= 0 && BattleRivalAlive(id, state.battleTarget)) {
            const auto& rival = gPlayers[state.battleTarget];
            const auto target = BattlePosition(rival);
            const int turn = std::abs(static_cast<int>(angleTo(target)));
            const float distance = BattleDistance(position, target);
            const bool sameLevel = std::abs(target.y - position.y) < 25.f;
            const bool clear = sameLevel && distance < 800.f && BattleCanDrive(position, target);
            if (item == ITEM_GREEN_SHELL || item == ITEM_TRIPLE_GREEN_SHELL) {
                fire = clear && distance < 450.f && (turn < 0x900 || turn > 0x7300);
                if (turn > 0x7300) stickY = -80;
            } else if (item == ITEM_RED_SHELL || item == ITEM_TRIPLE_RED_SHELL || item == ITEM_BLUE_SPINY_SHELL) {
                fire = clear && turn < 0x1800;
            } else if (item == ITEM_BANANA || item == ITEM_BANANA_BUNCH || item == ITEM_FAKE_ITEM_BOX || dragging) {
                fire = ((vertical || flat) && fire) || (clear && distance < 180.f && turn > 0x5800);
                // Prefer a nearby pursuer; also deploy traps after holding
                // them for a while instead of circling with them indefinitely.
            }
        }
        if (fire) {
            buttons |= Z_TRIG;
            state.battleFireAt = state.botSeconds + .8 + (id % 3) * .12;
        }
    }
    if ((vertical || flat) && CVarGetInteger("gDiagnostics.DetailedCrashLogging", 0) && state.botSeconds >= state.battleTraceAt) {
        SPDLOG_INFO("{} battle AI: slot={} task={} held={} target={} box={} route={}/{} pos=({:.1f},{:.1f},{:.1f}) goal=({:.1f},{:.1f},{:.1f}) drive=({:.1f},{:.1f},{:.1f}) yaw={} turn={} steer={} stickY={} buttons={:04x} safe={} reverse={} pivot={} reconnect={} goalAge={:.1f} remaining={:.1f} travelled={:.1f} drag={}",
                    vertical ? "Vertical" : "Flat", id + 1, state.battleBomb ? "bomb" : state.battleItem >= 0 ? "collect" : state.battleTarget >= 0 ? "attack" : "explore",
                    player.currentItemCopy, state.battleTarget + 1, state.battleItem, state.battleWaypoint, state.battleRoute.size(),
                    position.x, position.y, position.z, goal.x, goal.y, goal.z, driveGoal.x, driveGoal.y, driveGoal.z,
                    player.rotation[1], turnError, steering, stickY, buttons, safeApproach, reversing,
                    state.battlePivoting, state.battleReconnecting, state.botSeconds - state.battleGoalProgressAt, state.battleBestRemaining,
                    state.battleTravelSinceProgress, (player.triggers & DRAG_ITEM_EFFECT) != 0);
        state.battleTraceAt = state.botSeconds + 2;
    }
    controller = {};
    controller.rawStickX = static_cast<s16>(steering);
    controller.rawStickY = static_cast<s16>(stickY);
    controller.button = buttons;
    controller.buttonPressed = buttons & ~state.previousButtons;
    controller.buttonDepressed = state.previousButtons & ~buttons;
    state.previousButtons = buttons;
}

void UseAfkRaceItems(int id) {
    auto& state = afkPlayers[id];
    auto& controller = gControllers[id];
    state.input = controller;
    state.inputReplaced = true;
    state.botSeconds += TRACK_TIMER_ITER;
    // Keep native CPU steering, but supply normal item-button edges for the
    // human inventory, including follow-up throws and multi-use mushrooms.
    const u16 buttons = std::fmod(state.botSeconds + id * .13, 1.5) < TRACK_TIMER_ITER * 1.1 ? Z_TRIG : 0;
    controller = {};
    controller.button = buttons;
    controller.buttonPressed = buttons & ~state.previousButtons;
    controller.buttonDepressed = state.previousButtons & ~buttons;
    state.previousButtons = buttons;
}
const char* characterNames[] = {"Mario", "Luigi", "Yoshi", "Toad", "Donkey Kong", "Wario", "Peach", "Bowser"};
const int rewards[] = {9, 6, 3, 1};

void SetPaused(bool value, int player = 0) {
    paused = value;
    if (value) {
        pausePlayer = std::clamp(player, 0, participants - 1);
        pauseChoice = 0;
        pauseCursor = {};
        pauseCursor.paramf = 1.5f;
        pauseCursor.subState = 1;
    }
    gIsGamePaused = value ? pausePlayer + 1 : 0;
    func_800C9F90(value ? 1 : 0);
    // Menu presses must not trigger racing actions on the resume frame.
    for (int i = 0; i < participants; ++i) {
        gControllers[i].buttonPressed = 0;
        gControllers[i].stickPressed = 0;
    }
}

void NativeText(int x, int y, std::string text, int color = TEXT_YELLOW,
                float scale = .5f, int maxWidth = 290) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::toupper(c); });
    scale = std::min(scale, maxWidth / static_cast<float>(std::max(1, get_string_width(text.data()))));
    set_text_color(color);
    print_text1_center_mode_1(x, y, text.data(), 0, scale, scale);
}

void NativeBox(int x, int y, int width, int height, bool selected) {
    gDisplayListHead = draw_box_fill(gDisplayListHead, x, y, x + width, y + height,
                                    selected ? 65 : 8, selected ? 55 : 18, selected ? 12 : 40, 255);
    if (selected) {
        gDisplayListHead = draw_flash_select_case_slow(gDisplayListHead, x, y, x + width, y + height);
    }
}

int RaceCupCount() { return std::max(1, static_cast<int>(GetWorld()->Cups.size()) - 1); }

void ClampCourseSelection() {
    if (mode == 2) {
        auto& cups = GetWorld()->Cups;
        auto battleCup = std::find(cups.begin(), cups.end(), static_cast<Cup*>(GetBattleCup()));
        selectedCup = static_cast<int>(std::distance(cups.begin(), battleCup));
        selectedArena = std::clamp(selectedArena, 0, static_cast<int>((*battleCup)->mTracks.size()) - 1);
        selectedTrack = selectedArena;
        return;
    }
    selectedCup = std::clamp(selectedCup, 0, RaceCupCount() - 1);
    selectedTrack = std::clamp(selectedTrack, 0, static_cast<int>(GetWorld()->Cups[selectedCup]->mTracks.size()) - 1);
}

void ChooseCharacter(int player, int character) {
    // Choices belong to player slots; several slots may use the same character.
    characters[player] = character;
    ready[player] = false;
}

bool AllCharactersReady() {
    return std::all_of(ready.begin(), ready.begin() + participants, [](bool value) { return value; });
}

void MoveCharacterCursor(int player, u16 input) {
    if (ready[player]) return;
    const int cell = static_cast<int>(std::find(characterGrid.begin(), characterGrid.end(), characters[player]) - characterGrid.begin());
    const int row = cell / 4;
    int next = cell;
    int delta = 0;
    if (input & L_JPAD) delta = -1;
    else if (input & R_JPAD) delta = 1;
    else if ((input & U_JPAD) && row == 1) next -= 4;
    else if ((input & D_JPAD) && row == 0) next += 4;
    if (delta) next += delta;
    if (next != cell && next >= 0 && next < 8 && (!delta || next / 4 == row)) {
        ChooseCharacter(player, characterGrid[next]);
        reset_cycle_flash_menu();
        play_sound2(SOUND_MENU_CURSOR_MOVE);
    }
}

void StartTrack() {
    // Expanded races skip the native course-selection fade, which normally
    // stops sequence players 0/1. Clear both audio systems before race setup
    // queues the grid fanfare and later starts the course's music.
    func_800CA330(0);
    HMAS_Stop(HMAS_MUSIC);
    gameMenu = false;
    ClampCourseSelection();
    auto* world = GetWorld();
    auto* cup = world->Cups.at(selectedCup);
    world->SetCupIndex(selectedCup);
    world->SetCurrentCup(cup);
    cup->CursorPosition = mode == 0 ? raceInCup : selectedTrack;
    gCupSelection = selectedCup;
    gCourseIndexInCup = cup->CursorPosition;
    D_800DC540 = selectedCup;
    gModeSelection = mode == 0 ? GRAND_PRIX : mode == 1 ? VERSUS : mode == 2 ? BATTLE : TIME_TRIALS;
    if (mode != 0) TrackBrowser_SetTrackFromCup();
    // A registry-created track can reuse the previous track's address. Force
    // setup_race to reload its data and recreate the world for every new race.
    gCurrentlyLoadedTrackAddr = 0;
    // Rendering can run before the first logic tick initializes the lap data.
    // Clear the previous race now so the HUD cannot request a missing Lakitu.
    for (int i = 0; i < NUM_RACERS; i++) gLapCountByPlayerId[i] = -1;
    gCCSelection = mode == 2 ? CC_100 : engineClass;
    set_mirror_mode(mode != 2 && engineClass == CC_EXTRA);
    gPlaceItemBoxes = 1;
    gDemoMode = 0;
    gDemoUseController = 0;
    gDebugMenuSelection = 0;
    gEnableDebugMode = false;
    // The old menu/HUD tables have four entries. This lobby owns the actual
    // human count, rendering, scoring and finish flow independently of them.
    gPlayerCount = std::min(participants, 4);
    gPlayerCountSelection1 = gPlayerCount;
    gScreenModeSelection = participants == 1 ? SCREEN_MODE_1P : participants == 2 ?
        SCREEN_MODE_2P_SPLITSCREEN_HORIZONTAL : SCREEN_MODE_3P_4P_SPLITSCREEN;
    gActiveScreenMode = gScreenModeSelection;
    for (int i = 0; i < racers; i++) gCharacterSelections[i] = characters[i];
    gIsGamePaused = 0;
    gPauseTriggered = 0;
    gIsInQuitToMenuTransition = 0;
    gQuitToMenuTransitionCounter = 0;
    gTourComplete = true;
    gMenuSelection = COURSE_SELECT_MENU;
    CVarSetInteger("gFreecam", 0);
    TrackEditor::Editor::Instance->Disable();
    active = true;
    lobby = false;
    results = paused = false;
    battleRemaining = participants;
    battleWinner = -1;
    gPlayerWinningIndex = 0;
    // Force the normal race initialization at the next game-state dispatch,
    // including rematches where the destination is already RACING.
    gGamestate = 255;
    gGamestateNext = RACING;
}

std::array<int, NUM_RACERS> OrderByFinish() {
    std::array<int, NUM_RACERS> order;
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.begin() + racers, [](int a, int b) {
        if (mode == 2) return finishingPlace[a] < finishingPlace[b];
        if (finished[a] != finished[b]) return finished[a];
        if (finished[a]) return finishTimes[a] < finishTimes[b];
        return gPlayers[a].currentRank < gPlayers[b].currentRank;
    });
    return order;
}

bool CupComplete() {
    return mode == 0 && raceInCup + 1 >= GetWorld()->Cups[selectedCup]->GetSize();
}

std::array<int, NUM_RACERS> OrderByPoints() {
    std::array<int, NUM_RACERS> order;
    std::iota(order.begin(), order.end(), 0);
    // Match the native human-first tie break, using slots rather than characters.
    // Equal-scoring human players keep the lower controller number first.
    std::stable_sort(order.begin(), order.begin() + racers, [](int a, int b) {
        if (points[a] != points[b]) return points[a] > points[b];
        return a < b;
    });
    return order;
}

int BestHumanRank(const std::array<int, NUM_RACERS>& order) {
    for (int rank = 0; rank < racers; ++rank) {
        if (order[rank] < participants) return rank;
    }
    return 0;
}

void OpenCeremony() {
    const auto standings = OrderByPoints();
    ceremonyRank = BestHumanRank(standings);
    ceremonyCharacter = characters[standings[ceremonyRank]];
    ceremonyPending = true;
    for (int rank = 0; rank < NUM_RACERS; ++rank) {
        gCharacterIdByGPOverallRank[rank] = rank < racers ? characters[standings[rank]] : 0;
    }
    // Native ceremony spawns its own podium slots. Its camera/HUD tables expect 1P.
    gCharacterSelections[0] = ceremonyCharacter;
    gPlayerCount = gPlayerCountSelection1 = 1;
    gScreenModeSelection = gActiveScreenMode = SCREEN_MODE_1P;
    func_800CA330(0);
    HMAS_Stop(HMAS_MUSIC);
    active = results = paused = gameMenu = lobby = false;
    gIsGamePaused = 0;
    gGamestate = 255;
    gGamestateNext = ENDING;
}

float ItemWindowScale() {
    // Halve the previous 5P size, then subtract ten percentage points from
    // that new baseline per additional human (100/90/80/70%).
    constexpr float fourPlayerItemScale = 1.5f;
    if (participants == 3 || participants == 4) return fourPlayerItemScale * .7f;
    if (participants <= 4) return fourPlayerItemScale;
    constexpr float fivePlayerItemScale = fourPlayerItemScale * .9f * .5f;
    return fivePlayerItemScale * (1.f - .1f * (std::min(participants, 8) - 5));
}

}

extern "C" int MultiplayerOptions_SixteenEnabled(void) {
    return CVarGetInteger(CVAR_MULTIPLAYER_SIXTEEN, 1) != 0;
}
extern "C" int MultiplayerOptions_DisplayCount(void) {
    return std::clamp(CVarGetInteger(CVAR_MULTIPLAYER_SCREENS, 1), 1, 3);
}
extern "C" void MultiplayerOptions_OnSixteenChanged(void) {
    if (MultiplayerOptions_CanChange()) request = 5;
}

extern "C" int MultiplayerOptions_EightEnabled(void) {
    return CVarGetInteger(CVAR_MULTIPLAYER_EIGHT, 1) != 0;
}
extern "C" float RacerRenderDistance_Multiplier(void) {
    if (gGamestate != RACING) return 1.f;
    return static_cast<float>(std::clamp(CVarGetInteger(CVAR_RACER_RENDER_DISTANCE, 2), 1, 5));
}
extern "C" float RacerRenderDistance_FarPlane(float normalFar) {
    return normalFar * RacerRenderDistance_Multiplier();
}
extern "C" int MultiplayerOptions_BotsEnabled(void) {
    return CVarGetInteger(CVAR_MULTIPLAYER_BOTS, 1) != 0;
}
extern "C" int MultiplayerOptions_CanChange(void) {
    // Include transitions so a pending race cannot have its configuration changed.
    return gGamestate >= START_MENU_FROM_QUIT && gGamestate <= COURSE_SELECT_MENU_FROM_QUIT &&
           gGamestateNext >= START_MENU_FROM_QUIT && gGamestateNext <= COURSE_SELECT_MENU_FROM_QUIT;
}
extern "C" void MultiplayerOptions_OnEightChanged(void) {
    if (!MultiplayerOptions_CanChange()) return;
    lobby = false;
    gameMenu = false;
    ready.fill(false);
    // Cancel a Start request if the lobby was disabled before the next logic tick.
    if (!lobby && request == 1) request = 0;
}
extern "C" void MultiplayerOptions_OnBotsChanged(void) {
    if (MultiplayerOptions_CanChange()) request = 5;
}

extern "C" int LocalMP_IsActive(void) { return active && gGamestate == RACING; }
extern "C" int LocalMP_IsAfkBot(int player) {
    return player >= 0 && player < MAX_HUMAN_PLAYERS && afkPlayers[player].driving;
}

extern "C" void LocalMP_RestoreAfkInput(void) {
    // Bot inputs must never look like real activity on the next logic tick.
    for (int id = 0; id < MAX_HUMAN_PLAYERS; ++id) {
        auto& state = afkPlayers[id];
        if (state.inputReplaced) {
            gControllers[id] = state.input;
            state.inputReplaced = false;
        }
    }
}

extern "C" void LocalMP_ResetAfk(void) {
    LocalMP_RestoreAfkInput();
    for (int id = 0; id < MAX_HUMAN_PLAYERS; ++id) ReleaseAfkPlayer(id);
    battleNavigation = {};
}

extern "C" void LocalMP_UpdateAfk(void) {
    LocalMP_RestoreAfkInput();
    if (!CVarGetInteger(CVAR_AFK_BOTS, 1) || gGamestate != RACING || gDemoMode ||
        gRaceState < RACE_IN_PROGRESS || gRaceState >= RACE_FINISHED) {
        LocalMP_ResetAfk();
        return;
    }
    if (gIsGamePaused) return;
    const int timeout = std::clamp(CVarGetInteger(CVAR_AFK_SECONDS, 5), 1, 86400);
    const int count = std::clamp(LocalMP_Count(), 1, MAX_HUMAN_PLAYERS);
    for (int id = 0; id < MAX_HUMAN_PLAYERS; ++id) {
        auto& state = afkPlayers[id];
        auto& player = gPlayers[id];
        const bool eliminatedBattlePlayer = gModeSelection == BATTLE && gPlayerBalloonCount[id] < 0;
        // Battle elimination fixes ranking, but does not end control of a bomb.
        // The native explosion clears PLAYER_EXISTS; results stop all takeover.
        if (id >= count || !(player.type & PLAYER_EXISTS) || !(player.type & PLAYER_HUMAN) ||
            (player.type & PLAYER_START_SEQUENCE) ||
            (!eliminatedBattlePlayer && (player.type & (PLAYER_CINEMATIC_MODE | PLAYER_INVISIBLE_OR_BOMB))) ||
            (gModeSelection != BATTLE && LocalMP_IsActive() && finished[id])) {
            ReleaseAfkPlayer(id);
            continue;
        }
        const auto& input = gControllers[id];
        const bool activity = input.button || input.buttonPressed || input.buttonDepressed ||
            std::abs(input.rawStickX) > 12 || std::abs(input.rawStickY) > 12 ||
            std::abs(input.rightRawStickX) > 12 || std::abs(input.rightRawStickY) > 12;
        if (activity) {
            ReleaseAfkPlayer(id);
            continue;
        }
        state.idleSeconds += TRACK_TIMER_ITER;
        if (!state.driving && state.idleSeconds >= timeout) {
            state.driving = true;
            if (gModeSelection != BATTLE) {
                reset_cpu_behaviour(id);
                player.type |= PLAYER_CPU;
            }
        }
        if (state.driving) {
            if (gModeSelection == BATTLE) DriveAfkBattlePlayer(id);
            else UseAfkRaceItems(id);
        }
    }
}
extern "C" int LocalMP_IsLobbyOpen(void) {
    return MultiplayerOptions_EightEnabled() && MultiplayerOptions_CanChange() && lobby && !active;
}
extern "C" int LocalMP_GameMenuIsOpen(void) {
    return gameMenu && MultiplayerOptions_EightEnabled() && MultiplayerOptions_CanChange() &&
           gMenuSelection == MAIN_MENU && !lobby;
}

extern "C" int LocalMP_GameMenuTick(void) {
    if (!MultiplayerOptions_EightEnabled() || !MultiplayerOptions_CanChange() || gMenuSelection != MAIN_MENU) {
        gameMenu = false;
        return 0;
    }
    if (lobby) return 0;
    const u16 buttons = gControllers[0].buttonPressed | gControllers[0].stickPressed;
    if (battleSetupRequest) {
        gameMenu = true;
        gameMenuStep = battleSetupRequest == 7 ? GameMenuStep::Course : GameMenuStep::Characters;
        resumeBattleSession = true;
        characterEditor = courseField = coursePreviewTimer = 0;
        coursePreviewCup = selectedCup;
        coursePreviewTrack = selectedTrack;
        ready.fill(battleSetupRequest == 7);
        battleSetupRequest = 0;
        return 1;
    }
    if (!gameMenu) {
        const bool nextPage = gPlayerCount == 4 && (buttons & R_JPAD);
        const bool previousPage = gPlayerCount == 1 && (buttons & L_JPAD);
        if (gMainMenuSelection != MAIN_MENU_PLAYER_SELECT ||
            is_screen_being_faded() || (!nextPage && !previousPage)) return 0;
        // The original menu uses four-entry tables. Keep its count in range and
        // let this native page own the extra choices and setup sequence.
        participants = menuPlayerCount = previousPage ?
            (MultiplayerOptions_SixteenEnabled() ? NUM_RACERS : NUM_PLAYERS) : 5;
        resumeBattleSession = false;
        mode = 0;
        gameMenu = true;
        gameMenuStep = GameMenuStep::Players;
        for (int player = 0; player < MAX_HUMAN_PLAYERS; ++player) characters[player] = characterGrid[player % 8];
        ready.fill(false);
        play_sound2(SOUND_MENU_CURSOR_MOVE);
        return 1;
    }
    if (is_screen_being_faded()) return 1;
    if (!MultiplayerOptions_SixteenEnabled() && menuPlayerCount > NUM_PLAYERS)
        menuPlayerCount = participants = std::min(participants, 8);
    auto selectSound = [] { reset_cycle_flash_menu(); play_sound2(SOUND_MENU_SELECT); };
    auto moveSound = [] { reset_cycle_flash_menu(); play_sound2(SOUND_MENU_CURSOR_MOVE); };
    const bool confirm = buttons & (A_BUTTON | START_BUTTON);
    switch (gameMenuStep) {
        case GameMenuStep::Players:
            resumeBattleSession = false;
            if ((buttons & B_BUTTON) && menuPlayerCount > NUM_PLAYERS) {
                menuPlayerCount = menuPlayerCount > 12 ? 12 : 8;
                participants = menuPlayerCount;
                play_sound2(SOUND_MENU_GO_BACK);
            } else if ((buttons & B_BUTTON) || ((buttons & L_JPAD) && menuPlayerCount == 5)) {
                gameMenu = false;
                gPlayerCount = gPlayerCountSelection1 = 4;
                gScreenModeSelection = SCREEN_MODE_3P_4P_SPLITSCREEN;
                play_sound2(SOUND_MENU_GO_BACK);
            } else if (buttons & R_JPAD) {
                const int maxPlayers = MultiplayerOptions_SixteenEnabled() ? NUM_RACERS : NUM_PLAYERS;
                if (menuPlayerCount == maxPlayers) {
                    gameMenu = false;
                    gPlayerCount = gPlayerCountSelection1 = 1;
                    gScreenModeSelection = SCREEN_MODE_1P;
                } else {
                    participants = ++menuPlayerCount;
                }
                moveSound();
            } else if ((buttons & L_JPAD) && menuPlayerCount > 5) {
                --menuPlayerCount;
                participants = menuPlayerCount;
                moveSound();
            } else if (confirm) {
                {
                    participants = menuPlayerCount;
                    gameMenuStep = GameMenuStep::Mode;
                    selectSound();
                }
            }
            break;
        case GameMenuStep::Mode:
            resumeBattleSession = false;
            if (buttons & B_BUTTON) {
                menuPlayerCount = participants;
                gameMenuStep = GameMenuStep::Players;
                play_sound2(SOUND_MENU_GO_BACK);
            }
            else if (buttons & (U_JPAD | D_JPAD)) {
                const int nextMode = std::clamp(mode + ((buttons & D_JPAD) ? 1 : -1), 0, 2);
                if (nextMode != mode) { mode = nextMode; moveSound(); }
            } else if (confirm) {
                gameMenuStep = mode == 2 ? GameMenuStep::Confirm : GameMenuStep::Class;
                reset_cycle_flash_menu();
                play_sound2(mode == 0 ? SOUND_MENU_GP : mode == 1 ? SOUND_MENU_VERSUS : SOUND_MENU_BATTLE);
            }
            break;
        case GameMenuStep::Class: {
            const int maxClass = has_unlocked_extra_mode() ? 3 : 2;
            engineClass = std::clamp(engineClass, 0, maxClass);
            if (buttons & B_BUTTON) {
                gameMenuStep = GameMenuStep::Mode;
                play_sound2(SOUND_MENU_GO_BACK);
            }
            else if ((buttons & U_JPAD) && engineClass > 0) { --engineClass; moveSound(); }
            else if ((buttons & D_JPAD) && engineClass < maxClass) { ++engineClass; moveSound(); }
            else if (confirm) {
                gameMenuStep = GameMenuStep::Confirm;
                selectSound();
            }
            break;
        }
        case GameMenuStep::Confirm:
            if (buttons & B_BUTTON) {
                gameMenuStep = mode == 2 ? GameMenuStep::Mode : GameMenuStep::Class;
                reset_cycle_flash_menu();
                play_sound2(SOUND_MENU_GO_BACK);
            } else if (confirm) {
                ready.fill(false);
                characterEditor = 0;
                gameMenuStep = GameMenuStep::Characters;
                play_sound2(SOUND_MENU_OK_CLICKED);
            }
            break;
        case GameMenuStep::Characters: {
            const bool wasAllReady = AllCharactersReady();
            // Every controller uses the normal four-column portrait grid.
            // P1's shoulder buttons also allow setup of unmapped player slots.
            if (buttons & L_TRIG) { characterEditor = (characterEditor + participants - 1) % participants; moveSound(); }
            else if (buttons & R_TRIG) { characterEditor = (characterEditor + 1) % participants; moveSound(); }
            for (int controller = 0; controller < participants; ++controller) {
                const int player = controller == 0 ? characterEditor : controller;
                const u16 input = gControllers[controller].buttonPressed | gControllers[controller].stickPressed;
                if (input & B_BUTTON) {
                    if (ready[player]) ready[player] = false;
                    else if (controller == 0) gameMenuStep = GameMenuStep::Confirm;
                    play_sound2(SOUND_MENU_GO_BACK);
                    continue;
                }
                if (gameMenuStep != GameMenuStep::Characters) break;
                MoveCharacterCursor(player, input);
                if (input & (A_BUTTON | START_BUTTON)) {
                    if (wasAllReady && AllCharactersReady()) {
                        ClampCourseSelection();
                        courseField = 0;
                        coursePreviewCup = -1;
                        coursePreviewTrack = mode == 0 ? 0 : selectedTrack;
                        coursePreviewTimer = 0;
                        gameMenuStep = GameMenuStep::Course;
                        play_sound2(SOUND_MENU_OK_CLICKED);
                        break;
                    }
                    if (!ready[player]) {
                        ready[player] = true;
                        reset_cycle_flash_menu();
                        func_800C90F4(player, 0x2900800E + (characters[player] << 4));
                        play_sound2(SOUND_MENU_SELECT);
                    }
                }
            }
            break;
        }
        case GameMenuStep::Course: {
            ClampCourseSelection();
            u16 courseButtons = buttons;
            for (int player = 1; player < participants && !courseButtons; ++player) {
                courseButtons = gControllers[player].buttonPressed | gControllers[player].stickPressed;
            }
            const int lastField = mode == 1 ? 2 : 1;
            if (selectedCup != coursePreviewCup) {
                coursePreviewCup = selectedCup;
                coursePreviewTrack = coursePreviewTimer = 0;
            }
            if (mode == 0 && courseField == 0) {
                // The normal GP selector rotates the highlighted course preview.
                if (++coursePreviewTimer >= (coursePreviewTrack == 0 ? 51 : 31)) {
                    coursePreviewTimer = 0;
                    coursePreviewTrack = (coursePreviewTrack + 1) % 4;
                }
            } else coursePreviewTrack = mode == 0 ? 0 : selectedTrack;
            if (courseButtons & B_BUTTON) {
                if (courseField > 0) --courseField;
                else {
                    gameMenuStep = GameMenuStep::Characters;
                    ready[characterEditor] = false;
                }
                reset_cycle_flash_menu();
                play_sound2(SOUND_MENU_GO_BACK);
            } else if (mode != 2 && courseField == 0 && (courseButtons & (L_JPAD | R_JPAD))) {
                const int nextCup = std::clamp(selectedCup + ((courseButtons & R_JPAD) ? 1 : -1), 0, RaceCupCount() - 1);
                if (nextCup != selectedCup) {
                    selectedCup = nextCup;
                    selectedTrack = coursePreviewTrack = coursePreviewTimer = 0;
                    coursePreviewCup = selectedCup;
                    moveSound();
                }
            } else if (((mode == 2 && courseField == 0) || (mode == 1 && courseField == 1)) &&
                       (courseButtons & (U_JPAD | D_JPAD))) {
                const int tracks = static_cast<int>(GetWorld()->Cups[selectedCup]->mTracks.size());
                const int nextTrack = std::clamp(selectedTrack + ((courseButtons & D_JPAD) ? 1 : -1), 0, tracks - 1);
                if (nextTrack != selectedTrack) {
                    selectedTrack = coursePreviewTrack = nextTrack;
                    if (mode == 2) selectedArena = nextTrack;
                    ClampCourseSelection();
                    moveSound();
                }
            } else if (courseButtons & (A_BUTTON | START_BUTTON)) {
                if (courseField == lastField) {
                    startedFromGameMenu = true;
                    request = resumeBattleSession ? 3 : 1;
                    resumeBattleSession = false;
                    gameMenu = false;
                    play_sound2(SOUND_MENU_OK_CLICKED);
                } else {
                    ++courseField;
                    if (mode == 0) selectedTrack = coursePreviewTrack = 0;
                    selectSound();
                }
            }
            break;
        }
    }
    return 1;
}

extern "C" void LocalMP_DrawGameMenu(void) {
    if (!LocalMP_GameMenuIsOpen()) return;
    if (gameMenuStep != GameMenuStep::Characters && gameMenuStep != GameMenuStep::Course) draw_expanded_menu_background();
    char title[70];
    if (gameMenuStep == GameMenuStep::Players || gameMenuStep == GameMenuStep::Mode ||
        gameMenuStep == GameMenuStep::Class || gameMenuStep == GameMenuStep::Confirm) {
        draw_expanded_menu_title();
        const bool choosingPlayers = gameMenuStep == GameMenuStep::Players;
        const bool choosingClass = gameMenuStep == GameMenuStep::Class;
        const bool confirming = gameMenuStep == GameMenuStep::Confirm;
        const int pageStart = 5 + ((menuPlayerCount - 5) / 4) * 4;
        // Once the player count is selected, the native menu keeps just that
        // column and opens the class list beside the selected game mode.
        for (int slot = 0; slot < (choosingPlayers ? 4 : 1); slot++) {
            const int x = choosingPlayers ? 21 + slot * 71 : 104;
            const int humanCount = choosingPlayers ? slot + pageStart : participants;
            const bool chosen = (choosingPlayers ? menuPlayerCount : participants) == humanCount;
            draw_expanded_menu_card_base(x, 62, chosen, !choosingPlayers);
            draw_expanded_player_card(x, 62);
            std::snprintf(title, sizeof(title), "%dP", humanCount);
            NativeText(x + 31, 55, title, chosen ? TEXT_YELLOW : TEXT_BLUE, .5f, 56);
            const bool modeChosen = chosen && !choosingPlayers;
            draw_expanded_menu_button(x, 127, 0, modeChosen && mode == 0, choosingClass || confirming);
            draw_expanded_menu_button(x, 145, 1, modeChosen && mode == 1, choosingClass || confirming);
            draw_expanded_menu_button(x, 163, 6, modeChosen && mode == 2, confirming);
        }
        if ((choosingClass || confirming) && mode != 2) {
            const int classCount = has_unlocked_extra_mode() ? 4 : 3;
            for (int option = 0; option < classCount; ++option) {
                draw_expanded_menu_button(168, 127 + mode * 18 + option * 18,
                                          option + 2, engineClass == option, confirming);
            }
        }
        if (confirming) {
            draw_expanded_menu_button(240, 200, 7, true, false);
        } else if (!choosingClass) {
            const int bots = mode != 2 && MultiplayerOptions_SixteenEnabled() ? NUM_RACERS - participants :
                             mode == 0 && participants < 8 && MultiplayerOptions_BotsEnabled() ? 8 - participants : 0;
            std::snprintf(title, sizeof(title), "%d PLAYERS + %d BOTS", participants, bots);
            NativeText(160, 201, mode == 2 ? "BATTLE - THREE BALLOONS EACH" : title, TEXT_YELLOW, .42f);
            NativeText(160, 212, gameMenuStep == GameMenuStep::Players ? "LEFT / RIGHT: PLAYERS" : "UP / DOWN: GAME MODE", TEXT_BLUE, .35f);
        }
    } else if (gameMenuStep == GameMenuStep::Characters) {
        std::array<s32, MAX_HUMAN_PLAYERS> roster{};
        std::array<u8, MAX_HUMAN_PLAYERS> confirmed{};
        for (int player = 0; player < participants; ++player) {
            roster[player] = characters[player];
            confirmed[player] = ready[player];
        }
        draw_expanded_character_select(roster.data(), confirmed.data(), participants, AllCharactersReady());
        if (!AllCharactersReady()) {
            std::snprintf(title, sizeof(title), "P1 L / R: EDIT P%d   A: SELECT   B: BACK", characterEditor + 1);
            NativeText(160, 234, title, TEXT_YELLOW, .3f);
        } else NativeText(160, 234, "A / START: OK   B: CHANGE CHARACTER", TEXT_YELLOW, .33f);
    } else {
        ClampCourseSelection();
        draw_expanded_course_select(selectedCup, coursePreviewTrack, mode, courseField, engineClass);
    }
    if (gameMenuStep != GameMenuStep::Characters) NativeText(160, 234,
        gameMenuStep == GameMenuStep::Class ?
            "UP / DOWN: CLASS   A: SELECT   B: BACK" : "A: SELECT   B: BACK", TEXT_YELLOW, .33f);
}

extern "C" void LocalMP_DrawGameMenuHint(void) {
    if (!MultiplayerOptions_EightEnabled() || !MultiplayerOptions_CanChange() ||
        gMenuSelection != MAIN_MENU || gameMenu || lobby) return;
    NativeText(160, 233, MultiplayerOptions_SixteenEnabled() ?
        "LEFT / RIGHT: CYCLE 1P - 16P" : "LEFT / RIGHT: CYCLE 1P - 8P", TEXT_YELLOW, .33f);
}
extern "C" int LocalMP_Count(void) { return LocalMP_IsActive() ? participants : gPlayerCount; }
extern "C" int LocalMP_RaceResultsOpen(void) {
    return LocalMP_IsActive() && results && mode != 2;
}
extern "C" int LocalMP_BattleResultsOpen(void) {
    return LocalMP_IsActive() && results && mode == 2 && (participants == 3 || participants > 4);
}
extern "C" int LocalMP_RacerCount(void) {
    if (LocalMP_IsActive()) return racers;
    if (gGamestate == ENDING) return LocalMP_CeremonyRacerCount();
    return gModeSelection == GRAND_PRIX || gModeSelection == TIME_TRIALS ? NUM_PLAYERS : gPlayerCount;
}
extern "C" int LocalMP_IsFinished(int player) {
    // Battle players keep their native eliminated-player bomb physics and camera.
    return player < 0 || player >= racers || results || (mode != 2 && finished[player]);
}
extern "C" int LocalMP_CanCoastAfterFinish(int player) {
    // Results lock scoring and player input, but native cinematic CPU driving
    // and camera updates continue behind the ranking screen.
    return LocalMP_IsActive() && mode != 2 && player >= 0 && player < racers && finished[player];
}
extern "C" int LocalMP_CeremonyResult(int* character, int* rank) {
    // Reset for native ceremonies too, so a previous expanded cup cannot
    // leave extra spectators in a later normal ceremony.
    ceremonyRacers = 4;
    if (!ceremonyPending || gGamestate != ENDING) return false;
    ceremonyRacers = std::clamp(racers, 4, NUM_RACERS);
    *character = ceremonyCharacter;
    *rank = ceremonyRank;
    ceremonyPending = false;
    return true;
}

extern "C" int LocalMP_CeremonyRacerCount(void) {
    return gGamestate == ENDING ? ceremonyRacers : 4;
}

extern "C" int LocalMP_CanSkipGrandPrixFinale(void) {
    const int humans = LocalMP_Count();
    return gGamestate == RACING && gGamestateNext == RACING &&
           gModeSelection == GRAND_PRIX && !gDemoMode && !gIsInQuitToMenuTransition &&
           humans >= 3 && humans <= MAX_HUMAN_PLAYERS && !GetWorld()->Cups.empty();
}

extern "C" void LocalMP_SkipGrandPrixFinale(void) {
    // Change game state on the next logic tick, never while drawing the PC menu.
    if (!request && LocalMP_CanSkipGrandPrixFinale()) request = 9;
}

extern "C" void LocalMP_ProcessRequest(void) {
    const int action = std::exchange(request, 0);
    if (!action) return;
    if (action == 9) {
        if (!LocalMP_CanSkipGrandPrixFinale()) return;
        if (!LocalMP_IsActive()) {
            // The native 3/4-player GP with bots stores points by character.
            // Snapshot its actual grid into slots before using the shared ending.
            participants = gPlayerCount;
            racers = NUM_PLAYERS;
            mode = 0;
            engineClass = gCCSelection;
            selectedCup = static_cast<int>(GetWorld()->GetCupIndex());
            for (int player = 0; player < NUM_PLAYERS; ++player) {
                characters[player] = gPlayers[player].characterId;
                points[player] = gGPPointsByCharacterId[characters[player]];
            }
        }
        if (gIsGamePaused) func_800C9F90(0);
        // This previews the ceremony without granting unearned saved trophies.
        raceInCup = static_cast<int>(GetWorld()->Cups.at(selectedCup)->GetSize()) - 1;
        OpenCeremony();
        return;
    }
    if (action == 6) {
        if (LocalMP_IsActive() && results && CupComplete()) OpenCeremony();
        return;
    }
    if (action == 4 || action == 5 || action == 7 || action == 8) {
        if (paused) SetPaused(false);
        if (active) {
            func_800CA330(0);
            HMAS_Stop(HMAS_MUSIC);
        }
        gameMenu = false;
        battleSetupRequest = action == 7 || action == 8 ? action : 0;
        resumeBattleSession = false;
        if (battleSetupRequest) {
            startedFromGameMenu = true;
            lobby = false;
        }
        ceremonyPending = false;
        active = results = paused = false;
        if (action == 4) lobby = !startedFromGameMenu && MultiplayerOptions_EightEnabled();
        ready.fill(false);
        gIsGamePaused = 0;
        gIsInQuitToMenuTransition = 0;
        gPlayerCount = gPlayerCountSelection1 = 1;
        gScreenModeSelection = gActiveScreenMode = SCREEN_MODE_1P;
        gMenuSelection = MAIN_MENU;
        gMenuFadeType = MENU_FADE_TYPE_MAIN;
        gGamestate = 255;
        gGamestateNext = MAIN_MENU_FROM_QUIT;
        return;
    }
    if (action == 1 && !MultiplayerOptions_EightEnabled()) return;
    if (paused) SetPaused(false);
    if (action == 1) {
        ceremonyPending = false;
        progressDisplay = false;
        sharedHUDPlayer = 0;
        // Freeze the race grid for this session. The 16-racer option takes
        // priority over the smaller Grand Prix bot grid, except in Battle.
        racers = mode != 2 && MultiplayerOptions_SixteenEnabled() ? NUM_RACERS :
                 mode == 0 && participants >= 3 && participants <= 7 &&
                 MultiplayerOptions_BotsEnabled() ? NUM_PLAYERS : participants;
        for (int bot = participants; bot < racers; ++bot) characters[bot] = bot % NUM_PLAYERS;
        points.fill(0);
        raceInCup = 0;
        for (int i = 0; i < racers; i++) gGPCurrentRaceRankByPlayerId[i] = racers - i - 1;
    } else if (action == 2 && mode == 0) {
        ++raceInCup;
    }
    StartTrack();
}

extern "C" void LocalMP_ResetRace(void) {
    LocalMP_ResetAfk();
    const bool sixteenRace = gModeSelection != BATTLE && MultiplayerOptions_SixteenEnabled();
    const bool sharedSmallLayout = MultiplayerOptions_EightEnabled() && gPlayerCount >= 2 && gPlayerCount <= 4;
    if (!active && gGamestate == RACING && !gDemoMode && (sixteenRace || sharedSmallLayout || gPlayerCount == 3 ||
        (gModeSelection == GRAND_PRIX && (gPlayerCount == 3 || gPlayerCount == 4) &&
         MultiplayerOptions_EightEnabled() && !MultiplayerOptions_BotsEnabled()))) {
        // Use the same viewport/HUD path for native-menu 2-4P with either
        // expanded mode enabled. Racer count still follows the chosen options.
        participants = gPlayerCount;
        racers = sixteenRace ? NUM_RACERS :
            gModeSelection == GRAND_PRIX && (participants == 2 || MultiplayerOptions_BotsEnabled()) ? NUM_PLAYERS : participants;
        for (int player = 0; player < racers; ++player) {
            characters[player] = player < participants ? gCharacterSelections[player] : player % NUM_PLAYERS;
            gCharacterSelections[player] = characters[player];
        }
        mode = gModeSelection == GRAND_PRIX ? 0 : gModeSelection == VERSUS ? 1 : gModeSelection == BATTLE ? 2 : 3;
        engineClass = gCCSelection;
        selectedCup = static_cast<int>(GetWorld()->GetCupIndex());
        selectedTrack = raceInCup = static_cast<int>(GetWorld()->GetCurrentCup()->CursorPosition);
        if (mode == 2) selectedArena = static_cast<int>(static_cast<Cup*>(GetBattleCup())->CursorPosition);
        points.fill(0);
        ready.fill(true);
        for (int player = 0; player < racers; ++player) gGPCurrentRaceRankByPlayerId[player] = racers - player - 1;
        active = startedFromGameMenu = true;
        gTourComplete = true;
        ceremonyPending = progressDisplay = false;
        sharedHUDPlayer = 0;
        gameMenu = lobby = false;
        request = 0;
    }
    if (!LocalMP_IsActive()) return;
    // Initialize the visibility fallback once per race. Layout runs every
    // frame; resetting it there makes off-floor cameras draw section 1.
    for (int view = 0; view < MAX_HUMAN_PLAYERS; ++view) gScreenContexts[view].pathCounter = 1;
    std::fill_n(D_8018E838, MAX_HUMAN_PLAYERS, 0);
    std::fill_n(D_8018E840, MAX_HUMAN_PLAYERS, 0);
    finished.fill(false);
    finishTimes.fill(std::numeric_limits<float>::infinity());
    finishingPlace.fill(-1);
    estimatedTime.fill(false);
    finalLapMusicStarted = false;
    battleRemaining = participants;
    battleWinner = -1;
    battleResultChoice = 0;
    battleResultCursor = {};
    battleResultCursor.paramf = 1.5f;
    battleResultCursor.subState = 1;
    results = paused = false;
    gIsGamePaused = 0;
    raceResultStandings = false;
    raceResultChoice = 0;
    raceResultCursor = {};
    raceResultCursor.paramf = 1.5f;
    raceResultCursor.subState = 1;
    LocalMP_Layout();
}

extern "C" void LocalMP_Layout(void) {
    if (!LocalMP_IsActive()) return;
    const int displays = std::min(MultiplayerOptions_DisplayCount(), participants);
    const auto edge = [](int index, int extent, int divisions) {
        return ((index * extent + divisions) / (2 * divisions)) * 2;
    };
    int firstPlayer = 0;
    for (int display = 0; display < displays; ++display) {
        const int count = participants / displays + (display < participants % displays);
        int panelLeft, panelTop, panelRight, panelBottom;
        MultiplayerDisplays_Panel(display, displays, &panelLeft, &panelTop, &panelRight, &panelBottom);
        const int width = panelRight - panelLeft, height = panelBottom - panelTop;
        int columns;
        if (displays == 1 && participants <= 8) {
            columns = participants == 1 ? 1 : participants == 2 ?
                (gActiveScreenMode == SCREEN_MODE_2P_SPLITSCREEN_VERTICAL ? 2 : 1) :
                participants <= 4 ? 2 : (participants + 1) / 2;
        } else if (displays > 1) columns = (count + 1) / 2;
        else columns = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(count))));
        const int rows = (count + columns - 1) / columns;
        for (int cell = 0; cell < count; ++cell) {
            const int player = firstPlayer + cell;
            auto& screen = gScreenContexts[player];
            int slot = cell;
            // Spread incomplete bottom rows across the full panel.
            const bool equalBottomRow = displays == 1 &&
                (participants == 3 || participants == 5 || participants == 7 || participants == 11 || participants == 15) &&
                slot / columns == rows - 1;
            int row = slot / columns;
            int column = slot % columns;
            int rowColumns = equalBottomRow ? count - columns * (rows - 1) : columns;
            // Ten/thirteen players use 4 / 3 / 3 and 4 / 3 / 3 / 3.
            if (displays == 1 && (participants == 10 || participants == 13) && slot >= 4) {
                row = 1 + (slot - 4) / 3;
                column = (slot - 4) % 3;
                rowColumns = 3;
            }
            // Fourteen players use 4 / 4 / 3 / 3.
            if (displays == 1 && participants == 14 && slot >= 8) {
                row = 2 + (slot - 8) / 3;
                column = (slot - 8) % 3;
                rowColumns = 3;
            }
            const int left = panelLeft + edge(column, width, rowColumns);
            const int right = panelLeft + edge(column + 1, width, rowColumns);
            const int top = panelTop + edge(row, height, rows);
            const int bottom = panelTop + edge(row + 1, height, rows);
            screen.screenWidth = right - left;
            screen.screenHeight = bottom - top;
            screen.screenStartX = left + screen.screenWidth / 2;
            screen.screenStartY = top + screen.screenHeight / 2;
            screen.player = &gPlayers[player];
            screen.controllers = &gControllers[player];
            screen.unkC = &D_8015F790[player * 16];
        }
        firstPlayer += count;
    }
    gScreenAspect = static_cast<float>(gScreenContexts[0].screenWidth) / gScreenContexts[0].screenHeight;
    if (LocalMP_BattleResultsOpen() || LocalMP_RaceResultsOpen()) {
        // The render loop selects gPlayerWinningIndex for both race and battle
        // rankings. Enlarge that same view, including when P2/P3/P4 wins.
        const int resultViewer = std::clamp(static_cast<int>(gPlayerWinningIndex), 0, participants - 1);
        auto& screen = gScreenContexts[resultViewer];
        screen.screenWidth = SCREEN_WIDTH;
        screen.screenHeight = SCREEN_HEIGHT;
        screen.screenStartX = SCREEN_WIDTH / 2;
        screen.screenStartY = SCREEN_HEIGHT / 2;
    }
}

extern "C" void LocalMP_OnInput(void) {
    if (MultiplayerOptions_EightEnabled() && MultiplayerOptions_CanChange() && lobby && !active) {
        for (int i = 0; i < participants; i++) {
            const auto buttons = gControllers[i].buttonPressed | gControllers[i].stickPressed;
            if (buttons & L_JPAD) ChooseCharacter(i, (characters[i] + 7) % 8);
            if (buttons & R_JPAD) ChooseCharacter(i, (characters[i] + 1) % 8);
            if (buttons & A_BUTTON) ready[i] = true;
        }
        bool everyoneReady = std::all_of(ready.begin(), ready.begin() + participants, [](bool value) { return value; });
        if (everyoneReady && (gControllers[0].buttonPressed & START_BUTTON)) {
            startedFromGameMenu = false;
            request = 1;
        }
        return;
    }
    if (!LocalMP_IsActive()) return;
    if (LocalMP_BattleResultsOpen()) {
        u16 input = 0;
        for (int player = 0; player < participants && !input; ++player)
            input = gControllers[player].buttonPressed | gControllers[player].stickPressed;
        const int previous = battleResultChoice;
        if (input & U_JPAD) battleResultChoice = std::max(0, battleResultChoice - 1);
        else if (input & D_JPAD) battleResultChoice = std::min(3, battleResultChoice + 1);
        if (previous != battleResultChoice) {
            battleResultCursor.subState = battleResultChoice > previous ? 1 : -1;
            battleResultCursor.paramf = 5.5f;
            play_sound2(SOUND_MENU_CURSOR_MOVE);
        }
        if (input & B_BUTTON) {
            battleResultChoice = 3;
            play_sound2(SOUND_MENU_GO_BACK);
        } else if (input & (A_BUTTON | START_BUTTON)) {
            const int actions[] = {3, 7, 8, 4};
            if (battleResultChoice == 3) startedFromGameMenu = true;
            request = actions[battleResultChoice];
            play_sound2(SOUND_MENU_OK_CLICKED);
        }
        return;
    }
    if (LocalMP_RaceResultsOpen()) {
        u16 input = 0;
        for (int player = 0; player < participants && !input; ++player)
            input = gControllers[player].buttonPressed | gControllers[player].stickPressed;
        const int previous = raceResultChoice;
        if (input & U_JPAD) raceResultChoice = 0;
        else if (input & D_JPAD) raceResultChoice = 1;
        if (previous != raceResultChoice) {
            raceResultCursor.subState = raceResultChoice > previous ? 1 : -1;
            raceResultCursor.paramf = 5.5f;
            play_sound2(SOUND_MENU_CURSOR_MOVE);
        }
        if (mode == 0 && (input & (L_JPAD | R_JPAD | L_CBUTTONS | R_CBUTTONS))) {
            raceResultStandings = !raceResultStandings;
            play_sound2(SOUND_MENU_CURSOR_MOVE);
        } else if (input & B_BUTTON) {
            if (mode == 0 && raceResultStandings) {
                raceResultStandings = false;
                play_sound2(SOUND_MENU_GO_BACK);
            } else {
                raceResultChoice = 1;
                play_sound2(SOUND_MENU_GO_BACK);
            }
        } else if (input & (A_BUTTON | START_BUTTON)) {
            if (raceResultChoice == 1) request = 4;
            else if (mode == 0 && !raceResultStandings) raceResultStandings = true;
            else request = mode == 0 ? (CupComplete() ? 6 : 2) : 3;
            play_sound2(SOUND_MENU_OK_CLICKED);
        }
        return;
    }
    if (startedFromGameMenu && results) {
        // Retain native-menu continuation for the existing 2-4 player Battle fallback.
        const u16 input = gControllers[0].buttonPressed;
        if (input & B_BUTTON) request = 4;
        else if (input & (A_BUTTON | START_BUTTON)) request = 3;
        return;
    }
    if (paused && !results) {
        const auto buttons = gControllers[pausePlayer].buttonPressed;
        const auto input = buttons | gControllers[pausePlayer].stickPressed;
        const int previous = pauseChoice;
        if (input & U_JPAD) pauseChoice = std::max(0, pauseChoice - 1);
        else if (input & D_JPAD) pauseChoice = std::min(2, pauseChoice + 1);
        if (previous != pauseChoice) {
            pauseCursor.subState = pauseChoice > previous ? 1 : -1;
            pauseCursor.paramf = 5.5f;
            play_sound2(SOUND_MENU_CURSOR_MOVE);
        }
        if (buttons & B_BUTTON) {
            if (pauseChoice == 0) SetPaused(false);
            else { pauseChoice = 0; play_sound2(SOUND_MENU_GO_BACK); }
        } else if (buttons & (A_BUTTON | START_BUTTON)) {
            if (pauseChoice == 0) SetPaused(false);
            else {
                request = pauseChoice == 1 ? 3 : 4;
                play_sound2(SOUND_MENU_SELECT);
            }
        }
        return;
    }
    if (results || gRaceState < RACE_IN_PROGRESS) return;
    for (int i = 0; i < participants; i++) {
        if (gControllers[i].buttonPressed & START_BUTTON) {
            SetPaused(true, i);
            return;
        }
    }
    bool changedDisplay = false;
    if (participants > 1 && (participants == 3 || participants > 4 || racers > NUM_PLAYERS)) {
        for (int player = 0; player < participants; ++player) {
            if (gControllers[player].buttonPressed & R_CBUTTONS) {
                progressDisplay = !progressDisplay;
                sharedHUDPlayer = player;
                changedDisplay = true;
                break; // Simultaneous presses change the shared display only once.
            }
        }
    }
    if (changedDisplay) play_sound2(SOUND_ACTION_PING);
}

extern "C" void LocalMP_DrawItemWindows(void) {
    // One human keeps the native corner map and perimeter progress HUD,
    // regardless of how many CPU racers fill the grid.
    if (!LocalMP_IsActive() || participants == 1 ||
        (participants != 3 && participants <= 4 && racers <= NUM_PLAYERS && MultiplayerOptions_DisplayCount() == 1) ||
        paused || results || gHUDDisable) return;
    init_rdp();
    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(D_802B8880));
    AddEffectMatrixOrtho();
    gSPDisplayList(gDisplayListHead++, (Gfx*) D_02007F18);
    const float baseItemScale = ItemWindowScale();
    const auto order = OrderByFinish();
    for (int player = 0; (participants == 3 || participants > 4 || MultiplayerOptions_DisplayCount() > 1) && player < participants; ++player) {
        const auto& screen = gScreenContexts[player];
        const int left = screen.screenStartX - screen.screenWidth / 2;
        const int top = screen.screenStartY - screen.screenHeight / 2;
        const int right = left + screen.screenWidth;
        const int bottom = top + screen.screenHeight;
        gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, left, top, right, bottom);
        if (finished[player]) continue;
        const float scale = baseItemScale * std::min(1.f,
            screen.screenWidth * GameEngine_GetAspectRatio() / (80.f * (4.f / 3.f)));
        // Native item-window slides have opposite signs on left/right players.
        // Normalize their progress so every expanded view enters from its own edge.
        const float progress = std::clamp(std::abs(playerHUD[player].slideItemBoxX) / 128.f, 0.f, 1.f);
        const float x = right - 4.f - 20.f * scale + (1.f - progress) * (40.f * scale + 8.f);
        const float y = top + 4.f + 16.f * scale;
        gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, left, top, right, bottom);
        draw_expanded_item_window(player, static_cast<int>(x), static_cast<int>(y), scale);
    }
    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    if (gRaceState > RACE_STAGING && gIsHUDVisible && (!progressDisplay || mode != 2)) {
        // Match the viewport groups so each physical screen gets its own map.
        // Separate viewers also keep vertex storage and interpolation IDs unique.
        const int displays = std::min(MultiplayerOptions_DisplayCount(), participants);
        int firstPlayer = 0;
        for (int display = 0; display < displays; ++display) {
            const int count = participants / displays + (display < participants % displays);
            int left, top, right, bottom;
            MultiplayerDisplays_Panel(display, displays, &left, &top, &right, &bottom);
            const int centerX = (left + right) / 2;
            const int centerY = (top + bottom) / 2;
            const int viewer = sharedHUDPlayer >= firstPlayer && sharedHUDPlayer < firstPlayer + count ?
                               sharedHUDPlayer : firstPlayer;
            gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, left, top, right, bottom);
            if (!progressDisplay) {
                draw_expanded_minimap(viewer, racers, order.data(), centerX, centerY, 152, 88);
            } else {
                draw_expanded_race_progress(viewer, racers, order.data(),
                    left, centerY - 3, right - left, .4f);
            }
            firstPlayer += count;
        }
        gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    }
    gSPDisplayList(gDisplayListHead++, (Gfx*) D_02007F48);
}

extern "C" void LocalMP_DrawRacePositions(void) {
    if (!LocalMP_IsActive() || mode == 2 || participants == 1 ||
        (participants != 3 && participants < 5 && MultiplayerOptions_DisplayCount() == 1) ||
        paused || results || gHUDDisable || gRaceState <= RACE_STAGING) return;
    init_rdp();
    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(D_802B8880));
    AddEffectMatrixOrtho();
    gSPDisplayList(gDisplayListHead++, (Gfx*) D_02007F18);
    // Native 4P initializes rankScaling to 0.5. Reduce that baseline by
    // ten percentage points for each additional human: 90/80/70/60 percent.
    constexpr float fourPlayerPositionScale = .5f;
    // Shrink both expanded race indicators another 20%, retaining the
    // existing per-player-count reduction and corner anchors.
    constexpr float expandedIndicatorScale = .8f;
    const float basePositionScale = expandedIndicatorScale * fourPlayerPositionScale * (1.f - .1f * (std::clamp(participants, 4, 8) - 4));
    const float baseLapScale = expandedIndicatorScale * (1.f - .1f * (std::clamp(participants, 4, 8) - 4));
    for (int player = 0; player < participants; ++player) {
        const auto& screen = gScreenContexts[player];
        const int left = screen.screenStartX - screen.screenWidth / 2;
        const int top = screen.screenStartY - screen.screenHeight / 2;
        const int right = left + screen.screenWidth;
        const int bottom = top + screen.screenHeight;
        const float fit = std::min(1.f,
            screen.screenWidth * GameEngine_GetAspectRatio() / (80.f * (4.f / 3.f)));
        const float scale = basePositionScale * fit;
        const float lapScale = baseLapScale * fit;
        const int rank = finished[player] && finishingPlace[player] >= 0 ?
                         finishingPlace[player] : gPlayers[player].currentRank;
        gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, left, top, right, bottom);
        draw_expanded_race_position(player, rank, left + 4 + static_cast<int>(64 * scale),
                                    bottom - 4 - static_cast<int>(32 * scale), scale);
        if (!finished[player]) {
            draw_expanded_race_lap(player, std::clamp(static_cast<int>(gLapCountByPlayerId[player]), 0, 2),
                                   right - 4 - static_cast<int>(16 * lapScale),
                                   bottom - 4 - static_cast<int>(8 * lapScale), lapScale);
        }
    }
    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    gSPDisplayList(gDisplayListHead++, (Gfx*) D_02007F48);
}

extern "C" void LocalMP_DrawPauseMenu(void) {
    if (!LocalMP_IsActive() || !paused || results) return;
    // Draw once after every racing viewport, in the native full-screen UI pass.
    // This avoids four-player pause-coordinate tables for controller ports 5-16.
    init_rdp();
    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(D_802B8880));
    AddEffectMatrixOrtho();
    gSPDisplayList(gDisplayListHead++, (Gfx*) D_02007F18);
    // Battle can leave the last player's scissor active. Restore full-screen
    // clipping before drawing text; the 3D cursor installs its own clipping.
    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    gDisplayListHead = draw_box_wide(gDisplayListHead, OTRGetDimensionFromLeftEdge(0), 0,
                                    OTRGetDimensionFromRightEdge(SCREEN_WIDTH), SCREEN_HEIGHT, 0, 0, 0, 140);
    char title[160];
    std::snprintf(title, sizeof(title), "%s  %s", GetWorld()->Cups[selectedCup]->Name, D_800E76CC[gCCSelection]);
    NativeText(160, 55, mode == 0 ? title : mode == 1 ? "VS RACE" : mode == 3 ? "TIME TRIALS" : "BALLOON BATTLE", TEXT_YELLOW, .7f, 296);
    if (auto* properties = CM_GetProps()) NativeText(160, 82, properties->Name, TEXT_YELLOW, .7f, 296);
    const int labels[] = {0, 1, 4}; // Original CONTINUE GAME / RETRY / QUIT strings.
    for (int option = 0; option < 3; ++option) {
        text_rainbow_effect(pauseChoice, option, TEXT_YELLOW);
        print_text_mode_1(100, 125 + option * 22, gTextPauseButton[labels[option]], 0, .75f, .75f);
    }
    std::snprintf(title, sizeof(title), "P%d PAUSED", pausePlayer + 1);
    NativeText(160, 201, title, TEXT_YELLOW, .4f);
    NativeText(160, 221, "UP / DOWN: SELECT   A / START: CONFIRM", TEXT_BLUE, .3f);
    Unk_D_800E70A0 cursorPosition{};
    cursorPosition.column = 88;
    cursorPosition.row = 117 + pauseChoice * 22;
    pause_menu_item_box_cursor(&pauseCursor, &cursorPosition);
    gSPDisplayList(gDisplayListHead++, (Gfx*) D_02007F48);
}

extern "C" void LocalMP_DrawBattleResults(void) {
    if (!LocalMP_BattleResultsOpen()) return;
    init_rdp();
    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(D_802B8880));
    AddEffectMatrixOrtho();
    gSPDisplayList(gDisplayListHead++, (Gfx*) D_02007F18);
    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    draw_expanded_battle_results(characters.data(), points.data(), participants, battleWinner,
                                battleResultChoice, &battleResultCursor);
    gSPDisplayList(gDisplayListHead++, (Gfx*) D_02007F48);
}

extern "C" void LocalMP_DrawRaceResults(void) {
    if (!LocalMP_RaceResultsOpen()) return;
    const auto order = raceResultStandings ? OrderByPoints() : OrderByFinish();
    std::array<ExpandedRaceResultRow, NUM_RACERS> rows{};
    for (int rank = 0; rank < racers; ++rank) {
        const int player = order[rank];
        int place = rank;
        if (raceResultStandings)
            while (place > 0 && points[order[place - 1]] == points[player]) --place;
        const int finishRank = finishingPlace[player];
        rows[rank] = {place + 1, player, characters[player], points[player],
                     mode == 0 && finishRank >= 0 && finishRank < 4 ? rewards[finishRank] : 0,
                     finished[player], estimatedTime[player], finishTimes[player]};
    }
    init_rdp();
    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(D_802B8880));
    AddEffectMatrixOrtho();
    gSPDisplayList(gDisplayListHead++, (Gfx*) D_02007F18);
    gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    draw_expanded_race_results(rows.data(), racers, participants, mode, raceResultStandings,
                              raceInCup + 1, static_cast<int>(GetWorld()->Cups[selectedCup]->GetSize()),
                              CupComplete(), raceResultChoice, engineClass, &raceResultCursor);
    gSPDisplayList(gDisplayListHead++, (Gfx*) D_02007F48);
}

extern "C" void LocalMP_UpdateBattle(void) {
    if (!LocalMP_IsActive() || mode != 2 || results || gRaceState != RACE_IN_PROGRESS) return;
    int survivor = -1;
    int alive = 0;
    int eliminatedThisTick = 0;
    for (int player = 0; player < participants; ++player) {
        if (!finished[player] && gPlayerBalloonCount[player] < 0) ++eliminatedThisTick;
    }
    battleRemaining -= eliminatedThisTick;
    for (int player = 0; player < participants; ++player) {
        if (gPlayerBalloonCount[player] < 0) {
            if (!finished[player]) {
                finished[player] = true;
                finishTimes[player] = gCourseTimer;
                // Simultaneous eliminations share a place. The native balloon
                // pop already starts the fade into a controllable bomb kart.
                finishingPlace[player] = battleRemaining;
                gPlayers[player].type &= ~PLAYER_CINEMATIC_MODE;
                func_800CA118(static_cast<u8>(player));
            }
        } else {
            survivor = player;
            ++alive;
        }
    }
    if (alive > 1) return;
    battleWinner = alive == 1 ? survivor : -1;
    gPlayerWinningIndex = battleWinner >= 0 ? battleWinner : 0;
    if (battleWinner >= 0) {
        finishingPlace[battleWinner] = 0;
        finishTimes[battleWinner] = gCourseTimer;
        finished[battleWinner] = true;
        gPlayerWinningIndex = battleWinner;
        func_800CA118(static_cast<u8>(battleWinner));
        ++points[battleWinner];
    }
    for (int player = 0; player < participants; ++player) {
        gPlayers[player].currentRank = finishingPlace[player];
        gGPCurrentRaceRankByPlayerId[player] = finishingPlace[player];
        gPlayers[player].type |= PLAYER_CINEMATIC_MODE;
    }
    // Arena music runs on native sequence player 0 unless it has an HMAS
    // replacement. Ranking music uses player 1, so stop both native music
    // players as well as HMAS before queuing the results sequence.
    func_800CA330(0);
    Audio_StopExpandedRaceSounds();
    SPDLOG_INFO("Battle results: stopped racing sound loops for {} players", participants);
    HMAS_Stop(HMAS_MUSIC);
    play_sequence2(MUSIC_SEQ_VS_BATTLE_RESULTS);
    gRaceState = RACE_FINISHED;
    results = true;
}

extern "C" void LocalMP_UpdateLaps(void) {
    if (!LocalMP_IsActive() || mode == 2) return;
    for (int i = 0; i < racers; i++) {
        if (finished[i]) continue;
        if (gLapCountByPlayerId[i] != gPlayers[i].lapCount) {
            gPlayers[i].lapCount = gLapCountByPlayerId[i];
            if (i < participants && gPlayers[i].lapCount == 2 && !finalLapMusicStarted) {
                finalLapMusicStarted = true;
                func_800CA49C(static_cast<u8>(i));
            }
            if (gPlayers[i].lapCount >= 3) {
                finished[i] = true;
                finishTimes[i] = func_8028EE8C(i);
                gTimePlayerLastTouchedFinishLine[i] = finishTimes[i];
                // Lock the result, but keep native CPU driving active so the
                // finished kart clears the line while other humans finish.
                gPlayers[i].type |= PLAYER_CINEMATIC_MODE | PLAYER_CPU;
                gPlayers[i].effects &= ~DRIFTING_EFFECT;
                reset_cpu_behaviour(i);
                if (i < participants) func_800CA118(static_cast<u8>(i));
                gRaceState = RACE_CALCULATE_RANKS;
            }
        }
    }
    const auto order = OrderByFinish();
    for (int rank = 0; rank < racers; rank++) {
        if (finished[order[rank]]) {
            finishingPlace[order[rank]] = rank;
            gPlayers[order[rank]].currentRank = rank;
        }
    }
    // CPU rivals keep following an active human after player one finishes.
    int bestHuman = -1;
    for (int human = 0; human < participants; human++) {
        if (!finished[human] && (bestHuman < 0 ||
            gPlayers[human].currentRank < gPlayers[bestHuman].currentRank)) bestHuman = human;
    }
    if (bestHuman >= 0) gBestRankedHumanPlayer = bestHuman;
}

extern "C" int LocalMP_FinishTick(void) {
    if (!LocalMP_IsActive()) return false;
    if (gRaceState == RACE_FINISHED) return true;
    if (mode == 2) return false; // Use the normal battle staging/physics dispatch.
    if (gRaceState != RACE_CALCULATE_RANKS) return false;
    update_race_position_data();
    LocalMP_UpdateLaps();
    const bool everyoneFinished = std::all_of(finished.begin(), finished.begin() + participants, [](bool value) { return value; });
    const int finishedRacers = static_cast<int>(std::count(finished.begin(), finished.begin() + racers, true));
    const bool lastRacerRemaining = racers > 1 && finishedRacers == racers - 1;
    if (everyoneFinished || lastRacerRemaining) {
        // All human finishers may close a race with CPU rivals still racing.
        // Also close when just one racer (human or CPU) remains: last place
        // is already determined, even if that driver is stuck or far behind.
        if (finishedRacers < racers) {
            func_8028E298();
            for (int player = 0; player < racers; ++player) {
                if (finished[player]) continue;
                finished[player] = estimatedTime[player] = true;
                const float estimate = gTimePlayerLastTouchedFinishLine[player];
                finishTimes[player] = std::isfinite(estimate) ?
                    std::max(estimate, gCourseTimer + TRACK_TIMER_ITER_f) : gCourseTimer + TRACK_TIMER_ITER;
                gTimePlayerLastTouchedFinishLine[player] = finishTimes[player];
                gPlayers[player].type |= PLAYER_CINEMATIC_MODE | PLAYER_CPU;
                gPlayers[player].effects &= ~DRIFTING_EFFECT;
                reset_cpu_behaviour(player);
                SPDLOG_INFO("Race auto-finish: slot {} assigned remaining place; time {} estimated; last racer={}",
                            player + 1, finishTimes[player], lastRacerRemaining);
            }
        }
        const auto order = OrderByFinish();
        for (int rank = 0; rank < racers; rank++) {
            int player = order[rank];
            finishingPlace[player] = rank;
            gGPCurrentRaceRankByPlayerId[player] = rank;
            gGPCurrentRacePlayerIdByRank[rank] = player;
            gPlayers[player].currentRank = rank;
            if (mode == 0 && rank < 4) points[player] += rewards[rank];
        }
        gPlayerWinningIndex = order[BestHumanRank(order)];
        if (CupComplete() && selectedCup < 4 && engineClass >= CC_50 && engineClass <= CC_EXTRA) {
            // Same trophy persistence as a native cup, without character-ID lookup.
            func_800B536C(BestHumanRank(OrderByPoints()));
        }
        Audio_StopExpandedRaceSounds();
        SPDLOG_INFO("Race results: stopped racing sound loops for {} humans / {} racers", participants, racers);
        HMAS_Stop(HMAS_MUSIC);
        if (mode == 0) {
            const int rank = finishingPlace[gPlayerWinningIndex];
            play_sequences(rank == 0 ? MUSIC_SEQ_FINISH_1ST_PLACE :
                           rank < 4 ? MUSIC_SEQ_FINISH_2ND_4TH_PLACE : MUSIC_SEQ_FINISH_5TH_8TH_PLACE,
                           rank < 4 ? MUSIC_SEQ_WINNING_RESULTS : MUSIC_SEQ_LOSING_RESULTS);
        } else {
            ++points[order[0]];
            play_sequences(MUSIC_SEQ_FINISH_1ST_PLACE, MUSIC_SEQ_VS_BATTLE_RESULTS);
        }
        gRaceState = RACE_FINISHED;
        results = true;
    }
    return true;
}

extern "C" void LocalMP_DrawUI(void) {
    if (!TrackBrowser::Instance || GetWorld()->Cups.empty()) return;
    if (!optionsInitialized) {
        lobby = false;
        optionsInitialized = true;
    }
    if (!MultiplayerOptions_EightEnabled()) {
        lobby = false;
        if (!LocalMP_IsActive()) return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F8)) {
        if (LocalMP_IsActive()) {
            if (!results && gRaceState >= RACE_IN_PROGRESS) SetPaused(!paused);
        } else if (MultiplayerOptions_CanChange()) {
            lobby = !lobby;
            gameMenu = false;
        }
    }
    if (MultiplayerOptions_EightEnabled() && MultiplayerOptions_CanChange() && lobby && !active) {
        ImGui::SetNextWindowSize(ImVec2(570, 630), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Local Multiplayer - 2 to 16 Players", &lobby)) {
            const int maxHumans = MultiplayerOptions_SixteenEnabled() ? MAX_HUMAN_PLAYERS : 8;
            participants = std::clamp(participants, 2, maxHumans);
            ImGui::SliderInt("Players", &participants, 2, maxHumans);
            ImGui::Combo("Mode", &mode, "Grand Prix\0VS Race\0Battle\0");
            const int bots = mode != 2 && MultiplayerOptions_SixteenEnabled() ? NUM_RACERS - participants :
                             mode == 0 && participants >= 3 && participants <= 7 &&
                             MultiplayerOptions_BotsEnabled() ? NUM_PLAYERS - participants : 0;
            ImGui::Text("%d human players + %d bots = %d racers", participants, bots, participants + bots);
            if (mode != 2) ImGui::Combo("Engine class", &engineClass, "50cc\0 100cc\0 150cc\0Extra / Mirror\0");
            auto& cups = GetWorld()->Cups;
            const int raceCupCount = std::max(1, static_cast<int>(cups.size()) - 1);
            ClampCourseSelection();
            if (mode != 2 && ImGui::BeginCombo("Cup", cups[selectedCup]->Name)) {
                for (int i = 0; i < raceCupCount; i++) {
                    if (ImGui::Selectable(cups[i]->Name, selectedCup == i)) { selectedCup = i; selectedTrack = 0; }
                }
                ImGui::EndCombo();
            }
            auto* cup = cups[selectedCup];
            selectedTrack = std::clamp(selectedTrack, 0, static_cast<int>(cup->mTracks.size()) - 1);
            if (mode == 2 && ImGui::BeginCombo("Arena", gTrackRegistry.GetInfo(cup->mTracks[selectedArena])->Name.c_str())) {
                for (int arena = 0; arena < cup->mTracks.size(); ++arena) {
                    if (ImGui::Selectable(gTrackRegistry.GetInfo(cup->mTracks[arena])->Name.c_str(), selectedArena == arena)) {
                        selectedArena = selectedTrack = arena;
                    }
                }
                ImGui::EndCombo();
            }
            if (mode == 1 && ImGui::BeginCombo("Track", gTrackRegistry.GetInfo(cup->mTracks[selectedTrack])->Name.c_str())) {
                for (int i = 0; i < cup->mTracks.size(); i++) {
                    if (ImGui::Selectable(gTrackRegistry.GetInfo(cup->mTracks[i])->Name.c_str(), selectedTrack == i)) selectedTrack = i;
                }
                ImGui::EndCombo();
            }
            ImGui::Separator();
            ImGui::TextWrapped("Choose each player's character. Players can also press left/right and A on their controller to get ready.");
            for (int i = 0; i < participants; i++) {
                ImGui::PushID(i);
                int character = characters[i];
                char label[30]; std::snprintf(label, sizeof(label), "Player %d", i + 1);
                if (ImGui::Combo(label, &character, characterNames, 8)) ChooseCharacter(i, character);
                ImGui::SameLine(); ImGui::TextUnformatted(ready[i] ? "Ready" : "");
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::Button("Controller settings (ports 1-8)")) {
                auto gui = Ship::Context::GetRawInstance()->GetWindow()->GetGui();
                if (auto editor = gui->GetGuiWindow("Input Editor")) editor->Show();
            }
            if (ImGui::Button(mode == 2 ? "Start battle" : "Start race", ImVec2(180, 36))) { startedFromGameMenu = false; request = 1; }
            ImGui::SameLine(); ImGui::TextUnformatted("F8: show/hide this lobby");
        }
        ImGui::End();
    }
    if (!LocalMP_IsActive()) return;
    if (results && mode == 2 && !LocalMP_BattleResultsOpen()) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(.5f, .5f));
        ImGui::SetNextWindowSize(ImVec2(490, 0), ImGuiCond_Appearing);
        // Sixteen race rows plus cup standings must remain scrollable on
        // small windows, with the continuation controls inside the window.
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
            ImVec2(std::numeric_limits<float>::max(), ImGui::GetMainViewport()->Size.y * .85f));
        ImGui::Begin("Multiplayer Results", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
        if (results) {
            const bool cupComplete = CupComplete();
            if (mode == 2) {
                if (battleWinner >= 0) ImGui::Text("P%d %s wins!", battleWinner + 1, characterNames[characters[battleWinner]]);
                else ImGui::TextUnformatted("Draw - all players eliminated");
            } else {
                ImGui::TextUnformatted(cupComplete ? "Cup complete" : "Race complete");
                if (mode == 0) {
                    ImGui::Text("Grand Prix - Race %d/%d", raceInCup + 1,
                                static_cast<int>(GetWorld()->Cups[selectedCup]->GetSize()));
                }
            }
            ImGui::Separator();
            const auto order = OrderByFinish();
            for (int rank = 0; rank < racers; rank++) {
                int i = order[rank];
                const char* kind = i < participants ? "P" : "CPU";
                const int number = i < participants ? i + 1 : i - participants + 1;
                if (mode == 2) {
                    ImGui::Text("%d. P%d  %-12s  %s  Wins: %d", finishingPlace[i] + 1, i + 1, characterNames[characters[i]],
                                i == battleWinner ? "Winner" : "Eliminated", points[i]);
                } else if (finished[i]) {
                    ImGui::Text("%d. %s%d  %-12s  %d:%05.2f%s   %s: %d", rank + 1, kind, number,
                                characterNames[characters[i]], static_cast<int>(finishTimes[i]) / 60,
                                std::fmod(finishTimes[i], 60.f), estimatedTime[i] ? "*" : "",
                                mode == 0 ? "Points" : "Wins", points[i]);
                } else {
                    ImGui::Text("%d. %s%d  %-12s  DNF   Points: %d", rank + 1, kind, number,
                                characterNames[characters[i]], points[i]);
                }
            }
            if (mode == 0) {
                const auto standings = OrderByPoints();
                ImGui::Separator(); ImGui::TextUnformatted(cupComplete ? "Final cup standings" : "Current cup standings");
                for (int rank = 0; rank < racers; rank++) {
                    int i = standings[rank];
                    int displayRank = rank;
                    while (displayRank > 0 && points[standings[displayRank - 1]] == points[i]) --displayRank;
                    ImGui::Text("%d. %s%d - %s: %d points", displayRank + 1,
                                i < participants ? "P" : "CPU", i < participants ? i + 1 : i - participants + 1,
                                characterNames[characters[i]], points[i]);
                }
                if (std::any_of(estimatedTime.begin(), estimatedTime.begin() + racers, [](bool value) { return value; }))
                    ImGui::TextUnformatted("* CPU time estimated from track progress, as in native GP.");
                if (cupComplete && ImGui::Button("View ceremony")) request = 6;
            }
            if (mode == 0 && !cupComplete && ImGui::Button("Next race")) request = 2;
            if (mode != 0 && ImGui::Button("Rematch")) request = 3;
        }
        if (startedFromGameMenu) ImGui::TextUnformatted("A / Start: continue   B: game menu");
        if (ImGui::Button(startedFromGameMenu ? "Return to game menu" : "Return to lobby")) request = 4;
        ImGui::End();
    }
}
