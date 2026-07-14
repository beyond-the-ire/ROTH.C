/* obj3_build — the fixed loader that turns the C-expressed data (obj3_data.c)
 * back into a relocated image. This is the small, reusable piece a real
 * image-free build would call once at init; it is NOT generated.
 *
 * Order matters (workstream-B owned symbols, obj3_owned.json):
 *   1. zero-fill          — ownership reached 100% (obj3_template is
 *                           all-zero, proven by datac-probe's residue-zero gate
 *                           native + m32), so the loader no longer COPIES the
 *                           anonymous blob — it memset(0)s the arena. The
 *                           obj3_template[] array therefore goes unreferenced in
 *                           the imgfree link and --gc-sections drops it (leg 2 of
 *                           make verify-swap: `nm | grep obj3_template` is EMPTY).
 *                           The array stays DEFINED in obj3_data.c so the datac
 *                           lane's residue-zero gate keeps re-proving all-zero.
 *                           memset is EQUIVALENT to the old copy (template==0) and
 *                           STRICTLY STRONGER for the dual-base byte-identity gate:
 *                           a non-zero residue byte would now diverge (memset zeros
 *                           it) instead of being masked by the template copy.
 *   2. owned overlay      — each real standalone C definition (obj3_owned.c)
 *                           is copied to its canon offset: the C object is the
 *                           ONLY source of those bytes (drop this loop and
 *                           datac-probe goes red structurally);
 *   3. relocation pass    — pointer slots re-resolved for the given bases
 *                           (the manifest generators hard-error if a reloc
 *                           slot ever falls inside an owned region). */
#include <string.h>
#include "obj3_data.h"
#include "obj3_owned.h"

void obj3_build(unsigned char *out, const uint32_t base[4])
{
    memset(out, 0, OBJ3_SIZE);   /* template is all-zero (residue-zero gate) — no blob copy */
    for (unsigned i = 0; i < obj3_owned_count; i++) {
        const obj3_owned_t *o = &obj3_owned_tab[i];
        const unsigned char *src = (const unsigned char *)o->ptr;
        for (unsigned j = 0; j < o->size; j++)
            out[o->off + j] = src[j];
    }
    for (unsigned i = 0; i < obj3_nrelocs; i++) {
        const obj3_reloc_t *r = &obj3_relocs[i];
        uint32_t v = base[r->obj] + r->off;       /* re-resolve the pointer */
        out[r->slot + 0] = (unsigned char)(v);
        out[r->slot + 1] = (unsigned char)(v >> 8);
        out[r->slot + 2] = (unsigned char)(v >> 16);
        out[r->slot + 3] = (unsigned char)(v >> 24);
    }
}
