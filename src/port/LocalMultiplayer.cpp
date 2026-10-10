#include "LocalMultiplayer.h"
#include <libultraship.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <limits>
#include <numeric>
#include <utility>
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
int racers = 8; // Race snapshot: humans have screens; bots occupy the remaining grid slots.
int mode = 0; // 0: Grand Prix, 1: VS, 2: Battle.
int engineClass = 1;
int selectedCup = 0;
int selectedTrack = 0;
int selectedArena = 0;
int battleRemaining = 0;
int battleWinner = -1;
int raceInCup = 0;
int request = 0; // 1: new session, 2: next race, 3: rematch, 4: lobby, 5: refresh menu, 6: ceremony
bool ceremonyPending = false;
bool finalLapMusicStarted = false;
int ceremonyCharacter = 0;
int ceremonyRank = 0;
std::array<int, 8> characters = {0, 1, 2, 3, 4, 5, 6, 7};
std::array<bool, 8> ready = {};
// Native portrait order differs from the racing character IDs.
constexpr std::array<int, 8> characterGrid = {0, 1, 6, 3, 2, 4, 5, 7};
std::array<bool, 8> finished = {};
std::array<float, 8> finishTimes = {};
std::array<int, 8> finishingPlace = {};
std::array<bool, 8> estimatedTime = {};
std::array<int, 8> points = {};
bool progressDisplay = false; // C-right switches the one shared map/progress display.
int sharedHUDPlayer = 0; // Highlight the player who last changed the shared display.
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
    gModeSelection = mode == 0 ? GRAND_PRIX : mode == 1 ? VERSUS : BATTLE;
    if (mode != 0) TrackBrowser_SetTrackFromCup();
    // A registry-created track can reuse the previous track's address. Force
    // setup_race to reload its data and recreate the world for every new race.
    gCurrentlyLoadedTrackAddr = 0;
    // Rendering can run before the first logic tick initializes the lap data.
    // Clear the previous race now so the HUD cannot request a missing Lakitu.
    for (int i = 0; i < NUM_PLAYERS; i++) gLapCountByPlayerId[i] = -1;
    gCCSelection = mode == 2 ? CC_100 : engineClass;
    set_mirror_mode(mode != 2 && engineClass == CC_EXTRA);
    gPlaceItemBoxes = 1;
    gDemoMode = 0;
    gDemoUseController = 0;
    gDebugMenuSelection = 0;
    gEnableDebugMode = false;
    // The old menu/HUD tables have four entries. This lobby owns the actual
    // human count, rendering, scoring and finish flow independently of them.
    gPlayerCount = participants == 3 ? 3 : 4;
    gPlayerCountSelection1 = gPlayerCount;
    gScreenModeSelection = SCREEN_MODE_3P_4P_SPLITSCREEN;
    gActiveScreenMode = SCREEN_MODE_3P_4P_SPLITSCREEN;
    for (int i = 0; i < 8; i++) gCharacterSelections[i] = characters[i];
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

std::array<int, 8> OrderByFinish() {
    std::array<int, 8> order;
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

std::array<int, 8> OrderByPoints() {
    std::array<int, 8> order;
    std::iota(order.begin(), order.end(), 0);
    // Match the native human-first tie break, using slots rather than characters.
    // Equal-scoring human players keep the lower controller number first.
    std::stable_sort(order.begin(), order.begin() + racers, [](int a, int b) {
        if (points[a] != points[b]) return points[a] > points[b];
        return a < b;
    });
    return order;
}

int BestHumanRank(const std::array<int, 8>& order) {
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
    for (int rank = 0; rank < NUM_PLAYERS; ++rank) {
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
    if (participants <= 4) return fourPlayerItemScale;
    constexpr float fivePlayerItemScale = fourPlayerItemScale * .9f * .5f;
    return fivePlayerItemScale * (1.f - .1f * (participants - 5));
}

}

extern "C" int MultiplayerOptions_EightEnabled(void) {
    return CVarGetInteger(CVAR_MULTIPLAYER_EIGHT, 0) != 0;
}
extern "C" float RacerRenderDistance_Multiplier(void) {
    if (gGamestate != RACING) return 1.f;
    return static_cast<float>(std::clamp(CVarGetInteger(CVAR_RACER_RENDER_DISTANCE, 2), 1, 5));
}
extern "C" float RacerRenderDistance_FarPlane(float normalFar) {
    return normalFar * RacerRenderDistance_Multiplier();
}
extern "C" int MultiplayerOptions_BotsEnabled(void) {
    return CVarGetInteger(CVAR_MULTIPLAYER_BOTS, 0) != 0;
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
        if (gMainMenuSelection != MAIN_MENU_PLAYER_SELECT || gPlayerCount != 4 ||
            is_screen_being_faded() || !(buttons & R_JPAD)) return 0;
        // The original menu uses four-entry tables. Keep its count at 4 and
        // let this native page own the extra choices and setup sequence.
        participants = 5;
        resumeBattleSession = false;
        mode = 0;
        gameMenu = true;
        gameMenuStep = GameMenuStep::Players;
        characters = characterGrid;
        ready.fill(false);
        play_sound2(SOUND_MENU_CURSOR_MOVE);
        return 1;
    }
    if (is_screen_being_faded()) return 1;
    auto selectSound = [] { reset_cycle_flash_menu(); play_sound2(SOUND_MENU_SELECT); };
    auto moveSound = [] { reset_cycle_flash_menu(); play_sound2(SOUND_MENU_CURSOR_MOVE); };
    const bool confirm = buttons & (A_BUTTON | START_BUTTON);
    switch (gameMenuStep) {
        case GameMenuStep::Players:
            resumeBattleSession = false;
            if ((buttons & B_BUTTON) || ((buttons & L_JPAD) && participants == 5)) {
                gameMenu = false;
                gPlayerCount = gPlayerCountSelection1 = 4;
                gScreenModeSelection = SCREEN_MODE_3P_4P_SPLITSCREEN;
                play_sound2(SOUND_MENU_GO_BACK);
            } else if ((buttons & R_JPAD) && participants < 8) { ++participants; moveSound(); }
            else if ((buttons & L_JPAD) && participants > 5) { --participants; moveSound(); }
            else if (confirm) { gameMenuStep = GameMenuStep::Mode; selectSound(); }
            break;
        case GameMenuStep::Mode:
            resumeBattleSession = false;
            if (buttons & B_BUTTON) {
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
        // Once the player count is selected, the native menu keeps just that
        // column and opens the class list beside the selected game mode.
        for (int slot = 0; slot < (choosingPlayers ? 4 : 1); slot++) {
            const int x = choosingPlayers ? 21 + slot * 71 : 104;
            const int humanCount = choosingPlayers ? slot + 5 : participants;
            const bool chosen = participants == humanCount;
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
            const int bots = mode == 0 && participants < 8 && MultiplayerOptions_BotsEnabled() ? 8 - participants : 0;
            std::snprintf(title, sizeof(title), "%d PLAYERS + %d BOTS", participants, bots);
            NativeText(160, 201, mode == 2 ? "BATTLE - THREE BALLOONS EACH" : title, TEXT_YELLOW, .42f);
            NativeText(160, 212, gameMenuStep == GameMenuStep::Players ? "LEFT / RIGHT: PLAYERS" : "UP / DOWN: GAME MODE", TEXT_BLUE, .35f);
        }
    } else if (gameMenuStep == GameMenuStep::Characters) {
        std::array<s32, 8> roster{};
        std::array<u8, 8> confirmed{};
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
    if (gameMenuStep != GameMenuStep::Characters) NativeText(160, 234, gameMenuStep == GameMenuStep::Class ?
               "UP / DOWN: CLASS   A: SELECT   B: BACK" : "A: SELECT   B: BACK", TEXT_YELLOW, .33f);
}

extern "C" void LocalMP_DrawGameMenuHint(void) {
    if (!MultiplayerOptions_EightEnabled() || !MultiplayerOptions_CanChange() ||
        gMenuSelection != MAIN_MENU || gameMenu || lobby) return;
    NativeText(160, 233, "RIGHT PAST 4P: 5P 6P 7P 8P", TEXT_YELLOW, .33f);
}
extern "C" int LocalMP_Count(void) { return LocalMP_IsActive() ? participants : gPlayerCount; }
extern "C" int LocalMP_BattleResultsOpen(void) {
    return LocalMP_IsActive() && results && mode == 2 && participants > 4;
}
extern "C" int LocalMP_RacerCount(void) { return LocalMP_IsActive() ? racers : gPlayerCount; }
extern "C" int LocalMP_IsFinished(int player) {
    // Battle players keep their native eliminated-player bomb physics and camera.
    return player < 0 || player >= racers || results || (mode != 2 && finished[player]);
}
extern "C" int LocalMP_CeremonyResult(int* character, int* rank) {
    if (!ceremonyPending || gGamestate != ENDING) return false;
    *character = ceremonyCharacter;
    *rank = ceremonyRank;
    ceremonyPending = false;
    return true;
}

extern "C" void LocalMP_ProcessRequest(void) {
    const int action = std::exchange(request, 0);
    if (!action) return;
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
    if (!MultiplayerOptions_EightEnabled()) return;
    if (paused) SetPaused(false);
    if (action == 1) {
        ceremonyPending = false;
        progressDisplay = false;
        sharedHUDPlayer = 0;
        // Freeze the roster for this cup. With both options on, GP fills to eight
        // for 3-7 humans; VS and two-player lobby races remain human-only.
        racers = mode == 0 && participants >= 3 && participants <= 7 &&
                 MultiplayerOptions_BotsEnabled() ? NUM_PLAYERS : participants;
        points.fill(0);
        raceInCup = 0;
        for (int i = 0; i < racers; i++) gGPCurrentRaceRankByPlayerId[i] = racers - i - 1;
    } else if (action == 2 && mode == 0) {
        ++raceInCup;
    }
    StartTrack();
}

extern "C" void LocalMP_ResetRace(void) {
    if (!active && gGamestate == RACING && !gDemoMode && gModeSelection == GRAND_PRIX &&
        (gPlayerCount == 3 || gPlayerCount == 4) && MultiplayerOptions_EightEnabled() &&
        !MultiplayerOptions_BotsEnabled()) {
        // The normal menus can now launch GP with only the 8-player option on.
        // Use the existing per-player cup/results flow to run without CPU slots.
        participants = racers = gPlayerCount;
        for (int player = 0; player < NUM_PLAYERS; ++player) characters[player] = gCharacterSelections[player];
        mode = 0;
        engineClass = gCCSelection;
        selectedCup = static_cast<int>(GetWorld()->GetCupIndex());
        selectedTrack = raceInCup = static_cast<int>(GetWorld()->GetCurrentCup()->CursorPosition);
        points.fill(0);
        ready.fill(true);
        for (int player = 0; player < racers; ++player) gGPCurrentRaceRankByPlayerId[player] = racers - player - 1;
        active = startedFromGameMenu = true;
        gameMenu = lobby = false;
        request = 0;
    }
    if (!LocalMP_IsActive()) return;
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
    LocalMP_Layout();
}

extern "C" void LocalMP_Layout(void) {
    if (!LocalMP_IsActive()) return;
    const int columns = participants <= 4 ? 2 : 3;
    const int rows = (participants + columns - 1) / columns;
    // Round shared edges to the nearest even coordinate before deriving sizes.
    // Rounding each width down independently left holes in the three-column grid.
    // Even edges also preserve exact integer centers in viewport/scissor setup.
    const auto edge = [](int index, int extent, int divisions) {
        return ((index * extent + divisions) / (2 * divisions)) * 2;
    };
    std::array<int, 4> xEdges{}, yEdges{};
    for (int column = 0; column <= columns; ++column) xEdges[column] = edge(column, SCREEN_WIDTH, columns);
    for (int row = 0; row <= rows; ++row) yEdges[row] = edge(row, SCREEN_HEIGHT, rows);
    const bool hasWideSlot = participants == 5 || participants == 7 || participants == 8;
    const int preferredPlayer = CVarGetInteger("gMultiplayer.WidePlayer", 0) - 1;
    const int widePlayer = preferredPlayer >= 0 && preferredPlayer < participants ?
                           preferredPlayer : participants - 1;
    for (int i = 0; i < participants; i++) {
        auto& screen = gScreenContexts[i];
        // Swap the preferred player's cell with the bottom-row wide cell.
        // Context indices still belong to the same player/controller/HUD.
        int slot = i;
        if (hasWideSlot) {
            if (i == widePlayer) slot = participants - 1;
            else if (i == participants - 1) slot = widePlayer;
        }
        const int left = xEdges[slot % columns];
        const int right = hasWideSlot && slot == participants - 1 ? SCREEN_WIDTH :
                          xEdges[slot % columns + 1];
        const int top = yEdges[slot / columns];
        const int bottom = yEdges[slot / columns + 1];
        screen.screenWidth = right - left;
        screen.screenHeight = bottom - top;
        screen.screenStartX = left + screen.screenWidth / 2;
        screen.screenStartY = top + screen.screenHeight / 2;
        screen.player = &gPlayers[i];
        screen.controllers = &gControllers[i];
        screen.unkC = &D_8015F790[i * 16];
        screen.pathCounter = 1;
    }
    gScreenAspect = static_cast<float>(gScreenContexts[0].screenWidth) / gScreenContexts[0].screenHeight;
    if (LocalMP_BattleResultsOpen()) {
        auto& screen = gScreenContexts[battleWinner >= 0 ? battleWinner : 0];
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
    if (startedFromGameMenu && results) {
        const u16 input = gControllers[0].buttonPressed;
        if (input & B_BUTTON) request = 4;
        else if (input & (A_BUTTON | START_BUTTON)) {
            const bool moreRaces = mode == 0 && raceInCup + 1 < GetWorld()->Cups[selectedCup]->GetSize();
            request = moreRaces ? 2 : mode != 0 ? 3 : 6;
        }
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
    if (participants > 4) {
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
    if (!LocalMP_IsActive() || participants <= 4 || paused || results || gHUDDisable) return;
    init_rdp();
    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(D_802B8880));
    AddEffectMatrixOrtho();
    gSPDisplayList(gDisplayListHead++, (Gfx*) D_02007F18);
    const float scale = ItemWindowScale();
    const auto order = OrderByFinish();
    for (int player = 0; player < participants; ++player) {
        const auto& screen = gScreenContexts[player];
        const int left = screen.screenStartX - screen.screenWidth / 2;
        const int top = screen.screenStartY - screen.screenHeight / 2;
        const int right = left + screen.screenWidth;
        const int bottom = top + screen.screenHeight;
        gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, left, top, right, bottom);
        if (finished[player]) continue;
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
        // Keep the shared position line on the first horizontal divider.
        // The course outline instead belongs inside the widest player view.
        const int rows = (participants + 2) / 3;
        const int centerY = ((SCREEN_HEIGHT + rows) / (2 * rows)) * 2;
        const int viewer = std::clamp(sharedHUDPlayer, 0, participants - 1);
        const int mapWidth = 76;
        const int mapHeight = 44;
        const float uiLeft = OTRGetDimensionFromLeftEdge(0);
        const float uiWidth = OTRGetDimensionFromRightEdge(SCREEN_WIDTH) - uiLeft;
        if (!progressDisplay) {
            const int preferred = CVarGetInteger("gMultiplayer.WidePlayer", 0) - 1;
            int mapPlayer = preferred >= 0 && preferred < participants ? preferred : participants - 1;
            for (int player = 0; player < participants; ++player) {
                const auto& candidate = gScreenContexts[player];
                const auto& current = gScreenContexts[mapPlayer];
                if (candidate.screenWidth * current.screenHeight > current.screenWidth * candidate.screenHeight)
                    mapPlayer = player;
            }
            const auto& screen = gScreenContexts[mapPlayer];
            const int left = screen.screenStartX - screen.screenWidth / 2;
            const int top = screen.screenStartY - screen.screenHeight / 2;
            const int right = left + screen.screenWidth;
            const int bottom = top + screen.screenHeight;
            const int halfMapWidth = static_cast<int>(mapWidth * SCREEN_WIDTH / uiWidth / 2.f) + 3;
            // Left quarter leaves the kart visible in the center. In the
            // 5P/8P layout this retains the old X and lowers it to kart height.
            const int mapX = std::clamp(left + screen.screenWidth / 4,
                                       left + halfMapWidth, right - halfMapWidth);
            const int mapY = std::clamp(top + screen.screenHeight * 65 / 100,
                                       top + mapHeight / 2 + 3, bottom - mapHeight / 2 - 3);
            gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, left, top, right, bottom);
            draw_expanded_minimap(viewer, racers, order.data(), mapX, mapY, mapWidth, mapHeight);
            gDPSetScissor(gDisplayListHead++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
        } else {
            const int logicalWidth = static_cast<int>(124.f * SCREEN_WIDTH / uiWidth);
            draw_expanded_race_progress(viewer, racers, order.data(),
                (SCREEN_WIDTH - logicalWidth) / 2, centerY - 3, logicalWidth, .4f);
        }
    }
    gSPDisplayList(gDisplayListHead++, (Gfx*) D_02007F48);
}

extern "C" void LocalMP_DrawRacePositions(void) {
    if (!LocalMP_IsActive() || mode == 2 || participants < 5 || paused || results || gHUDDisable || gRaceState <= RACE_STAGING) return;
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
    const float scale = expandedIndicatorScale * fourPlayerPositionScale * (1.f - .1f * (participants - 4));
    const float lapScale = expandedIndicatorScale * (1.f - .1f * (participants - 4));
    for (int player = 0; player < participants; ++player) {
        const auto& screen = gScreenContexts[player];
        const int left = screen.screenStartX - screen.screenWidth / 2;
        const int top = screen.screenStartY - screen.screenHeight / 2;
        const int right = left + screen.screenWidth;
        const int bottom = top + screen.screenHeight;
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
    // This avoids four-player pause-coordinate tables for controller ports 5-8.
    init_rdp();
    gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(D_802B8880));
    AddEffectMatrixOrtho();
    gSPDisplayList(gDisplayListHead++, (Gfx*) D_02007F18);
    gDisplayListHead = draw_box_wide(gDisplayListHead, OTRGetDimensionFromLeftEdge(0), 0,
                                    OTRGetDimensionFromRightEdge(SCREEN_WIDTH), SCREEN_HEIGHT, 0, 0, 0, 140);
    char title[160];
    std::snprintf(title, sizeof(title), "%s  %s", GetWorld()->Cups[selectedCup]->Name, D_800E76CC[gCCSelection]);
    NativeText(160, 55, mode == 0 ? title : mode == 1 ? "VS RACE" : "BALLOON BATTLE", TEXT_YELLOW, .7f, 296);
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
                gPlayers[i].type |= PLAYER_CINEMATIC_MODE;
                if (i < participants) gPlayers[i].type &= ~PLAYER_CPU;
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
    if (everyoneFinished) {
        // Native GP closes the race after all humans finish and estimates any
        // remaining CPU times from lap/path progress. Preserve that behavior.
        if (mode == 0 && racers > participants) {
            func_8028E298();
            for (int player = participants; player < racers; ++player) {
                if (finished[player]) continue;
                finished[player] = estimatedTime[player] = true;
                finishTimes[player] = gTimePlayerLastTouchedFinishLine[player];
                gPlayers[player].type |= PLAYER_CINEMATIC_MODE;
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
        if (ImGui::Begin("Local Multiplayer - 2 to 8 Players", &lobby)) {
            ImGui::SliderInt("Players", &participants, 2, 8);
            ImGui::Combo("Mode", &mode, "Grand Prix\0VS Race\0Battle\0");
            const int bots = mode == 0 && participants >= 3 && participants <= 7 &&
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
    if (results && !LocalMP_BattleResultsOpen()) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(.5f, .5f));
        ImGui::SetNextWindowSize(ImVec2(490, 0), ImGuiCond_Appearing);
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
