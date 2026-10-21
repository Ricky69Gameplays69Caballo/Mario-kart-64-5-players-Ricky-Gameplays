#include <racing/actors.h>
#include <main.h>
#include "port/LocalMultiplayer.h"

static s32 item_box_respawn_updates(void) {
    return gGamestate == RACING && MultiplayerOptions_SixteenEnabled() ? 2 : 1;
}

/**
 * @brief Updates the item box under the hot air balloon object.
 * It used in Luigi Raceway.
 * (here it's just update the rotation of the item box not the position, for the position it's in object)
 *
 * @param itemBox
 */
static void update_item_box_hot_air_balloon_step(struct ItemBox* itemBox) {
    switch (itemBox->state) {
        case 5:
            itemBox->rot[0] += 0xB6;
            itemBox->rot[1] -= 0x16C;
            itemBox->rot[2] += 0xB6;
            break;
        case 3:
            if (itemBox->someTimer == 0x14) {
                itemBox->state = 5;
                itemBox->flags = -0x4000;
            } else {
                itemBox->someTimer++;
                itemBox->rot[0] += 0x444;
                itemBox->rot[1] -= 0x2D8;
                itemBox->rot[2] += 0x16C;
            }
            break;
    }
}

void update_actor_item_box_hot_air_balloon(struct ItemBox* itemBox) {
    const s32 updates = itemBox->state == 3 ? item_box_respawn_updates() : 1;
    for (s32 step = 0; step < updates; ++step) {
        update_item_box_hot_air_balloon_step(itemBox);
    }
}

/**
 * @brief Updates the item box actor.
 *
 * @param itemBox
 */
static void update_item_box_step(struct ItemBox* itemBox) {
    switch (itemBox->state) {
        case 0:
            itemBox->state = 1;
            break;
        case 1:
            if ((itemBox->pos[1] - itemBox->origY) < 8.66f) {
                itemBox->pos[1] += 0.45f;
            } else {
                itemBox->pos[1] = itemBox->origY + 8.66f;
                itemBox->state = 2;
                itemBox->flags = 0xC000;
            }
            break;
        case 2:
            itemBox->rot[0] += 0xB6;
            itemBox->rot[1] -= 0x16C;
            itemBox->rot[2] += 0xB6;
            break;
        case 3:
            if (itemBox->someTimer == 20) {
                itemBox->state = 0;
                itemBox->pos[1] = itemBox->resetDistance - 20.0f;
                itemBox->flags = 0xC000;
            } else {
                itemBox->someTimer++;
                itemBox->rot[0] += 0x444;
                itemBox->rot[1] -= 0x2D8;
                itemBox->rot[2] += 0x16C;
            }
            break;
    }
}

void update_actor_item_box(struct ItemBox* itemBox) {
    // Advance both the break-up timer and the rising/return animation twice
    // during recovery. Keep the idle rotation at its normal speed.
    const s32 updates = itemBox->state == 2 ? 1 : item_box_respawn_updates();
    for (s32 step = 0; step < updates; ++step) {
        update_item_box_step(itemBox);
    }
}
