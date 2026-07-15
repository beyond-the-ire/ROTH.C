/* mingw_compat.h — Windows (MinGW) cross-compile prelude.
 *
 * Force-included ahead of every translation unit on the MinGW i686 axis (and
 * NEVER on the native build). Its sole job today is to defuse a name collision
 * on `stricmp`:
 *
 *   - The engine declares `int32_t stricmp(const uint8_t *, const uint8_t *)` —
 *     the original case-insensitive string compare, carrying its own historical
 *     ABI signature.
 *   - The MinGW C runtime's <string.h> also declares a deprecated CRT alias
 *     `int stricmp(const char *, const char *)` (part of the non-standard
 *     "oldnames" set). glibc has no such alias, so the native Linux build never
 *     sees the clash — only the Windows CRT brings the second declaration, and
 *     the two signatures conflict.
 *
 * Pulling <string.h> in FIRST lets the CRT declare its `stricmp` under the real
 * name; renaming the identifier only afterward makes the engine's compare a
 * distinct symbol (roth_engine_stricmp) everywhere it is declared, defined and
 * called. Because every unit on this axis is force-included with this same
 * prelude, the rename is uniform and the symbol resolves coherently at link
 * time. (A bare command-line define renames the identifier before <string.h> is
 * parsed and merely reproduces the clash under the new name — the include-first
 * ordering here is what makes this work.)
 *
 * Suppressing the entire oldnames set instead (NO_OLDNAMES) would also strip
 * strcasecmp, which the host layer relies on — so this narrow rename, not a
 * blanket suppression, is the correct fix.
 */
#ifndef ROTH_MINGW_COMPAT_H
#define ROTH_MINGW_COMPAT_H

#include <string.h>
#define stricmp roth_engine_stricmp

#endif /* ROTH_MINGW_COMPAT_H */
