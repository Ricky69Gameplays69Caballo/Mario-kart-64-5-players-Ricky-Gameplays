#ifndef LOCAL_MULTIPLAYER_H
#define LOCAL_MULTIPLAYER_H
#ifdef __cplusplus
extern "C" {
#endif
// Saved PC menu options. Changes are accepted only outside gameplay.
#define CVAR_MULTIPLAYER_EIGHT "gMultiplayer.Enable8Players"
#define CVAR_MULTIPLAYER_BOTS "gMultiplayer.Enable3P4PBots"
#define CVAR_RACER_RENDER_DISTANCE "gMultiplayer.RacerRenderDistance"
// Live graphics setting: available in every racing mode, independent of toggles.
float RacerRenderDistance_Multiplier(void);
float RacerRenderDistance_FarPlane(float normalFar);
int MultiplayerOptions_EightEnabled(void);
int MultiplayerOptions_BotsEnabled(void);
int MultiplayerOptions_CanChange(void);
void MultiplayerOptions_OnEightChanged(void);
void MultiplayerOptions_OnBotsChanged(void);
void MultiplayerOptions_UpdateMenus(void);
int LocalMP_IsActive(void);
int LocalMP_IsLobbyOpen(void);
int LocalMP_GameMenuIsOpen(void);
int LocalMP_GameMenuTick(void);
void LocalMP_DrawGameMenu(void);
void LocalMP_DrawGameMenuHint(void);
void LocalMP_DrawPauseMenu(void);
int LocalMP_BattleResultsOpen(void);
void LocalMP_DrawBattleResults(void);
void LocalMP_DrawRacePositions(void);
void LocalMP_DrawItemWindows(void);
int LocalMP_Count(void);
int LocalMP_RacerCount(void);
int LocalMP_IsFinished(int player);
// Consume the expanded cup's slot-based ceremony result during ending setup.
int LocalMP_CeremonyResult(int* character, int* rank);
void LocalMP_ProcessRequest(void);
void LocalMP_OnInput(void);
void LocalMP_ResetRace(void);
void LocalMP_Layout(void);
void LocalMP_UpdateLaps(void);
void LocalMP_UpdateBattle(void);
int LocalMP_FinishTick(void);
void LocalMP_DrawUI(void);
#ifdef __cplusplus
}
#endif
#endif
