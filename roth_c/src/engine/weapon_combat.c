/* lift_weapon_combat.c — verified-C lift of the ROTH `weapon_combat` subsystem.
 *
 * Lift lens: docs/reference/lift/weapon_combat.md.
 * Per-subsystem TU (recomp.md §4.6): includes common.h, defines the lifts declared in engine.h.
 *
 * Cluster order (bottom-up, increasing cross-subsystem coupling):
 *   C. damage to player    — pure HP arithmetic (oracle).
 *   B. projectiles         — spawn/resolve/hit; entity-pool callees bridged.
 *   D. enemy attack        — flow_succ pair from the entity_ai think loop.
 *   A. weapon equip + fire — attrs parse + equip/ammo cycle + fire entry.
 *   E. weapon HUD/viewmodel — DAS-backed renderers; in-game.
 *
 * 4 functions are already lifted in renderer.c (reached via engine.h): arm_weapon_fire 0x17629,
 * compute_projectile_hit_damage 0x427f3, apply_damage_to_player 0x32023, setup_player_viewmodel_sprite 0x29f50.
 */
#include <stdint.h>
#include <string.h>
#include "common.h"

/* ======================================================================================
 * C — Damage to player (pure HP arithmetic; oracle).
 * ====================================================================================== */

/* apply_direct_damage_to_player (0x32058): EAX = damage. The shared flow_succ tail also entered by
 * apply_damage_to_player 0x32023 (after its scaling) and apply_reduced_damage_to_player 0x320c6.
 * Sets the "took damage this frame" flag [0x83e7c]=1, bumps the pain-flash accumulator [0x89f3b]
 * ((dmg+0x20)/2, clamped 0x164, only while accumulator <= dmg+0x20 < 0x35c6), then subtracts the
 * raw damage from health [0x8a0f0] (clamped at 0). Void (global write-set). */
void apply_direct_damage_to_player(uint32_t eax)
{
    uint32_t dmg = eax;
    G32(VA_g_active_weapon_ammo_cap + 0x8) = 1;
    eax = dmg + 0x20;
    if ((int32_t)eax < 0x35c6 && (int32_t)eax >= (int32_t)G32(VA_g_damage_flash_level)) {
        eax >>= 1;
        uint32_t pain = (uint32_t)G32(VA_g_damage_flash_level) + eax;
        G32(VA_g_damage_flash_level) = (int32_t)pain;
        if (pain >= 0x164) G32(VA_g_damage_flash_level) = 0x164;
    }
    uint32_t h = (uint32_t)G32(VA_g_player_health) - dmg;
    G32(VA_g_player_health) = (int32_t)h;
    if ((int32_t)h < 0) G32(VA_g_player_health) = 0;
}

/* apply_reduced_damage_to_player (0x320c6): EAX = damage. For the normal (small) damage range, reduces
 * damage by the fraction [0x81e30]/2048 then applies it directly only if the reduced value is still > 0.
 * Huge damage (>= 0x35c6) bypasses the reduction and applies directly. (The original's `jae 0x32058`
 * path leaves an unbalanced `push edx` on the stack — a latent bug only reachable at damage >= 13766,
 * which never happens with real damage values; the logical behavior is reproduced faithfully.) Void. */
void apply_reduced_damage_to_player(uint32_t eax)
{
    if (eax >= 0x35c6u) {                                  /* cmp eax,0x35c6 ; jae 0x32058 */
        apply_direct_damage_to_player(eax);
        return;
    }
    uint32_t edx = (uint32_t)((int32_t)eax * (int32_t)G32(VA_g_value_reduction_factor));  /* imul edx,[0x81e30] */
    edx >>= 0xb;                                           /* shr edx,0xb */
    eax -= edx;                                            /* sub eax,edx */
    if ((int32_t)eax > 0)                                  /* or eax,eax ; jg 0x32058 */
        apply_direct_damage_to_player(eax);
}

/* damage_player_from_emitter (0x34579): walk the active damage-emitter list rooted at [0x8a120]
 * (esi = [[0x8a120]]; advance esi = [esi] until 0). For each emitter node not already flagged
 * (record [edi+2] bit3 == 0; edi = node[+8] = emitter def record), iterate its coord list
 * (count = node[+0xc] & 0xffff at node+0x10..) computing the distance to the player via
 * point_to_wall_distance_sq 0x3e03f (direct C; pure, no writes). On the first
 * coord within range (dist <= 0x7ff00): apply the record's damage byte [edi+7] to the player
 * (apply_damage_to_player with DL=0xff scale-flag, ECX=0) and, if [edi+6] bit4 set, latch the
 * node's "already damaged" bit ([edi+2] |= 8); then stop scanning that node. Void (global write-set
 * + the latched node flag). */
void damage_player_from_emitter(void)
{
    uint32_t esi = (uint32_t)G32(VA_g_damage_emitter_ptr);
    for (;;) {
        esi = *(volatile uint32_t *)(uintptr_t)esi;                 /* 0x34585: mov esi,[esi] (loop top) */
        uint32_t edi = *(volatile uint32_t *)(uintptr_t)(esi + 8);  /* edi = node[+8] = emitter record */
        if (!(*(volatile uint8_t *)(uintptr_t)(edi + 2) & 8)) {     /* test byte[edi+2],8 ; jne next */
            uint32_t ebx = esi + 0xc;
            int32_t ecx = (int32_t)((uint32_t)(*(volatile uint32_t *)(uintptr_t)ebx) & 0xffff);
            ebx += 4;
            for (;;) {                                              /* inner coord loop 0x3459e */
                uint32_t eax = *(volatile uint16_t *)(uintptr_t)ebx;          /* X coord = wall geom offset */
                uint32_t edx = *(volatile uint16_t *)(uintptr_t)(edi + 0xa);  /* radius word */
                edx = edx + edx + 0x1c;                             /* lea edx,[edx+edx+0x1c] */
                /* 0x3e03f re-pointed: real leaf. It reads the wall record (eax = geom offset) via
                 * the geom base [0x90aa8] + the vertex coords via [0x90aac], returning dist²-to-player (or
                 * the 0x7ffff AABB sentinel). test_weapon_combat stages the geom/vertex buffers + selectors
                 * and places the player for the intended hit/miss. */
                eax = point_to_wall_distance_sq(eax, (uint16_t)edx);
                if (eax <= 0x7ff00u) {                              /* cmp eax,0x7ff00 ; ja skip */
                    uint32_t dmg = *(volatile uint8_t *)(uintptr_t)(edi + 7);  /* damage byte */
                    apply_damage_to_player(dmg, 0xff, 0);    /* edx=-1 (DL=0xff), ecx=0 */
                    if (*(volatile uint8_t *)(uintptr_t)(edi + 6) & 0x10)      /* test byte[edi+6],0x10 */
                        *(volatile uint8_t *)(uintptr_t)(edi + 2) |= 8;        /* or byte[edi+2],8 */
                    goto done;                                      /* jmp 0x345db */
                }
                ebx += 2;
                if (--ecx <= 0) break;                              /* dec ecx ; jg inner */
            }
        }
        esi = *(volatile uint32_t *)(uintptr_t)esi;                 /* 0x345d5: mov esi,[esi] (advance) */
        if (esi == 0) break;                                        /* or esi,esi ; jne 0x34585 */
    }
done: ;
}

/* ======================================================================================
 * B — Projectiles (spawn / resolve / hit). Entity-pool + audio callees bridged.
 * ====================================================================================== */

/* check_projectile_sector_clearance (0x42c3c): EAX = sector byte-offset, EDI = projectile record.
 * Sibling of check_entity_sector_clearance 0x42c04. Sector record = EAX + geom base [0x90aa8];
 * ecx = [0x90fdc] >> 1 (a half-extent). Returns CF (0=clear / 1=blocked, via the shared `stc` epilogue
 * at 0x42c69): blocked if (int16)(sec[+2] + ecx) > (int16)rec[+0xa], OR (int16)(sec[+0] - ecx) <
 * (int16)rec[+0xa]. All arithmetic is 16-bit (dx); only dx is ever compared. */
int check_projectile_sector_clearance(uint32_t eax, uint32_t edi)
{
    uint32_t ecx = (uint32_t)G32(VA_g_projectile_collision_width) >> 1;
    uint32_t sec = eax + (uint32_t)G32(VA_g_map_geometry_buffer);
    int16_t ent_h = *(int16_t *)(uintptr_t)(edi + 0xa);
    int16_t hi = (int16_t)(uint16_t)((uint16_t)(*(uint16_t *)(uintptr_t)(sec + 2)) + (uint16_t)ecx);
    if (hi > ent_h) return 1;                                /* jg -> stc */
    int16_t lo = (int16_t)(uint16_t)((uint16_t)(*(uint16_t *)(uintptr_t)sec) - (uint16_t)ecx);
    if (lo < ent_h) return 1;                                /* jl -> stc */
    return 0;                                                /* clc -> clear */
}

/* resolve_projectile_target_entity (0x4271c): EAX = projectile slot ptr. edi=eax; esi=[edi]; ebx=[esi]
 * (the entity's current def ptr). If ebx != 0: refresh the def (revalidate_entity_def 0x426fc; direct-C,
 * EBX-out reconstructed via *esi); on CF clear (always — revalidate ends `clc`) play the entity's
 * distance-variant impact sound (play_distance_variant_sound 0x4269b; audio bridge KEPT, eax=[edi+4]).
 * EAX in is preserved (push/pop). Void from the caller's view. */
void resolve_projectile_target_entity(uint32_t eax, uint32_t d2)
{
    uint32_t edi = eax;
    uint32_t esi = *(volatile uint32_t *)(uintptr_t)edi;
    uint32_t ebx = *(volatile uint32_t *)(uintptr_t)esi;
    if (ebx == 0) return;                                    /* or ebx,ebx ; je skip */
    /* re-pointed: revalidate_entity_def 0x426fc [L, entity_ai] direct-C. Disasm 0x42710/0x42716: on the
     * clc path the orig leaves the refreshed def in EBX == *esi (it `mov [esi],eax` then `mov ebx,eax`;
     * the equal path leaves both unchanged) — the same *esi read-back idiom used at
     * begin_enemy_attack/launch_enemy_attack_animation. */
    if (revalidate_entity_def((uint8_t *)(uintptr_t)esi, (uint8_t *)(uintptr_t)ebx))
        return;                                              /* jb skip (revalidate returned stc/miss) */
    ebx = *(volatile uint32_t *)(uintptr_t)esi;              /* refreshed def == orig EBX-out */
    /* Direct C: play_distance_variant_sound 0x4269b. Its d2 (=ECX at the call) is the DISTANCE
     * resolve INHERITS in ECX from its single caller (0x42a5f). Disasm proof that d2 is
     * reconstructible (the prior "can't reconstruct" keep was WRONG):
     *   - resolve (0x4271c) never touches ECX before `call 0x4269b` (disasm), so ECX there == resolve's
     *     entry ECX;
     *   - the only intervening call, revalidate_entity_def 0x426fc, PRESERVES ECX on both exit paths:
     *     the equal path takes no call; the mismatch path calls entity_def_cache_lookup 0x1e2f6 which is
     *     `push ecx ... pop ecx; ret` (verified) — so ECX survives;
     *   - resolve's caller sets ECX = (base+part)>>1 = `dmg` at 0x42a51-0x42a59 (imul/shr/add/sar), which
     *     the lifted C caller (lift_entity_ai.c ~1001) ALREADY computes as `dmg`.
     * So d2 == dmg, threaded here as the new proto arg. The callee reads EAX=[edi+4], EBX=def, ECX=d2
     * (EDX is overwritten by [ebx+0x22] before use — not an input); it is oracle-verified standalone in
     * test_audio (w_pdvs). NB this also FIXES a latent bug: the old bridge passed ecx=0, silently dropping
     * distance-variant sound selection. In-game: verify enemy-projectile impact sounds (ROTH_LIFT=weapon_combat). */
    play_distance_variant_sound(*(volatile uint32_t *)(uintptr_t)(edi + 4), ebx, d2);
}

/* init_projectile_from_item (0x42b8c): ESI = the spawning item record (edi = [esi] = the projectile
 * entity). Builds the entity's item descriptor into a 0x40-byte stack local via init_inventory_item_object
 * 0x18598 (inventory bridge, EAX=item-id [esi+0x14], EDX=&local). On success: optionally play the entity
 * spawn sound (local[+2]!=0 -> play_entity_sound 0x271c4, audio bridge), then if the resolved def
 * local[+0]!=0 commit it to the entity (def [edi+4]=local[0], mark [edi+7]|=0x40, set item phase
 * [esi+8]=2, raise the global action timer [0x853f6] to local[9] if larger, clear rec [esi+0x1a]=0).
 * Returns the CF: 0 (clc, 0x42bf5) on the committed path; **1 (STC, 0x42bfb) on ALL THREE bail
 * paths** — the tick caller destroys the projectile on CF=1. (in-game fix: the bails
 * returned 0, so dead projectiles stayed in flight re-hitting the player every pass — the
 * entity_ai one-shot/multi-sound bug; also [esi+0x1a], not [edi+0x1a], per disasm 0x42bec.)
 * Oracle: stub both bridges (controlled local + return). */
int init_projectile_from_item(uint32_t esi)
{
    uint32_t edi = *(volatile uint32_t *)(uintptr_t)esi;
    uint16_t itemid = *(volatile uint16_t *)(uintptr_t)(esi + 0x14);
    if (itemid == 0) return 1;                               /* je 0x42bfb (stc) */
    uint8_t local[0x40];
    /* re-pointed: init_inventory_item_object 0x18598 [L, inventory] direct-C (EAX=item-id, EDX=&local
     * -> EAX=template id, fills the object). t_init_projectile_from_item stages a real dbase100
     * template record so the callee (a pure trigger-block table walk) runs REAL + symmetric.
     * NB target 0x18598 also bridges from spawn_projectile_from_aim (0x424xx) which reads its
     * EBX/EBP outputs (skip-list), so the target only fully retires when that site converts too. */
    uint32_t iio_ret = init_inventory_item_object(itemid, (uint32_t)(uintptr_t)local);
    if (iio_ret == 0) return 1;                              /* je 0x42bfb (stc) */
    uint16_t snd = *(uint16_t *)(local + 2);
    if (snd != 0) {                                          /* mov ax,[esp+2]; je skip-sound */
        /* re-pointed: play_entity_sound 0x271c4 [L, audio] direct-C (id,param,bx,cx = watcall) */
        play_entity_sound((uint32_t)(snd - 1), 0,
                                 *(volatile uint16_t *)(uintptr_t)edi,        /* bx=[edi] */
                                 *(volatile uint16_t *)(uintptr_t)(edi + 2)); /* cx=[edi+2] */
    }
    uint16_t def = *(uint16_t *)(local + 0);
    if (def == 0) return 1;                                  /* mov dx,[esp]; je 0x42bfb (stc) */
    *(volatile uint16_t *)(uintptr_t)(edi + 4) = def;        /* entity def word */
    *(volatile uint8_t  *)(uintptr_t)(edi + 7) |= 0x40;      /* mark projectile */
    *(volatile uint8_t  *)(uintptr_t)(esi + 8) = 2;          /* item phase */
    uint8_t al = local[9];
    if (al > (uint8_t)G8(VA_g_render_sector_walk_mode + 0x23)) G8(VA_g_render_sector_walk_mode + 0x23) = al;         /* raise the action timer (unsigned) */
    *(volatile uint16_t *)(uintptr_t)(esi + 0x1a) = 0;       /* rec+0x1a (0x42bec — was edi+0x1a) */
    return 0;                                                /* clc */
}

/* ======================================================================================
 * D — Enemy attack (flow_succ pair from the entity_ai think loop; shared tail at 0x43500).
 * ====================================================================================== */

/* enemy_attack_apply (0x43500..0x43557): the shared tail of begin_enemy_attack + launch_enemy_attack_animation.
 * EDI = enemy entity, EBX = its (revalidated) def, EDX = primary/secondary selector (bit0). Picks the
 * primary (def[+6] frame, def[+0x10] anim, def[+0x14] sound) or secondary (def[+7]/[+0x12]/[+0x16], def
 * advanced +2 + flag [edi+8]|=1) attack, writes the chosen anim word to [edi+0x1c] and frame+1 to [edi+0x1e],
 * plays the attack sound (play_entity_sound 0x271c4; audio bridge) when both anim+sound words are nonzero,
 * then marks the entity attacking ([edi+8]|=8, [edi+0x1a]=0). */
static void enemy_attack_apply(uint32_t edi, uint32_t ebx, uint32_t edx)
{
    uint8_t al;
    if (edx & 1) {                                           /* test dl,1 ; je -> primary (ZF when dl&1==0) */
        *(volatile uint8_t *)(uintptr_t)(edi + 8) |= 1;      /* secondary: or byte[edi+8],1 */
        al = *(volatile uint8_t *)(uintptr_t)(ebx + 7);      /* al = def[+7] */
        ebx += 2;                                            /* add ebx,2 */
    } else {
        al = *(volatile uint8_t *)(uintptr_t)(ebx + 6);      /* primary: al = def[+6] */
    }
    al = (uint8_t)(al + 1);                                  /* inc al */
    *(volatile uint8_t *)(uintptr_t)(edi + 0x1e) = al;
    uint16_t w10 = *(volatile uint16_t *)(uintptr_t)(ebx + 0x10);
    if (w10 != 0) {                                          /* je 0x4354d */
        *(volatile uint16_t *)(uintptr_t)(edi + 0x1c) = w10;
        uint16_t w14 = *(volatile uint16_t *)(uintptr_t)(ebx + 0x14);
        if (w14 != 0) {                                      /* je 0x4354d */
            uint32_t esi2 = *(volatile uint32_t *)(uintptr_t)(edi + 4);
            /* re-pointed: play_entity_sound 0x271c4 [L, audio] direct-C */
            play_entity_sound((uint32_t)(w14 - 1), 0,
                                     *(volatile uint16_t *)(uintptr_t)esi2,        /* bx=[esi] */
                                     *(volatile uint16_t *)(uintptr_t)(esi2 + 2)); /* cx=[esi+2] */
        }
    }
    *(volatile uint8_t *)(uintptr_t)(edi + 8) |= 8;          /* or byte[edi+8],8 */
    *(volatile uint16_t *)(uintptr_t)(edi + 0x1a) = 0;
}

/* begin_enemy_attack (0x434d8): EDI = enemy entity. esi=[edi], ebx=[esi]=def. Refresh the def
 * (revalidate_entity_def 0x426fc; bridge, threads ebx/CF — always clc). Clears [edi+8] low 2 bits,
 * then selects the attack: forced primary if no secondary attack (def[+0x12]==0), forced secondary if
 * [edi+0xa] bit1 set, else a coin flip on the LCG global [0x72730] (bit0); hands off to enemy_attack_apply. */
void begin_enemy_attack(uint32_t edi)
{
    uint32_t esi = *(volatile uint32_t *)(uintptr_t)edi;
    uint32_t ebx = *(volatile uint32_t *)(uintptr_t)esi;
    if (ebx == 0) return;                                    /* or ebx,ebx ; je */
    /* re-pointed: revalidate_entity_def 0x426fc [L, entity_ai] direct-C. On the clc path the orig
     * leaves the refreshed def in EBX; per disasm 0x42710/0x42716 that value == *esi (it stores eax
     * to [esi] then mov ebx,eax), so read it back from *esi (same idiom as lift_entity_ai.c:87). */
    if (revalidate_entity_def((uint8_t *)(uintptr_t)esi, (uint8_t *)(uintptr_t)ebx))
        return;                                              /* jb (never; revalidate ends clc) */
    ebx = *(volatile uint32_t *)(uintptr_t)esi;              /* refreshed def == orig EBX-out */
    *(volatile uint8_t *)(uintptr_t)(edi + 8) &= 0xfc;       /* and byte[edi+8],0xfc */
    uint32_t edx;
    if (*(volatile uint16_t *)(uintptr_t)(ebx + 0x12) == 0)  /* no secondary -> primary */
        edx = 0;
    else if (*(volatile uint8_t *)(uintptr_t)(edi + 0xa) & 2)/* forced secondary */
        edx = 1;
    else
        edx = (uint32_t)G32(VA_g_ai_wander_rng);                        /* coin flip on LCG bit0 */
    enemy_attack_apply(edi, ebx, edx);
}

/* launch_enemy_attack_animation (0x4347e): EDI = enemy entity. First advances the LCG global
 * [0x72730] (*0x5e5 + 0x29). esi=[edi], ebx=[esi]=def; refresh def (revalidate bridge). Chooses
 * primary vs secondary from the entity's attack-mode bits [edi+9]&6 (==6 -> high-roll [0x72731]>0x80
 * picks primary; ==2 -> primary; else secondary), gated on the chosen attack's anim-count being
 * nonzero (primary def[+0x10], secondary def[+0x12]); hands off to enemy_attack_apply. */
void launch_enemy_attack_animation(uint32_t edi)
{
    uint32_t edx = (uint32_t)G32(VA_g_ai_wander_rng);
    edx = edx * 0x5e5u + 0x29u;                              /* imul edx,edx,0x5e5 ; add edx,0x29 */
    G32(VA_g_ai_wander_rng) = (int32_t)edx;
    uint32_t esi = *(volatile uint32_t *)(uintptr_t)edi;
    uint32_t ebx = *(volatile uint32_t *)(uintptr_t)esi;
    if (ebx == 0) return;                                    /* je 0x434d5 */
    /* re-pointed: revalidate_entity_def 0x426fc [L] direct-C; EBX-out == *esi on clc (see above). */
    if (revalidate_entity_def((uint8_t *)(uintptr_t)esi, (uint8_t *)(uintptr_t)ebx))
        return;                                              /* jb */
    ebx = *(volatile uint32_t *)(uintptr_t)esi;
    uint8_t al = (uint8_t)(*(volatile uint8_t *)(uintptr_t)(edi + 9) & 6);
    int primary;
    if (al == 6)
        primary = ((uint8_t)G8(VA_g_ai_wander_rng + 0x1) > 0x80);             /* ja 0x434ca */
    else
        primary = (al == 2);                                 /* je 0x434ca ; else secondary */
    if (primary) {                                           /* 0x434ca */
        if (*(volatile uint16_t *)(uintptr_t)(ebx + 0x10) == 0) return;
        enemy_attack_apply(edi, ebx, 0);
    } else {                                                 /* 0x434bc */
        if (*(volatile uint16_t *)(uintptr_t)(ebx + 0x12) == 0) return;
        enemy_attack_apply(edi, ebx, 1);
    }
}

/* ======================================================================================
 * A — Weapon equip + fire trigger.
 * ====================================================================================== */

/* reset_weapon_fire_timing (0x1765c): alt-entry of arm_weapon_fire/reset_transition_timer 0x17629 that
 * jumps PAST the [0x7fddc] guard straight to 0x1763c -> UNCONDITIONALLY clears [0x7fdf0], sets the
 * cooldown [0x7fde0]=0x64 + the weapon anim-state [0x7fddc]=2. */
void reset_weapon_fire_timing(void)
{
    G32(VA_g_weapon_fire_lock + 0x20) = 0;
    G32(VA_g_weapon_fire_lock + 0x10) = 0x64;
    G32(VA_g_weapon_fire_lock + 0xc) = 2;
}

/* arm_weapon_and_cache_def (0x17668): EAX = weapon def ptr. Runs arm_weapon_fire (0x17629 =
 * reset_transition_timer [L]: clears [0x7fdf0], conditionally arms cooldown/state), then caches
 * three def fields into the active-fire globals: [0x7fdf8]=def[+0], [0x7fde8]=def[+0x20], [0x7fdec]=def[+0x1c]. */
void arm_weapon_and_cache_def(uint32_t eax)
{
    uint32_t edx = eax;
    arm_weapon_fire();                         /* call 0x17629 (arm_weapon_fire) */
    G32(VA_g_pending_weapon_def + 0x4) = *(volatile int32_t *)(uintptr_t)(edx);
    G32(VA_g_weapon_fire_lock + 0x18) = *(volatile int32_t *)(uintptr_t)(edx + 0x20);
    G32(VA_g_weapon_fire_lock + 0x1c) = *(volatile int32_t *)(uintptr_t)(edx + 0x1c);
}

/* apply_weapon_action_attributes (0x18260, 587B PURE parser): EAX = dbase100 record index, EDX = dest
 * WeaponAttributes base (g_active_weapon_attrs 0x811b4; stride 0x50), EBX = max slots. Zeroes the first
 * 0x50-byte slot (mem_fill [L]), sets [dest+0xc]=1 + [dest+0x38]=index. Resolves the record via the
 * dbase100 index table [0x81e20] + base [0x81e1c]; requires record flag [rec+4]&0x10. Walks the
 * trigger-block list at rec+0x14: each block header dword = {size:24, code:8}; size==0 ends (return slot
 * count); code!=5 skips (advance by size rounded down to 4); code==5 (WeaponAction) parses its
 * (size/4 - 1) sub-record dwords ({value:24, opcode:8 incl 0x80 sign flag}) into the current slot's
 * fields via the opcode map below, then advances to the next slot. Returns the slot count (0 on failure).
 * esi/ebx are value accumulators flushed by the "group" opcodes 0x12/0x13/0x1f; latch (init 8) is set
 * by 0x2c and consumed by 0x12/0x1f. Only the FIRST slot is zeroed/inited (faithful). */
uint32_t apply_weapon_action_attributes(uint32_t eax, uint32_t edx, uint32_t ebx)
{
    uint32_t idx = eax;
    uint32_t ecx = edx;                                      /* current dest slot ptr */
    uint32_t limit = ebx;
    uint32_t slot = 0;
    mem_fill((void *)(uintptr_t)ecx, 0, 0x50);        /* zero first slot */
    *(volatile int32_t *)(uintptr_t)(ecx + 0xc) = 1;
    if ((int32_t)idx <= 0) return 0;                         /* test esi,esi; jg -> else return 0 */
    *(volatile int32_t *)(uintptr_t)(ecx + 0x38) = (int32_t)idx;
    uint32_t rec_off = *(volatile uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_inventory_table) + idx * 4);
    if (rec_off == 0) return 0;
    uint32_t recptr = (uint32_t)G32(VA_g_dbase100_base) + rec_off;
    if (!(*(volatile uint8_t *)(uintptr_t)(recptr + 4) & 0x10)) return 0;
    uint32_t blockptr = recptr + 0x14;
    for (;;) {                                               /* outer block loop (0x182ba) */
        uint32_t header = *(volatile uint32_t *)(uintptr_t)blockptr;
        uint32_t blocksize = header & 0xffffff;
        if (blocksize == 0) return slot;                     /* size==0 -> return slot count */
        if ((header >> 0x18) != 5) {                         /* code != 5 -> skip block */
            blockptr += (blocksize >> 2) << 2;
            continue;
        }
        uint32_t subptr = blockptr + 4;                      /* code==5: parse sub-records */
        uint32_t latch = 8;
        uint32_t acc_esi = 0, acc_ebx = 0;
        uint32_t subcount = (blocksize >> 2) - 1;
        for (uint32_t subi = 0; subi < subcount; subi++) {
            uint32_t dw = *(volatile uint32_t *)(uintptr_t)subptr;
            subptr += 4;
            uint32_t op_hi = dw >> 0x18;                      /* full high byte (incl 0x80 sign) */
            uint32_t op = op_hi & 0x7f;
            uint32_t val = dw & 0xffffff;
            switch (op) {
            case 0x12:                                       /* group commit A */
                *(volatile int32_t *)(uintptr_t)(ecx + 0x00) = (int32_t)val;
                *(volatile int32_t *)(uintptr_t)(ecx + 0x20) = (int32_t)acc_esi;
                *(volatile int32_t *)(uintptr_t)(ecx + 0x1c) = (int32_t)acc_ebx;
                *(volatile int32_t *)(uintptr_t)(ecx + 0x3c) = (int32_t)latch;
                acc_esi = 0; acc_ebx = 0; latch = 0;
                break;
            case 0x13:                                       /* group commit B */
                *(volatile int32_t *)(uintptr_t)(ecx + 0x08) = (int32_t)val;
                *(volatile int32_t *)(uintptr_t)(ecx + 0x18) = (int32_t)acc_esi;
                *(volatile int32_t *)(uintptr_t)(ecx + 0x14) = (int32_t)acc_ebx;
                acc_esi = 0; acc_ebx = 0; latch = 0;
                break;
            case 0x14:                                       /* signed scalar -> [+4] */
                *(volatile int32_t *)(uintptr_t)(ecx + 0x04) =
                    (op_hi & 0x80) ? -(int32_t)val : (int32_t)val;
                break;
            case 0x18: *(volatile int32_t *)(uintptr_t)(ecx + 0x0c) = (int32_t)val; break;
            case 0x19: *(volatile int32_t *)(uintptr_t)(ecx + 0x10) = (int32_t)val; break;
            case 0x1e: acc_ebx = (op_hi & 0x80) ? (uint32_t)(-(int32_t)val) : val; break;
            case 0x1f:                                       /* group commit C */
                *(volatile int32_t *)(uintptr_t)(ecx + 0x2c) = (int32_t)val;
                *(volatile int32_t *)(uintptr_t)(ecx + 0x28) = (int32_t)acc_esi;
                *(volatile int32_t *)(uintptr_t)(ecx + 0x24) = (int32_t)acc_ebx;
                *(volatile int32_t *)(uintptr_t)(ecx + 0x40) = (int32_t)latch;
                acc_esi = 0; acc_ebx = 0; latch = 0;
                break;
            case 0x20: *(volatile int32_t *)(uintptr_t)(ecx + 0x30) = (int32_t)val; break;
            case 0x21: *(volatile int32_t *)(uintptr_t)(ecx + 0x34) = (int32_t)val; break;
            case 0x22: acc_esi = (op_hi & 0x80) ? (uint32_t)(-(int32_t)val) : val; break;
            case 0x2c: latch = val; break;
            case 0x2d:                                       /* flag bits into [+0x4c] */
                if (val == 5)      *(volatile uint8_t *)(uintptr_t)(ecx + 0x4c) |= 1;
                else if (val == 6) *(volatile uint8_t *)(uintptr_t)(ecx + 0x4c) |= 2;
                break;
            case 0x30: *(volatile int32_t *)(uintptr_t)(ecx + 0x44) = (int32_t)val; break;
            case 0x33: *(volatile int32_t *)(uintptr_t)(ecx + 0x48) = (int32_t)val; break;
            default: break;                                  /* unknown opcode -> skip */
            }
        }
        slot++;                                              /* next slot (0x18482) */
        ecx += 0x50;
        if ((int32_t)slot >= (int32_t)limit) return slot;    /* slot==limit -> return */
        blockptr += (blocksize >> 2) << 2;                   /* advance to next block (0x18494) */
    }
}

/* activate_weapon_item (0x184ab): EAX = item record ptr, EDX = dbase100 index. Equips a weapon:
 * publishes g_active_weapon_ptr [0x7fe00] = &g_active_weapon_attrs (relocated ptr, GADDR), stores the
 * item record [0x81038], parses its WeaponAction attrs (apply_weapon_action_attributes [L], dest
 * 0x811b4, max 4 slots) -> slot count [0x812f4], records the item id [0x83e70]=word[rec+2]. Ammo model:
 * if attrs byte [0x811e5]&0x80 -> ammo [0x83e6c]=0, cap [0x83e74]=0x100; else cap=1 and ammo = the
 * player's inventory count of ammo-id [0x811e4] (query_player_inventory 0x1ccf7; inventory bridge) or 0.
 * Finally arms + caches the def (arm_weapon_and_cache_def [L]) and bumps the equip counter [0x810c4]. */
void activate_weapon_item(uint32_t eax, uint32_t edx)
{
    uint32_t ecx = eax;                                      /* item record */
    uint32_t idx = edx;                                      /* dbase index (arg2) */
    if (ecx == 0) idx = (uint32_t)G32(VA_g_selected_item_primary + 0x10);              /* test ecx,ecx; jne -> else [0x81054] */
    G32(VA_g_active_weapon_ptr) = (int32_t)GADDR(VA_g_active_weapon_attrs);                  /* g_active_weapon_ptr = &attrs (reloc ptr) */
    G32(VA_g_selected_item_secondary) = (int32_t)ecx;
    uint32_t slots = apply_weapon_action_attributes(idx, (uint32_t)GADDR(VA_g_active_weapon_attrs), 4);
    G32(VA_g_active_weapon_ammo_id + 0x110) = (int32_t)slots;
    /* rec==0 is REAL in-game: fists/unarmed (key 1) and the savegame-load re-equip both arrive
     * here with a null item record. The original's `movsx word[rec+2]` then reads LINEAR 0x2
     * (the IVT under DOS4GW); the trap host services that game-code fault from its zero
     * g_lowmem shadow -> value 0. A host-C deref of 0x2 is a REAL SIGSEGV (gotcha H, DOS
     * null-page tolerance) — reproduce the benign zero read (in-game: the
     * fist-switch + savegame-load crash, first fault addr=0x2). */
    G32(VA_g_active_weapon_item_id) = (ecx < 0x10000u) ? 0
                 : (int32_t)*(volatile int16_t *)(uintptr_t)(ecx + 2);   /* movsx word[rec+2] */
    if (G8(VA_g_active_weapon_ammo_id + 0x1) & 0x80) {
        G32(VA_g_active_weapon_ammo) = 0;
        G32(VA_g_active_weapon_ammo_cap) = 0x100;
    } else {
        G32(VA_g_active_weapon_ammo_cap) = 1;
        if (G32(VA_g_active_weapon_ammo_id) != 0) {
            /* re-pointed: query_player_inventory 0x1ccf7 [L, inventory] direct-C (eax=id, edx=flags) */
            uint32_t cnt = query_player_inventory(
                (uint32_t)(int32_t)*(volatile int16_t *)(uintptr_t)GADDR(VA_g_active_weapon_ammo_id), 0);  /* movsx word[0x811e4] */
            G32(VA_g_active_weapon_ammo) = (int32_t)cnt;
        } else {
            G32(VA_g_active_weapon_ammo) = 0;
        }
    }
    arm_weapon_and_cache_def((uint32_t)GADDR(VA_g_active_weapon_attrs));   /* call 0x17668 (eax=&attrs) */
    G32(VA_g_inspect_popup_state + 0x4) = (int32_t)((uint32_t)G32(VA_g_inspect_popup_state + 0x4) + 1);
}

/* show_no_ammo_message (0x1f8cb): EAX = message index. Resolves the no-ammo text into a 0x200-byte stack
 * buffer (resolve_dbase100_text [L], maxlen 0x1f8); if found (nonzero) lays it out as a timed on-screen
 * message (layout_timed_message_text 0x1f3d3; dialogue_ui bridge; eax/edx = reloc string ptrs, ebx=buf,
 * ecx=[0x85498] screen width, stack arg 0xa) and latches the timed-message globals [0x827e5]=0x46 +
 * [0x827e1]=the layout handle. */
void show_no_ammo_message(uint32_t eax)
{
    uint32_t arg = eax;
    uint8_t local[0x200];
    uint32_t r = resolve_dbase100_text((uint32_t)(uintptr_t)local, 0x1f8, arg, 0);
    if (r == 0) return;                                      /* test eax,eax; je done */
    local[0x190] = 0;                                        /* mov byte[ebp-0x70],0 (within the buffer) */
    /* layout_timed_message_text 0x1f3d3 — lifted (dialogue_ui), called direct (re-pointed from
     * the call_orig bridge): meta=EAX line table, out=EDX work buf, src=EBX, maxw=ECX, maxlines=stack */
    int32_t handle = layout_timed_message_text(
        (int32_t *)GADDR(VA_g_timed_message_lines), (uint8_t *)GADDR(VA_g_timed_message_text_buffer),
        local, G32(VA_g_screen_pitch), 0xa);
    G32(VA_g_timed_message_timer) = 0x46;
    G32(VA_g_timed_message_line_count) = handle;
}

/* rebuild_weapon_inventory_list (0x2245c): void. Scans the player inventory [0x80c30] (4-byte slots,
 * [0x80c2c] used count) and builds the weapon-selection list at 0x83d84 (8-byte entries, max 16). For
 * each non-empty slot (consuming the used count), parses the item's attrs into a 0x50-byte stack buffer
 * (apply_weapon_action_attributes [L], dest=local, max 1 slot); if the item is a weapon (attrs[+0x30]
 * bit15 set) appends an entry { [0]=slot ptr, [4]=word(attrs[+4]), [6]=word(attrs[+0x30]&0x7fff) }.
 * Stores the weapon count at [0x83e04]. Inventory list/dest are relocated pointers (GADDR). */
void rebuild_weapon_inventory_list(void)
{
    uint32_t count = 0;
    uint32_t dest = (uint32_t)GADDR(VA_g_weapon_hud_anim_table);                /* [ebp-8] */
    uint32_t slot = (uint32_t)GADDR(VA_g_inventory_slots);                /* [ebp-0xc] */
    int32_t remaining = G32(VA_g_inventory_count);                        /* [ebp-4] */
    if (remaining == 0) { G32(VA_g_weapon_hud_anim_count) = 0; return; }
    do {
        uint16_t w = *(volatile uint16_t *)(uintptr_t)slot;
        if (w != 0) {                                        /* empty slot -> skip (no dec) */
            remaining--;
            if (count < 0x10) {                              /* list not full */
                uint8_t local[0x50];
                uint32_t itemid = (uint32_t)(int32_t)(int16_t)(uint16_t)(w & 0x7fff);  /* and ah,0x7f; cwde */
                apply_weapon_action_attributes(itemid, (uint32_t)(uintptr_t)local, 1);
                uint32_t a30 = *(uint32_t *)(local + 0x30);
                if (a30 & 0x8000) {                          /* weapon flag */
                    *(volatile uint32_t *)(uintptr_t)(dest + 0) = slot;
                    *(volatile uint16_t *)(uintptr_t)(dest + 4) = (uint16_t)*(uint32_t *)(local + 4);
                    *(volatile uint16_t *)(uintptr_t)(dest + 6) = (uint16_t)(a30 & 0x7fff);
                    count++;
                    dest += 8;
                }
            }
        }
        slot += 4;
    } while (remaining != 0);
    G32(VA_g_weapon_hud_anim_count) = (int32_t)count;
}

/* ======================================================================================
 * E — Weapon HUD + viewmodel renderer (leaves; the big renderers are in-game live-swap).
 * ====================================================================================== */

/* free_hud_weapon_das_handle (0x2268e): void. If the cached HUD weapon DAS handle [0x83d6c] is non-null,
 * free it from the DAS-cache Pool (pool_free_handle 0x360b3; memory_pool bridge — eax=pool [0x85c3c],
 * edx=handle [0x83d6c]); then clear [0x83d6c]. */
void free_hud_weapon_das_handle(void)
{
    if (G32(VA_g_hud_weapon_das_handle) != 0) {
        /* re-pointed: pool_free_handle 0x360b3 [L, memory_pool] direct-C (eax=pool [0x85c3c],
         * edx=handle slot [0x83d6c]). t_free_hud_weapon_das_handle stages a real pool + allocated
         * handle so the callee (pool[3] guard + release_chunk + *handle=0) runs REAL + symmetric. */
        pool_free_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle),
                                (uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_hud_weapon_das_handle));
    }
    G32(VA_g_hud_weapon_das_handle) = 0;
}

/* redraw_weapon_hud_panel (0x23869): void. If the HUD panel DAS handle [0x83d78] is non-null, recomposite
 * it (render_ui_texture_panel 0x227e9; menu_hud_ui bridge — eax=handle [0x83d78], edx=panel descriptor
 * [0x83e78]). No own state writes. */
void redraw_weapon_hud_panel(void)
{
    if (G32(VA_g_hud_panel_das_handle1) != 0) {
        /* re-pointed: render_ui_texture_panel 0x227e9 [L, menu_hud_ui] direct-C (eax=handle
         * 0x83d78, edx=panel descriptor 0x83e78). t_redraw_weapon_hud_panel now stages a REAL fixture
         * (custom harness): a real int* handle whose [0] is a (d[1]+3)*d[0] buffer, plus a BOUNDED mode-1
         * descriptor with d[0xb]*item>>8 == 0 -> no blit, d[4]/d[7]/d[8]=0 + g_reloc_base(0x7f56c)=0 so
         * resolve_reloc_ptr returns 0 safely, and g_active_weapon_ammo(0x83e6c)=0 so the ammo-icon/count-
         * text (font) branch is skipped. That runs the real renderer's geometry publish (0x83e1c..),
         * mem_fill of the handle buffer, and dirty-rect (0x83e0c..0x83e18) REAL on both sides — diffing
         * obj3 AND the handle buffer. The bar-mode/DAS/font pixel paths stay in-game-differential. */
        render_ui_texture_panel((uint32_t)G32(VA_g_hud_panel_das_handle1), (uint32_t)G32(VA_g_active_weapon_ammo_cap + 0x4));
    }
}

/* key_toggle_weapon_overlay (0x175d8, DEAD per the static graph but counts in the gate): toggles the
 * weapon raise/lower anim state. No-op while the fire lock [0x7fdd0] is set; otherwise dispatches on the
 * anim state [0x7fddc] (0..3, via the jump table at 0x175c8) — state 0: start raise (cooldown
 * [0x7fde0]=0x64, state=2, clear [0x7fdf0]); state 1: -> state 2 + clear [0x7fdf0]; state 2: -> state 1;
 * state 3: cooldown 0 + state 1. PURE (obj3 only). */
void key_toggle_weapon_overlay(void)
{
    if (G32(VA_g_weapon_fire_lock) != 0) return;                           /* fire lock set -> no-op */
    uint32_t s = (uint32_t)G32(VA_g_weapon_fire_lock + 0xc);
    if (s > 3) return;                                       /* cmp eax,3 ; ja ret */
    switch (s) {
    case 0: G32(VA_g_weapon_fire_lock + 0x10) = 0x64; G32(VA_g_weapon_fire_lock + 0xc) = 2; G32(VA_g_weapon_fire_lock + 0x20) = 0; break;  /* 0x175e3 */
    case 1: G32(VA_g_weapon_fire_lock + 0xc) = 2; G32(VA_g_weapon_fire_lock + 0x20) = 0; break;                       /* 0x175ed */
    case 2: G32(VA_g_weapon_fire_lock + 0xc) = 1; break;                                         /* 0x1760c */
    case 3: G32(VA_g_weapon_fire_lock + 0x10) = 0; G32(VA_g_weapon_fire_lock + 0xc) = 1; break;                       /* 0x17602 */
    }
}

/* ======================================================================================
 * B — Projectile spawn path (IN-GAME tier: entity-pool mutation + player-view state; verify via
 * ROTH_LIFT=weapon_combat / ROTH_LIFT_DIFF). Bridges init_inventory_item_object 0x18598 (inventory)
 * + spawn_entity_at_position 0x4254e (entity_ai) + isqrt_fixed 0x3bfe5 [L].
 * ====================================================================================== */

/* angle->aim arcsin-ish lookup embedded at canon 0x423c5 (indices 0..0x39). */
static const uint8_t g_spawn_aim_table[0x3a] = {
    0x00,0x01,0x02,0x03,0x05,0x06,0x08,0x09,0x0a,0x0c,0x0d,0x0e,0x0f,0x10,0x11,0x13,
    0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1c,0x1d,0x1e,0x1f,0x20,0x21,0x22,0x23,0x24,
    0x25,0x26,0x27,0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,0x30,0x31,0x32,0x33,0x34,
    0x35,0x35,0x36,0x36,0x37,0x38,0x38,0x39,0x3a,0x3b
};

/* shared finalize 0x424ee..0x42541: ESI=Z, EAX=X, EDX=Y, EBX/EBP=spawn-pos regs, ECX=descriptor slot,
 * loc=&0x40 stack local (holds the item descriptor from init_inventory_item_object). Spawns the entity
 * (spawn_entity_at_position 0x4254e; bridge) and fills its projectile fields. Returns the entity ptr or 0. */
static uint32_t spawn_entity_and_fill(uint32_t eaxX, uint32_t edxY, uint32_t ebxv, uint32_t ebpv,
                                      uint32_t esiZ, uint32_t ecx_slot, uint8_t *loc)
{
    esiZ -= *(uint32_t *)(loc + 0x3c);                       /* sub esi,[esp+0x3c] */
    /* re-pointed: spawn_entity_at_position 0x4254e [L, entity_ai] direct-C. It takes a const regs_t*
     * and reads exactly these 6 regs (eax/edx/ebx/ebp/esi/ecx); build the frame and pass it. */
    regs_t io; memset(&io, 0, sizeof io);
    io.eax = eaxX; io.edx = edxY; io.ebx = ebxv; io.ebp = ebpv; io.esi = esiZ; io.ecx = ecx_slot;
    uint32_t eax = spawn_entity_at_position(&io);     /* -> EAX rec / 0 */
    if (eax == 0) return 0;                                  /* je 0x42542 */
    uint32_t ebx = *(volatile uint32_t *)(uintptr_t)eax;     /* mov ebx,[eax] */
    *(volatile uint8_t  *)(uintptr_t)(ebx + 8) = 0;          /* [ebx+8]=0 */
    *(volatile uint8_t  *)(uintptr_t)(eax + 8) |= 1;         /* [eax+8]|=1 */
    *(volatile uint16_t *)(uintptr_t)(eax + 0x1a) = (uint16_t)G32(VA_g_dos_dta_name + 0x6a);
    *(volatile uint16_t *)(uintptr_t)(eax + 0x14) = (uint16_t)(*(volatile uint32_t *)(uintptr_t)GADDR(VA_g_weapon_spawn_scratch));
    *(volatile uint16_t *)(uintptr_t)(eax + 0x16) = *(uint16_t *)(loc + 6);
    *(volatile uint8_t  *)(uintptr_t)(eax + 9)    = *(uint8_t  *)(loc + 8);
    *(volatile uint8_t  *)(uintptr_t)(eax + 0x18) = *(uint8_t  *)(loc + 4);
    *(volatile uint8_t  *)(uintptr_t)(eax + 0x19) = *(uint8_t  *)(loc + 5);
    return eax;
}

/* spawn_projectile_from_aim (0x42400): EAX=item def, EDX=aim pitch, EBX=aim yaw. Converts the aim deltas
 * into screen-relative spawn offsets (the 0x423c5 arcsin table for pitch, a scaled divide for yaw),
 * allocates the projectile descriptor (init_inventory_item_object 0x18598; bridge), then spawns + fills
 * the entity at the player-view-relative position (shared finalize). Returns the entity ptr or 0. */
uint32_t spawn_projectile_from_aim(uint32_t eax, uint32_t edx, uint32_t ebx)
{
    uint8_t loc[0x40];
    int32_t off38;
    if (edx != 0) {                                          /* or edx,edx ; je 0x42457 */
        int32_t e = (int32_t)edx - G32(VA_g_view_x);
        e += e;
        e -= G32(VA_g_view_w);
        e >>= 1;                                             /* sar edx,1 */
        if (G8(VA_g_view_scale_flags) & 1) e >>= 1;
        int32_t q = (int32_t)((uint32_t)e << 6) / (int32_t)(uint16_t)G16(VA_g_init_stage_error_strings + 0x118);  /* shl 6; cdq; idiv (u16) */
        uint16_t aq = (q < 0) ? (uint16_t)(-(int16_t)(uint16_t)q) : (uint16_t)q;     /* jns ; neg ax */
        uint32_t t = aq;
        if (aq <= 0x39) t = g_spawn_aim_table[aq];           /* cmp 0x39 ; ja ; mov dl,table[edx] */
        off38 = (q < 0) ? -(int32_t)t : (int32_t)t;          /* jns ; neg edx */
    } else {
        off38 = 0;
    }
    *(uint32_t *)(loc + 0x38) = (uint32_t)off38;
    int32_t off3c;
    if (ebx != 0) {                                          /* or ebx,ebx ; je 0x4248d */
        int32_t b = (int32_t)ebx - G32(VA_g_view_y);
        b += b;
        b -= G32(VA_g_view_h);
        b >>= 1;
        int32_t m = 0x7e;
        if (G8(VA_g_hires_line_doubling_flag) != 0) m += m;                        /* edx = 0xfc */
        int64_t prod = (int64_t)b * (int64_t)m;              /* imul edx (64-bit) */
        off3c = (int32_t)(prod / (int64_t)(int32_t)G32(VA_g_das_render_scale + 0x4));   /* idiv [0x85408] */
    } else {
        off3c = (int32_t)ebx;                                /* ebx unchanged (0) */
    }
    *(uint32_t *)(loc + 0x3c) = (uint32_t)off3c;

    G32(VA_g_weapon_spawn_scratch) = (int32_t)eax;                             /* mov [0x422e8],eax (code scratch) */
    /* re-pointed: init_inventory_item_object 0x18598 [L, inventory] direct-C (EAX=item
     * def, EDX=&loc descriptor -> EAX slot). This site reads the post-call EBX/EBP high halves
     * below, but the disasm shows the callee PRESERVES both (0x18598 `push ebx`/`enter 8,0`;
     * 0x1868c `leave`/0x1868f `pop ebx`) — so with the pre-call frame all-zero (the old bridge
     * memset-0'd io) the (&0xffff0000) high halves are provably 0 and are hard-coded so. Behaviour-
     * identical to the bridge. Oracle-neutral: the caller spawn_projectile_from_aim is in-game tier
     * (not oracle-tested); the callee is lifted+verified (t_init_projectile / t_spawn_projectile). */
    uint32_t ecx_slot = init_inventory_item_object(eax, (uint32_t)(uintptr_t)loc);
    if (ecx_slot == 0) return 0;                             /* je 0x42542 */
    G32(VA_g_dos_dta_name + 0x6a) = 0;

    /* view-relative spawn position (0x424b1..0x424de). EBX/EBP were callee-PRESERVED (see above)
     * so the &0xffff0000 high halves are 0. */
    uint32_t ebx_pos = (uint16_t)G16(VA_g_player_z);               /* mov bx,[0x90a92] (bx high half 0) */
    int16_t hy = (int16_t)(uint16_t)G16(VA_g_player_height);
    hy = (int16_t)(hy - 0x14);                               /* sub ax,0x14 */
    if (hy >= 0) ebx_pos += (uint32_t)(int32_t)hy;           /* js skip ; add ebx,eax */
    uint32_t eaxX = (uint16_t)G16(VA_g_player_x);
    uint32_t edxY = (uint16_t)G16(VA_g_player_y);
    uint32_t ebp_pos = (uint16_t)G16(VA_g_player_angle);               /* mov bp,[0x90a8a] (bp high half 0) */
    ebp_pos -= *(uint32_t *)(loc + 0x38);                    /* sub ebp,[esp+0x38] */
    uint32_t esiZ = (uint32_t)G32(VA_g_view_pitch);
    if (esiZ == 0) esiZ = (uint32_t)G32(VA_g_view_pitch_applied);            /* or esi,esi ; jne ; mov esi,[0x8c108] */
    return spawn_entity_and_fill(eaxX, edxY, ebx_pos, ebp_pos, esiZ, ecx_slot, loc);
}

/* spawn_object_projectile_at_player (0x42300): EAX=item def, EDX=target/emitter record. Spawns a
 * projectile from a target record (vs the player's aim): stashes target[+0xc] in [0x90fd4], allocates
 * the descriptor (init_inventory_item_object; bridge), and (unless the player-fire flag [0x911cb] is set
 * or the target is out of vertical range) computes a horizontal aim scale via isqrt_fixed [L]; spawns at
 * the target's position. Shares the finalize tail with spawn_projectile_from_aim. Returns entity or 0. */
uint32_t spawn_object_projectile_at_player(uint32_t eax, uint32_t edx)
{
    uint8_t loc[0x40];
    uint32_t ebx = edx;                                      /* ebx = target record */
    G32(VA_g_dos_dta_name + 0x6a) = (int32_t)(uint16_t)*(volatile uint16_t *)(uintptr_t)(edx + 0xc);  /* [0x90fd4]=word[edx+0xc] */
    *(uint32_t *)(loc + 0x3c) = 0;
    *(uint32_t *)(loc + 0x38) = 0;
    G32(VA_g_weapon_spawn_scratch) = (int32_t)eax;
    /* re-pointed: init_inventory_item_object 0x18598 [L, inventory] direct-C (only EAX read here). */
    uint32_t ecx_slot = init_inventory_item_object(eax, (uint32_t)(uintptr_t)loc);
    if (ecx_slot == 0) return 0;

    uint32_t ebp = (uint32_t)(*(volatile uint8_t *)(uintptr_t)(ebx + 6));   /* movzx ebp,byte[ebx+6] */
    ebp += 0x80; ebp += ebp; ebp = (uint32_t)(-(int32_t)ebp);               /* add 0x80; add ebp,ebp; neg ebp */
    uint32_t esi = 0;
    int do_aim = 1;
    if (G8(VA_g_spawn_projectile_is_player) != 0) do_aim = 0;                        /* cmp byte[0x911cb],0 ; jne 0x423af */
    if (do_aim) {
        /* vertical-range check (0x42355) */
        int16_t v = (int16_t)((uint16_t)G16(VA_g_player_z) + (uint16_t)G16(VA_g_player_height));
        v = (int16_t)(v - 0x20);
        v = (int16_t)(v - (uint16_t)*(volatile uint16_t *)(uintptr_t)(ebx + 0xa));
        int32_t vd = (int32_t)v - G32(VA_g_dos_dta_name + 0x62);
        vd = (int32_t)(int16_t)(uint16_t)vd;                 /* movsx eax,ax */
        if (vd > 0x10 || vd <= -0x10) {                      /* cmp 0x10 jg ; cmp -0x10 jg(skip) */
            /* horizontal distance -> aim scale (0x4237d) */
            int32_t dxs = (int32_t)(int16_t)*(volatile int16_t *)(uintptr_t)ebx - (int32_t)(int16_t)(uint16_t)G16(VA_g_player_x);
            int32_t dys = (int32_t)(int16_t)*(volatile int16_t *)(uintptr_t)(ebx + 2) - (int32_t)(int16_t)(uint16_t)G16(VA_g_player_y);
            int32_t d2 = dxs * dxs + dys * dys;
            int32_t r = isqrt_fixed((uint32_t)d2);    /* call 0x3bfe5 [L] */
            esi = (uint32_t)((int32_t)((uint32_t)vd << 7) / (r == 0 ? 1 : r));  /* shl eax,7 ; cdq ; idiv esi */
            (void)r;
        }
    }
    uint32_t eaxX = (uint16_t)*(volatile uint16_t *)(uintptr_t)ebx;          /* mov ax,[ebx] */
    uint32_t edxY = (uint16_t)*(volatile uint16_t *)(uintptr_t)(ebx + 2);    /* mov dx,[ebx+2] */
    uint32_t ebx_pos = (ebx & 0xffff0000u) | (uint16_t)*(volatile uint16_t *)(uintptr_t)(ebx + 0xa);  /* mov bx,[ebx+0xa] */
    ebx_pos += (uint32_t)G32(VA_g_dos_dta_name + 0x62);                       /* add ebx,[0x90fcc] */
    return spawn_entity_and_fill(eaxX, edxY, ebx_pos, ebp, esi, ecx_slot, loc);
}

/* spawn_player_projectile_flagged (0x422ec): EAX=item def, EDX=target. Sets the player-fire flag
 * [0x911cb]=1 (so spawn_object_projectile_at_player skips the AI aim-lead), spawns, clears the flag. */
uint32_t spawn_player_projectile_flagged(uint32_t eax, uint32_t edx)
{
    G8(VA_g_spawn_projectile_is_player) = 1;
    uint32_t r = spawn_object_projectile_at_player(eax, edx);
    G8(VA_g_spawn_projectile_is_player) = 0;
    return r;
}

/* ======================================================================================
 * A — Fire pipeline (IN-GAME tier).
 * ====================================================================================== */

/* fire_pending_weapon_shot (0x16da2): __watcall(int param_1) — EAX = an active-record pointer (the
 * caller 0x1729c does `mov esi,eax; call` so param_1 = ESI, a struct ptr), NOT the pending def. If a
 * weapon shot is latched (the GLOBAL g_pending_weapon_def [0x7fdf4] != 0): raise the action timer
 * [0x853f6] to *param_1's* [+0x44] field if larger (disasm: `cmp [eax+0x44],edx` — EAX is param_1,
 * the global is only reloaded into EAX at 0x16dcd for the spawn arg), spawn the projectile
 * (spawn_projectile_from_aim with eax=g_pending_weapon_def, edx=g_pending_fire_aim [0x7fe10],
 * ebx=[0x7fe14]); if a shot scale [0x7fe0c] is set, multiply the new projectile's DAMAGE word [+0x16]
 * by it; clear [0x7fe0c]. NB: g_pending_weapon_def is a small ITEM-DEF ID (passed to
 * init_inventory_item_object), not a pointer — conflating it with param_1 for the [+0x44] deref
 * faulted at id+0x44 (the original 0x6b+0x44=0xaf SIGSEGV). Returns scratch EAX (caller discards). */
uint32_t fire_pending_weapon_shot(uint32_t param_1)
{
    if (G32(VA_g_pending_weapon_def) == 0) return param_1;                   /* cmpl [0x7fdf4],0 ; je 0x16e00 (eax unchanged) */
    uint32_t t = (uint8_t)G8(VA_g_render_sector_walk_mode + 0x23);                       /* movzx edx,byte[0x853f6] */
    if (t < (uint32_t)*(volatile uint32_t *)(uintptr_t)(param_1 + 0x44))  /* cmp [eax+0x44],edx ; jae (EAX=param_1) */
        G8(VA_g_render_sector_walk_mode + 0x23) = *(volatile uint8_t *)(uintptr_t)(param_1 + 0x44);    /* mov al,[eax+0x44] ; mov [0x853f6],al */
    uint32_t def = (uint32_t)G32(VA_g_pending_weapon_def);                   /* mov eax,[0x7fdf4] (reloaded for the spawn arg) */
    uint32_t ent = spawn_projectile_from_aim(def, (uint32_t)G32(VA_g_pending_fire_aim), (uint32_t)G32(VA_g_pending_fire_aim + 0x4));
    if (ent != 0 && G32(VA_g_pending_shot_scale) != 0) {                     /* test eax ; cmp [0x7fe0c],0 */
        uint16_t scale = (uint16_t)G16(VA_g_pending_shot_scale);
        uint16_t v = *(volatile uint16_t *)(uintptr_t)(ent + 0x16);
        *(volatile uint16_t *)(uintptr_t)(ent + 0x16) = (uint16_t)(v * scale);   /* imul ebx,edx (16-bit store) */
    }
    G32(VA_g_pending_shot_scale) = 0;
    return ent;                                              /* spawn path leaves eax=entity (scratch) */
}

/* trigger_weapon_fire (0x1768a, 674B; the fire-trigger state machine; IN-GAME tier). EAX=arg1 (latched
 * to g_pending_fire_aim-pair [0x7fe10]), EDX=arg2 (->edi, latched to [0x7fe14]). Validates the fire lock
 * [0x7fdd0] + active weapon, decrements ammo (single or burst, [0x811e5]&0x80 = full-clip step 0x100),
 * on empty either shows the no-ammo message (show_no_ammo_message [L]) + auto-equips (equip_first_usable_weapon
 * [L]) or picks a random alt slot (rng_range [L]) or consumes an ammo item (remove_item; inventory bridge);
 * commits the shot: sets the fire lock, arms the cooldown (arm_weapon_fire [L]), plays the fire SFX
 * (play_sound_effect; audio bridge), latches the pending weapon def/fields ([0x7fdf4]/[0x7fdf8]/[0x7fde8]/
 * [0x7fdfc]/[0x7fdec]/[0x7fe10]/[0x7fe14]). Drives render_weapon_view + redraw_weapon_hud_panel [L] for the
 * ammo HUD. Faithful goto-transcription (labels = canon VAs). Returns scratch EAX (caller discards). */
uint32_t trigger_weapon_fire(uint32_t eax, uint32_t edx, uint32_t ebx_ign, uint32_t ecx_ign)
{
    (void)ebx_ign; (void)ecx_ign;
    uint32_t arg1 = eax;                         /* [ebp-0xc] */
    uint32_t edi = edx;                          /* mov edi,edx (arg2) */
    uint32_t loc4 = 0, loc8 = 0;
    uint32_t edxp, ecx = 0, ebx = 0, esi = 0;

    if (G32(VA_g_weapon_fire_lock) != 0) goto L_17926;         /* fire lock */
    edxp = (uint32_t)G32(VA_g_active_weapon_ptr);               /* g_active_weapon_ptr */
    if (G32(VA_g_active_weapon_ammo_id + 0x110) == 0) goto L_17926;
    if (edxp == 0) { edxp = (uint32_t)GADDR(VA_g_active_weapon_attrs); G32(VA_g_active_weapon_ptr) = (int32_t)edxp; }
    if (*(volatile int32_t *)(uintptr_t)edxp == 0) goto L_17926;
    if ((uint32_t)G32(VA_g_active_weapon_ptr) != (uint32_t)GADDR(VA_g_active_weapon_attrs)) {
        if (G32(VA_g_active_weapon_attrs + 0x4) >= 1 && (uint32_t)G32(VA_g_active_weapon_ammo_id) >= (uint32_t)G32(VA_g_active_weapon_ammo_cap)) {
            edxp = (uint32_t)GADDR(VA_g_active_weapon_attrs); G32(VA_g_active_weapon_ptr) = (int32_t)edxp;
        }
    }
    /* L_176fa */
    G32(VA_g_weapon_fire_lock + 0x20) = 0;
    if (G32(VA_g_selected_item_secondary) == 0) goto L_178a5;
    {
        int32_t a = *(volatile int32_t *)(uintptr_t)(edxp + 4);
        if (a < 0 || a < 1) goto L_178a5;
        ebx = 1;
        if (G8(VA_g_active_weapon_ammo_id + 0x1) & 0x80) ebx = 0x100;
        uint32_t e = (uint32_t)G32(VA_g_selected_item_secondary);
        int32_t ammo = *(volatile int16_t *)(uintptr_t)(e + 2);
        if ((uint32_t)ammo < ebx) goto L_177a3;
        *(volatile uint16_t *)(uintptr_t)(e + 2) -= (uint16_t)ebx;
        if (ebx == 1) goto L_1777e;
        if (G32(VA_g_active_weapon_attrs + 0x4) <= 1) goto L_1777e;
        ecx = 1;
        for (;;) {                               /* 0x1775c burst loop */
            uint32_t edxl = (uint32_t)G32(VA_g_selected_item_secondary);
            int32_t am = *(volatile int16_t *)(uintptr_t)(edxl + 2);
            if ((uint32_t)am < ebx) break;       /* jb 0x17778 */
            ecx++;
            *(volatile uint16_t *)(uintptr_t)(edxl + 2) -= (uint16_t)ebx;
            render_weapon_view(0);         /* re-pointed 0x22e7b [L] (EDX preserved-not-read by orig) */
        }
        G32(VA_g_pending_shot_scale) = (int32_t)ecx;
    L_1777e:
        {
            uint32_t e2 = (uint32_t)G32(VA_g_selected_item_secondary);
            G32(VA_g_active_weapon_item_id) = (int32_t)*(volatile int16_t *)(uintptr_t)(e2 + 2);
            edxp = (uint32_t)GADDR(VA_g_active_weapon_attrs);
            render_weapon_view(0);         /* re-pointed 0x22e7b [L] */
            G32(VA_g_active_weapon_ptr) = (int32_t)edxp;
        }
        goto L_178a5;
    }
L_177a3:
    if (G32(VA_g_active_weapon_ammo_id) != 0) goto L_17800;
    if ((uint32_t)G32(VA_g_active_weapon_ammo_id + 0x110) > 1) goto L_177e1;
    if (G8(VA_g_active_weapon_ammo_id + 0x1) & 0x80) {
        show_no_ammo_message(5);
        goto L_17926;
    }
    show_no_ammo_message(6);
    equip_first_usable_weapon();
    goto L_17926;
L_177e1:
    if (edxp != (uint32_t)GADDR(VA_g_active_weapon_attrs)) goto L_178a5;
    {
        uint32_t r = rng_range((uint32_t)G32(VA_g_active_weapon_ammo_id + 0x110) - 1);
        r += 1;
        edxp += r * 0x50;
    }
    G32(VA_g_active_weapon_ptr) = (int32_t)edxp;                /* 0x17798: mov [0x7fe00],edx */
    goto L_178a5;
L_17800:
    if (*(volatile uint8_t *)(uintptr_t)(edxp + 0x31) & 0x80) goto L_177ac;
    if (G32(VA_g_active_weapon_ammo) == 0) goto L_177ac;
    {
        /* re-pointed: remove_item 0x1d077 [L, inventory] direct-C (EAX=ammo id; return discarded) */
        remove_item((uint32_t)(int32_t)*(volatile int16_t *)(uintptr_t)(edxp + 0x30));
    }
    if (*(volatile int32_t *)(uintptr_t)(edxp + 4) > 1) {
        uint32_t e3 = (uint32_t)G32(VA_g_selected_item_secondary);
        *(volatile uint16_t *)(uintptr_t)(e3 + 2) += *(volatile uint16_t *)(uintptr_t)(edxp + 4);
        uint32_t e4 = (uint32_t)G32(VA_g_selected_item_secondary);
        G32(VA_g_active_weapon_item_id) = (int32_t)*(volatile int16_t *)(uintptr_t)(e4 + 2);
    }
    /* L_17839 */
    edxp = (uint32_t)GADDR(VA_g_active_weapon_attrs); G32(VA_g_active_weapon_ptr) = (int32_t)edxp;
    if (G32(VA_g_active_weapon_attrs + 0x2c) == 0) goto L_1787b;
    G32(VA_g_pending_weapon_def) = 0;                            /* [0x811e0]!=0 path -> straight to commit */
    esi  = (uint32_t)G32(VA_g_active_weapon_ammo_id + 0x10);
    loc8 = (uint32_t)G32(VA_g_active_weapon_attrs + 0x28);
    ecx  = (uint32_t)G32(VA_g_active_weapon_ammo_id + 0x4);
    loc4 = (uint32_t)G32(VA_g_active_weapon_attrs + 0x24);
    ebx  = (uint32_t)G32(VA_g_active_weapon_attrs + 0x2c);
    goto L_178c1;
L_1787b:
    {
        int32_t cap = G32(VA_g_active_weapon_ammo_cap);
        G32(VA_g_active_weapon_item_id) = G32(VA_g_active_weapon_item_id) - cap;
        uint32_t e5 = (uint32_t)G32(VA_g_selected_item_secondary);
        G32(VA_g_pending_weapon_def) = 0;
        *(volatile uint16_t *)(uintptr_t)(e5 + 2) -= (uint16_t)G16(VA_g_active_weapon_ammo_cap);
        redraw_weapon_hud_panel();
    }
    goto L_178a5;
L_177ac:
    /* low-ammo with no ammo-item: fall back to the no-ammo handling (== 0x177ac target) */
    if ((uint32_t)G32(VA_g_active_weapon_ammo_id + 0x110) > 1) goto L_177e1;
    if (G8(VA_g_active_weapon_ammo_id + 0x1) & 0x80) { show_no_ammo_message(5); goto L_17926; }
    show_no_ammo_message(6);
    equip_first_usable_weapon();
    goto L_17926;
L_178a5:
    loc8 = (uint32_t)*(volatile int32_t *)(uintptr_t)(edxp + 0x20);
    ecx  = (uint32_t)*(volatile int32_t *)(uintptr_t)(edxp + 0x10);
    loc4 = (uint32_t)*(volatile int32_t *)(uintptr_t)(edxp + 0x1c);
    ebx  = (uint32_t)*(volatile int32_t *)(uintptr_t)(edxp + 0);
    esi  = (uint32_t)*(volatile int32_t *)(uintptr_t)(edxp + 0x3c);
    G32(VA_g_pending_weapon_def) = *(volatile int32_t *)(uintptr_t)(edxp + 8);
L_178c1:
    if (ebx == 0) goto L_17926;
    G32(VA_g_weapon_fire_lock) = -1;                            /* set fire lock */
    G32(VA_g_format_flags + 0x3) = (int32_t)((G32(VA_g_selected_item_secondary) != 0 ? 1u : 0u) + 0x101u);
    arm_weapon_fire();             /* arm_weapon_fire 0x17629 [L] */
    if (ecx != 0) {
        /* re-pointed: play_sound_effect 0x27270 [L, audio] direct-C (id=ecx-1, param=0xfcff) */
        play_sound_effect(ecx - 1, 0xfcff);
    }
    /* L_178fc — latch the shot fields */
    G32(VA_g_pending_weapon_def + 0x4) = (int32_t)ebx;
    G32(VA_g_weapon_fire_lock + 0x18) = (int32_t)loc8;
    G32(VA_g_pending_weapon_def + 0x8) = (int32_t)esi;
    G32(VA_g_weapon_fire_lock + 0x1c) = (int32_t)loc4;
    G32(VA_g_pending_fire_aim + 0x4) = (int32_t)edi;
    G32(VA_g_pending_fire_aim) = (int32_t)arg1;
L_17926:
    return arg1;
}

/* key_fire_weapon (0x14cb6): the input keybind. Clears [0x7f568] then tail-calls trigger_weapon_fire(0,0). */
void key_fire_weapon(void)
{
    G32(VA_g_dev_mode_flag + 0x8) = 0;
    trigger_weapon_fire(0, 0, 0, 0);
}

/* reset_weapon_hud (0x1be8e): void. Re-equips the "no weapon" state (activate_weapon_item(0,0)) then
 * recomposites the weapon HUD (render_weapon_hud(1, &attrs)). NB: activate_weapon_item(0,0) takes the
 * ecx==0 path whose `movsx word[ecx+2]` is a DS-relative low access (DS:2); the lifted body reproduces
 * that access BYTE-FOR-BYTE (raw addr 2), so it's now called DIRECT. IN-GAME tier. */
void reset_weapon_hud(void)
{
    /* re-pointed: activate_weapon_item(0,0) 0x184ab [L] -> direct-C. The ecx==0 path's
     * `movsx word[ecx+2]` (disasm 0x184de) reads DS:[2]=linear addr 2; the lifted body reproduces it
     * IDENTICALLY (lift_weapon_combat.c activate_weapon_item: *(int16_t*)(ecx+2) with ecx=0 ->
     * raw host addr 2), so the access is the SAME whether bridged (call_orig runs that same original in
     * this host address space) or direct -> conversion is behavior-neutral in-game. ORACLE-NEUTRAL:
     * reset_weapon_hud is reached only from equip_first_usable_weapon's remaining==0 / none-found arms;
     * the sole oracle test (t_equip_first_usable_weapon) stages a FOUND usable weapon so those arms are
     * unreached -> the flat-addr-2 read never runs in the oracle. The in-game low-page mapping is a
     * separate host boundary, identical for bridge and direct. */
    activate_weapon_item(0, 0);           /* 0x184ab [L] (eax=0,edx=0) — re-pointed */
    /* re-pointed: render_weapon_hud 0x24165 [L] direct-C (mode 1, attrs=&g_active_weapon_attrs) */
    render_weapon_hud(1, (uint32_t)GADDR(VA_g_active_weapon_attrs));
}

/* tick_weapon_hud_ammo_anim (0x2250e): per-frame HUD ammo-counter animation. Gated on weapon count
 * [0x83e04] and the accumulator [0x83e08] (+= [0x85324], must exceed 0x10). For each weapon-list entry
 * (0x83d84, stride 8; entry[0]=item record, [4]=full-ammo, [6]=anim-rate): if the displayed ammo
 * (word[rec+2]) hasn't settled to its target ((cur>>8) < entry[4]), step it by 0x1000/(entry[6]*0x46)
 * divided by (target+1) (min 1) and store back; when the entry is the active weapon [0x81038], publish
 * [0x83e70] and redraw (render_weapon_view 0x22e7b; framebuffer bridge) iff the displayed value changed.
 * The interpolation arithmetic + obj3 writes are oracle-verified (render_weapon_view stubbed). */
void tick_weapon_hud_ammo_anim(void)
{
    if (G32(VA_g_weapon_hud_anim_count) == 0) return;
    G32(VA_g_weapon_hud_anim_accum) = (int32_t)((uint32_t)G32(VA_g_weapon_hud_anim_accum) + (uint32_t)G32(VA_g_frame_time_scale));
    if ((uint32_t)G32(VA_g_weapon_hud_anim_accum) <= 0x10) return;
    uint32_t entry = (uint32_t)GADDR(VA_g_weapon_hud_anim_table);
    G32(VA_g_weapon_hud_anim_accum) = (int32_t)((uint32_t)G32(VA_g_weapon_hud_anim_accum) - 0x10);
    for (uint32_t i = 0; i < (uint32_t)G32(VA_g_weapon_hud_anim_count); i++, entry += 8) {
        uint32_t itemrec = *(volatile uint32_t *)(uintptr_t)entry;
        int32_t cur = *(volatile int16_t *)(uintptr_t)(itemrec + 2);   /* movsx word[rec+2] */
        uint32_t target = (uint32_t)cur >> 8;                          /* [ebp-8] */
        if ((uint32_t)*(volatile uint16_t *)(uintptr_t)(entry + 4) <= target) continue;
        int32_t ebxv = (int32_t)(uint16_t)*(volatile uint16_t *)(uintptr_t)(entry + 6) * 0x46;
        int32_t step = (int32_t)0x1000 / ebxv;                         /* idiv */
        if (step == 0) step = 1;
        step = (int32_t)((uint32_t)step / (target + 1));               /* div by ([ebp-8]+1) */
        cur += step;
        *(volatile uint16_t *)(uintptr_t)(itemrec + 2) = (uint16_t)cur;
        if ((uint32_t)G32(VA_g_selected_item_secondary) != itemrec) continue;
        G32(VA_g_active_weapon_item_id) = cur;
        int redraw;
        if (*(volatile uint16_t *)(uintptr_t)(entry + 4) == 1) {
            redraw = 1;                                                /* entry[4]==1 -> always redraw */
        } else {
            redraw = (((uint32_t)cur >> 8) != target);                 /* shr cur,8 ; cmp [ebp-8] ; jne */
        }
        if (redraw) {
            /* re-pointed: render_weapon_view 0x22e7b [L] direct-C. t_tick_weapon_hud_ammo_anim
             * stages a REAL mode-1 panel descriptor with box_w(R[0x2c])=0 (0x83e78) + a surface holder
             * (0x83d78), and drives scenarios that actually FIRE this redraw. With box_w=0 the mode-1 handler
             * computes scaled=rem=0 and SKIPS both blit branches (no image resolve / DAS / framebuffer
             * writes) — leaving only the deterministic dirty-rect accumulation (0x83e0c..0x83e18), which
             * runs REAL + symmetric on both sides (verified via redraw_fire/redraw_step). The full pixel
             * paths (real box_w, mode 0/2 images) stay in-game-differential (ROTH_LIFT_DIFF=render_weapon_view). */
            render_weapon_view(1);                              /* 0x22e7b [L] */
        }
    }
}

/* render_weapon_hud (0x24165): EAX=mode (1=re-show), EDX=weapon attrs ptr. The weapon-ammo HUD panel
 * compositor (IN-GAME tier; framebuffer). Orchestration only — the pixel work is in the bridged callees:
 * computes the panel rect/scale (widescreen [0x85498]==0x280 -> [0x83d70]=1; layout 0x120x0x3c vs 0x90x0x1e),
 * frees the old panel (free_das_cache_handle 0x13136 + the dirty-rect of the prior region via
 * register_dirty_rect 0x15b5b / add_dirty_rect 0x15b69; video_display bridges), saves the framebuffer
 * under the new rect (save_framebuffer_region 0x13062; blit_2d bridge), allocs a DAS panel handle
 * (ensure_das_cache_heap_space 0x414d2 bridge + pool_alloc_handle 0x360f9 [L]), draws the panel from the
 * 0x718b8 table (render_ui_texture_panel 0x227e9; menu_hud_ui bridge), then rebuilds the weapon list
 * (rebuild_weapon_inventory_list [L]). anim frame attrs[+0x48] (clamped 0..0xf; 0 -> just free + return,
 * NO rebuild — the rebuild tail 0x243b3 is reached only via the full attrs[+0x48]!=0 path). */
void render_weapon_hud(uint32_t eax_arg, uint32_t edx_arg)
{
    uint32_t arg1 = eax_arg;
    uint32_t attrs = edx_arg;
    G32(VA_g_active_weapon_ammo_cap + 0x4) = 0;
    G32(VA_g_hud_weapon_das_handle + 0x4) = 0;
    if (G32(VA_g_screen_pitch) == 0x280) G32(VA_g_hud_weapon_das_handle + 0x4) = 1;
    if (arg1 == 1 && G32(VA_g_hud_panel_das_handle0) != 0) {
        /* dirty-rect marks — lifted (video_display), called direct */
        if (G32(VA_g_hud_weapon_das_handle + 0x4) != 0)
            add_dirty_rect((uint32_t)G32(VA_g_weapon_hud_anim_accum + 0x14), G32(VA_g_weapon_hud_anim_accum + 0x18),
                                  (uint32_t)G32(VA_g_weapon_hud_anim_accum + 0x1c) * 2 + (uint32_t)G32(VA_g_weapon_hud_anim_accum + 0x14),
                                  (uint32_t)G32(VA_g_weapon_hud_anim_accum + 0x20) * 2 + (uint32_t)G32(VA_g_weapon_hud_anim_accum + 0x18));
        else
            register_dirty_rect((uint32_t)G32(VA_g_weapon_hud_anim_accum + 0x14), G32(VA_g_weapon_hud_anim_accum + 0x18),
                                       (uint32_t)G32(VA_g_weapon_hud_anim_accum + 0x14) + (uint32_t)G32(VA_g_weapon_hud_anim_accum + 0x1c),
                                       (uint32_t)G32(VA_g_weapon_hud_anim_accum + 0x18) + (uint32_t)G32(VA_g_weapon_hud_anim_accum + 0x20));
        /* re-pointed: free_das_cache_handle 0x13136 [L, das_assets] direct-C (EAX=handle -> 0) */
        G32(VA_g_hud_panel_das_handle0) = (int32_t)free_das_cache_handle((uint32_t)G32(VA_g_hud_panel_das_handle0));
    }
    /* L_24223 — anim frame clamp */
    if ((uint32_t)*(volatile uint32_t *)(uintptr_t)(attrs + 0x48) > 0xf)
        *(volatile uint32_t *)(uintptr_t)(attrs + 0x48) = 0;
    if (*(volatile uint32_t *)(uintptr_t)(attrs + 0x48) == 0) {
        free_hud_panel_das_handles();                           /* re-pointed 0x22633 [L] */
        G32(VA_g_weapon_hud_anim_accum + 0x1c) = 0; G32(VA_g_active_weapon_ammo_cap + 0x4) = 0;
        return;   /* 0x24258 jmp 0x243b8 (leave): the anim-frame-0 path does NOT rebuild the weapon
                   * list — the rebuild at 0x243b3 is reached ONLY by the full (attrs[+0x48]!=0) path.
                   * (fix: the prior `goto L_243b3` erroneously rebuilt here; latent because
                   * render_weapon_hud was stub-bridged until now — exposed by the equip re-point.) */
    }
    G32(VA_g_active_weapon_ammo_cap + 0x10) = *(volatile int32_t *)(uintptr_t)(attrs + 0x48) - 1;
    free_hud_panel_das_handles();                               /* re-pointed 0x22633 [L] */
    uint32_t loc1c, loc18, loc10, loc14;
    if (G32(VA_g_screen_pitch + 0x4) != G32(VA_g_view_h)) {
        uint32_t loc24 = *(volatile uint32_t *)(uintptr_t)(GADDR(VA_g_choice_selected_index + 0x6c) + ((uint32_t)G32(VA_g_screen_resolution_index) << 3));
        if (G32(VA_g_hud_weapon_das_handle + 0x4) != 0) { loc1c = 0x120; loc18 = 0x3c; }
        else                   { loc1c = 0x90;  loc18 = 0x1e; }
        loc10 = (uint32_t)G32(VA_g_screen_pitch + 0x4) - loc18 - loc24;
        loc14 = (((uint32_t)G32(VA_g_screen_pitch) - loc1c) >> 1) & 0xfffffffcu;
        uint32_t loc20 = loc18 + 3;
        if (G32(VA_g_hud_weapon_das_handle + 0x4) != 0) loc20 += 3;
        if (loc20 + loc10 > (uint32_t)G32(VA_g_screen_pitch + 0x4)) loc20 = (uint32_t)G32(VA_g_screen_pitch + 0x4) - loc10;
        /* re-pointed: save_framebuffer_region 0x13062 [L, blit_2d] direct-C
         * (x=EAX=loc14, y=EDX=loc10, width=EBX=loc1c, height=ECX=loc20; CF-out unused, only EAX read) */
        G32(VA_g_hud_panel_das_handle0) = (int32_t)save_framebuffer_region(loc14, loc10, loc1c, loc20, NULL);
    } else {
        loc1c = 0x90; loc18 = 0x1e; loc10 = 1;
        loc14 = ((uint32_t)G32(VA_g_screen_pitch) - 0x90) >> 1;
    }
    (void)loc1c; (void)loc18;
    G32(VA_g_weapon_hud_anim_accum + 0x2c) = (int32_t)loc10;
    G32(VA_g_weapon_hud_anim_accum + 0x30) = (int32_t)loc14;
    G32(VA_g_weapon_hud_anim_accum + 0x1c) = 0;
    ensure_das_cache_heap_space(0x1290);                        /* re-pointed 0x414d2 [L] */
    uint32_t h = pool_alloc_handle((uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_das_cache_heap_handle), 0x1290);  /* [L] */
    G32(VA_g_hud_panel_das_handle1) = (int32_t)h;
    if (h != 0) {
        uint32_t panel = *(volatile uint32_t *)(uintptr_t)(GADDR(VA_g_choice_selected_index + 0x548) + ((uint32_t)G32(VA_g_active_weapon_ammo_cap + 0x10) << 2));
        G32(VA_g_active_weapon_ammo_cap + 0x4) = (int32_t)panel;
        render_ui_texture_panel(h, panel);                      /* re-pointed 0x227e9 [L, menu_hud_ui] */
    }
    /* 0x243b3 rebuild tail — reached only by the full (attrs[+0x48]!=0) path fall-through */
    rebuild_weapon_inventory_list();                            /* [L] */
}

/* draw_player_viewmodel_sprite (0x139a0, 571B; IN-GAME fb): EAX=sprite frame base, EBX=screen X,
 * ECX=screen Y, EDX=frame index. Navigates the DAS animation frame chain (flags word[esi] bit 0x10 =
 * multi-frame, advancing esi by [esi+0xc] while the frame is unfinished), then per the frame flags
 * (bit8=has-blit, bit0x10=+8, bit4=offset-adjust loads x/y/w/h deltas, bit2 clear=+0x300) builds the
 * blit descriptor (a 0x2c-byte frame at EBP) — vertical scale from (y-0x64)*[0x85408]/0x9b + [0x85cdc]
 * half-offset, horizontal scale from (x-0xa0)*[0x85404]/0x9b + [0x85cd8] half-offset, source column-skip
 * for off-screen-left, screen offset from [0x90a98]/[0x85ce0]/[0x85ce4] — then blits via the shared-EBP
 * transparent-sprite blitter. The horizontal scale + blitter pair branch on [0x85404]: if >0x9b (high-res/
 * widescreen) -> divisor 0x136 + X-doubled + blitters 0x13c60/0x13d90; else divisor 0x9b + X-once +
 * blitters 0x13be0/0x13cf0 (shaded variant when 0<[0x8c1de]<0x80, else clipped). The frame-index (pushed
 * edx) is bookkeeping discarded at the tail. IN-GAME tier (live-swap visual verify; no fb-diff handler). */
void draw_player_viewmodel_sprite(uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    int32_t loc[0xb];                            /* 0x2c-byte descriptor frame (EBP) */
    memset(loc, 0, sizeof loc);
    uint32_t esi = eax + 4;
    uint32_t frame_idx = edx;
    loc[0] = (int32_t)ebx;                       /* [ebp+0] = X */
    loc[1] = (int32_t)ecx;                       /* [ebp+4] = Y */
    loc[8 / 4] = (uint16_t)*(volatile uint16_t *)(uintptr_t)(esi + 4);   /* [ebp+8] = w */
    loc[0xc / 4] = (uint16_t)*(volatile uint16_t *)(uintptr_t)(esi + 6); /* [ebp+0xc] = h */
    uint16_t di = *(volatile uint16_t *)(uintptr_t)esi;
    while (frame_idx != 0 && (di & 0x10) &&
           (int16_t)(*(volatile uint16_t *)(uintptr_t)(esi + 8) - 1) > (int16_t)*(volatile uint16_t *)(uintptr_t)(esi + 0xa)) {
        esi += (uint32_t)*(volatile int32_t *)(uintptr_t)(esi + 0xc);
        frame_idx--;
        di = *(volatile uint16_t *)(uintptr_t)esi;
    }
    /* the di&0x10 edx-adjust block (0x139e6) only touches the discarded frame index -> skipped */
    if (!(di & 8)) return;                       /* no blit */
    esi += 8;
    if (di & 0x10) esi += 8;
    if (di & 4) {                                /* offset-adjust sub-record */
        loc[0]       += (uint16_t)*(volatile uint16_t *)(uintptr_t)(esi + 0);
        loc[1]       += (uint16_t)*(volatile uint16_t *)(uintptr_t)(esi + 4);
        loc[8 / 4]    = (uint16_t)*(volatile uint16_t *)(uintptr_t)(esi + 2);
        loc[0xc / 4]  = (uint16_t)*(volatile uint16_t *)(uintptr_t)(esi + 6);
        esi += 8;
    }
    if (!(di & 2)) esi += 0x300;
    if (loc[8 / 4] == 0 || loc[0xc / 4] == 0) return;

    /* vertical scale */
    int64_t yp = (int64_t)(loc[1] - 0x64) * (int64_t)(int32_t)G32(VA_g_das_render_scale + 0x4);
    int32_t yq = (int32_t)(yp / 0x9b);
    int32_t e18 = G32(VA_g_view_h);
    if (G8(VA_g_hires_line_doubling_flag) != 0) e18 += e18;
    loc[0x18 / 4] = e18;
    yq += (e18 >> 1);
    loc[1] = yq;
    loc[0x24 / 4] = (int32_t)((int64_t)0x9b0000 / (int64_t)(int32_t)G32(VA_g_das_render_scale + 0x4));
    uint32_t ebx_src = esi;                       /* mov ebx,esi */
    esi = esi + (uint32_t)loc[0xc / 4] * 4;       /* esi = src + h*4 */
    loc[0x1c / 4] = (int32_t)esi;
    uint32_t edi = (uint32_t)G32(VA_g_framebuffer_ptr);

    /* horizontal column-skip for negative Y (loc[4]<0 here is the post-scale Y) */
    if (loc[1] < 0) {
        int32_t neg = -loc[1];
        int32_t cnt = (int32_t)((int64_t)neg * 0x9b / (int64_t)(int32_t)G32(VA_g_das_render_scale + 0x4));
        while (cnt > 0) {                         /* skip `cnt` source columns */
            esi += (uint8_t)*(volatile uint8_t *)(uintptr_t)(ebx_src + 2);
            ebx_src += 4;
            cnt--;
        }
    }
    /* base screen ROW offset (0x13ad7): eax = (post-scale Y if Y>=0 else 0) + [0x85ce4]; +[0x85ce4]
     * again if [0x90cbd]; then *[0x85498] (stride). The Y>=0 term comes from the jns at 0x13aae NOT
     * zeroing eax (only the Y<0 column-skip path does `sub eax,eax` at 0x13ad5). */
    int32_t y_for_base = (loc[1] >= 0) ? loc[1] : 0;
    int32_t ybase = y_for_base + G32(VA_g_view_y);
    if (G8(VA_g_hires_line_doubling_flag) != 0) ybase += G32(VA_g_view_y);
    ybase = (int32_t)((int64_t)ybase * (int64_t)(int32_t)G32(VA_g_screen_pitch));   /* imul [0x85498] */

    uint32_t blit_clip, blit_shaded;
    if ((uint32_t)G32(VA_g_das_render_scale) > 0x9b) {
        /* HIGH-RES / widescreen horizontal scale (0x13b6b): divisor 0x136, [0x85cd8] shifted LOGICAL
         * (loc[0x10]=>>1, eax+=>>2), X added TWICE (double-width dest), blitters 0x13c60/0x13d90. */
        int64_t xp = (int64_t)(loc[0] - 0xa0) * (int64_t)(int32_t)G32(VA_g_das_render_scale);
        int32_t xq = (int32_t)(xp / 0x136);
        uint32_t e10 = (uint32_t)G32(VA_g_view_w) >> 1;   /* shr 1 (logical) */
        loc[0x10 / 4] = (int32_t)e10;
        xq += (int32_t)(e10 >> 1);                    /* shr 1 again */
        loc[0] = xq;
        edi += (uint32_t)(ybase + loc[0] + loc[0] + G32(VA_g_view_x));   /* add edx,eax TWICE */
        blit_clip = 0x13c60u; blit_shaded = 0x13d90u;
    } else {
        /* NORMAL horizontal scale (0x13aff): divisor 0x9b, [0x85cd8] sar 1, X added once,
         * blitters 0x13be0/0x13cf0. */
        int64_t xp = (int64_t)(loc[0] - 0xa0) * (int64_t)(int32_t)G32(VA_g_das_render_scale);
        int32_t xq = (int32_t)(xp / 0x9b);
        int32_t e10 = G32(VA_g_view_w);
        loc[0x10 / 4] = e10;
        xq += (e10 >> 1);                             /* sar 1 (arithmetic) */
        loc[0] = xq;
        edi += (uint32_t)(ybase + loc[0] + G32(VA_g_view_x));   /* add edx,eax once */
        blit_clip = 0x13be0u; blit_shaded = 0x13cf0u;
    }

    /* blit gate (common to both paths, 0x13b33 / 0x13ba3) */
    int32_t height = loc[0x18 / 4];
    if (loc[1] >= 0) height -= loc[1];
    if (height <= 0) return;                          /* jle done */
    if (ebx_src >= (uint32_t)loc[0x1c / 4]) return;   /* jae done */

    uint8_t shade = (uint8_t)G8(VA_g_viewmodel_shade_level);
    uint32_t blit_va = blit_clip;                     /* clipped (shade==0 or -(shade-0x80)<=0) */
    uint8_t shade_al = 0;                             /* al at the call: 0 (je path) or 0x80-shade */
    if (shade != 0) {
        shade_al = (uint8_t)-(int8_t)(uint8_t)(shade - 0x80);   /* sub al,0x80; neg al */
        if ((int8_t)shade_al > 0)
            blit_va = blit_shaded;                    /* shaded — consumes AL as the LUT shade row */
    }
    regs_t io; memset(&io, 0, sizeof io);
    io.va = blit_va + OBJ_DELTA;
    io.eax = shade_al;                                /* fix: the shaded blitter (0x13cf0/0x13d90)
                                                       * reads AL = clamped shade row; passing 0 drew the
                                                       * dark-sector viewmodel through LUT row 0 */
    io.ebx = ebx_src; io.esi = esi; io.edi = edi; io.edx = (uint32_t)height;
    io.ebp = (uint32_t)(uintptr_t)loc;                /* the blitter reads the descriptor through EBP */
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    switch (blit_va) {   /* the 4-variant viewmodel blitter family (lift_blit_2d.c);
                          * ABI EBX=colrec ESI=src EDI=dst EDX=rows EBP=desc (+AL=shade) */
    case 0x13be0u: blit_transparent_sprite_clipped(ebx_src, esi, edi, height, loc); break;
    case 0x13c60u: blit_transparent_sprite_clipped_x2(ebx_src, esi, edi, height, loc); break;
    case 0x13cf0u: blit_transparent_sprite_clipped_shaded(shade_al, ebx_src, esi, edi, height, loc); break;
    case 0x13d90u: blit_transparent_sprite_clipped_x2_shaded(shade_al, ebx_src, esi, edi, height, loc); break;
    default: roth_unreachable(io.va - OBJ_DELTA);    /* viewmodel sprite blitter — in-game weapon-render tier */
    }
#endif
}

/* ---- render_weapon_view 0x22e7b (1905B; IN-GAME fb) helpers ---- */
/* bridge resolve_reloc_ptr 0x226c6 (EAX=rec slot -> EAX resolved ptr) */
static uint32_t rwv_resolve(uint32_t slot)
{
    return resolve_reloc_ptr(slot);                   /* re-pointed 0x226c6 [L] */
}
/* blit a DAS image into the scratch buffer then row-copy `count` bytes/row for `rows` rows into the
 * back-buffer dest (dest += row_stride/row, src += 0x64/row). img = resolved image ptr; buf = scratch. */
static void rwv_blit_rows(uint32_t img, uint8_t *buf, uint32_t buf_off, uint32_t dest,
                          uint32_t count, uint32_t rows, uint32_t row_stride)
{
    /* re-pointed: blit_das_image_to_buffer 0x1325b [L, das_assets] (img=EAX, dst=EDX, ebx=0x64, ecx=1) */
    blit_das_image_to_buffer(img, (uint32_t)(uintptr_t)buf, 0x64, 1);
    uint32_t src = (uint32_t)(uintptr_t)buf + buf_off;
    uint32_t d = dest;
    for (uint32_t k = 0; k < rows; k++) {
        memcpy_return_dest((uint8_t *)(uintptr_t)d, (const uint8_t *)(uintptr_t)src, count);
        src += 0x64;
        d += row_stride;
    }
}

/* render_weapon_view (0x22e7b, 1905B; IN-GAME fb): EAX=arg1 (the slot count for mode-0). Composites the
 * first-person weapon viewmodel / ammo HUD into the panel surface ([0x83d78][0]) from the panel record
 * rec=[0x83e78]. Dispatches on rec[+0x50] (jump table 0x2354f): mode 1 = scaled single image (Phase A),
 * mode 2 = two-image split (Phase B), mode 0 = arg1<=0 scroll (Phase D) / ==1 ammo-digit grid (Phase C)
 * / else noop, mode 3 = noop. Pixel work in bridged callees (resolve_reloc_ptr / blit_das_image_to_buffer
 * / draw_ui_panel_image_block / clear_buffer_rows) + lifted memcpy_return_dest row-copies. Accumulates the
 * touched region into the dirty-rect [0x83e0c..0x83e18].
 * verify ROTH_LIFT_DIFF=render_weapon_view. */
uint32_t render_weapon_view(uint32_t eax_arg)
{
    uint32_t arg1 = eax_arg;
    uint8_t scratch[0x800];
    uint32_t rec = (uint32_t)G32(VA_g_active_weapon_ammo_cap + 0x4);
    if (rec == 0) goto tail_exit;                            /* je 0x235f7 (no box update) */
    #define R(o) (*(volatile int32_t *)(uintptr_t)(rec + (o)))
    uint32_t surface = *(volatile uint32_t *)(uintptr_t)(uint32_t)G32(VA_g_hud_panel_das_handle1);
    uint32_t stride = (uint32_t)R(0);
    int32_t box_x = R(0x24), box_y = R(0x30);                /* [ebp-0x14]/[ebp-0x10] running x/y */
    int32_t box_w = R(0x2c), box_h = R(0x38);                /* [ebp-0xc]/[ebp-8] running w/h */
    uint32_t mode = (uint32_t)R(0x50);

    if (mode == 1) {                                         /* Phase A: scaled single image */
        uint32_t v = (uint32_t)G32(VA_g_active_weapon_item_id);
        box_w = R(0x2c); box_h = R(0x38);
        if (v > 0x100) v = 0x100;
        uint32_t scaled = (uint32_t)((int32_t)R(0x2c) * (int32_t)v) >> 8;
        uint32_t dest = surface + (uint32_t)(R(0x30) * (int32_t)stride) + (uint32_t)R(0x24);
        if (scaled != 0)
            rwv_blit_rows(rwv_resolve((uint32_t)R(0x1c)), scratch, 0, dest, scaled, (uint32_t)R(0x38), stride);
        dest += scaled;
        uint32_t rem = (uint32_t)R(0x2c) - scaled;
        if (rem != 0)
            rwv_blit_rows(rwv_resolve((uint32_t)R(0x20)), scratch, scaled, dest, rem, (uint32_t)R(0x38), stride);
    } else if (mode == 2) {                                  /* Phase B: two-image split (reversed slots) */
        uint32_t v = (uint32_t)G32(VA_g_active_weapon_item_id);
        box_w = R(0x2c); box_h = R(0x38);
        if (v > 0x100) v = 0x100;
        uint32_t scaled = (uint32_t)((int32_t)R(0x2c) * (int32_t)v) >> 8;
        uint32_t dest = surface + (uint32_t)(R(0x30) * (int32_t)stride) + (uint32_t)R(0x24);
        uint32_t rem = (uint32_t)R(0x2c) - scaled;
        if (rem != 0)
            rwv_blit_rows(rwv_resolve((uint32_t)R(0x20)), scratch, 0, dest, rem, (uint32_t)R(0x38), stride);
        dest += scaled;   /* (mirror of A; the original advances by [ebp-0x430]=scaled before the 2nd) */
        if (scaled != 0)
            rwv_blit_rows(rwv_resolve((uint32_t)R(0x1c)), scratch, scaled, dest, scaled, (uint32_t)R(0x38), stride);
    } else if (mode == 0) {
        if ((int32_t)arg1 <= 0) {
            /* Phase D (0x23408): scroll/wrap one element via [0x83d7c] */
            box_w = R(0x28) * R(0x2c); box_h = R(0x34) * R(0x38);
            box_x = R(0x24); box_y = R(0x30);
            int32_t idx = (int32_t)G32(VA_g_hud_panel_das_handle1 + 0x4) - 1;
            G32(VA_g_hud_panel_das_handle1 + 0x4) = idx;
            if (idx < 0) goto tail_exit;
            if (idx >= R(0x28)) {
                idx -= R(0x28);                              /* 0x2344a: LOCAL wrap for row/col positioning
                                                              * ONLY — the original stores [0x83d7c] just
                                                              * once (0x23424, = idx-1); it does NOT re-store
                                                              * the wrapped value (the prior extra store here
                                                              * over-decremented the linear ammo-scroll
                                                              * counter after a row wrap). */
                if (idx >= R(0x28)) goto tail_exit;
                if (R(0x34) == 1) goto tail_exit;
                box_y += R(0x38);
            }
            box_x += R(0x2c) * idx;
            /* BUGFIX (in-game — HUD ammo frozen while firing): match the disasm branch at
             * 0x23488 (`cmp [rec+0x20],0 ; je 0x234db`) EXACTLY. The two arms ERASE the just-spent cell
             * two different ways:
             *   R(0x20)!=0 (0x23491): resolve R(0x20) = the BLANK-cell image + draw_ui_panel_image_block
             *                         0x227b1 -> overpaints the cell with the blank glyph.
             *   R(0x20)==0 (0x234db): resolve R(0x1c) for its dims only + clear_buffer_rows 0x22760
             *                         -> clears the cell to background, then box_h += 3 (0x23525).
             * The prior lift ALWAYS took the draw_ui_panel_image_block arm and, when R(0x20)==0, drew the
             * R(0x1c) glyph — i.e. it REDREW THE FILLED BULLET instead of clearing it, so the fired round's
             * cell never blanked -> the HUD ammo appeared frozen while firing.
             * Phase C (full rebuild) was unaffected, which is why weapon-switch-away-and-back
             * still refreshed. The wrap-store fix (single [0x83d7c] store) is disasm-correct but was
             * NOT the cause — this clear-vs-draw arm is the per-shot bug. */
            uint32_t img;
            if (R(0x20) != 0) {
                img = rwv_resolve((uint32_t)R(0x20));                /* blank-cell image */
                box_w = (int32_t)(int16_t)*(volatile int16_t *)(uintptr_t)(img + 4);
                box_h = (int32_t)(int16_t)*(volatile int16_t *)(uintptr_t)(img + 6);
                /* re-pointed: draw_ui_panel_image_block 0x227b1 [L] (dest=EAX,img=EDX,x=EBX,y=ECX,pitch=stack) */
                draw_ui_panel_image_block(surface, img, box_x, box_y, stride);
            } else {
                img = rwv_resolve((uint32_t)R(0x1c));                /* R(0x1c) image used for cell dims only */
                box_w = (int32_t)(int16_t)*(volatile int16_t *)(uintptr_t)(img + 4);
                box_h = (int32_t)(int16_t)*(volatile int16_t *)(uintptr_t)(img + 6);
                /* re-pointed: clear_buffer_rows 0x22760 [L, blit_2d] (base=EAX,off=EDX,row0=EBX,stride=ECX,
                 * count=w,rows=h on stack) — ERASES the spent cell to background (0x234db..0x23520). */
                clear_buffer_rows(surface, (uint32_t)box_x, (uint32_t)box_y, stride,
                                         (uint32_t)box_w, (uint32_t)box_h);
                box_h += 3;                                          /* 0x23525 add [ebp-8],3 */
            }
        } else if (arg1 == 1) {
            /* Phase C (0x23252): ammo-digit grid */
            int32_t val = G32(VA_g_active_weapon_item_id);
            if (G32(VA_g_active_weapon_ammo_cap) > 2) val >>= 8;
            uint32_t blank = 0;
            uint32_t digit = R(0x1c) ? rwv_resolve((uint32_t)R(0x1c)) : 0;
            if (R(0x20)) blank = rwv_resolve((uint32_t)R(0x20));
            G32(VA_g_hud_panel_das_handle1 + 0x4) = val;
            for (int32_t row = 0; row < R(0x34); row++) {
                int32_t cx = box_x;                          /* [ebp-0x84c] */
                for (int32_t col = 0; col < R(0x28); col++) {
                    val--;
                    if (val >= 0) {                          /* draw digit */
                        draw_ui_panel_image_block(surface, digit, cx, box_y, stride);  /* re-pointed 0x227b1 [L] */
                    } else if (blank == 0) {                 /* clear */
                        /* re-pointed: clear_buffer_rows 0x22760 [L, blit_2d]
                         * (base=EAX,off=EDX,row0=EBX,stride=ECX,count/rows=stack: img w/h) */
                        clear_buffer_rows(surface, (uint32_t)cx, (uint32_t)box_y, stride,
                            (uint32_t)(int32_t)*(volatile int16_t *)(uintptr_t)(digit + 4),
                            (uint32_t)(int32_t)*(volatile int16_t *)(uintptr_t)(digit + 6));
                    } else {                                 /* draw blank glyph */
                        draw_ui_panel_image_block(surface, blank, cx, box_y, stride);  /* re-pointed 0x227b1 [L] */
                    }
                    cx += R(0x2c);
                }
                box_y += R(0x38);
            }
            box_x = R(0x24); box_y = R(0x30);
            box_w = R(0x28) * R(0x2c); box_h = R(0x34) * R(0x38);
            if (blank == 0) box_h += 3;
        }
        /* arg1 not 0/<=0/1 -> noop tail */
    } else if (mode == 3) {
        /* dispatch table[3] (0x22eb9) = `jmp 0x235f7` -> tail exit, NO dirty-rect accumulation.
         * (fix: the prior fall-through erroneously ran the dirty-rect for mode 3; latent because
         * render_weapon_view was stub-bridged. NB modes >3 DO fall through to the dirty-rect — the
         * original's `cmp mode,3; ja 0x23572` — so only mode==3 tail-exits.) */
        goto tail_exit;
    }
    /* mode >= 4 -> fall through to the dirty-rect accumulation (original: ja 0x2354d -> 0x23572) */

    /* dirty-rect bounding box accumulation (0x23572) */
    if (G32(VA_g_weapon_hud_anim_accum + 0xc) == 0) {
        G32(VA_g_weapon_hud_anim_accum + 0x4) = box_x;
        G32(VA_g_weapon_hud_anim_accum + 0x8) = box_y;
        G32(VA_g_weapon_hud_anim_accum + 0xc) = box_x + box_w;
        G32(VA_g_weapon_hud_anim_accum + 0x10) = box_y + box_h;
    } else {
        if (G32(VA_g_weapon_hud_anim_accum + 0x4) > box_x) G32(VA_g_weapon_hud_anim_accum + 0x4) = box_x;
        int32_t x2 = box_x + box_w;
        if (G32(VA_g_weapon_hud_anim_accum + 0xc) < x2) G32(VA_g_weapon_hud_anim_accum + 0xc) = x2;
        if (G32(VA_g_weapon_hud_anim_accum + 0x8) > box_y) G32(VA_g_weapon_hud_anim_accum + 0x8) = box_y;
        int32_t y2 = box_y + box_h;
        if (G32(VA_g_weapon_hud_anim_accum + 0x10) < y2) G32(VA_g_weapon_hud_anim_accum + 0x10) = y2;
    }
    #undef R
tail_exit:
    return 0;     /* EAX is scratch in the original; callers discard it (ABI_EAX) */
}

/* equip_first_usable_weapon (0x1bd8c): void. Scans the inventory [0x80c30] for the first usable weapon
 * (slot word not high-bit-tagged; has a dbase record whose category nibble (rec[+4]>>8)&0xf == 1; attrs
 * parse [L]; attrs[+8]!=0; and it has ammo — infinite if attrs[+0x31]&0x80, else the slot's +2 count or
 * the player's inventory count of ammo-id attrs[+0x30] via query_player_inventory [inventory bridge]).
 * On finding one, equips it: (if it's the tracked item [0x81044], clear it + update_selected_item_icon
 * [bridge]) reset_weapon_fire_timing [L] + activate_weapon_item [L] + render_weapon_hud [layer-E bridge].
 * Already-equipped ([0x81038]==slot) short-circuits. If none found, reset_weapon_hud [bridge]. Tail-jumps
 * the shared Watcom epilogue (return). */
void equip_first_usable_weapon(void)
{
    int32_t remaining = G32(VA_g_inventory_count);
    uint32_t ecx = (uint32_t)GADDR(VA_g_inventory_slots);
    if (remaining == 0) {                                    /* je 0x1be84 */
        reset_weapon_hud();                           /* re-pointed 0x1be8e [L] */
        return;
    }
    do {
        int32_t esi = *(volatile int16_t *)(uintptr_t)ecx;   /* movsx word[ecx] */
        int skip = 0;
        if (esi == 0) goto advance;                          /* empty slot -> advance (no dec) */
        if ((uint32_t)esi & 0x8000) { skip = 1; goto dec_advance; }  /* test si,0x8000 ; jne */
        {
            uint32_t rec_off = *(volatile uint32_t *)(uintptr_t)((uint32_t)G32(VA_g_dbase100_inventory_table) + (uint32_t)esi * 4);
            if (rec_off == 0) { skip = 1; goto dec_advance; }
            uint32_t recdword = *(volatile uint32_t *)(uintptr_t)(rec_off + (uint32_t)G32(VA_g_dbase100_base) + 4);
            if (((recdword >> 8) & 0xf) != 1) { skip = 1; goto dec_advance; }   /* category != weapon */

            uint8_t local[0x54];
            int32_t edi = *(volatile int16_t *)(uintptr_t)(ecx + 2);   /* movsx word[ecx+2] (apply preserves edi) */
            apply_weapon_action_attributes((uint32_t)esi, (uint32_t)(uintptr_t)local, 1);
            if (local[0x31] & 0x80) {                        /* test byte[ebp-0x23],0x80 -> infinite ammo */
                edi = (int32_t)(((uint32_t)edi & 0x7fff) >> 8);
            } else if (edi == 0) {
                if (*(int32_t *)(local + 0x30) != 0) {       /* ammo id present */
                    /* re-pointed: query_player_inventory 0x1ccf7 [L, inventory] (eax=id, edx=flags) */
                    edi = (int32_t)query_player_inventory(
                        (uint32_t)(int32_t)*(int16_t *)(local + 0x30), 0);
                }
            }
            if (edi == 0) { skip = 1; goto dec_advance; }    /* no ammo */
            if (*(int32_t *)(local + 8) == 0) { skip = 1; goto dec_advance; }   /* attrs[+8]==0 */

            /* found a usable weapon -> equip */
            if (ecx == (uint32_t)G32(VA_g_selected_item_secondary)) return;       /* already equipped -> epilogue */
            if (ecx == (uint32_t)G32(VA_g_selected_item_primary)) {
                G32(VA_g_selected_item_primary) = 0;
                update_selected_item_icon();          /* re-pointed 0x1bb4b [L] (0x81044=0 -> no DAS I/O) */
            }
            reset_weapon_fire_timing();               /* 0x1765c [L] */
            activate_weapon_item(ecx, (uint32_t)esi); /* 0x184ab [L] (eax=slot, edx=item id) */
            /* re-pointed: render_weapon_hud 0x24165 [L] direct-C (mode 1, attrs=&g_active_weapon_attrs).
             * In the equip path activate_weapon_item just ran apply_weapon_action_attributes, which
             * mem_fill-zeroed the 0x811b4 slot, so attrs[+0x48]==0 -> render_weapon_hud takes the BOUNDED
             * "free + rebuild" branch (no save_framebuffer / pool_alloc / render_ui_texture_panel / font
             * work). t_equip_first_usable_weapon zeroes the panel handles 0x83d74/0x83d78/0x83d6c so the
             * frees are no-ops, then runs render_weapon_hud REAL on both sides (byte-verified via the equip
             * write-set: free_hud_panel_das_handles + rebuild_weapon_inventory_list). Cf. the reset_weapon_hud
             * direct site (:936). */
            render_weapon_hud(1, (uint32_t)GADDR(VA_g_active_weapon_attrs));
            return;
        }
    dec_advance:
        if (skip) remaining--;                               /* 0x1be74: dec [ebp-4] */
    advance:
        ecx += 4;                                            /* 0x1be77: add ecx,4 */
    } while (remaining != 0);                                /* cmp [ebp-4],0 ; ja */
    reset_weapon_hud();                               /* re-pointed 0x1be8e [L] (none found) */
}
