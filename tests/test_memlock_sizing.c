#include <glib.h>
#include "common.h"

/* secmem_pool_from_limits() turns a locked-memory budget (RLIMIT_MEMLOCK) into
 * the size of the libgcrypt secure pool otpclient asks for. The invariant that
 * matters for bug #1141809 is that it must never claim the whole budget: it has
 * to leave at least SECMEM_HEADROOM_VALUE unlocked so GTK/libadwaita can still
 * mlock its 16 KiB GtkPasswordEntryBuffer block for the unlock field. Before the
 * fix the app locked 100% of the budget and starved GTK, producing the reported
 * "couldn't lock 16384 bytes of memory (gtk)" warning. */

#define MIB (1048576ULL)

static void
test_budget_above_threshold_gets_default (void)
{
    /* Budget comfortably above default pool + headroom: full 64 MiB, OK. */
    gint32 pool = -1;
    g_assert_cmpint (secmem_pool_from_limits (128 * MIB, &pool), ==, MEMLOCK_OK);
    g_assert_cmpint (pool, ==, DEFAULT_MEMLOCK_VALUE);
}

static void
test_budget_exactly_at_threshold_gets_default (void)
{
    /* Exactly default + headroom is the smallest budget that still yields the
     * full pool without eating into the reserve. */
    guint64 budget = (guint64) DEFAULT_MEMLOCK_VALUE + SECMEM_HEADROOM_VALUE;
    gint32 pool = -1;
    g_assert_cmpint (secmem_pool_from_limits (budget, &pool), ==, MEMLOCK_OK);
    g_assert_cmpint (pool, ==, DEFAULT_MEMLOCK_VALUE);
}

static void
test_infinity_gets_default (void)
{
    /* RLIM_INFINITY reaches the helper as a very large value; it must not
     * overflow the subtraction and must yield the default pool. */
    gint32 pool = -1;
    g_assert_cmpint (secmem_pool_from_limits (G_MAXUINT64, &pool), ==, MEMLOCK_OK);
    g_assert_cmpint (pool, ==, DEFAULT_MEMLOCK_VALUE);
}

static void
test_just_below_threshold_reserves_headroom (void)
{
    /* One byte below the threshold: we no longer get the full default, we get a
     * pool shrunk by exactly the headroom, and the status drops to TOO_LOW. */
    guint64 budget = (guint64) DEFAULT_MEMLOCK_VALUE + SECMEM_HEADROOM_VALUE - 1;
    gint32 pool = -1;
    g_assert_cmpint (secmem_pool_from_limits (budget, &pool), ==, MEMLOCK_TOO_LOW);
    g_assert_cmpint (pool, ==, (gint32) (budget - SECMEM_HEADROOM_VALUE));
    g_assert_cmpuint (budget - (guint64) pool, >=, (guint64) SECMEM_HEADROOM_VALUE);
}

static void
test_typical_8mib_leaves_headroom (void)
{
    /* The reporter's case: a small systemd DefaultLimitMEMLOCK. The pool must be
     * shrunk so at least the headroom (>> GTK's 16 KiB) stays lockable. */
    guint64 budget = 8 * MIB;
    gint32 pool = -1;
    g_assert_cmpint (secmem_pool_from_limits (budget, &pool), ==, MEMLOCK_TOO_LOW);
    g_assert_cmpint (pool, ==, (gint32) (budget - SECMEM_HEADROOM_VALUE));
    g_assert_cmpuint (budget - (guint64) pool, >=, 16384); /* GTK's 16 KiB block fits */
}

static void
test_exactly_64mib_still_reserves (void)
{
    /* A limit of exactly the old default now reserves headroom instead of
     * claiming all of it. The old code returned the full 64 MiB here, which is
     * precisely what left GTK nothing to lock. */
    guint64 budget = DEFAULT_MEMLOCK_VALUE;
    gint32 pool = -1;
    g_assert_cmpint (secmem_pool_from_limits (budget, &pool), ==, MEMLOCK_TOO_LOW);
    g_assert_cmpint (pool, ==, (gint32) (budget - SECMEM_HEADROOM_VALUE));
}

static void
test_floor_budget_uses_all (void)
{
    /* When the budget is too small to carve out headroom without dropping below
     * the floor, use what we have (a small locked pool beats none) and report
     * TOO_LOW. GTK may still warn here, which is unavoidable at this limit. */
    guint64 budget = (guint64) SECMEM_HEADROOM_VALUE + MIN_SECMEM_POOL_VALUE; /* boundary, not > */
    gint32 pool = -1;
    g_assert_cmpint (secmem_pool_from_limits (budget, &pool), ==, MEMLOCK_TOO_LOW);
    g_assert_cmpint (pool, ==, (gint32) budget);
}

static void
test_just_above_floor_reserves (void)
{
    /* One byte above the floor boundary flips to the reserve branch. */
    guint64 budget = (guint64) SECMEM_HEADROOM_VALUE + MIN_SECMEM_POOL_VALUE + 1;
    gint32 pool = -1;
    g_assert_cmpint (secmem_pool_from_limits (budget, &pool), ==, MEMLOCK_TOO_LOW);
    g_assert_cmpint (pool, ==, (gint32) (budget - SECMEM_HEADROOM_VALUE));
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/memlock-sizing/above-threshold-default", test_budget_above_threshold_gets_default);
    g_test_add_func ("/memlock-sizing/at-threshold-default",    test_budget_exactly_at_threshold_gets_default);
    g_test_add_func ("/memlock-sizing/infinity-default",        test_infinity_gets_default);
    g_test_add_func ("/memlock-sizing/just-below-threshold",    test_just_below_threshold_reserves_headroom);
    g_test_add_func ("/memlock-sizing/typical-8mib",            test_typical_8mib_leaves_headroom);
    g_test_add_func ("/memlock-sizing/exactly-64mib",           test_exactly_64mib_still_reserves);
    g_test_add_func ("/memlock-sizing/floor-uses-all",          test_floor_budget_uses_all);
    g_test_add_func ("/memlock-sizing/just-above-floor",        test_just_above_floor_reserves);

    return g_test_run ();
}
