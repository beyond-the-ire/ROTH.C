/* lift_dialogue_ui.c — verified-C lifts for the `dialogue_ui` subsystem.
 *
 * ROTH's on-screen inspect popup, the topic-choice menu, the monologue / timed (subtitle)
 * text panels, and the cursor hit-test/dispatch. See
 * docs/reference/lift/dialogue_ui.md. (ROTH has NO NPC dialogue panels — "dialogue" =
 * inspect choices + player monologue + cutscene subtitles.)
 *
 * ABI / behavior transcribed STRICTLY FROM THE DISASM (tools/roth_disasm.py). The corpus
 * decompile is used for intent only; several places it mis-models the Watcom multi-reg
 * returns (e.g. eval_dialogue_record_condition_with_cleanup's "extraout_EBX/extraout_ECX" —
 * disasm proves it PRESERVES both, so those are the caller's own preserved input regs).
 *
 * Globals this subsystem reads/writes (canon VAs; runtime = canon + OBJ_DELTA):
 *   g_choice_selected_index      0x71370  int  (-1 = none selected)
 *   g_dialogue_segments          0x827fd  array, stride 0x14 (5 dwords); +0x10 = record ptr
 *   g_choice_line_records        0x83189  array, stride 0x14 (the built choice lines)
 *   g_choice_text_buffer         0x832c9
 *   g_subtitle_center_lo         0x83135  int
 *   g_choice_marker_normal       0x83139  byte  (repaint-clear marker for segments path)
 *   g_choice_saved_index         0x8313d  int   (committed selection)
 *   g_active_dialogue_context    0x83115  int   (also = live dialogue-segment count)
 *   g_move_freeze_gate           0x83125  int   (0x6ffff while a panel/monologue shows)
 *   g_choice_option_records      0x83149  array of source-record ptrs (1-based lookup)
 *   g_subtitle_x_origin          0x83add  int
 *   g_dialogue_busy_flag         0x83aea  int
 *   g_choice_line_count          0x83ac9  int
 *   g_choice_interaction_mode    0x83acd  int
 *   g_choice_marker_normal_alt   0x83ae9  byte  (repaint-clear marker for choice-lines path)
 *   g_choice_last_pos            0x83b16  int
 *   g_pending_choice_accept_index 0x81304 int
 *   g_screen_pitch               0x85498  int
 *   g_subtitle_center_hi         0x8549c  int
 *   g_map_menu_marker_selected   0x7675d  byte  (the highlight marker written on the active line)
 *
 * Bridged callees (lifted elsewhere / host):
 *   execute_dialogue_branch                     0x1dc02 (dbase100_interpreter)
 *   layout_timed_message_text                   0x1f3d3 (this TU, sub-B)
 * Re-pointed to direct C:
 *   free_das_cache_handle    0x13136 (das_assets), read_next_dialogue_line 0x1e8cc (dbase100),
 *   eval_dialogue_record_condition_with_cleanup 0x1db89 (dbase100; proto extended with ebx_io/ecx_io
 *   pass-through out-params — the dlg_eval_cond helper; NB the dlg_call4(0x1db89) action-2 site at the
 *   bottom of this TU is still a call_orig bridge)
 */
#include <stdint.h>
#include <string.h>
#include "common.h"

/* raw flat read/write (records are relocatable runtime pointers — deref RAW, gotcha A4). */
#define RB(a)    (*(volatile uint8_t  *)(uintptr_t)(a))
#define RW(a)    (*(volatile uint16_t *)(uintptr_t)(a))
#define RD(a)    (*(volatile uint32_t *)(uintptr_t)(a))
#define WB(a,v)  (*(volatile uint8_t  *)(uintptr_t)(a) = (uint8_t)(v))
#define WW(a,v)  (*(volatile uint16_t *)(uintptr_t)(a) = (uint16_t)(v))
#define WD(a,v)  (*(volatile uint32_t *)(uintptr_t)(a) = (uint32_t)(v))

/* re-pointed: eval_dialogue_record_condition_with_cleanup 0x1db89 [L, dbase100] direct-C. The proto was
 * extended with ebx_io/ecx_io out-params (proto-extension wave). Disasm proves EBX and ECX are PRESERVED
 * end-to-end (0x1daea push/pops both around its body; the finish tail 0x1db5e's optional call 0x20905
 * ALSO push/pops ebx/ecx/edx; 0x1db89 itself only writes EAX/EDX) — so what the corpus models as
 * "extraout_EBX/extraout_ECX" is just THIS caller's own preserved input. The out-params are pure
 * pass-through: pre-load the ebx/ecx cells with the input and read them back unchanged. */
static int32_t dlg_eval_cond(uint32_t record, uint32_t mode, uint32_t *ebx, uint32_t *ecx)
{
    return (int32_t)eval_dialogue_record_condition_with_cleanup(record, mode, ebx, ecx);
}

/* ============================ node_has_available_choice (0x1fa91) ============================
 * EAX = node record ptr. Returns 1 if the node has an immediately-available choice/action:
 *   - node+5 bit 0x10 set                          -> 1 (immediate)
 *   - node+4 bit 0x08 clear                         -> 0 (no variable records)
 *   - else scan variable records at node+0x14; a type-1 or type-4 record whose condition
 *     (eval_dialogue_record_condition_with_cleanup(rec,2)) is true -> 1; run to end -> 0.
 * The original preserves EDX (=param_2) via push/pop; the boolean is in EAX. */
int32_t node_has_available_choice(uint32_t node)
{
    if (RB(node + 5) & 0x10)
        return 1;
    if (!(RB(node + 4) & 8))
        return 0;

    uint32_t p = node + 0x14;                 /* puVar4 */
    for (;;) {
        uint32_t word = RD(p);                /* ecx = *puVar4 */
        int32_t  hi   = (int32_t)word >> 24;  /* sar eax,0x18 */
        if (hi == 1 || hi == 4) {
            uint32_t ebx = p, ecx = word;
            int32_t r = dlg_eval_cond(p, 2, &ebx, &ecx);
            if (r != 0)
                return 1;
            word = ecx;                       /* extraout_ECX (record word for advance) */
            p    = ebx;                       /* extraout_EBX (record ptr)              */
        }
        uint32_t lo = word & 0xffff;
        p += (lo >> 2) << 2;                  /* advance by (lo>>2) dwords */
        if (lo == 0)
            return 0;
    }
}

/* ---- shared repaint tail of choice_select_prev / _next (physically at 0x1fb4d..0x1fb19).
 * Clears the old highlight marker off every active choice line, then stamps
 * g_map_menu_marker_selected onto the newly-selected line. Chooses the segment list
 * (g_dialogue_segments / context count / g_choice_marker_normal) unless no dialogue is
 * active and interaction-mode==1, in which case it repaints the built choice lines
 * (g_choice_line_records / g_choice_line_count / g_choice_marker_normal_alt). */
static void dlg_repaint_selected(void)
{
    uint32_t base   = GADDR(VA_g_dialogue_segments);         /* &g_dialogue_segments */
    int32_t  count  = G32(VA_g_active_dialogue_context);           /* g_active_dialogue_context */
    uint8_t  marker = G8(VA_g_dialogue_reveal_ramp + 0x10);            /* g_choice_marker_normal */

    if (count == 0) {                         /* context == 0 */
        if (G32(VA_g_choice_interaction_mode) == 1) {              /* interaction mode == 1 */
            base   = GADDR(VA_g_choice_line_records);          /* &g_choice_line_records */
            count  = G32(VA_g_choice_line_count);            /* g_choice_line_count */
            marker = G8(VA_g_choice_reveal_ramp + 0x4);             /* g_choice_marker_normal_alt */
        }
        if (count == 0)
            return;
    }

    int32_t sel = G32(VA_g_choice_selected_index);
    G32(VA_g_dialogue_reveal_ramp + 0x14) = sel;                       /* g_choice_saved_index = sel */

    uint32_t *arr = (uint32_t *)base;
    for (int32_t i = 0; i < count; i++) {
        uint32_t rec = arr[i * 5 + 4];        /* puVar1[4] */
        if (RB(rec) == 1)
            WB(rec + 1, marker);
    }
    uint32_t rec = arr[sel * 5 + 4];
    WB(rec + 1, G8(VA_g_map_menu_marker_selected));                 /* g_map_menu_marker_selected */
}

/* ============================ choice_select_prev (0x1fb1e) ============================
 * Move the choice selection UP one (wrap to last), then repaint. No-op while a dialogue
 * is active but the move-freeze gate isn't engaged. */
void choice_select_prev(void)
{
    int32_t ctx = G32(VA_g_active_dialogue_context);               /* g_active_dialogue_context */
    int32_t count;
    if (ctx != 0) {
        count = ctx;
        if (G32(VA_g_move_freeze_gate) != 0x6ffff)
            return;
    } else {
        count = G32(VA_g_choice_line_count);                 /* g_choice_line_count */
    }

    int32_t sel = G32(VA_g_choice_selected_index);
    if (sel == -1) {
        G32(VA_g_choice_selected_index) = 0;
    } else {
        sel = sel - 1;
        G32(VA_g_choice_selected_index) = sel;
        if (sel < 0)
            G32(VA_g_choice_selected_index) = count - 1;
    }
    dlg_repaint_selected();
}

/* ============================ choice_select_next (0x1fc16) ============================
 * Move the choice selection DOWN one (wrap to 0), then repaint. */
void choice_select_next(void)
{
    int32_t ctx = G32(VA_g_active_dialogue_context);
    int32_t count;
    if (ctx != 0) {
        count = ctx;
        if (G32(VA_g_move_freeze_gate) != 0x6ffff)
            return;
    } else {
        count = G32(VA_g_choice_line_count);
    }

    int32_t sel = G32(VA_g_choice_selected_index);
    if (sel == -1) {
        G32(VA_g_choice_selected_index) = 0;
    } else {
        sel = sel + 1;
        G32(VA_g_choice_selected_index) = sel;
        if ((uint32_t)count <= (uint32_t)sel)
            G32(VA_g_choice_selected_index) = 0;
    }
    dlg_repaint_selected();
}

/* ============================ activate_selected_choice_record (0x1badd) ============================
 * EAX = 1-based visible choice index. Maps it through g_choice_option_records, clears the
 * choice-line count, evaluates the selected record (mode 0), and sets interaction mode 2. */
void activate_selected_choice_record(uint32_t idx)
{
    if (idx == 0)
        return;
    uint32_t off = (idx - 1) * 4;
    uint32_t rec = RD(GADDR(VA_g_choice_option_records) + off);  /* g_choice_option_records[idx-1] */
    if (rec == 0)
        return;
    G32(VA_g_choice_line_count) = 0;                         /* g_choice_line_count = 0 */
    uint32_t ebx = rec, ecx = 0;
    dlg_eval_cond(rec, 0, &ebx, &ecx);        /* eval(record, 0) */
    G32(VA_g_choice_interaction_mode) = 2;                         /* g_choice_interaction_mode = 2 */
}

/* ============================ update_dialogue_choice_highlight (0x1f71d) ============================
 * EAX=param_1 (playback position), EDX=param_2 (cursor offset within the line). Syncs the
 * highlighted subtitle/choice line to the input/playback position while a dialogue line is
 * active (g_dialogue_busy_flag) and the freeze gate is engaged (0x6ffff). Returns 0 (not
 * active), 1 (a hit re-stamped the highlight), or 2 (active but no hit / already selected). */
/* ============================ choice_accept_selected (0x1fbba) ============================
 * Enter/confirm the highlighted choice: records the pending accept index (menu path), clears
 * the dialogue/choice state, and tail-calls execute_dialogue_branch(g_choice_saved_index) to
 * run the selected branch. Side-effecting (fires a dbase100 chain) -> in-game; the oracle
 * ret-stubs execute_dialogue_branch to verify the state-machine writes. */
void choice_accept_selected(void)
{
    if (G32(VA_g_choice_selected_index) == -1)                    /* g_choice_selected_index */
        return;
    if (G32(VA_g_active_dialogue_context) == 0 && G32(VA_g_choice_interaction_mode) == 1)
        G32(VA_g_pending_choice_accept_index) = G32(VA_g_choice_selected_index) + 1;       /* g_pending_choice_accept_index = sel+1 */
    G32(VA_g_active_dialogue_context) = 0;                          /* g_active_dialogue_context */
    G32(VA_g_move_freeze_gate) = 0;                          /* g_move_freeze_gate */
    G32(VA_g_dialogue_busy_flag) = 0;                          /* g_dialogue_busy_flag */
    int32_t saved = G32(VA_g_dialogue_reveal_ramp + 0x14);              /* eax = g_choice_saved_index (loaded before sel=-1) */
    G32(VA_g_choice_selected_index) = -1;                         /* g_choice_selected_index = -1 */

    execute_dialogue_branch((uint32_t)saved);   /* tail-call 0x1dc02 — lifted, direct */
}

/* ============================ update_dialogue_choice_highlight (0x1f71d) ============================ */
uint32_t update_dialogue_choice_highlight(int32_t param_1, int32_t param_2)
{
    if (G32(VA_g_dialogue_busy_flag) == 0 || G32(VA_g_move_freeze_gate) != 0x6ffff)
        return 0;

    uint8_t marker_normal = G8(VA_g_dialogue_reveal_ramp + 0x10);
    int32_t pos = param_1 + param_2;
    if (pos != G32(VA_g_dialogue_busy_flag + 0x2c))
        G32(VA_g_choice_selected_index) = -1;                    /* g_choice_selected_index = -1 */
    G32(VA_g_dialogue_busy_flag + 0x2c) = pos;                       /* g_choice_last_pos = pos */

    if (G32(VA_g_choice_selected_index) != -1)
        return 2;

    uint32_t *seg = (uint32_t *)GADDR(VA_g_dialogue_segments);
    int32_t ctx = G32(VA_g_active_dialogue_context);
    /* pass 1: clear the old highlight off every type-1 active segment line */
    for (int32_t i = 0; i < ctx; i++) {
        uint32_t rec = seg[i * 5 + 4];
        if (RB(rec) == 1)
            WB(rec + 1, marker_normal);
    }

    /* pass 2: region-test the playback position against each segment; stamp the first hit */
    int32_t iVar8 = (param_1 + 0x28) - G32(VA_g_choice_interaction_mode + 0x10);
    uint32_t uVar6 = (uint32_t)(G32(VA_g_screen_pitch + 0x4) - G32(VA_g_dialogue_reveal_ramp + 0xc)) >> 1;
    uint32_t *p = (uint32_t *)GADDR(VA_g_dialogue_segments);
    for (int32_t i = 0; i < ctx; i++) {
        if (param_2 >= (int32_t)uVar6 && param_2 <= (int32_t)(p[2] + uVar6) &&
            iVar8 >= (int32_t)p[0] && iVar8 <= (int32_t)p[1]) {
            uint32_t rec = p[4];
            WB(rec + 1, G8(VA_g_map_menu_marker_selected));          /* g_map_menu_marker_selected */
            G32(VA_g_choice_selected_index) = -1;
            G32(VA_g_dialogue_reveal_ramp + 0x14) = i;                  /* g_choice_saved_index = i */
            return 1;
        }
        uVar6 = uVar6 + p[2] + 4;
        p += 5;
    }
    return 2;
}

/* ===================== timed-message text cache (sub-B) =====================
 * A 32-node MRU cache of recently-shown timed messages, head ptr at g_message_cache_head
 * 0x820c1; nodes are 0x20 bytes: +0 next, +4 key, +8 len(word), +0xa color(byte), +0xc text.
 * init_freelist_820c1 0x1e792 [L] builds the free list. */

/* ============================ lookup_cached_timed_message (0x1e7c3) ============================
 * EAX=dest buffer, EDX=key -> EAX=length (0 if absent). On a hit: MRU-move the node to the
 * front, set g_timed_message_color from node+0xa, copy node's text into dest. */
int32_t lookup_cached_timed_message(uint32_t dest, int32_t key)
{
    uint32_t head = G32(VA_g_message_cache_head);
    if (head == 0) {
        init_freelist_820c1();
        return 0;
    }
    uint32_t prev = GADDR(VA_g_message_cache_head);           /* &g_message_cache_head */
    uint32_t node = head;
    for (;;) {
        if (key == (int32_t)RD(node + 4)) {
            if (node != (uint32_t)G32(VA_g_message_cache_head)) {        /* not head -> move to front */
                uint32_t oldhead = (uint32_t)G32(VA_g_message_cache_head);
                uint32_t next    = RD(node);
                G32(VA_g_message_cache_head) = (int32_t)node;            /* head = node */
                WD(node, oldhead);                       /* node.next = old head */
                WD(prev, next);                          /* prev.next = node.next (unlink) */
            }
            G8(VA_g_timed_message_color) = RB(node + 0xa);                /* g_timed_message_color = node.color */
            uint32_t src = node + 0xc;
            int32_t  len = (int16_t)RW(node + 8);
            for (int32_t i = 0; i < len; i++)
                WB(dest + i, RB(src + i));
            return len;
        }
        prev = node;
        node = RD(node);
        if (node == 0)
            return 0;
    }
}

/* ============================ store_cached_timed_message (0x1e827) ============================
 * EAX=text src, EDX=key, EBX=length, CL=color. Reuses the oldest (tail) node, moves it to the
 * front, and stores key/color/length + the text bytes at node+0xc. */
void store_cached_timed_message(uint32_t text, int32_t key, int32_t len, uint8_t color)
{
    uint32_t oldhead = (uint32_t)G32(VA_g_message_cache_head);
    uint32_t node    = oldhead;
    uint32_t prev    = GADDR(VA_g_message_cache_head);        /* &g_message_cache_head */
    while (RD(node) != 0) {                    /* walk to the tail (oldest) */
        prev = node;
        node = RD(node);
    }
    WD(prev, 0);                               /* prev.next = 0 (unlink tail) */
    G32(VA_g_message_cache_head) = (int32_t)node;              /* head = tail */
    WD(node, oldhead);                         /* node.next = old head */
    WW(node + 8, (uint16_t)len);               /* len (word, from BX) */
    WB(node + 0xa, color);                     /* color (CL) */
    WD(node + 4, (uint32_t)key);               /* key */
    uint32_t dst = node + 0xc;
    for (int32_t i = 0; i < len; i++)          /* ECX=EBX bytes copied */
        WB(dst + i, RB(text + i));
}

/* ============================ layout_timed_message_text (0x1f3d3) ============================
 * EAX=param_1 line-meta array (int, stride 0x14), EDX=param_2 output text buffer, EBX=param_3
 * source ASCII, ECX=param_4 max pixel width, stack=param_5 max lines. Emits control-coded,
 * word-wrapped + centered line records; per line: [1][color] header, [2][xlo][xhi] x-offset,
 * then the glyph bytes, terminated by 0. Doubled '^' -> newline. Returns the line count.
 * measure_font_char_advance 0x1508a is the verified text_font lift (clobbers only EAX).
 * Transcribed strictly from the disasm — the decompile's extraout_ECX/EDX/EBX are spurious
 * (measure preserves every register but EAX). */
int32_t layout_timed_message_text(int32_t *param_1, uint8_t *param_2,
                                         const uint8_t *param_3, int32_t param_4, int32_t param_5)
{
    int32_t *meta = param_1;                  /* esi */
    const uint8_t *src = param_3;             /* ebx */
    int32_t  maxw_const = param_4;            /* [ebp-0x38] */
    int32_t  textw      = param_4 - 0xc;      /* [ebp-0x20] */
    int32_t  run_width  = 0;                  /* [ebp-0xc]  local_18 */
    int32_t  wrap_cnt   = 0;                  /* [ebp-0x10] local_1c */
    int32_t  height     = 0;                  /* [ebp-0x14] local_20 */
    int32_t  min_x      = param_4;            /* [ebp-0x18] local_24 */
    int32_t  max_w      = 0;                  /* [ebp-0x1c] local_28 */
    int32_t  line_cnt   = 1;                  /* [ebp-0x24] local_30 */
    int32_t  brk_width  = 0;                  /* [ebp-0x28] local_34 */
    int32_t  brk_lw     = 0;                  /* [ebp-0x34] local_40 */
    int32_t  last_adv   = 0;                  /* [ebp-0x40] */
    int32_t  centerx    = 0;                  /* [ebp-8]    local_8 */
    uint8_t *line_start = param_2;            /* [ebp-0x2c] local_38 */
    const uint8_t *brk_src = param_3;         /* [ebp-0x3c] local_48 */
    uint8_t *brk_out = param_2;               /* edi */
    uint8_t  ch = 0;                          /* [ebp-4]    local_10 */
    int32_t  cur;                             /* [ebp-0x30] uVar3 */

    /* first-line header: [1][color], then x-offset control [2][0][0] */
    uint8_t *wr = param_2;                    /* edx */
    *wr = 1; wr += 2; wr[-1] = G8(VA_g_timed_message_color);
    *wr = 2;
    uint8_t *xctl = wr;                       /* ecx */
    wr[1] = 0; wr[2] = 0; wr += 3;

    for (;;) {
        ch = *src; src++;
        if (ch == 0x5e && *src == 0x5e) { ch = 0xa; src++; }
        if (ch == 0xa) {
            if (run_width == 0) { ch = *src; src++; }
            else { brk_out = wr; brk_src = src; brk_width = run_width; }
        }
        cur = (uint8_t)ch;

        if (cur <= 9) {                       /* control-char path (0x1f5d9) */
            if (cur == 1) {
                *wr = ch; wr++;
                *wr = *src; src++; wr++;
            }
            if (ch == 0) break;               /* end-check (skips space-check) */
            continue;
        }

        last_adv = (int32_t)measure_font_char_advance((uint32_t)cur);
        run_width += last_adv;

        if (run_width >= textw || cur == 0xa) {   /* WRAP (0x1f495) */
            brk_lw = brk_width;
            src = brk_src;
            if (brk_width == 0)
                return 0;
            wr = brk_out + 1;
            if (cur == 0xa) {
                wrap_cnt = 0;
                *brk_out = 0;
            } else if (line_cnt < 0xe && param_5 <= wrap_cnt + 1) {
                wrap_cnt = 0;
                brk_out[0] = 0x2e; brk_out[1] = 0x2e; brk_out[2] = 0x2e; brk_out[3] = 0;
                wr = brk_out + 4;
                brk_lw = brk_width + 6;
            } else {
                wrap_cnt++;
                *brk_out = 0xa;
            }

            centerx = (maxw_const - brk_lw) >> 1;       /* sar */
            if (min_x > centerx) min_x = centerx;
            xctl[1] = (uint8_t)centerx;
            xctl[2] = (uint8_t)(centerx >> 8);
            ch = 0x61;                                   /* sentinel: skip space/end checks */
            centerx += brk_lw + 2;
            if (max_w < centerx) max_w = centerx;
            height += 0xb;

            if (wrap_cnt == 0) {                         /* emit a completed line record */
                meta[0] = min_x;
                meta[1] = max_w + 1;
                meta[2] = height;
                meta[4] = (int32_t)(uintptr_t)line_start;
                meta[3] = (int32_t)(wr - line_start);
                line_cnt++;
                meta += 5;
                max_w = 0;
                height = 0;
                line_start = wr;
                *wr = 1; wr += 2; wr[-1] = G8(VA_g_timed_message_color); /* new line header */
                min_x = textw;
            }
            /* 0x1f599: reset per-line state + new x-offset control */
            run_width = 0;
            *wr = 2;
            brk_width = 0;
            wr[1] = 0; wr[2] = 0;
            xctl = wr;
            wr += 3;
            continue;
        }

        /* emit glyph (0x1f5b9) */
        *wr = ch; wr++;

        /* space breakpoint (0x1f5bf) */
        if (cur == 0x20) {
            brk_out = wr - 1;
            brk_width = run_width - last_adv;
            brk_src = src;
        }
        if (ch == 0) break;                    /* end-check */
    }

    /* flush the final partial line (0x1f5f4) */
    if (run_width != 0) {
        height += 0xb;
        centerx = (maxw_const - run_width) >> 1;
        if (min_x > centerx) min_x = centerx;
        xctl[1] = (uint8_t)centerx;
        xctl[2] = (uint8_t)(centerx >> 8);
        centerx += run_width + 2;
        if (max_w < centerx) max_w = centerx;
    } else if (wrap_cnt == 0) {
        line_cnt--;
    }

    /* final line-meta record + terminator (0x1f643) */
    meta[0] = min_x;
    meta[1] = max_w + 1;
    meta[2] = height;
    meta[4] = (int32_t)(uintptr_t)line_start;
    meta[3] = (int32_t)(wr - line_start);
    *wr = 0;
    return line_cnt;
}

/* ===================== inspect popup leaves (sub-D) ===================== */

/* ============================ copy_record_block_op7 (0x1854b) ============================
 * EAX=record, EDX=dest, EBX=cap. Walks the record's trigger-block list (record+0x14, each
 * {opcode<<24 | size24}; gated by record+4 bit 2), copies the first opcode-7 block's payload
 * (min(size/4 - 1, cap) dwords) into dest. Returns the dword count copied. Leaf. */
uint32_t copy_record_block_op7(uint32_t record, uint32_t dest, uint32_t cap)
{
    if (!(RB(record + 4) & 2))
        return 0;
    uint32_t p = record + 0x14;
    for (;;) {
        uint32_t word = RD(p);
        uint32_t size = word & 0xffffff;
        if (size == 0)
            return 0;
        uint32_t nd = size >> 2;
        if ((word >> 24) == 7) {
            uint32_t count = nd - 1;
            if (count > cap)
                count = cap;
            uint32_t bytes = count << 2;
            uint32_t s = p + 4;
            for (uint32_t i = 0; i < bytes; i++)
                WB(dest + i, RB(s + i));
            return count;
        }
        p += nd << 2;
    }
}

/* ============================ find_oninspect_block_by_id (0x1ddeb) ============================
 * EAX=def/template id. Resolves the def through g_dbase100_inventory_table 0x81e20 +
 * g_dbase100_base 0x81e1c (matching record+2 == id, id >= 0x200), then scan_tag4_chunk to
 * locate its trigger-0x04 OnInspect block. 1-entry (id/result) cache at 0x81efa/0x81efe.
 * scan_tag4_chunk 0x1dda8 is the verified lift (called directly). */
uint32_t find_oninspect_block_by_id(uint32_t id)
{
    uint32_t table = (uint32_t)G32(VA_g_dbase100_inventory_table);      /* g_dbase100_inventory_table */
    if (id == (uint32_t)G32(VA_g_dbase100_choice_record_indices + 0x44))
        return (uint32_t)G32(VA_g_dbase100_choice_record_indices + 0x48);            /* cache hit */
    G32(VA_g_dbase100_choice_record_indices + 0x44) = 0;
    G32(VA_g_dbase100_choice_record_indices + 0x48) = 0;
    if ((int32_t)id < 0x200)
        return 0;
    uint32_t p = table;
    for (int32_t i = 0; ; i++) {
        uint32_t base = (uint32_t)G32(VA_g_dbase100_base);   /* g_dbase100_base (reloaded each iter) */
        if (i >= (int32_t)RD(base + 0x10))
            return 0;
        p += 4;
        if (RD(p) == 0)
            continue;
        uint32_t rec = base + RD(p);
        if ((int32_t)(int16_t)RW(rec + 2) == (int32_t)id)
            return scan_tag4_chunk(rec);
    }
}

/* ============================ hit_test_dialogue_ui_action (0x1ad2f) ============================
 * EAX=p1 (cursor col), EDX=p2 (cursor row), EBX=p3 (flags). Pure leaf: region-tests the cursor
 * against the active UI (scroll-drag, inspect-popup grid cells, choice lines, or the held-item
 * slot table) and returns an action code; also repaints the choice-line highlight. Reads live
 * cursor coords g_cursor_col 0x707b3 / g_cursor_row 0x707b7 and many UI-state globals.
 * Transcribed from disasm (no callees). */
int32_t hit_test_dialogue_ui_action(int32_t p1, int32_t p2, uint32_t p3)
{
    int32_t esi = p1, ecx = p2;                 /* esi=p1, ecx=p2 */
    G32(VA_g_inventory_synthetic_primary) = 0;
    if (G32(VA_g_dialogue_busy_flag) != 0)                      /* g_dialogue_busy_flag */
        return 0x27;

    if (p3 & 8) {                               /* scroll-drag (bl & 8) */
        if (G32(VA_g_inventory_ui_action) == 1) {
            int32_t idx = G32(VA_g_cursor_active_list);
            int32_t d = RD(GADDR(VA_g_cursor_list_positions) + idx * 4) - RD(GADDR(VA_g_cursor_scroll_offsets) + idx * 4);
            G32(VA_g_inventory_ui_action) = 0;
            if ((uint32_t)d < 0xa)
                return d + 0x1c;
        }
        if (G32(VA_g_inventory_ui_action) == 2) {
            int32_t idx = G32(VA_g_cursor_active_list);
            int32_t d = RD(GADDR(VA_g_cursor_list_positions) + idx * 4);
            G32(VA_g_inventory_ui_action) = 0;
            d -= RD(GADDR(VA_g_cursor_scroll_offsets) + idx * 4);
            G32(VA_g_inventory_synthetic_primary) = 1;
            if ((uint32_t)d < 0xa)
                return d + 0x1c;
        }
        G32(VA_g_inventory_ui_action) = 0;
        return 0;
    }

    /* not a scroll-drag */
    if (G32(VA_g_inspect_popup_active) == 0) {                     /* inspect popup NOT active: bounds-check */
        if (G32(VA_g_mouse_y) < G32(VA_g_ui_panel_anchor_y)) return 0x26;
        if (G32(VA_g_mouse_x) < G32(VA_g_ui_panel_anchor_x)) return 0x26;
        if (G32(VA_g_ui_panel_anchor_y) + 0x50 < G32(VA_g_mouse_y)) return 0x26;
        if (G32(VA_g_ui_panel_anchor_x) + 0x120 < G32(VA_g_mouse_x)) return 0x26;
    }
    if (G32(VA_g_inspect_popup_active) == 0) {                     /* still not active -> slot-table scan */
        uint32_t ep = GADDR(VA_g_inventory_tab_context_map + 0x5);            /* entry-pointer list */
        uint32_t entry = RD(ep);
        ep += 4;
        while (entry != 0) {
            uint32_t slot = entry;
            while (RW(slot) != 0) {              /* slot.x0 word terminator */
                int32_t dx = esi - (int16_t)RW(slot);
                if ((uint32_t)RB(slot + 4) >= (uint32_t)dx) {
                    int32_t dy = ecx - (int16_t)RW(slot + 2);
                    if ((uint32_t)dy <= (uint32_t)RB(slot + 5)) {
                        if (G32(VA_g_cursor_active_list) == 2 && G32(VA_g_current_cursor_entry) != 0) {
                            int32_t a = (int16_t)RW(slot + 6);
                            if (a == 0x2a || a == 0x2b)
                                return 0;
                        }
                        return (int16_t)RW(slot + 6);
                    }
                }
                slot += 8;
            }
            entry = RD(ep);
            ep += 4;
        }
        return 0;
    }

    /* inspect popup active */
    if (G32(VA_g_choice_interaction_mode) == 0 && G32(VA_g_inspect_info_available) != 0)  /* mode 0 + info available -> info button */
        return 4;

    if (G32(VA_g_choice_interaction_mode) == 1) {                     /* choice mode: repaint + line hit-test */
        uint8_t marker_alt = G8(VA_g_choice_reveal_ramp + 0x4);
        if (G32(VA_g_choice_selected_index) == -1) {
            uint32_t edx = 0;
            for (int32_t edi = 0; edi < G32(VA_g_choice_line_count); edi++) {
                uint32_t rec = RD(GADDR(VA_g_choice_line_records + 0x10) + edx);
                if (RB(rec) == 1)
                    WB(rec + 1, marker_alt);
                edx += 0x14;
            }
        }
        int32_t lim = G32(VA_g_choice_line_count) * 0x14;
        int32_t edx = G32(VA_g_choice_interaction_mode + 0x14) - (G32(VA_g_choice_interaction_mode + 0xc) >> 1);
        int32_t adj = esi - G32(VA_g_choice_interaction_mode + 0x10);
        int32_t li = 0;
        for (int32_t eax = 0; eax < lim; eax += 0x14, li++) {
            int32_t ok = (adj >= (int32_t)RD(GADDR(VA_g_choice_line_records) + eax)) &&
                         (adj <= (int32_t)RD(GADDR(VA_g_choice_line_records + 0x4) + eax)) &&
                         (ecx >= edx) &&
                         ((uint32_t)(ecx - edx) <= (uint32_t)RD(GADDR(VA_g_choice_line_records + 0x8) + eax));
            if (ok) {
                uint32_t rec = RD(GADDR(VA_g_choice_line_records + 0x10) + eax);
                if (RB(rec) == 1)
                    WB(rec + 1, G8(VA_g_map_menu_marker_selected));
                G32(VA_g_choice_selected_index) = -1;
                return li + 7;
            }
            /* miss: accumulate this line's height (disasm reads [eax+0x14+0x8317d] = [eax+0x83191]) */
            edx += (int32_t)RD(GADDR(VA_g_choice_line_records + 0x8) + eax);
        }
        /* fall through to grid-cell scan */
    }

    /* grid-cell scan (6 cells @ 0x80b40, stride 8) */
    for (int32_t eax = 0, edx = 0; eax < 0x30; eax += 8, edx++) {
        if (RW(GADDR(VA_g_inspect_popup_layout) + eax) == 0)
            continue;
        int32_t cx = ecx - (uint16_t)RW(GADDR(VA_g_inventory_inspect_request + 0x4) + eax);
        if ((uint32_t)cx >= (uint32_t)(uint16_t)RW(GADDR(VA_g_inspect_popup_layout) + eax))
            continue;
        int32_t cy = esi - (uint16_t)RW(GADDR(VA_g_inspect_popup_layout + 0x2) + eax);
        if ((uint32_t)(uint16_t)RW(GADDR(VA_g_inspect_popup_layout + 0x4) + eax) <= (uint32_t)cy)
            continue;
        if (edx == 1 && G32(VA_g_dialogue_busy_flag) != 0)
            return 0;
        return edx + 1;
    }
    return 0;
}

/* ---- generic bridge to an original callee (threads no output regs; for void side-effect
 *      callees like the DAS-free / dirty-rect / framebuffer helpers). ---- */
static void dlg_call4(uint32_t canon, uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    if (canon == 0x15b5bu) {                    /* register_dirty_rect — lifted, direct (all sites) */
        register_dirty_rect(eax, (int32_t)edx, ebx, ecx);
        return;
    }
    regs_t io; memset(&io, 0, sizeof io);
    io.va = canon + OBJ_DELTA;
    io.eax = eax; io.edx = edx; io.ebx = ebx; io.ecx = ecx;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    switch (canon) {   /* routes into the verified lifted bodies (void context — returns dropped).
                        * Args register-positional (EAX,EDX,EBX,ECX) unless the proto comment differs. */
    /* void, no-arg */
    case 0x1e9b5u: voice_stream_pump(); return;
    case 0x1661fu: handle_cursor_click(); return;
    case 0x15dd9u: flush_dirty_rects(); return;
    case 0x1f330u: mark_overlay_dirty_rects(); return;
    case 0x20a8au: present_cutscene_frame(); return;
    case 0x1fc5cu: clear_cutscene_region(); return;
    case 0x1a2d2u: draw_inventory_tabs(); return;
    case 0x1a2efu: draw_equipped_item_left(); return;
    case 0x1bfaau: draw_equipped_item_right(); return;
    case 0x1a2b5u: select_inventory_tab(); return;
    case 0x1c469u: refresh_inventory_grid(); return;
    case 0x1a88bu: commit_held_cursor_item(); return;
    case 0x1b141u: use_item_on_self(); return;   /* body reads g_current_cursor_entry; site EAX is dead */
    /* EAX only */
    case 0x12a08u: set_cursor_shape(eax); return;
    case 0x19d30u: build_inventory_entry_list(eax); return;
    case 0x19ee6u: draw_panel_slot_tile(eax); return;
    case 0x1c0b1u: (void)format_inventory_item_label(eax, edx); return;
    case 0x1b26du: combine_held_item_with_target(eax); return;
    case 0x1bb12u: move_cursor_entry_clamped(eax); return;
    case 0x1b0e3u: scroll_entry_into_view((uint32_t *)(uintptr_t)eax); return;
    /* EAX,EDX */
    case 0x184abu: activate_weapon_item(eax, edx); return;
    case 0x1b007u: swap_inventory_entries(eax, edx); return;
    case 0x1c512u: draw_reloc_ui_row((int32_t)eax, (int32_t)edx); return;   /* EAX=y, EDX=off */
    case 0x1f671u: dialogue_voice_force_end(eax, edx); return;
    case 0x1db89u: (void)eval_dialogue_record_condition_with_cleanup(eax, edx, NULL, NULL); return;
    /* EAX,EDX,EBX */
    case 0x18e48u: blit_das_image_auto_scale(eax, edx, ebx); return;
    case 0x18e68u: blit_reloc_das_image(eax, edx, ebx); return;
    case 0x1a10au: blit_das_image_at_xy(eax, (int32_t)edx, (int32_t)ebx); return;  /* EAX=img,EDX=x,EBX=y */
    case 0x4b360u: (void)mem_fill((void *)(uintptr_t)eax, edx, ebx); return;
    case 0x360b3u: (void)pool_free_handle((uint32_t *)(uintptr_t)eax,
                                                 (uint32_t *)(uintptr_t)edx); return;
    /* EAX,EDX,EBX,ECX */
    case 0x14d04u: draw_text_to_buffer(eax, edx, ebx, ecx); return;
    case 0x12ceau: clear_framebuffer_rect(eax, edx, ebx, ecx); return;
    case 0x12ddeu: draw_popup_shadow_border_smc((int32_t)eax, (int32_t)edx,
                                                       (int32_t)ebx, (int32_t)ecx); return;
    case 0x13544u: blit_item_icon(eax, edx, ebx, ecx); return;
    case 0x15b69u: add_dirty_rect(eax, (int32_t)edx, ebx, ecx); return;
    case 0x1a079u: draw_text_at_screen_xy(eax, ebx, ecx, edx); return;   /* EAX=str,EBX=x,ECX=y,EDX=flags */
    default: break;
    }
    roth_unreachable(canon);   /* dialogue-UI renderer/action bridge — in-game dialogue tier */
#endif
}

/* ---- same, but returns the callee's EAX (for value-returning bridges like resolve_reloc_ptr_dup). ---- */
static uint32_t dlg_call4_r(uint32_t canon, uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx)
{
    regs_t io; memset(&io, 0, sizeof io);
    io.va = canon + OBJ_DELTA;
    io.eax = eax; io.edx = edx; io.ebx = ebx; io.ecx = ecx;
#ifndef ROTH_STANDALONE
    call_orig(&io);
#else
    switch (canon) {   /* routes (value-returning) */
    case 0x18e2cu: return resolve_reloc_ptr(eax);   /* resolve_reloc_ptr_dup = byte-identical dup of 0x226c6 */
    case 0x1dda8u: return scan_tag4_chunk(eax);
    case 0x18a2au: return try_interrupt_dialogue_voice();
    case 0x1b0b2u: return get_item_tab_index(eax);
    default: break;
    }
    roth_unreachable(canon);   /* dialogue-UI value-returning bridge — in-game dialogue tier */
#endif
    return io.eax;
}

/* ============================ free_inspect_overlay_image (0x18cb9) ============================
 * EDX=param_2 (free flags). Releases the inspect close-up overlay DAS handle 0x7fed4 back to the
 * cache (storing the new handle) and marks its screen region (0x7fed8/0x7fe0.. rect) dirty.
 * Bridges free_das_cache_handle 0x13136 + register_dirty_rect 0x15b5b. */
void free_inspect_overlay_image(uint32_t param_1, uint32_t param_2)
{
    (void)param_1;
    (void)param_2;                                     /* EDX (free flags) is dead: 0x13136 push/pops edx */
    if (G32(VA_g_active_item_hud_icon + 0x4) == 0)
        return;
    /* re-pointed: free_das_cache_handle 0x13136 [L, das_assets] direct-C (EAX=handle -> EAX=0).
     * Test du_foi_case stages a real cache pool (0x85c3c) + a live descriptor handle so the callee
     * (blit_descriptor_rows + pool_free_handle, both native) runs REAL + symmetric on both sides. */
    G32(VA_g_active_item_hud_icon + 0x4) = (int32_t)free_das_cache_handle((uint32_t)G32(VA_g_active_item_hud_icon + 0x4));
    dlg_call4(0x15b5b, (uint32_t)G32(VA_g_active_item_hud_icon + 0x8), (uint32_t)G32(VA_g_active_item_hud_icon + 0x10),
                       (uint32_t)G32(VA_g_active_item_hud_icon + 0xc), (uint32_t)G32(VA_g_active_item_hud_icon + 0x14));
}

/* ============================ free_inspect_popup_and_redraw (0x19678) ============================
 * EAX=handle, EDX=param_2. Frees the inspect close-up image + marks its screen footprint
 * (geometry 0x810e4/6/8/a) dirty. Bridges free_das_cache_handle + register_dirty_rect. */
void free_inspect_popup_and_redraw(uint32_t handle, uint32_t param_2)
{
    if (handle == 0)
        return;
    (void)param_2;                                     /* EDX (free flags) dead: 0x13136 push/pops edx */
    free_das_cache_handle(handle);              /* re-pointed 0x13136 [L, das_assets]; not oracle-covered */
    int32_t e4 = (int16_t)RW(GADDR(VA_g_inspect_popup_state + 0x24));
    int32_t e6 = (int16_t)RW(GADDR(VA_g_inspect_popup_state + 0x26));
    int32_t e8 = (int16_t)RW(GADDR(VA_g_inspect_popup_state + 0x28));
    int32_t ea = (int16_t)RW(GADDR(VA_g_inspect_popup_state + 0x2a));
    dlg_call4(0x15b5b, (uint32_t)(e4 - 4), (uint32_t)(e6 - 4),
                       (uint32_t)(e8 + e4 + 3), (uint32_t)(ea + e6 + 3));
}

/* save_framebuffer_region 0x13062 — CONVERTED to DIRECT C. All five dialogue_ui renderers
 * (render_dialogue_text_panel/_choice_text_panel/_text_ui/_active_timed_message/_inspect_popup_window)
 * now call save_framebuffer_region (blit_2d [L], oracle-verified in test_blit_2d) directly; the
 * old dlg_save_fb call_orig bridge is retired. The oracle drives each renderer through du_render_diff /
 * du_rip_case — bespoke pool+fb-restoring differentials (test_dialogue_ui.c) that snapshot the host-side
 * du_pool and du_fb and restore them between the original and lift runs, so both alloc the same chunk
 * from the same fresh pool -> the handle stored in obj3 (and rip's EAX return) matches by construction.
 * du_fb sized to 0x40000 (rip:hires's doubled-y read) and du_pool to 0x10000 (hires panel / rip:wide);
 * rip:savefail exhausts the pool to hit the alloc-fail early-return. (lift_inventory.c's inv_call site
 * converted in the same wave.) */

/* apply_ui_palette_rect 0x12c36 — direct-C. ABI resolved from disasm 0x12c36:
 * EAX=x0, EDX=y0, EBX=x1, ECX=y1, stack[0]=level (ret 4); the leaf self-loads FS from
 * g_text_color_ramp_selector 0x90c0e over a 0x2000-byte ramp and remaps each fb pixel to
 * ramp[((level&0x1f)<<8)|pixel]. The lifted proto takes the resolved ramp base as `fs`
 * (same g_os_sel_base(0x90c0e) idiom as the five converted blit_2d/text_font sites).
 * Proto param order is (x0,y0,y1,level,x1,fs) — note ECX=y1 is the 3rd proto arg, EBX=x1 the 5th. */
static void dlg_apply_rect(uint32_t eax, uint32_t edx, uint32_t ebx, uint32_t ecx, uint32_t s0)
{
    uint8_t *fs = (uint8_t *)(uintptr_t)
        (g_os_sel_base ? g_os_sel_base((uint16_t)G16(VA_g_text_color_ramp_selector)) : 0);
    apply_ui_palette_rect((int32_t)eax, (int32_t)edx, (int32_t)ecx,
                                 s0, (int32_t)ebx, fs);
}

/* ============================ render_dialogue_text_panel (0x1ec78) ============================
 * EAX=param_1 (1 = suppress the fade rect). Renders the multi-line dialogue/story text records
 * from g_dialogue_segments 0x827fd, vertically centered; saves the framebuffer region, optionally
 * applies the fade/highlight rect per line, and draws each control-coded line. Bridges
 * save_framebuffer_region / apply_ui_palette_rect / draw_text_to_buffer / register_dirty_rect. */
void render_dialogue_text_panel(int32_t param_1)
{
    int32_t iVar2 = G32(VA_g_choice_interaction_mode + 0x10) - 0x28;
    int32_t uVar4 = (int32_t)((uint32_t)(G32(VA_g_screen_pitch + 0x4) - G32(VA_g_dialogue_reveal_ramp + 0xc)) >> 1);
    G32(VA_g_dialogue_busy_flag + 0x4) = G32(VA_g_dialogue_reveal_ramp + 0x4);
    G32(VA_g_dialogue_busy_flag + 0xc) = G32(VA_g_dialogue_reveal_ramp + 0x8);
    G32(VA_g_dialogue_busy_flag + 0x8) = uVar4;
    G32(VA_g_dialogue_busy_flag + 0x10) = uVar4 + G32(VA_g_dialogue_reveal_ramp + 0xc);
    int32_t w = G32(VA_g_dialogue_busy_flag + 0xc) - G32(VA_g_dialogue_busy_flag + 0x4) + 1;
    int32_t h = G32(VA_g_dialogue_reveal_ramp + 0xc) + 1;
    G32(VA_g_dialogue_reveal_ramp + 0x18) = (int32_t)save_framebuffer_region((uint32_t)G32(VA_g_dialogue_busy_flag + 0x4), (uint32_t)uVar4,
                                                           (uint32_t)w, (uint32_t)h, NULL);  /* 0x13062 direct-C */

    int32_t local_c = (G8(VA_g_hires_line_doubling_flag) != 0) ? 1 : 0;
    uint32_t esi = GADDR(VA_g_dialogue_segments);
    int32_t edi = uVar4;
    int32_t pitch = G32(VA_g_screen_pitch);
    uint32_t fb = (uint32_t)G32(VA_g_framebuffer_ptr);
    for (int32_t i = 0; i < G32(VA_g_active_dialogue_context); i++) {
        if (param_1 != 1) {
            dlg_apply_rect((uint32_t)((int32_t)RD(esi) + iVar2), (uint32_t)edi,
                           (uint32_t)((int32_t)RD(esi + 4) - 2 + iVar2),
                           (uint32_t)((int32_t)RD(esi + 8) + edi),
                           (uint32_t)(G32(VA_g_dialogue_reveal_ramp) >> 1));
        }
        int32_t ry = (G8(VA_g_hires_line_doubling_flag) != 0) ? edi + edi : edi;
        dlg_call4(0x14d04, RD(esi + 0x10), fb + (uint32_t)(iVar2 + ry * pitch),
                           (uint32_t)pitch, (uint32_t)local_c);
        int32_t adv = (int32_t)RD(esi + 8) + 4;
        esi += 0x14;
        edi += adv;
    }
    dlg_call4(0x15b5b, (uint32_t)G32(VA_g_dialogue_busy_flag + 0x4), (uint32_t)G32(VA_g_dialogue_busy_flag + 0x8),
                       (uint32_t)G32(VA_g_dialogue_busy_flag + 0xc), (uint32_t)G32(VA_g_dialogue_busy_flag + 0x10));
}

/* ============================ render_choice_text_panel (0x1efa4) ============================
 * Renders the choice/menu text records from g_choice_line_records 0x83189, centered around
 * 0x83ae1 (height 0x83ad9), with a per-frame reveal ramp (0x83ae5) + per-line fade rect. Bridges
 * the same 4 framebuffer helpers as render_dialogue_text_panel. */
void render_choice_text_panel(void)
{
    int32_t iVar2 = G32(VA_g_choice_interaction_mode + 0x14) - (G32(VA_g_choice_interaction_mode + 0xc) >> 1);
    int32_t iVar1 = G32(VA_g_choice_interaction_mode + 0x10);
    G32(VA_g_dialogue_busy_flag + 0x14) = G32(VA_g_choice_interaction_mode + 0x4) + iVar1;
    G32(VA_g_dialogue_busy_flag + 0x1c) = G32(VA_g_choice_interaction_mode + 0x8) + iVar1;
    G32(VA_g_dialogue_busy_flag + 0x18) = iVar2;
    G32(VA_g_dialogue_busy_flag + 0x20) = iVar2 + G32(VA_g_choice_interaction_mode + 0xc);
    if (G32(VA_g_choice_reveal_ramp) < 0x20) {
        G32(VA_g_choice_reveal_ramp) += G32(VA_g_frame_time_scale);
        if (G32(VA_g_choice_reveal_ramp) > 0x20)
            G32(VA_g_choice_reveal_ramp) = 0x20;
    }
    int32_t ecx = G32(VA_g_choice_interaction_mode + 0xc) + 1;
    int32_t ebx = G32(VA_g_dialogue_busy_flag + 0x1c) - G32(VA_g_dialogue_busy_flag + 0x14) + 1;
    G32(VA_g_dialogue_reveal_ramp + 0x1c) = (int32_t)save_framebuffer_region((uint32_t)G32(VA_g_dialogue_busy_flag + 0x14), (uint32_t)iVar2,
                                        (uint32_t)ebx, (uint32_t)ecx, NULL);  /* 0x13062 direct-C */

    int32_t local_c = (G8(VA_g_hires_line_doubling_flag) != 0) ? 1 : 0;
    uint32_t esi = GADDR(VA_g_choice_line_records);
    int32_t edi = iVar2;
    int32_t pitch = G32(VA_g_screen_pitch);
    uint32_t fb = (uint32_t)G32(VA_g_framebuffer_ptr);
    for (int32_t i = 0; i < G32(VA_g_choice_line_count); i++) {
        dlg_apply_rect((uint32_t)((int32_t)RD(esi) + iVar1), (uint32_t)edi,
                       (uint32_t)((int32_t)RD(esi + 4) - 2 + iVar1),
                       (uint32_t)((int32_t)RD(esi + 8) + edi - 1),
                       (uint32_t)(G32(VA_g_choice_reveal_ramp) >> 1));
        int32_t ry = (G8(VA_g_hires_line_doubling_flag) != 0) ? edi + edi : edi;
        int32_t oldh = (int32_t)RD(esi + 8);
        uint32_t textptr = RD(esi + 0x10);
        esi += 0x14;
        dlg_call4(0x14d04, textptr, fb + (uint32_t)(iVar1 + ry * pitch),
                           (uint32_t)pitch, (uint32_t)local_c);
        edi += oldh;
    }
    dlg_call4(0x15b5b, (uint32_t)G32(VA_g_dialogue_busy_flag + 0x14), (uint32_t)G32(VA_g_dialogue_busy_flag + 0x18),
                       (uint32_t)G32(VA_g_dialogue_busy_flag + 0x1c), (uint32_t)G32(VA_g_dialogue_busy_flag + 0x20));
}

/* ============================ render_active_timed_message (0x1ed9c) ============================
 * Draws the queued timed-message overlay while g_timed_message_timer 0x827e5 > 0 (ticks down by
 * g_frame_time_scale 0x85324; clears the line count 0x827e1 on expiry). Computes the panel rect
 * (0x827ed/f1/f5/f9), saves the framebuffer region behind it, and draws each laid-out line
 * (records @0x824c9 stride 0x14) via draw_text_to_buffer. Bridges save_framebuffer_region /
 * register_dirty_rect / draw_text_to_buffer -> in-game. */
void render_active_timed_message(void)
{
    G32(VA_g_timed_message_timer) -= G32(VA_g_frame_time_scale);
    if (G32(VA_g_timed_message_timer) < 1) {                       /* expired */
        G32(VA_g_timed_message_line_count) = 0;
        return;
    }
    int32_t edi = 0;
    int32_t pitch = G32(VA_g_screen_pitch);
    uint32_t fb = (uint32_t)G32(VA_g_framebuffer_ptr);

    if (G32(VA_g_timed_message_line_count) <= 1) {                       /* 0/1-line path */
        int32_t x0 = G32(VA_g_timed_message_lines);
        int32_t eax = G32(VA_g_timed_message_lines + 0x8) + 1;            /* height+1 */
        int32_t x1 = G32(VA_g_timed_message_lines + 0x4);
        int32_t y0;
        if (G32(VA_g_inventory_panel_open) != 0)
            y0 = G32(VA_g_ui_panel_anchor_y) - eax;
        else
            y0 = G32(VA_g_view_y) + 2;
        G32(VA_g_timed_message_timer + 0x10) = x1;
        int32_t y1 = eax + y0;
        G32(VA_g_timed_message_timer + 0x8) = x0;
        G32(VA_g_timed_message_timer + 0x14) = y1;
        G32(VA_g_timed_message_timer + 0xc) = y0;
        G32(VA_g_timed_message_timer + 0x4) = (int32_t)save_framebuffer_region((uint32_t)x0, (uint32_t)y0,
                                            (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), NULL);  /* 0x13062 direct-C */
        dlg_call4(0x15b5b, (uint32_t)G32(VA_g_timed_message_timer + 0x8), (uint32_t)G32(VA_g_timed_message_timer + 0xc),
                           (uint32_t)G32(VA_g_timed_message_timer + 0x10), (uint32_t)G32(VA_g_timed_message_timer + 0x14));
        int32_t ry = y0;
        if (G8(VA_g_hires_line_doubling_flag) != 0) { edi = 1; ry += ry; }
        dlg_call4(0x14d04, (uint32_t)G32(VA_g_timed_message_lines + 0x10), fb + (uint32_t)(ry * pitch),
                           (uint32_t)pitch, (uint32_t)edi);
        return;
    }

    /* >= 2 lines: compute the bounding box over the line records */
    int32_t x0 = G32(VA_g_timed_message_lines);                     /* esi = min x0 */
    int32_t x1 = G32(VA_g_timed_message_lines + 0x4);                     /* edx = max x1 */
    for (int32_t i = 1; i < G32(VA_g_timed_message_line_count); i++) {
        int32_t lx0 = G32(VA_g_timed_message_lines + i * 0x14);
        if (x0 > lx0) x0 = lx0;
        int32_t lx1 = G32((VA_g_timed_message_lines + 0x4) + i * 0x14);
        if (x1 < lx1) x1 = lx1;
    }
    int32_t h = (G32(VA_g_timed_message_lines + 0x8) + 1) * G32(VA_g_timed_message_line_count);
    G32(VA_g_timed_message_timer + 0xc) = 0x20;
    G32(VA_g_timed_message_timer + 0x10) = x1;
    G32(VA_g_timed_message_timer + 0x14) = h + 0x20;
    G32(VA_g_timed_message_timer + 0x8) = x0;
    G32(VA_g_timed_message_timer + 0x4) = (int32_t)save_framebuffer_region((uint32_t)x0, 0x20, (uint32_t)(x1 - x0),
                                        (uint32_t)h, NULL);  /* 0x13062 direct-C */
    dlg_call4(0x15b5b, (uint32_t)G32(VA_g_timed_message_timer + 0x8), (uint32_t)G32(VA_g_timed_message_timer + 0xc),
                       (uint32_t)G32(VA_g_timed_message_timer + 0x10), (uint32_t)G32(VA_g_timed_message_timer + 0x14));

    int32_t stride = 0xb;
    int32_t basey = 0x20;
    if (G8(VA_g_hires_line_doubling_flag) != 0) { basey = 0x40; stride = 0x16; edi = 1; }
    int32_t y = basey;
    for (int32_t i = 0; i < G32(VA_g_timed_message_line_count); i++) {
        dlg_call4(0x14d04, (uint32_t)G32((VA_g_timed_message_lines + 0x10) + i * 0x14), fb + (uint32_t)(y * pitch),
                           (uint32_t)pitch, (uint32_t)edi);
        y += stride;
    }
}

/* ============================ render_text_ui (0x1f0e8) ============================
 * Per-frame text/UI overlay dispatcher + the active-monologue line SEQUENCER. Draws the timed
 * message, then (by dialogue state) the choice panel, the dialogue panel, or advances the
 * monologue segment (dwell via g_move_freeze_gate 0x83125 == 0x6ffff/0x7ffff) and draws the
 * current segment. Calls the lifted panel renderers directly; bridges the framebuffer helpers. */
void render_text_ui(int32_t param_1)
{
    if (G32(VA_g_timed_message_line_count) != 0)
        render_active_timed_message();

    if (G32(VA_g_active_dialogue_context) == 0) {                       /* no active dialogue */
        if (G32(VA_g_choice_interaction_mode) == 1)
            render_choice_text_panel();
        return;
    }

    /* dialogue active: advance the reveal ramp */
    if (G32(VA_g_dialogue_reveal_ramp) < 0x20) {
        G32(VA_g_dialogue_reveal_ramp) += G32(VA_g_frame_time_scale);
        if (G32(VA_g_dialogue_reveal_ramp) > 0x20)
            G32(VA_g_dialogue_reveal_ramp) = 0x20;
    }

    if (G32(VA_g_move_freeze_gate) == 0x6ffff) {                 /* gate engaged -> draw the dialogue panel */
        render_dialogue_text_panel(param_1);
        return;
    }

    if (G32(VA_g_choice_interaction_mode) == 1)
        render_choice_text_panel();
    if (G32(VA_g_move_freeze_gate) != 0x7ffff)
        G32(VA_g_move_freeze_gate) -= G32(VA_g_frame_time_scale);

    if (G32(VA_g_move_freeze_gate) < 1) {                         /* dwell expired -> advance segment */
        if (G32(VA_g_dialogue_segment_index) + 1 == G32(VA_g_active_dialogue_context) && G32(VA_g_voice_stream_state) == 1) {
            G32(VA_g_move_freeze_gate) = 0;
        } else {
            G32(VA_g_dialogue_segment_index)++;
            if (G32(VA_g_dialogue_line_dwell) == 0)
                G32(VA_g_move_freeze_gate) = 0x7ffff;
            else
                G32(VA_g_move_freeze_gate) = (G32(VA_g_dialogue_line_dwell) * G32(VA_g_dialogue_timer_rate)) / G32(VA_g_dialogue_dwell_divisor);
            if (G32(VA_g_active_dialogue_context) <= G32(VA_g_dialogue_segment_index)) {
                G32(VA_g_active_dialogue_context) = 0;
                G32(VA_g_move_freeze_gate) = 0;
                return;
            }
        }
    }

    /* draw the current monologue segment */
    int32_t si = G32(VA_g_dialogue_segment_index);
    int32_t x0 = (int32_t)RD(GADDR(VA_g_dialogue_segments) + si * 0x14);
    int32_t iVar3 = (int32_t)RD(GADDR(VA_g_dialogue_segments + 0x4) + si * 0x14) - x0;
    int32_t iVar2 = (int32_t)RD(GADDR(VA_g_dialogue_segments + 0x8) + si * 0x14) + 1;
    int32_t iVar4;
    if (G32(VA_g_inventory_panel_open) != 0)
        iVar4 = G32(VA_g_ui_panel_anchor_y) + 0x54;
    else
        iVar4 = G32(VA_g_view_y) + 4 + G32(VA_g_view_h);
    int32_t esi = iVar4;
    if ((uint32_t)G32(VA_g_screen_pitch + 0x4) < (uint32_t)(iVar2 + esi + 3))
        esi = G32(VA_g_screen_pitch + 0x4) - iVar2 - 3;

    G32(VA_g_dialogue_reveal_ramp + 0x18) = (int32_t)save_framebuffer_region((uint32_t)x0, (uint32_t)esi,
                                        (uint32_t)iVar3, (uint32_t)iVar2, NULL);  /* 0x13062 direct-C */
    G32(VA_g_dialogue_busy_flag + 0x4) = x0;
    G32(VA_g_dialogue_busy_flag + 0xc) = x0 + iVar3 - 1;
    G32(VA_g_dialogue_busy_flag + 0x8) = esi;
    G32(VA_g_dialogue_busy_flag + 0x10) = iVar2 + esi - 1;
    if (param_1 != 1) {
        dlg_apply_rect((uint32_t)x0, (uint32_t)esi, (uint32_t)(G32(VA_g_dialogue_busy_flag + 0xc) - 1),
                       (uint32_t)G32(VA_g_dialogue_busy_flag + 0x10), (uint32_t)(G32(VA_g_dialogue_reveal_ramp) >> 1));
    }
    dlg_call4(0x15b5b, (uint32_t)G32(VA_g_dialogue_busy_flag + 0x4), (uint32_t)G32(VA_g_dialogue_busy_flag + 0x8),
                       (uint32_t)G32(VA_g_dialogue_busy_flag + 0xc), (uint32_t)G32(VA_g_dialogue_busy_flag + 0x10));
    int32_t local_10 = 0, ry = esi;
    if (G8(VA_g_hires_line_doubling_flag) != 0) { local_10 = 1; ry += ry; }
    int32_t pitch = G32(VA_g_screen_pitch);
    uint32_t fb = (uint32_t)G32(VA_g_framebuffer_ptr);
    uint32_t textptr = RD(GADDR(VA_g_dialogue_timer_rate + 0x4) + si * 0x14);
    dlg_call4(0x14d04, textptr, fb + (uint32_t)(ry * pitch), (uint32_t)pitch, (uint32_t)local_10);
}

/* ============================ build_available_choice_menu (0x1f950) ============================
 * EAX=node. Builds the visible choice/menu line records from a dialogue node: scans the node's
 * variable records (node+0x14, gated by node+4 bit 8), filters type-1 records through
 * eval_dialogue_record_condition_with_cleanup(rec,2), pulls each accepted line's text via
 * read_next_dialogue_line, stores the accepted source-record ptrs at g_choice_option_records
 * 0x83149, lays out the lines via layout_timed_message_text (verified lift), and computes the
 * choice-panel bounding box. Returns the visible choice count.
 * read_next_dialogue_line 0x1e8cc bridges DBASE400 file I/O + voice prime -> in-game; the
 * no-type-1-record path (count 0) is oracle-verified. */
uint32_t build_available_choice_menu(uint32_t node)
{
    uint8_t buf_backing[0x800];
    uint8_t *buf = buf_backing + 1;           /* local_820 (buf[-1] = a throwaway stack byte) */
    buf[0] = 0;
    G32(VA_g_choice_line_count) = 0;                         /* g_choice_line_count = 0 */
    int32_t count = 0;

    if (RB(node + 4) & 8) {
        int32_t opt_off = 0;
        uint32_t p = node + 0x14;
        uint32_t word;
        do {
            word = RD(p);
            if (((int32_t)word >> 24) == 1) {
                uint32_t ebx = RD(p + 4), ecx = 0;
                int32_t r = dlg_eval_cond(p, 2, &ebx, &ecx);
                if (r != 0) {
                    uint32_t voice_off = ebx & 0xffffff;
                    /* re-pointed: read_next_dialogue_line 0x1e8cc [L, dbase100] direct-C
                     * (EAX=dest, EDX=maxlen, EBX=voice_off, ECX=flag -> EAX=bytes). This type-1-record
                     * path is oracle-UNREACHED (bcm cases stage no type-1 records -> count stays 0), so
                     * the swap is a pure mechanical re-point; the callee is verified in test_dbase100. */
                    uint32_t n = read_next_dialogue_line((uint32_t)(uintptr_t)buf, 200, voice_off, 0);
                    if (n != 0) {
                        buf[-1] = 0xa;
                        buf += n;
                        count++;
                        WD(GADDR(VA_g_choice_option_records) + opt_off, p);   /* g_choice_option_records[opt] = record */
                        opt_off += 4;
                    }
                }
            }
            word &= 0xffff;
            p += (word >> 2) << 2;
        } while (word != 0);

        int32_t lc = layout_timed_message_text((int32_t *)GADDR(VA_g_choice_line_records),
                        (uint8_t *)GADDR(VA_g_choice_text_buffer), buf_backing + 1, G32(VA_g_screen_pitch) - 0x50, 0x10);
        G32(VA_g_choice_line_count) = lc;
        if (lc != 0) {
            uint32_t textptr0 = RD(GADDR(VA_g_choice_line_records + 0x10));
            G8(VA_g_choice_reveal_ramp + 0x4) = RB(textptr0 + 1);
            int32_t edi = (int32_t)RD(GADDR(VA_g_choice_line_records));   /* min x0 */
            int32_t ebx = (int32_t)RD(GADDR(VA_g_choice_line_records + 0x4));   /* max x1 */
            int32_t edx = (int32_t)RD(GADDR(VA_g_choice_line_records + 0x8));   /* height accumulator */
            uint32_t rp = GADDR(0x83189 + 0x14);
            for (int32_t i = 1; i < lc; i++) {
                edx += (int32_t)RD(rp + 8);
                int32_t x0 = (int32_t)RD(rp);
                if (edi > x0) edi = x0;
                int32_t x1 = (int32_t)RD(rp + 4);
                if (ebx < x1) ebx = x1;
                rp += 0x14;
            }
            G32(VA_g_choice_reveal_ramp) = 2;
            G32(VA_g_choice_interaction_mode + 0x4) = edi;
            G32(VA_g_choice_interaction_mode + 0x8) = ebx;
            G32(VA_g_choice_interaction_mode + 0xc) = edx;
        }
    }
    return (uint32_t)count;
}

/* ============================ draw_reloc_ui_row (0x1c512) ============================
 * EAX=param_1 (row y), EDX=param_2 (reloc offset). Resolves the reloc record fields (via the
 * verified resolve_reloc_record_fields lift), computes x = 0x810e4 + 0x11 + fieldX, blits the
 * image (blit_das_image_at_xy), and registers the row's dirty rect. */
void draw_reloc_ui_row(int32_t param_1, int32_t param_2)
{
    int32_t local[2] = { 0, 0 };
    int32_t esi = (int32_t)(int16_t)RW(GADDR(VA_g_inspect_popup_state + 0x24)) + 0x11;
    uint32_t rec = resolve_reloc_record_fields(&local[0], &local[1], (uint32_t)param_2, 1);
    esi += local[0];
    if (rec != 0) {
        dlg_call4(0x1a10a, rec, (uint32_t)esi, (uint32_t)param_1, 0);   /* blit_das_image_at_xy */
        int32_t x1 = (int32_t)(int16_t)RW(rec + 4) + esi - 1;
        int32_t y1 = (int32_t)(int16_t)RW(rec + 6) + param_1 - 1;
        dlg_call4(0x15b5b, (uint32_t)esi, (uint32_t)param_1, (uint32_t)x1, (uint32_t)y1);
    }
}

/* ============================ update_dialogue_cursor_and_click (0x16585) ============================
 * Per-frame cursor + click during a dialogue/choice modal. No button + a choice hovered
 * (update_dialogue_choice_highlight, the verified lift) -> hover cursor 0x248; no hover -> 0x240;
 * click while busy -> request skip (0x7f564), force-end voice, click cursor 0x268. Clears the
 * cursor action flags 0x7e938/0x7e939. Bridges set_cursor_shape / dialogue_voice_force_end. */
void update_dialogue_cursor_and_click(void)
{
    uint32_t uVar2 = (uint32_t)G8(VA_g_cursor_primary_action_flag) | (uint32_t)G8(VA_g_cursor_secondary_action_flag);
    if ((((uint32_t)G8(VA_g_mouse_buttons_prev) & 3) | uVar2) == 0) {
        if (G32(VA_g_dialogue_busy_flag) != 0) {
            int32_t r = (int32_t)update_dialogue_choice_highlight(
                            (int32_t)G32(VA_g_mouse_x), (int32_t)G32(VA_g_mouse_y));
            if (r == 1) {
                dlg_call4(0x12a08, 0x248, 0, 0, 0);        /* set_cursor_shape (no flag clear) */
                return;
            }
        }
        dlg_call4(0x12a08, 0x240, 0, 0, 0);
    } else {
        if (uVar2 == 0 || G32(VA_g_dialogue_busy_flag) == 0) {
            G8(VA_g_cursor_primary_action_flag) = 0; G8(VA_g_cursor_secondary_action_flag) = 0;
            return;
        }
        G32(VA_g_dev_mode_flag + 0x4) = 1;
        dlg_call4(0x1f671, (uint32_t)G32(VA_g_mouse_x), (uint32_t)G32(VA_g_mouse_y), 0, 0); /* voice_force_end */
        G8(VA_g_mouse_buttons_prev + 0x7) = 0;
        dlg_call4(0x12a08, 0x268, 0, 0, 0);
    }
    G8(VA_g_cursor_primary_action_flag) = 0;
    G8(VA_g_cursor_secondary_action_flag) = 0;
}

/* ============================ dbase100_open_dialogue_window (0x1eabc) ============================
 * EAX=param_1 (source text). Opens the mode-6 object/inspect dialogue window: lays out the text
 * (layout, capacity 10), stores the count in g_active_dialogue_context 0x83115, sets the freeze
 * gate 0x83125=0x6ffff + reveal ramp, anchors at the player position, computes the panel bbox
 * (0x8312d/0x83131/0x83135), and (if a cutscene overlay is active) presents a frame. Returns 1. */
uint32_t dbase100_open_dialogue_window(uint8_t *param_1)
{
    int32_t ctx = layout_timed_message_text((int32_t *)GADDR(VA_g_dialogue_segments),
                    (uint8_t *)GADDR(VA_g_dialogue_timer_rate + 0x10c), param_1, G32(VA_g_screen_pitch), 10);
    G32(VA_g_active_dialogue_context) = ctx;
    if (ctx == 0)
        return 0;                             /* returns eax=ctx=0 on the empty path */
    G32(VA_g_dialogue_segment_index) = 0;
    G32(VA_g_dialogue_line_dwell) = 0;
    G32(VA_g_move_freeze_gate) = 0x6ffff;
    G32(VA_g_dialogue_reveal_ramp) = 4;
    G32(VA_g_dialogue_busy_flag + 0x24) = G32(VA_g_player_angle + 0x2);
    G32(VA_g_dialogue_busy_flag + 0x28) = G32(VA_g_player_z + 0x2);
    int32_t edx = (int32_t)RD(GADDR(VA_g_dialogue_segments));
    int32_t ebx = edx;
    int32_t ecx = 0;
    uint32_t rp = GADDR(VA_g_dialogue_segments);
    for (int32_t i = 0; i < G32(VA_g_active_dialogue_context); i++) {
        ecx += (int32_t)RD(rp + 8) + 4;
        int32_t x0 = (int32_t)RD(rp);
        if (edx > x0) edx = x0;
        int32_t x1 = (int32_t)RD(rp + 4);
        if (ebx < x1) ebx = x1;
        rp += 0x14;
    }
    ecx -= 4;
    if (G32(VA_g_choice_interaction_mode) != 0) {
        int32_t adj = G32(VA_g_choice_interaction_mode + 0x10) - 0x28;
        edx += adj;
        ebx += adj;
    } else {
        G32(VA_g_choice_interaction_mode + 0x10) = 0x28;
    }
    G32(VA_g_dialogue_reveal_ramp + 0x4) = edx;
    G32(VA_g_dialogue_reveal_ramp + 0x8) = ebx;
    G32(VA_g_dialogue_reveal_ramp + 0xc) = ecx;
    G32(VA_g_dialogue_reveal_ramp + 0x10) = (int32_t)G8(VA_g_timed_message_color);
    if (G32(VA_g_cutscene_overlay_active) != 0)
        dlg_call4(0x20a8a, (uint32_t)G32(VA_g_dialogue_reveal_ramp + 0x10), (uint32_t)G32(VA_g_dialogue_reveal_ramp + 0x4), 0, 0); /* present_cutscene_frame */
    return 1;
}

/* ============================ dbase100_open_dialogue_window_alt (0x1ebb4) ============================
 * EAX=param_1. Sibling opener for the mode-7 dialogue window (gated on 0x8202c/0x83e90). Lays out
 * (capacity 3), and either sets the freeze gate 0x7ffff (no timing) or computes a timed dwell
 * (0x83119) + gate = dwell*rate/divisor. Returns 1. No host bridges -> fully oracle-able. */
uint32_t dbase100_open_dialogue_window_alt(uint8_t *param_1)
{
    if (G32(VA_g_voice_bytes_remaining + 0x14) != 0 && G32(VA_g_active_weapon_ammo_cap + 0x1c) != 0)
        return 1;
    int32_t ctx = layout_timed_message_text((int32_t *)GADDR(VA_g_dialogue_segments),
                    (uint8_t *)GADDR(VA_g_dialogue_timer_rate + 0x10c), param_1, G32(VA_g_screen_pitch), 3);
    G32(VA_g_active_dialogue_context) = ctx;
    if (ctx == 0)
        return 1;
    G32(VA_g_dialogue_segment_index) = 0;
    if (G32(VA_g_voice_bytes_remaining + 0x14) != 0) {
        G32(VA_g_dialogue_line_dwell) = (G32(VA_g_voice_bytes_remaining + 0x14) * 0x46) / G32(VA_g_voice_bytes_remaining + 0x18);
        int32_t ebx = 0;
        for (int32_t i = 0; i < G32(VA_g_active_dialogue_context); i++)
            ebx += (int32_t)RD(GADDR(VA_g_dialogue_segments) + i * 0x14 + 0xc);
        G32(VA_g_dialogue_dwell_divisor) = ebx;
        G32(VA_g_move_freeze_gate) = (G32(VA_g_dialogue_line_dwell) * G32(VA_g_dialogue_timer_rate)) / ebx;
    } else {
        G32(VA_g_dialogue_line_dwell) = 0;
        G32(VA_g_move_freeze_gate) = 0x7ffff;
    }
    G32(VA_g_dialogue_reveal_ramp) = 4;
    return 1;
}

/* ============================ update_inspect_popup_choices (0x18ada) ============================
 * Per-frame inspect-popup driver: when topics are pending (mode==2, not busy) builds the choice
 * list (build_available_choice_menu), pumps voice, steps the window slide-in animation (counter
 * 0x80b34 via draw_reloc_ui_row), routes the cursor click, activates a pending choice, and renders
 * the dialogue text (render_text_ui). Orchestrates the verified lifts + input/audio/video bridges.
 * Returns 1 (0 while 0x80b2c gates the render). */
uint32_t update_inspect_popup_choices(void)
{
    if (G32(VA_g_choice_interaction_mode) == 2 && G32(VA_g_dialogue_busy_flag) == 0) {
        build_available_choice_menu((uint32_t)G32(VA_g_inventory_panel_open + 0x4));
        G32(VA_g_choice_interaction_mode) = 1;
    }
    dlg_call4(0x1e9b5, 0, 0, 0, 0);                 /* voice_stream_pump */
    if (G32(VA_g_inspect_popup_active) != 0 && G32(VA_g_dialogue_busy_flag) == 0 && G32(VA_g_inventory_ui_action + 0x4) != 0) {
        if (G8(VA_g_inventory_ui_action + 0x4) & 1) {
            draw_reloc_ui_row((int32_t)(uint16_t)RW(GADDR(VA_g_inspect_popup_layout + 0x6)), 0x98);
            G32(VA_g_inventory_ui_action + 0x4) -= 1;
        }
        if (G32(VA_g_choice_interaction_mode) == 0 && (G8(VA_g_inventory_ui_action + 0x4) & 2)) {
            draw_reloc_ui_row((int32_t)(uint16_t)RW(GADDR(VA_g_inspect_popup_layout + 0xe)), 0x88);
            G32(VA_g_inventory_ui_action + 0x4) -= 2;
        }
    }
    dlg_call4(0x1661f, 0, 0, 0, 0);                 /* handle_cursor_click */
    if (G32(VA_g_pending_choice_accept_index) != 0) {
        activate_selected_choice_record((uint32_t)G32(VA_g_pending_choice_accept_index));
        G32(VA_g_pending_choice_accept_index) = 0;
    }
    if (G32(VA_g_ui_panel_anchor_y + 0x4) != 0)
        return 0;
    render_text_ui(0);
    dlg_call4(0x15dd9, 0, 0, 0, 0);                 /* flush_dirty_rects */
    dlg_call4(0x1f330, 0, 0, 0, 0);                 /* mark_overlay_dirty_rects */
    return 1;
}

/* ============================ render_inspect_popup_window (0x18e9e) ============================
 * EAX=width, EDX=height, EBX=node record, ECX=param_4 (double-indirect ptr to the close-up-image
 * slot). Lays out the on-screen inspect popup: derives the window geometry, publishes every
 * panel/scrollbar/arrow layout global (0x810d0.. content ptr+dims, 0x80b40.. panel origins,
 * 0x83add/0x83ae1 choice-panel origin), saves the framebuffer region under the popup, then blits
 * the frame chrome + optional document sprites + close-up image + item name + up/down arrows.
 * Returns the saved-region handle (EAX = the save_framebuffer_region result); 0 if the save failed.
 *
 * The pure record/layout callees are the verified lifts (copy_record_block_op7,
 * build_available_choice_menu, resolve_reloc_record_fields, screen_xy_to_framebuffer_ptr,
 * measure_control_text_width). The side-effecting framebuffer/DAS primitives are bridged via
 * call_orig (re-point debt) so the oracle can SMC-stub them and diff the pure layout write-set;
 * in-game they call the real primitives. `w`=[ebp-0x3c], `h`=[ebp-8], `edi`=window x, `esi`=window y.
 * Transcribed strictly from the disasm. */
uint32_t render_inspect_popup_window(uint32_t width, uint32_t height,
                                            uint32_t node, uint32_t p4)
{
    int32_t name_w = 0;                                /* [ebp-0x10] */

    /* 1. reset the layout globals */
    G32(VA_g_inspect_popup_state + 0x38) = 0;  G32(VA_g_inspect_page_count + 0x84) = 0;
    G16(VA_g_inspect_popup_layout) = 6;
    G16(VA_g_inspect_popup_layout + 0x8) = 0;  G16(VA_g_inspect_popup_layout + 0x10) = 0;  G16(VA_g_inspect_popup_layout + 0x18) = 0;  G16(VA_g_inspect_popup_layout + 0x20) = 0;  G16(VA_g_inspect_popup_layout + 0x28) = 0;
    G16(VA_g_inspect_popup_layout + 0x4) = 0x1c;
    G16(VA_g_inspect_popup_layout + 0xc) = 0x14;  G16(VA_g_inspect_popup_layout + 0x14) = 0x14;  G16(VA_g_inspect_popup_layout + 0x24) = 0x14;  G16(VA_g_inspect_popup_layout + 0x2c) = 0x14;
    G32(VA_g_inventory_panel_open + 0x8) = 0;

    /* 2. base dims */
    int32_t h = (int32_t)height;                       /* [ebp-8] */
    G32(VA_g_inspect_popup_state + 0x14) = (int32_t)width;
    G32(VA_g_inspect_popup_state + 0x18) = (int32_t)height;
    int32_t w = (int32_t)width + 0x28;                 /* [ebp-0x3c] */

    /* 3. widen for the item name (only when the name flag byte 0x8105c is set) */
    if (G8(VA_g_selected_item_primary + 0x18) != 0) {
        name_w = measure_control_text_width((const char *)GADDR(VA_g_selected_item_primary + 0x1a));
        if (name_w < 0x38) name_w = 0x38;
        name_w += 0x28;
        if (name_w > w) w = name_w;
    }

    /* 4. height baseline + pitch-dependent minimum popup width (used only by the page block) */
    int32_t min_w = 0x10e;
    h += 0x13;
    {
        uint32_t pitch = (uint32_t)G32(VA_g_screen_pitch);
        if (pitch > 0x17c)       min_w = 0x154;
        else if (pitch >= 0x168) min_w = 0x130;
    }

    /* 5. document-page count (node+4 bit 2) */
    G32(VA_g_inspect_page_count) = 0;
    if (RB(node + 4) & 2) {
        uint32_t pages = copy_record_block_op7(node, GADDR(VA_g_inspect_page_count + 0x4), 0x20);
        G32(VA_g_inspect_page_count) = (int32_t)pages;
        if (pages != 0) {
            if (pages <= (uint32_t)G32(VA_g_inspect_current_page))
                G32(VA_g_inspect_current_page) = (int32_t)(pages - 1);
            min_w += 0x28;
            if (min_w > w) w = min_w;
            h = 0x9f;
        }
    }

    /* 6. choice menu -> widen to the choice bbox */
    int32_t nchoices = (int32_t)build_available_choice_menu(node);  /* [ebp-0x24] */
    if (nchoices != 0) {
        int32_t bx = G32(VA_g_choice_interaction_mode + 0x8) - G32(VA_g_choice_interaction_mode + 0x4) + 0x26;
        int32_t by = G32(VA_g_choice_interaction_mode + 0xc) + 0x12;
        if (bx > w) w = bx;
        if (by > h) h = by;
    }

    /* 7. center the window + publish the choice-panel origin globals */
    int32_t esi = G32(VA_g_ui_panel_anchor_y) + 0x41 - h;             /* window y [esi] */
    if (esi < 4) esi = 4;
    int32_t a0    = (w - 0x28 - G32(VA_g_inspect_popup_state + 0x14)) >> 1;    /* sar */
    G32(VA_g_choice_interaction_mode + 0x14)  = esi + 6 + (h >> 1);                /* sar h */
    int32_t pitch = G32(VA_g_screen_pitch);
    int32_t edi   = (int32_t)((uint32_t)(pitch - w) >> 1);   /* shr; window x [edi] */
    edi += 0x24;
    edi += a0;
    int32_t l20   = ((h - 0x13 - G32(VA_g_inspect_popup_state + 0x18)) >> 1) + 0xf;  /* sar; [ebp-0x20] */
    edi &= ~3;                                          /* and di,0xfffc */
    int32_t win_x = edi;                               /* [ebp-0x1c] (pre-adjust window x) */
    int32_t ebx   = edi + (G32(VA_g_inspect_popup_state + 0x14) >> 1);         /* sar width0 */
    edi -= a0 + 0x24;                                  /* edi now = window_x - (a0+0x24) */
    ebx -= (int32_t)((uint32_t)(pitch - 0x50) >> 1);   /* shr */
    G32(VA_g_choice_interaction_mode + 0x10) = ebx;

    /* 8. save the framebuffer region under the popup */
    int32_t save_w = w + 9;
    w += 1;                                            /* inc [ebp-0x3c] */
    uint32_t ret_region = save_framebuffer_region((uint32_t)(edi - 4), (uint32_t)(esi - 4),
                                      (uint32_t)save_w, (uint32_t)(h + 8), NULL);  /* 0x13062 direct-C */
    if (ret_region == 0)
        return 0;

    /* 9. content pointer + geometry words */
    G32(VA_g_inspect_popup_state + 0x10) = (int32_t)(uintptr_t)screen_xy_to_framebuffer_ptr(win_x, l20 + esi);
    G16(VA_g_inspect_popup_state + 0x28) = (uint16_t)w;
    G16(VA_g_inspect_popup_state + 0x2a) = (uint16_t)h;
    G16(VA_g_inspect_popup_state + 0x1c) = (uint16_t)win_x;
    G16(VA_g_inspect_popup_state + 0x1e) = (uint16_t)(l20 + esi);
    G16(VA_g_inspect_popup_state + 0x20) = (uint16_t)G32(VA_g_inspect_popup_state + 0x14);
    G16(VA_g_inspect_popup_state + 0x22) = (uint16_t)G32(VA_g_inspect_popup_state + 0x18);
    G16(VA_g_inspect_popup_state + 0x2c) = (uint16_t)(edi + 0x24);
    G16(VA_g_inspect_popup_state + 0x2e) = (uint16_t)(esi + 0xf);
    int32_t l4   = w - 0x29;                            /* [ebp-4] */
    G16(VA_g_inspect_popup_state + 0x30) = (uint16_t)l4;
    G16(VA_g_inspect_popup_state + 0x32) = (uint16_t)(h - 0x13);
    G16(VA_g_inspect_popup_state + 0x24) = (uint16_t)edi;
    G16(VA_g_inspect_popup_state + 0x26) = (uint16_t)esi;
    int32_t l30  = w - 1;                               /* [ebp-0x30] */

    /* 10. frame border + interior clear */
    dlg_call4(0x12dde, (uint32_t)edi, (uint32_t)esi, (uint32_t)(w - 1), (uint32_t)h);  /* draw_popup_shadow_border_smc */
    dlg_call4(0x12cea, (uint32_t)edi, (uint32_t)esi, (uint32_t)l30,     (uint32_t)h);  /* clear_framebuffer_rect */

    /* 11. panel content-origin globals + optional scrollbar block */
    G16(VA_g_inspect_popup_layout + 0x2) = (uint16_t)(edi + 4);
    uint16_t x8  = (uint16_t)(edi + 8);
    G16(VA_g_inspect_popup_layout + 0xa) = x8;  G16(VA_g_inspect_popup_layout + 0x12) = x8;  G16(VA_g_inspect_popup_layout + 0x22) = x8;  G16(VA_g_inspect_popup_layout + 0x2a) = x8;
    if (G32(VA_g_inspect_page_count) != 0) {
        G16(VA_g_inspect_popup_layout + 0x16) = (uint16_t)(esi + 0x10);
        G16(VA_g_inspect_popup_layout + 0x18) = (uint16_t)(h - 0x16);
        G16(VA_g_inspect_popup_layout + 0x1a) = (uint16_t)(edi + 0x23);
        G32(VA_g_inspect_popup_state) = 1;
        G16(VA_g_inspect_popup_layout + 0x1c) = (uint16_t)l4;
    }

    /* 12. left/right document sprites (auto-scaled to the popup height) */
    {
        uint32_t r = dlg_call4_r(0x18e2c, 0xd0, 0, 0, 0);          /* resolve_reloc_ptr_dup(0xd0) */
        if (r != 0) {
            WW(r + 6, (uint16_t)(h - 3));
            uint8_t *dst = screen_xy_to_framebuffer_ptr(edi, esi);
            dlg_call4(0x18e48, (uint32_t)(uintptr_t)dst, r, (uint32_t)G32(VA_g_screen_pitch), 0);  /* blit_das_image_auto_scale */
        }
    }
    {
        uint32_t r = dlg_call4_r(0x18e2c, 0xe0, 0, 0, 0);
        if (r != 0) {
            WW(r + 6, (uint16_t)(h - 3));
            uint8_t *dst = screen_xy_to_framebuffer_ptr(w + edi - 5, esi);
            dlg_call4(0x18e48, (uint32_t)(uintptr_t)dst, r, (uint32_t)G32(VA_g_screen_pitch), 0);
        }
    }

    /* 13. frame chrome corner/edge tiles */
    int32_t ey = h + esi - 3;
    {
        uint8_t *dst = screen_xy_to_framebuffer_ptr(edi + 4, esi + 4);
        dlg_call4(0x18e68, (uint32_t)(uintptr_t)dst, 0x110, (uint32_t)G32(VA_g_screen_pitch), 0);  /* blit_reloc_das_image */
    }
    {
        uint8_t *dst = screen_xy_to_framebuffer_ptr(edi, ey);
        dlg_call4(0x18e68, (uint32_t)(uintptr_t)dst, 0xd8, (uint32_t)G32(VA_g_screen_pitch), 0);
    }
    {
        uint8_t *dst = screen_xy_to_framebuffer_ptr(w + edi - 5, ey);
        dlg_call4(0x18e68, (uint32_t)(uintptr_t)dst, 0xe8, (uint32_t)G32(VA_g_screen_pitch), 0);
    }
    int32_t l18    = w - 0x29;                          /* [ebp-0x18] */
    int32_t edge_x = edi + 0x23;
    {
        uint8_t *dst = screen_xy_to_framebuffer_ptr(edge_x, esi);
        dlg_call4(0x18e68, (uint32_t)(uintptr_t)dst, 0x300, (uint32_t)G32(VA_g_screen_pitch), 0);
    }
    edge_x += 1;
    while ((uint32_t)l18 >= 0x3a) {
        uint8_t *dst = screen_xy_to_framebuffer_ptr(edge_x, esi);
        l18 -= 0x36;
        dlg_call4(0x18e68, (uint32_t)(uintptr_t)dst, 0x308, (uint32_t)G32(VA_g_screen_pitch), 0);
        edge_x += 0x36;
    }
    {
        uint8_t *dst = screen_xy_to_framebuffer_ptr(w + edi - 0x3e, esi);
        dlg_call4(0x18e68, (uint32_t)(uintptr_t)dst, 0x310, (uint32_t)G32(VA_g_screen_pitch), 0);
    }
    {
        int32_t stretch_ebx = (int32_t)G32(VA_g_screen_pitch) + ((w - 0x28) << 16);
        uint8_t *dst = screen_xy_to_framebuffer_ptr(edi + 0x23, ey);
        dlg_call4(0x18e68, (uint32_t)(uintptr_t)dst, 0xf8, (uint32_t)stretch_ebx, 0);
    }

    /* 14. close-up item image (p4 -> *p4 -> **p4) */
    int32_t lc = esi + 0x24;                            /* [ebp-0xc] */
    uint32_t pp = RD(p4);
    if (pp != 0) {
        uint32_t img = RD(pp);
        if (img != 0) {
            int32_t iy    = ((0x38 - (int32_t)(int16_t)RW(img + 6)) >> 2) + esi + 4;  /* sar 2 */
            int32_t scale = (G8(VA_g_hires_line_doubling_flag) == 0) ? 1 : 0;
            int32_t l2c   = G32(VA_g_screen_pitch);
            int32_t ix    = ((0x1c - (int32_t)(int16_t)RW(img + 4)) >> 1) + edi + 4;  /* sar 1 */
            scale += 1;
            uint8_t *dst = screen_xy_to_framebuffer_ptr(ix, iy);
            dlg_call4(0x13544, img, (uint32_t)(uintptr_t)dst, (uint32_t)l2c, (uint32_t)scale);  /* blit_item_icon */
            G16(VA_g_inspect_popup_layout) = 0x1c;
            G16(VA_g_inventory_inspect_request + 0x4) = (uint16_t)(esi + 4);
        }
    }

    /* 15. item name text */
    if (name_w != 0) {
        G8(VA_g_selected_item_primary + 0x19) = G8(VA_g_map_menu_marker_selected);
        G8(VA_g_selected_item_primary + 0x18) = 1;
        dlg_call4(0x1a079, GADDR(VA_g_selected_item_primary + 0x18), 0, (uint32_t)(edi + 0x23), (uint32_t)(esi + 2));  /* draw_text_at_screen_xy */
    }

    /* 16. mark the popup footprint dirty */
    dlg_call4(0x15b5b, (uint32_t)(edi - 4), (uint32_t)(esi - 4),
                       (uint32_t)(w + edi + 3), (uint32_t)(h + esi + 4));  /* register_dirty_rect */

    /* 17. up-arrow (node+4 bit 0x40) */
    int32_t l38 = 0, l34 = 0;                           /* [ebp-0x38], [ebp-0x34] */
    G32(VA_g_inventory_ui_action + 0x4) = 0;
    edi += 0x11;
    if (RB(node + 4) & 0x40) {
        int32_t sprite;
        if (RB(node + 7) & 0x80) {
            sprite = 0x98;
        } else {
            sprite = 0xa0;
            G32(VA_g_inventory_ui_action + 0x4) = 1;
        }
        uint32_t rec = resolve_reloc_record_fields(&l34, &l38, (uint32_t)sprite, 1);
        if (rec != 0) {
            G16(VA_g_inspect_popup_layout + 0x6) = (uint16_t)(lc + l38);
            dlg_call4(0x1a10a, rec, (uint32_t)(l34 + edi), (uint32_t)(lc + l38), 0);  /* blit_das_image_at_xy */
            G16(VA_g_inspect_popup_layout + 0x8) = (uint16_t)RW(rec + 6);
            lc += (int32_t)(int16_t)RW(rec + 6) + 4;
        }
    }

    /* 18. down-arrow (choices present) */
    if (nchoices != 0) {
        l34 = 0; l38 = 0;
        uint32_t rec = resolve_reloc_record_fields(&l34, &l38, 0x88, 1);
        if (rec != 0) {
            G16(VA_g_inspect_popup_layout + 0xe) = (uint16_t)(lc + l38);
            dlg_call4(0x1a10a, rec, (uint32_t)(l34 + edi), (uint32_t)(lc + l38), 0);
            G16(VA_g_inspect_popup_layout + 0x10) = (uint16_t)RW(rec + 6);
            lc += (int32_t)(int16_t)RW(rec + 6) + 4;
        }
    }

    /* 19. multi-page scroll-thumb extent */
    if ((uint32_t)G32(VA_g_inspect_page_count) > 1) {
        G16(VA_g_inspect_popup_layout + 0x1e) = (uint16_t)(lc + l38);
        G16(VA_g_inspect_popup_layout + 0x26) = (uint16_t)(lc + l38 + 0x17);
    }

    /* 20. return the saved-region handle */
    return ret_region;
}

/* ============================ dispatch_dialogue_ui_action (0x1b4e5) ============================
 * EAX=action code, EDX=flags(->EBX; bits 2/8 gate the pick-up path). Returns EAX: 1 for the two
 * latch actions (action 1 -> g_0x7fecc; action 4 scroll-commit -> g_0x8118c/90/88), else 0 (via the
 * shared 0x1bac8 epilogue which also clears the click-edge bytes 0x7e938/0x7e939).
 *
 * The shared inspect-popup / inventory-grid input router: tab select, choice-highlight toggle,
 * pagination arrows, grid-cell click (pick up / use-on-self / combine / swap / equip), held-item
 * use (primary/secondary), and list scroll. NON-IDEMPOTENT (mutates the inventory arena, fires
 * dbase100 label lookups, gives/removes items) -> in-game-only, NOT oracle-able. Every inventory /
 * DAS / dbase100 callee is bridged via call_orig (the inventory subsystem lifts them; here we just
 * route). Transcribed strictly from the disasm; the goto labels mirror the asm's shared tails
 * (0x1bac8 epilogue, 0x1b78e refresh tail, 0x1b886/0x1b906/0x1b9a7 slot sub-paths). REGISTER ABI_EAX4
 * (captures the EAX return; only EAX+EDX are read as inputs). */
uint32_t dispatch_dialogue_ui_action(uint32_t action, uint32_t p2, uint32_t p3, uint32_t p4)
{
    (void)p3; (void)p4;
    uint32_t flags = p2;          /* EDX -> EBX */
    int32_t  cell = 0, entry_idx = 0;

    if (action < 0x17) {
        /* ---------------- LOW: actions 0..0x16 ---------------- */
        if (action < 4) {
            if (action < 2) {
                if (action == 1) { G32(VA_g_inventory_panel_open + 0x8) = (int32_t)action; return 1; }  /* 0x1b53c */
                goto done;                                                       /* action 0 */
            }
            if (action == 2) {
                /* 0x1b546: re-eval the inspect-choice highlight marker */
                if (G32(VA_g_inventory_panel_open + 0x4) == 0) goto done;
                uint32_t blk = dlg_call4_r(0x1dda8, (uint32_t)G32(VA_g_inventory_panel_open + 0x4), 0, 0, 0);   /* scan_tag4_chunk */
                if (blk == 0) goto done;
                dlg_call4(0x1db89, blk, 0, 0, 0);                                       /* eval_dialogue_record_condition_with_cleanup */
                if (G8(VA_g_inventory_ui_action + 0x4) & 1) goto done;
                dlg_call4(0x1c512, (uint16_t)RW(GADDR(VA_g_inspect_popup_layout + 0x6)), 0xa0, 0, 0);           /* draw_reloc_ui_row(y=[0x80b48],off=0xa0) */
                G32(VA_g_inventory_ui_action + 0x4) += 1;
                goto done;
            }
            /* action 3: 0x1ba96 choice-panel toggle */
            if (G32(VA_g_choice_line_count) == 0) { G32(VA_g_choice_reveal_ramp) = 2; goto done; }                     /* 0x1b6b1 */
            if (G32(VA_g_choice_interaction_mode) == 0) {                                                     /* 0x1b686 */
                G32(VA_g_choice_interaction_mode) = 1;
                if (!(G8(VA_g_inventory_ui_action + 0x4) & 2)) {
                    dlg_call4(0x1c512, (uint16_t)RW(GADDR(VA_g_inspect_popup_layout + 0xe)), 0x90, 0, 0);       /* draw_reloc_ui_row(y=[0x80b50],off=0x90) */
                    G32(VA_g_inventory_ui_action + 0x4) += 2;
                }
                G32(VA_g_choice_reveal_ramp) = 2;
                goto done;
            }
            G32(VA_g_choice_interaction_mode) = 0;
            G32(VA_g_choice_reveal_ramp) = 2;
            goto done;
        }
        if (action <= 4) {
            /* action 4: 0x1b63d scroll-drag state machine */
            if (G32(VA_g_inspect_popup_state) == 1) { G32(VA_g_inspect_popup_state) = 2; goto done; }
            if (G32(VA_g_inspect_popup_state) != 3) goto done;
            G32(VA_g_inspect_info_available + 0x4) = G32(VA_g_mouse_x);
            G32(VA_g_inspect_info_available + 0x8) = G32(VA_g_mouse_y);
            G32(VA_g_inspect_info_available) += 1;
            return 1;                                                                    /* 0x1b681 */
        }
        if (action < 6) {
            /* action 5: 0x1b595 page-up arrow */
            if (G32(VA_g_inspect_current_page) == 0) goto done;
            int32_t di = (uint16_t)RW(GADDR(VA_g_inspect_popup_layout + 0x1e));
            int32_t si = (uint16_t)RW(GADDR(VA_g_inspect_popup_layout + 0x22));
            G32(VA_g_inspect_current_page) -= 1;
            dlg_call4(0x15b5b, (uint32_t)si, (uint32_t)di, (uint32_t)(si + 0x12), (uint32_t)(di + 0x12));  /* register_dirty_rect */
            uint32_t dst = (uint32_t)(uintptr_t)screen_xy_to_framebuffer_ptr((int32_t)si, (int32_t)di);  /* re-point 0x18040 (pure ptr calc; pitch from [0x85498], EBX preserved) */
            dlg_call4(0x18e68, dst, 0x120, (uint32_t)G32(VA_g_screen_pitch), 0);                                     /* blit_reloc_das_image */
            G32(VA_g_inspect_page_count + 0x84) += 1;
            goto done;
        }
        if (action <= 6) {
            /* action 6: 0x1b5eb page-down arrow */
            if ((uint32_t)(G32(VA_g_inspect_page_count) - 1) <= (uint32_t)G32(VA_g_inspect_current_page)) goto done;
            int32_t si = (uint16_t)RW(GADDR(VA_g_inspect_popup_layout + 0x26));
            int32_t di = (uint16_t)RW(GADDR(VA_g_inspect_popup_layout + 0x2a));
            dlg_call4(0x15b5b, (uint32_t)di, (uint32_t)si, (uint32_t)(di + 0x12), (uint32_t)(si + 0x12));  /* register_dirty_rect */
            uint32_t dst = (uint32_t)(uintptr_t)screen_xy_to_framebuffer_ptr((int32_t)di, (int32_t)si);  /* re-point 0x18040 */
            dlg_call4(0x18e68, dst, 0x138, (uint32_t)G32(VA_g_screen_pitch), 0);                                     /* blit_reloc_das_image */
            G32(VA_g_inspect_current_page) += 1;
            G32(VA_g_inspect_page_count + 0x84) += 1;
            goto done;
        }
        /* actions 7..0x16: 0x1ba81 */
        G32(VA_g_pending_choice_accept_index) = (int32_t)(action - 6);
        goto done;
    }

    /* ---------------- action >= 0x17 ---------------- */
    if (action <= 0x1b) {
        /* 0x1b6c0: select inventory tab (action-0x17) */
        int32_t tab = (int32_t)(action - 0x17);
        G32(VA_g_inventory_synthetic_primary + 0x4) = 0x20;
        G32(VA_g_cursor_active_list) = tab;
        dlg_call4(0x19d30, (uint32_t)tab, 0, 0, 0);   /* build_inventory_entry_list */
        G8(VA_g_inventory_dirty_flags) += 2;
        dlg_call4(0x1a2d2, 0, 0, 0, 0);               /* draw_inventory_tabs */
        goto done;
    }

    /* precompute (0x1b4fd-0x1b50e): cl = byte[0x7f571]+2, pedx = [0x80b38]<<2 (current-tab scroll slot) */
    uint8_t cl   = (uint8_t)(G8(VA_g_inventory_dirty_flags) + 2);
    int32_t pedx = G32(VA_g_cursor_active_list) << 2;

    if (action < 0x29) {
        if (action < 0x26) { goto inventory_slot; }            /* 0x1c..0x25 grid cell */
        if (action == 0x26) {
            /* 0x1b9cc: commit the held cursor item */
            uint32_t r = dlg_call4_r(0x18a2a, 0, 0, 0, 0);     /* try_interrupt_dialogue_voice */
            if (r != 0) goto done;
            G32(VA_g_ui_panel_anchor_y + 0x4) += 1;
            dlg_call4(0x1a88b, 0, 0, 0, 0);                    /* commit_held_cursor_item */
            goto done;
        }
        if (action == 0x28) {
            /* 0x1b9f8: scroll list up */
            G32(VA_g_inventory_synthetic_primary + 0x4) = 0x20;
            if ((int32_t)G32(VA_g_cursor_scroll_offsets + pedx) <= 0) goto done;
            G8(VA_g_inventory_dirty_flags) = cl;
            G32(VA_g_cursor_scroll_offsets + pedx) -= 5;
            goto done;
        }
        goto done;                                             /* action 0x27 */
    }
    if (action == 0x29) {
        /* 0x1ba21: scroll list down */
        int32_t lim = G32(VA_g_cursor_entry_count) - 9;
        G32(VA_g_inventory_synthetic_primary + 0x4) = 0x20;
        if ((uint32_t)lim <= (uint32_t)G32(VA_g_cursor_scroll_offsets + pedx)) goto done;
        G8(VA_g_inventory_dirty_flags) = cl;
        G32(VA_g_cursor_scroll_offsets + pedx) += 5;
        goto done;
    }
    if (action == 0x2a) goto use_primary;
    if (action == 0x2b) goto use_secondary;
    if (action == 0x2c) { G32(VA_g_inventory_options_request) = 1; goto done; }
    goto done;                                                 /* action >= 0x2d */

use_primary: {
    /* 0x1b6e8: held-item primary use / hover-label */
    uint32_t held = (uint32_t)G32(VA_g_current_cursor_entry);
    if (G8(VA_g_cursor_primary_action_flag) != 0) {
        if (G32(VA_g_displayed_item_left) == 0) goto done;
        uint32_t item = (uint32_t)G32(VA_g_displayed_item_left);
        G32(VA_g_inventory_inspect_request) = (int32_t)item;
        dlg_call4(0x1c0b1, RD(item + 8), 0, 0, 0);             /* format_inventory_item_label */
        goto done;
    }
    if (G8(VA_g_cursor_secondary_action_flag) == 0) goto done;
    G32(VA_g_current_cursor_entry) = 0;
    if (held != 0) {
        dlg_call4(0x184ab, RD(held + 4), RD(held + 8), 0, 0);  /* activate_weapon_item */
        dlg_call4(0x19d30, (uint32_t)G32(VA_g_cursor_active_list), 0, 0, 0);   /* build_inventory_entry_list */
        G8(VA_g_inventory_dirty_flags) += 2;
        dlg_call4(0x19ee6, 0xa, 0, 0, 0);                      /* draw_panel_slot_tile */
        dlg_call4(0x1a2ef, 0, 0, 0, 0);                        /* draw_equipped_item_left */
        goto refresh_done;
    }
    if (G32(VA_g_selected_item_secondary) == 0) goto refresh_done;
    uint32_t p = (uint32_t)G32(VA_g_selected_item_secondary);
    G32(VA_g_inventory_active_tab) = (int32_t)dlg_call4_r(0x1b0b2, (uint32_t)(int32_t)(int16_t)RW(p), 0, 0, 0);  /* get_item_tab_index */
    dlg_call4(0x1a2b5, 0, 0, 0, 0);                            /* select_inventory_tab */
    dlg_call4(0x1b0e3, (uint32_t)G32(VA_g_left_hand_item), 0, 0, 0);       /* scroll_entry_into_view */
    goto refresh_done;
}

use_secondary: {
    /* 0x1b798: held-item secondary use / hover-label */
    uint32_t held = (uint32_t)G32(VA_g_current_cursor_entry);
    if (G8(VA_g_cursor_primary_action_flag) != 0) {
        if (G32(VA_g_displayed_item_right) == 0) goto done;
        uint32_t item = (uint32_t)G32(VA_g_displayed_item_right);
        G32(VA_g_inventory_inspect_request) = (int32_t)item;
        dlg_call4(0x1c0b1, RD(item + 8), 0, 0, 0);             /* format_inventory_item_label */
        goto done;
    }
    if (G8(VA_g_cursor_secondary_action_flag) == 0) goto done;
    G32(VA_g_current_cursor_entry) = 0;
    if (held != 0) {
        G32(VA_g_selected_item_primary) = (int32_t)RD(held + 4);
        dlg_call4(0x19d30, (uint32_t)G32(VA_g_cursor_active_list), 0, 0, 0);   /* build_inventory_entry_list */
        G8(VA_g_inventory_dirty_flags) += 2;
        dlg_call4(0x19ee6, 0xb, 0, 0, 0);                      /* draw_panel_slot_tile */
        dlg_call4(0x1bfaa, 0, 0, 0, 0);                        /* draw_equipped_item_right */
        goto refresh_done;
    }
    if (G32(VA_g_selected_item_primary) == 0) goto refresh_done;
    uint32_t p = (uint32_t)G32(VA_g_selected_item_primary);
    G32(VA_g_inventory_active_tab) = (int32_t)dlg_call4_r(0x1b0b2, (uint32_t)(int32_t)(int16_t)RW(p), 0, 0, 0);  /* get_item_tab_index */
    dlg_call4(0x1a2b5, 0, 0, 0, 0);                            /* select_inventory_tab */
    dlg_call4(0x1b0e3, (uint32_t)G32(VA_g_right_hand_item), 0, 0, 0);       /* scroll_entry_into_view */
    goto refresh_done;
}

inventory_slot: {
    /* 0x1b82c: grid-cell click (action-0x1c = cell) */
    int32_t tab = G32(VA_g_cursor_active_list);
    cell      = (int32_t)(action - 0x1c);
    entry_idx = G32(VA_g_cursor_scroll_offsets + tab * 4) + cell;
    uint32_t entry_ptr = (uint32_t)GADDR(VA_g_cursor_entry_table) + (uint32_t)(entry_idx * 0xc);
    if (entry_ptr == (uint32_t)G32(VA_g_current_cursor_entry)) {
        if (G8(VA_g_cursor_secondary_action_flag) != 0) { G32(VA_g_current_cursor_entry) = 0; goto done; }        /* secondary on held -> deselect */
        if (G8(VA_g_cursor_primary_action_flag) == 0 && G32(VA_g_inventory_synthetic_primary) == 0) goto slot_886;
        dlg_call4(0x1b141, (uint32_t)entry_idx, 0, 0, 0);             /* use_item_on_self */
        goto done;
    }
slot_886:
    if (G32(VA_g_current_cursor_entry) == 0) goto slot_pickup;
    if ((uint32_t)entry_idx >= (uint32_t)G32(VA_g_cursor_entry_count)) goto done;
    if (G8(VA_g_cursor_primary_action_flag) != 0 || G32(VA_g_inventory_synthetic_primary) != 0) {
        dlg_call4(0x1b26d, (uint32_t)entry_idx, 0, 0, 0);            /* combine_held_item_with_target */
        goto done;
    }
    uint32_t held = (uint32_t)G32(VA_g_current_cursor_entry);
    if (held == (uint32_t)G32(VA_g_displayed_item_left)) { dlg_call4(0x19ee6, 0xa, 0, 0, 0); goto done; }  /* draw_panel_slot_tile */
    if (held == (uint32_t)G32(VA_g_displayed_item_right)) { dlg_call4(0x19ee6, 0xb, 0, 0, 0); goto done; }
    dlg_call4(0x1b007, entry_ptr, held, 0, 0);                       /* swap_inventory_entries(eax=clicked entry, edx=held) */
    G32(VA_g_current_cursor_entry) = 0;
    G8(VA_g_inventory_dirty_flags) |= 2;
    goto refresh_done;
}

slot_pickup: {
    /* 0x1b906: no held item -> pick up the clicked entry */
    dlg_call4(0x1bb12, (uint32_t)cell, 0, 0, 0);                     /* move_cursor_entry_clamped */
    if (!(flags & 2) && !(flags & 8)) goto slot_refresh_setb3c;
    if ((uint32_t)entry_idx >= (uint32_t)G32(VA_g_cursor_entry_count)) goto slot_refresh_setb3c;
    uint32_t eptr = (uint32_t)GADDR(VA_g_cursor_entry_table) + (uint32_t)(entry_idx * 0xc);
    if (eptr == (uint32_t)G32(VA_g_left_hand_item)) {
        G32(VA_g_displayed_item_left) = 0;
        G32(VA_g_left_hand_item) = 0;
        dlg_call4(0x184ab, 0, 0, 0, 0);                              /* activate_weapon_item(0,0) */
        dlg_call4(0x19ee6, 0xa, 0, 0, 0);                            /* draw_panel_slot_tile */
        G8(VA_g_inventory_dirty_flags) += 2;
    }
    if (eptr == (uint32_t)G32(VA_g_right_hand_item)) {
        G32(VA_g_right_hand_item) = 0;
        G32(VA_g_displayed_item_right) = 0;
        G32(VA_g_selected_item_primary) = 0;
        dlg_call4(0x19ee6, 0xb, 0, 0, 0);                            /* draw_panel_slot_tile */
        G8(VA_g_inventory_dirty_flags) += 2;
    }
    G32(VA_g_current_cursor_entry) = (int32_t)eptr;
    goto refresh_done;
}

slot_refresh_setb3c:
    /* 0x1b9a7 */
    dlg_call4(0x1c469, 0, 0, 0, 0);                                  /* refresh_inventory_grid */
    if ((uint32_t)entry_idx >= (uint32_t)G32(VA_g_cursor_entry_count)) goto done;
    G32(VA_g_inventory_inspect_request) = (int32_t)((uint32_t)GADDR(VA_g_cursor_entry_table) + (uint32_t)(entry_idx * 0xc));
    goto done;

refresh_done:
    /* 0x1b78e */
    dlg_call4(0x1c469, 0, 0, 0, 0);                                  /* refresh_inventory_grid */
    goto done;

done:
    /* 0x1bac8: clear the click-edge bytes + return 0 */
    G8(VA_g_cursor_primary_action_flag) = 0;
    G8(VA_g_cursor_secondary_action_flag) = 0;
    return 0;
}

/* ============================ load_inspect_document_page (0x1951d) ============================
 * EAX=prior handle, EDX=param_2, EBX=panel descriptor. Draws the up/down pagination arrows (when
 * g_inspect_page_count 0x810fc > 1), frees the prior DAS handle, loads the current page's resource
 * (table 0x81100[page]) + decodes it into the panel descriptor, and clamps the scroll offsets.
 * Bridges screen_xy_to_framebuffer_ptr / blit_reloc_das_image / register_dirty_rect /
 * pool_free_handle / load_das_cache_resource / decode_das_to_padded_buffer (threading their
 * multi-reg outputs) -> in-game. Returns the decoded buffer ptr (or the prior handle). */
int32_t load_inspect_document_page(uint32_t param_1, uint32_t param_2, uint32_t param_3,
                                          uint32_t param_4)
{
    (void)param_4;
    uint32_t esi = param_3;
    if ((uint32_t)G32(VA_g_inspect_page_count) > 1) {
        WW(GADDR(VA_g_inspect_popup_layout + 0x20), 0x13);
        WW(GADDR(VA_g_inspect_popup_layout + 0x28), 0x13);
        int32_t sprite = (G32(VA_g_inspect_current_page) != 0) ? 0x118 : 0x128;
        int32_t x   = (uint16_t)RW(GADDR(VA_g_inspect_popup_layout + 0x22));
        int32_t y   = (uint16_t)RW(GADDR(VA_g_inspect_popup_layout + 0x1e));
        dlg_call4(0x15b5b, (uint32_t)x, (uint32_t)y, (uint32_t)(x + 0x12), (uint32_t)(y + 0x28));
        /* screen_xy_to_framebuffer_ptr — lifted, direct; EBX(pitch) preserved by the callee,
         * read live like every original site */
        uint8_t *fbp = screen_xy_to_framebuffer_ptr(x, y);
        dlg_call4(0x18e68, (uint32_t)(uintptr_t)fbp, (uint32_t)sprite,
                  (uint32_t)G32(VA_g_screen_pitch), 0);                    /* blit_reloc_das_image */

        int32_t x2 = (uint16_t)RW(GADDR(VA_g_inspect_popup_layout + 0x2a));
        int32_t sprite2 = ((uint32_t)(G32(VA_g_inspect_page_count) - 1) > (uint32_t)G32(VA_g_inspect_current_page)) ? 0x130 : 0x140;
        int32_t y2 = (uint16_t)RW(GADDR(VA_g_inspect_popup_layout + 0x26));
        /* re-point 0x18040 screen_xy_to_framebuffer_ptr (pure ptr calc; EBX preserved == input pitch). */
        uint8_t *fbp2 = screen_xy_to_framebuffer_ptr(x2, y2);
        dlg_call4(0x18e68, (uint32_t)(uintptr_t)fbp2, (uint32_t)sprite2, (uint32_t)G32(VA_g_screen_pitch), 0);
    }

    uint32_t res = RD(GADDR(VA_g_inspect_page_count + 0x4) + (uint32_t)G32(VA_g_inspect_current_page) * 4);
    G32(VA_g_inspect_page_count + 0x84) = 0;
    int32_t ret = (int32_t)param_1;
    if (res != 0) {
        if (param_1 != 0)
            dlg_call4(0x360b3, (uint32_t)G32(VA_g_das_cache_heap_handle), param_1, 0, 0);  /* pool_free_handle */
        /* re-pointed: load_das_cache_resource 0x1869b + decode_das_to_padded_buffer
         * 0x1874d chain, both direct-C. The old marker feared io3.ebx (esi+0x10) was a dropped 3rd
         * input, but the disasm resolves it: 0x1869b `xor ebx,ebx` @0x186af zeroes EBX before any
         * read (EBX DEAD) and `push ebx`/`pop ebx` PRESERVE it, so its return EBX == esi+0x10; and
         * decode's lifted proto is 3-arg (src_h, *out_w=esi+0xc, *out_h=esi+0x10) — it already
         * carries that pointer (writes *out_h @0x18773). So both protos are complete. Oracle-neutral:
         * load_inspect_document_page is live-swap/in-game tier (NOT oracle-tested), and both callees
         * are lifted (decode is oracle-verified in test_das_assets; load is re-pointed in
         * menu_hud_ui/inventory). */
        uint32_t h = load_das_cache_resource(res, param_2);   /* 0x1869b (EAX=idx, EDX=handle) */
        ret = (int32_t)decode_das_to_padded_buffer(h,        /* 0x1874d (EAX=src, EDX/EBX outs) */
                          (uint32_t *)(uintptr_t)(esi + 0xc),
                          (uint32_t *)(uintptr_t)(esi + 0x10));
        WD(esi, (uint32_t)ret);
        if ((uint32_t)((int32_t)RD(esi + 0x14) + (int32_t)RD(esi + 0x20)) > (uint32_t)RD(esi + 0xc)) {
            int32_t v = (int32_t)RD(esi + 0xc) - (int32_t)RD(esi + 0x14);
            WD(esi + 0x20, (uint32_t)v);
            if (v < 0) WD(esi + 0x20, 0);
        }
        if ((uint32_t)((int32_t)RD(esi + 0x18) + (int32_t)RD(esi + 0x24)) > (uint32_t)RD(esi + 0x10)) {
            int32_t v = (int32_t)RD(esi + 0x10) - (int32_t)RD(esi + 0x18);
            WD(esi + 0x24, (uint32_t)v);
            if (v < 0) WD(esi + 0x24, 0);
        }
    }
    WD(esi + 0x28, 4);
    return ret;
}

/* ============================ run_timed_message_sequence (0x1fce2) ============================
 * EAX=mode, EDX=param_2, EBX=param_3, ECX=param_4. The DBASE100 cutscene-SUBTITLE_SEQ player.
 * mode 0 = render the next timed-text entry from the sequence buffer 0x83b30 (entry [0]=size,
 * [2]=duration,[4]=color idx,+5=text) when the frame timer expires, via layout + draw_text_to_buffer
 * + add_dirty_rect, then flush to the VESA framebuffer (bank reg 0x71f04); modes 1/2 =
 * input_ring_dequeue skip; mode 3 = reset. Bridges the framebuffer/palette/input callees. */
uint32_t run_timed_message_sequence(uint32_t mode, uint32_t param_2, uint32_t param_3,
                                           uint32_t param_4)
{
    int32_t  meta[16];
    uint8_t  outbuf[0x400];
    (void)param_2;

    if (mode >= 2) {
        if (mode == 2) {
            int32_t r = (int32_t)input_ring_dequeue();
            if (r == 1) { G32(VA_g_dialogue_busy_flag + 0x32) += 1; return 2; }
            return 0;
        }
        if (mode != 3)
            return 0;
        if (param_3 != 0)
            G32(VA_g_dialogue_busy_flag + 0x3e) = (int32_t)param_3;
        if (G32(VA_g_choice_selected_index + 0x14) == 0)
            return 0;
        dlg_call4(0x4b360, GADDR(VA_g_dialogue_busy_flag + 0x62), 0, 0x100, 0);       /* mem_fill(color cache) */
        if (G32(VA_g_dialogue_busy_flag + 0x4a) == 0)
            return 0;
        G32(VA_g_dialogue_busy_flag + 0x5a) = 0;
        G32(VA_g_dialogue_busy_flag + 0x46) = G32(VA_g_dialogue_busy_flag + 0x4a);
        return 0;
    }
    if (mode != 0)
        return 0;

    if (G32(VA_g_dialogue_busy_flag + 0x46) != 0) {
        G32(VA_g_dialogue_busy_flag + 0x4e) = (int32_t)((uint32_t)G32(VA_g_gdv_audio_stream_base + 0x4) / 0x89d);
        uint32_t esi = (uint32_t)G32(VA_g_dialogue_busy_flag + 0x46);
        uint32_t edx = (uint32_t)G32(VA_g_dialogue_busy_flag + 0x4e) + 1;
        uint32_t dur = (uint16_t)RW(esi + 2);
        if (edx >= dur) {                                       /* entry's timer expired */
            if (G32(VA_g_dialogue_busy_flag + 0x5a) != 0) {
                dlg_call4(0x1fc5c, 0, 0, 0, 0);                 /* clear_cutscene_region */
                G32(VA_g_dialogue_busy_flag + 0x4a) = 0;
            } else {
                if ((int32_t)(int16_t)RW(esi) > 6) {            /* renderable entry */
                    uint8_t coloridx = RB(esi + 4);
                    uint32_t textptr = esi + 5;
                    if (RB(GADDR(VA_g_dialogue_busy_flag + 0x62) + coloridx) == 0) {
                        WB(GADDR(VA_g_dialogue_busy_flag + 0x62) + coloridx,           /* find_nearest_palette_index — direct */
                           find_nearest_palette_index(coloridx,
                               (const uint8_t *)(uintptr_t)(uint32_t)G32(VA_g_dialogue_busy_flag + 0x3e)));
                    }
                    G8(VA_g_timed_message_color) = RB(GADDR(VA_g_dialogue_busy_flag + 0x62) + coloridx);
                    layout_timed_message_text(meta, outbuf, (const uint8_t *)(uintptr_t)textptr,
                                                     G32(VA_g_screen_pitch), 2);
                    int32_t flag = 0xc;
                    int32_t edi = (G32(VA_g_screen_height) >> 1) + 0x48;
                    int32_t basey = meta[2];
                    if (G8(VA_g_hires_line_doubling_flag) != 0) { basey += basey; flag = 0xd; }
                    if ((uint32_t)G32(VA_g_screen_height) >= 0x12d) edi += 0x48;
                    int32_t pitch = G32(VA_g_screen_pitch);
                    uint32_t fb = (uint32_t)G32(VA_g_framebuffer_ptr);
                    int32_t rectx0 = meta[0];
                    int32_t rectx1 = meta[1] - 1;
                    dlg_call4(0x14d04, (uint32_t)meta[4], fb + (uint32_t)(edi * pitch),
                                       (uint32_t)pitch, (uint32_t)flag);
                    int32_t recty1 = basey + (edi - 1);
                    int32_t edi2 = edi + 1;
                    dlg_call4(0x15b69, (uint32_t)rectx0, (uint32_t)edi2,
                                       (uint32_t)rectx1, (uint32_t)recty1);      /* add_dirty_rect */
                    G32(VA_g_dialogue_busy_flag + 0x52) = rectx0;
                    G32(VA_g_dialogue_busy_flag + 0x5a) = rectx1;
                    G32(VA_g_dialogue_busy_flag + 0x56) = edi2;
                    G32(VA_g_dialogue_busy_flag + 0x5e) = recty1;
                }
                G32(VA_g_dialogue_busy_flag + 0x4a) = G32(VA_g_dialogue_busy_flag + 0x46);
                G32(VA_g_dialogue_busy_flag + 0x46) += (int32_t)(uint16_t)RW(esi);
            }
        }

        /* flush the dirty region to the VESA framebuffer (bank register 0x71f04) */
        WW(GADDR(VA_g_current_vesa_bank), 0x22b8);
        uint16_t dx = RW(GADDR(VA_g_init_stage_error_strings + 0x134));
        if (param_3 != 0) {
            int32_t edi = 0xa000, si = 0xa000;
            if (G32(VA_g_dpcm_step_table + 0x440) == G32(VA_g_dpcm_step_table + 0x43c)) {
                WW(GADDR(VA_g_init_stage_error_strings + 0x134), 0xa000);
                dlg_call4(0x15dd9, 0, 0, 0, 0);
            } else {
                if (G32(VA_g_dpcm_step_table + 0x440) != 0) edi = (G32(VA_g_dpcm_step_table + 0x440) >> 4) + 0xa000;
                if (G32(VA_g_dpcm_step_table + 0x43c) != 0) si  = (G32(VA_g_dpcm_step_table + 0x43c) >> 4) + 0xa000;
                WW(GADDR(VA_g_init_stage_error_strings + 0x134), (uint16_t)edi);
                dlg_call4(0x15dd9, 0, 0, 0, 0);
                if (si != edi) {
                    WW(GADDR(VA_g_init_stage_error_strings + 0x134), (uint16_t)si);
                    dlg_call4(0x15dd9, 0, 0, 0, 0);
                }
            }
            WW(GADDR(VA_g_init_stage_error_strings + 0x134), dx);
        } else {
            WW(GADDR(VA_g_init_stage_error_strings + 0x134), 0xa000);
            dlg_call4(0x15dd9, 0, 0, 0, 0);
            int32_t bank = 0;
            if (G32(VA_g_dpcm_step_table + 0x440) != 0) bank = G32(VA_g_dpcm_step_table + 0x440);
            else if (G32(VA_g_dpcm_step_table + 0x43c) != 0) bank = G32(VA_g_dpcm_step_table + 0x43c);
            if (bank != 0) {
                WW(GADDR(VA_g_init_stage_error_strings + 0x134), (uint16_t)((bank >> 4) + 0xa000));
                dlg_call4(0x15dd9, 0, 0, 0, 0);
            }
            WW(GADDR(VA_g_init_stage_error_strings + 0x134), dx);
        }
    }

    if (param_3 != 0)
        G32(VA_g_dialogue_busy_flag + 0x3a) = (int32_t)param_3;
    G32(VA_g_dialogue_busy_flag + 0x42) = (int32_t)param_4 >> 0x10;
    return 0;
}

/* ============================ dialogue_window_open_hook (0x1ef9e) ============================
 * DEAD in this build (6 bytes = the shared pop-5+ret epilogue of render_active_timed_message;
 * never entered as a standalone call). The decompile models it as a no-op. Present only to
 * close the coverage gate. */
void dialogue_window_open_hook(void)
{
}
