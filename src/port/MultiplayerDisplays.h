#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Update the desktop span from the main/render thread. Layout coordinates
// remain in the game's logical 320x240 space.
void MultiplayerDisplays_Update(void);
void MultiplayerDisplays_Refresh(void);
void MultiplayerDisplays_Stop(void);
void MultiplayerDisplays_Panel(int index, int count, int* left, int* top, int* right, int* bottom);
const char* MultiplayerDisplays_Status(void);
#ifdef __cplusplus
}
#endif
