#include "modding.h"
#include "global.h"
#include "recomputils.h"

#include "globalobjects_api.h"

// Must not be changed, needs to match the size of ObjectContext's slots array.
#define OBJECT_SLOT_COUNT 35

typedef struct {
    u8 numEntries;
    // numPersistentEntries is inherited from the global object context.
    s16 ids[OBJECT_SLOT_COUNT];
    void* objects[OBJECT_SLOT_COUNT];
} IdSlots;

typedef struct {
    u8 numEntries;
    u8 numPersistentEntries;
    u8 mainKeepSlot;
    u8 subKeepSlot;
    s16 ids[OBJECT_SLOT_COUNT];
    void* objects[OBJECT_SLOT_COUNT];
    DmaRequest dmaReqs[OBJECT_SLOT_COUNT];
} GlobalSlots;

IdSlots all_id_slots[ACTOR_ID_MAX];
GlobalSlots global_slots;

// Tracks how many layers of recursive slot loading are active. This is needed because actors can spawn other actors,
// which in turn loads the child actor ID's slot set.
// This tracking allows the mod to reload the parent actor's slot set when the child actor's spawning is finished.
#define SLOT_SET_STACK_SIZE 64
u32 slot_load_stack_depth = 0;
ActorId slot_load_id_stack[SLOT_SET_STACK_SIZE];

void propagate_persistent_slots(ObjectContext* objectCtx) {
    recomp_printf("Copying %d persistent slots\n", objectCtx->numPersistentEntries);
    for (int i = 0; i < ACTOR_ID_MAX; i++) {
        // Copy the ids and objects from the persistent slots in the global object context.
        for (int slot = 0; slot < objectCtx->numPersistentEntries; slot++) {
            all_id_slots[i].ids[slot]     = objectCtx->slots[slot].id;
            all_id_slots[i].objects[slot] = objectCtx->slots[slot].segment;
        }
        // Set the entry count based on the global object context's persistent entry count.
        all_id_slots[i].numEntries = objectCtx->numPersistentEntries;
    }
}

ObjectContext* spawn_persistent_ctx = NULL;
RECOMP_HOOK("Object_SpawnPersistent") void on_spawn_persistent(ObjectContext* objectCtx, s16 id) {
    // recomp_printf("Object_SpawnPersistent id %04X\n", id);
    spawn_persistent_ctx = objectCtx;
}

RECOMP_HOOK_RETURN("Object_SpawnPersistent") void after_spawn_persistent() {
    // recomp_printf("return Object_SpawnPersistent\n");
    propagate_persistent_slots(spawn_persistent_ctx);
    spawn_persistent_ctx = NULL;
}

void load_slots_impl(ObjectContext* objectCtx, ActorId id) {
    // Copy the slots from this ID into play's object context.
    IdSlots* cur_id_slots = &all_id_slots[id];
    for (int i = 0; i < OBJECT_SLOT_COUNT; i++) {
        objectCtx->slots[i].id = cur_id_slots->ids[i];
        objectCtx->slots[i].segment = cur_id_slots->objects[i];
    }
    objectCtx->numEntries = cur_id_slots->numEntries;
}

void load_slots(PlayState* play, ActorId id) {
    if (id < ACTOR_ID_MAX) {
        // recomp_printf("Loading slots for ID 0x%04X\n", id);

        // If there is no alternate slot set already in use, save the current object context slots as the global set.
        if (slot_load_stack_depth == 0) {
            global_slots.numEntries = play->objectCtx.numEntries;
            global_slots.numPersistentEntries = play->objectCtx.numPersistentEntries;
            global_slots.mainKeepSlot = play->objectCtx.mainKeepSlot;
            global_slots.subKeepSlot = play->objectCtx.subKeepSlot;
            for (int i = 0; i < OBJECT_SLOT_COUNT; i++) {
                global_slots.ids[i] = play->objectCtx.slots[i].id;
                global_slots.objects[i] = play->objectCtx.slots[i].segment;
                global_slots.dmaReqs[i] = play->objectCtx.slots[i].dmaReq;
            }
        }

        // Load the slot set for the given actor ID.
        load_slots_impl(&play->objectCtx, id);

        // Increment the slot stack depth.
        slot_load_id_stack[slot_load_stack_depth] = id;
        slot_load_stack_depth++;
    }
}

void unload_slots(PlayState* play, ActorId id) {
    if (id < ACTOR_ID_MAX) {
        // recomp_printf("Unloading slots for ID 0x%04X\n", id);

        // Copy the slots from play's object context back into this ID's slots.
        IdSlots* cur_id_slots = &all_id_slots[id];
        for (int i = 0; i < OBJECT_SLOT_COUNT; i++) {
            cur_id_slots->ids[i] = play->objectCtx.slots[i].id;
            cur_id_slots->objects[i] = play->objectCtx.slots[i].segment;
        }
        cur_id_slots->numEntries = play->objectCtx.numEntries;

        // If this is the parent-most actor in the chain, reload the global slot set into the object context.
        if (slot_load_stack_depth == 1) {
            // Restore the global context.
            play->objectCtx.numEntries = global_slots.numEntries;
            play->objectCtx.numPersistentEntries = global_slots.numPersistentEntries;
            play->objectCtx.mainKeepSlot = global_slots.mainKeepSlot;
            play->objectCtx.subKeepSlot = global_slots.subKeepSlot;
            for (int i = 0; i < OBJECT_SLOT_COUNT; i++) {
                play->objectCtx.slots[i].id = global_slots.ids[i];
                play->objectCtx.slots[i].segment = global_slots.objects[i];
                play->objectCtx.slots[i].dmaReq = global_slots.dmaReqs[i];
            }
        }
        // Otherwise, load the parent actor's slot set.
        else {
            load_slots_impl(&play->objectCtx, slot_load_id_stack[slot_load_stack_depth - 1]);
        }

        slot_load_stack_depth--;
    }
}

typedef struct {
    /* 0x00 */ PlayState* play;
    /* 0x04 */ Actor* actor;
    /* 0x08 */ u32 freezeExceptionFlag;
    /* 0x0C */ u32 canFreezeCategory;
    /* 0x10 */ Actor* talkActor;
    /* 0x14 */ Player* player;
    /* 0x18 */ u32 updateActorFlagsMask; // Actor will update only if at least 1 actor flag is set in this bitmask
} UpdateActor_Params;

bool auto_slot_loading_enabled = false;
PlayState* current_spawn_play = NULL;
ActorId current_spawn_id = ACTOR_ID_MAX;
RECOMP_HOOK("Actor_SpawnAsChildAndCutscene") void on_spawn(ActorContext* actorCtx, PlayState* play, s16 index, f32 x, f32 y, f32 z, s16 rotX,
                                     s16 rotY, s16 rotZ, s32 params, u32 csId, u32 halfDaysBits, Actor* parent)
{
    current_spawn_id = index;
    current_spawn_play = play;
    auto_slot_loading_enabled = true;
    load_slots(current_spawn_play, current_spawn_id);
}

RECOMP_HOOK_RETURN("Actor_SpawnAsChildAndCutscene") void after_spawn() {
    unload_slots(current_spawn_play, current_spawn_id);
    current_spawn_id = ACTOR_ID_MAX;
    current_spawn_play = NULL;
    auto_slot_loading_enabled = false;
}

PlayState* current_draw_play = NULL;
ActorId current_draw_id = ACTOR_ID_MAX;
RECOMP_HOOK("Actor_Draw") void on_draw(PlayState* play, Actor* actor) {
    current_draw_id = actor->id;
    current_draw_play = play;
    auto_slot_loading_enabled = true;
    load_slots(current_draw_play, current_draw_id);
}

RECOMP_HOOK_RETURN("Actor_Draw") void after_draw() {
    unload_slots(current_draw_play, current_draw_id);
    current_draw_id = ACTOR_ID_MAX;
    current_draw_play = NULL;
    auto_slot_loading_enabled = false;
}

PlayState* current_update_play = NULL;
ActorId current_update_id = ACTOR_ID_MAX;
RECOMP_HOOK("Actor_UpdateActor") void on_update(UpdateActor_Params* params) {
    PlayState* play = params->play;
    Actor* actor = params->actor;

    current_update_id = actor->id;
    current_update_play = play;
    auto_slot_loading_enabled = true;
    load_slots(current_update_play, current_update_id);
}

RECOMP_HOOK_RETURN("Actor_UpdateActor") void after_update() {
    unload_slots(current_update_play, current_update_id);
    current_update_id = ACTOR_ID_MAX;
    current_update_play = NULL;
    auto_slot_loading_enabled = false;
}

void print_context(ObjectContext* objectCtx) {
    recomp_printf("object context (%d entries, %d persistent)\n", objectCtx->numEntries, objectCtx->numPersistentEntries);
    for (int i = 0; i < OBJECT_SLOT_COUNT; i++) {
        recomp_printf("  id %04X, seg %08X\n", objectCtx->slots[i].id, (u32)objectCtx->slots[i].segment);
    }
}

// Patched to load objects if the slot wasn't found and a free space exists.
RECOMP_PATCH s32 Object_GetSlot(ObjectContext* objectCtx, s16 objectId) {
    s32 i;
    // recomp_printf("Getting slot for object 0x%04X\n", objectId);

    for (i = 0; i < objectCtx->numEntries; i++) {
        if (ABS_ALT(objectCtx->slots[i].id) == objectId) {
            // recomp_printf("  Found in slot %d\n", i);
            return i;
        }
    }

    // @mod Search for an empty slot and load the object if auto slot loading is currently enabled.
    if (auto_slot_loading_enabled) {
        if (objectCtx->numEntries < OBJECT_SLOT_COUNT) {
            int slot = objectCtx->numEntries;
            recomp_printf("Auto loading object 0x%04X into slot %d\n", objectId, i);
            objectCtx->numEntries++;
            objectCtx->slots[slot].id = objectId;
            objectCtx->slots[slot].segment = GlobalObjects_getGlobalObject(objectId);
            // print_context(objectCtx);
            return slot;
        }
    }

    // recomp_printf("  Not found\n");
    return OBJECT_SLOT_NONE;
}

// Patched to immediately load objects using global objects instead of deferring them to a later point.
RECOMP_PATCH void* func_8012F73C(ObjectContext* objectCtx, s32 slot, s16 id) {
    objectCtx->slots[slot].id = id;
    objectCtx->slots[slot].dmaReq.vromAddr = 0;
    objectCtx->slots[slot].segment = GlobalObjects_getGlobalObject(id);

    return NULL;
}

// Spawn redead at link if pressing L.
RECOMP_HOOK("Player_Update") void on_player_update(Actor* thisx, PlayState* play) {
    Player* player = GET_PLAYER(play);
    if (player == NULL) {
        return;
    }

    if (play->state.input[0].press.button & L_TRIG) {
        for (int i = 0; i < 8; i++) {
            u32 angle = (0x10000U / 8) * i;
            const float spawnDist = 20.0f;
            float offsetX = Math_SinS(angle) * spawnDist;
            float offsetZ = Math_CosS(angle) * spawnDist;
            Actor_Spawn(&play->actorCtx, play, ACTOR_EN_RD, player->actor.world.pos.x + offsetX, player->actor.world.pos.y, player->actor.world.pos.z + offsetZ,
                0, angle + 0x8000, 0,
                0x7F07);
        }
    }
}
