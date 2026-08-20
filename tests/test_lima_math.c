/*
 * test_lima_math.c — userspace Lima math and layout validation
 *
 * Mirrors constants and macros from lima_vm.c / lima_dump.h.
 * Validates correctness without loading the kernel module.
 *
 * Build: cc -Wall -Wextra -o test_lima_math test_lima_math.c && ./test_lima_math
 * Or via: gmake -C /root/bsdOS/hal/lima test-layout
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── Mirrored from lima_vm.c (must stay in sync) ────────────────────────── */

#define LIMA_PAGE_SIZE                4096
#define LIMA_PAGE_ENT_NUM             (LIMA_PAGE_SIZE / sizeof(uint32_t))  /* 1024 */
#define LIMA_VM_NUM_PT_PER_BT_SHIFT   3
#define LIMA_VM_NUM_PT_PER_BT         (1 << LIMA_VM_NUM_PT_PER_BT_SHIFT)  /* 8 */
#define LIMA_VM_NUM_BT                (LIMA_PAGE_ENT_NUM >> LIMA_VM_NUM_PT_PER_BT_SHIFT) /* 128 */

#define LIMA_VM_PD_SHIFT              22
#define LIMA_VM_PT_SHIFT              12
#define LIMA_VM_PB_SHIFT              (LIMA_VM_PD_SHIFT + LIMA_VM_NUM_PT_PER_BT_SHIFT)  /* 25 */
#define LIMA_VM_BT_SHIFT              LIMA_VM_PT_SHIFT  /* 12 */

#define LIMA_VM_PT_MASK               ((1u << LIMA_VM_PD_SHIFT) - 1u)  /* 0x3FFFFF */
#define LIMA_VM_BT_MASK               ((1u << LIMA_VM_PB_SHIFT) - 1u)  /* 0x1FFFFFF */

#define LIMA_PDE(va)  ((uint32_t)(va) >> LIMA_VM_PD_SHIFT)
#define LIMA_PTE(va)  (((uint32_t)(va) & LIMA_VM_PT_MASK) >> LIMA_VM_PT_SHIFT)
#define LIMA_PBE(va)  ((uint32_t)(va) >> LIMA_VM_PB_SHIFT)
#define LIMA_BTE(va)  (((uint32_t)(va) & LIMA_VM_BT_MASK) >> LIMA_VM_BT_SHIFT)

/* ── DLBU reserved VA range (from lima_vm.h) ───────────────────────────── */
#define LIMA_VA_RESERVE_START  UINT64_C(0x0FFF00000)
#define LIMA_VA_RESERVE_END    UINT64_C(0x100000000)

/* ── Mirrored from lima_dump.h ──────────────────────────────────────────── */
#define LIMA_DUMP_MAGIC   0x414d494cu  /* "LIMA" little-endian */
#define LIMA_DUMP_MAJOR   1
#define LIMA_DUMP_MINOR   1

/* These structs must match the kernel driver exactly (no padding changes). */
struct lima_dump_head {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t size;
    uint32_t num_tasks;
};  /* expected: 16 bytes */

struct lima_dump_task {
    uint32_t id;
    uint32_t size;
    uint32_t num_chunks;
};  /* expected: 12 bytes */

struct lima_dump_chunk {
    uint32_t id;
    uint32_t size;
};  /* expected: 8 bytes */

struct lima_dump_chunk_pid {
    uint32_t id;
    uint32_t size;
    uint32_t pid;
};  /* expected: 12 bytes */

struct lima_dump_chunk_buffer {
    uint32_t id;
    uint32_t size;
    uint64_t va;
};  /* expected: 16 bytes (uint64_t naturally aligned at offset 8) */

/* ── Test harness ────────────────────────────────────────────────────────── */

static int pass_count = 0;
static int fail_count = 0;

#define CHECK_EQ(got, want, msg) do { \
    unsigned long long _g = (unsigned long long)(got); \
    unsigned long long _w = (unsigned long long)(want); \
    if (_g == _w) { \
        printf("ok   %s\n", (msg)); \
        pass_count++; \
    } else { \
        printf("FAIL %s  (got %llu, want %llu)\n", (msg), _g, _w); \
        fail_count++; \
    } \
} while (0)

#define CHECK(cond, msg) CHECK_EQ(!!(cond), 1u, (msg))

int main(void)
{
    printf("=== Lima math and layout tests ===\n\n");

    /* ── Constants ──────────────────────────────────────────────────────── */
    printf("-- Constants --\n");
    CHECK_EQ(LIMA_PAGE_SIZE,           4096u,  "LIMA_PAGE_SIZE == 4096");
    CHECK_EQ(LIMA_PAGE_ENT_NUM,        1024u,  "LIMA_PAGE_ENT_NUM == 1024");
    CHECK_EQ(LIMA_VM_NUM_PT_PER_BT,       8u,  "LIMA_VM_NUM_PT_PER_BT == 8");
    CHECK_EQ(LIMA_VM_NUM_BT,            128u,  "LIMA_VM_NUM_BT == 128");
    CHECK_EQ(LIMA_VM_PD_SHIFT,           22u,  "LIMA_VM_PD_SHIFT == 22");
    CHECK_EQ(LIMA_VM_PT_SHIFT,           12u,  "LIMA_VM_PT_SHIFT == 12");
    CHECK_EQ(LIMA_VM_PB_SHIFT,           25u,  "LIMA_VM_PB_SHIFT == 25");
    CHECK_EQ(LIMA_VM_PT_MASK,      0x3FFFFFu,  "LIMA_VM_PT_MASK == 0x3FFFFF");
    CHECK_EQ(LIMA_VM_BT_MASK,     0x1FFFFFFu,  "LIMA_VM_BT_MASK == 0x1FFFFFF");
    CHECK_EQ(LIMA_DUMP_MAGIC,    0x414d494cu,  "LIMA_DUMP_MAGIC == 0x414d494c");

    /* ── DLBU reserve range ─────────────────────────────────────────────── */
    printf("\n-- DLBU reserve range --\n");
    CHECK_EQ(LIMA_VA_RESERVE_START, UINT64_C(0x0FFF00000), "DLBU start == 0x0FFF00000");
    CHECK_EQ(LIMA_VA_RESERVE_END,   UINT64_C(0x100000000), "DLBU end == 0x100000000");
    CHECK(LIMA_VA_RESERVE_END - LIMA_VA_RESERVE_START == 1024u * 1024u,
          "DLBU reserve is exactly 1 MiB");

    /* ── Page table address translation ─────────────────────────────────── */
    printf("\n-- Page table math --\n");

    /* va = 0x1000 (page 1, 4 KiB) */
    CHECK_EQ(LIMA_PDE(0x1000u), 0u,  "PDE(0x1000) == 0");
    CHECK_EQ(LIMA_PTE(0x1000u), 1u,  "PTE(0x1000) == 1");
    CHECK_EQ(LIMA_PBE(0x1000u), 0u,  "PBE(0x1000) == 0");
    CHECK_EQ(LIMA_BTE(0x1000u), 1u,  "BTE(0x1000) == 1");

    /* va = 0x400000 (4 MiB, PDE boundary) */
    CHECK_EQ(LIMA_PDE(0x400000u),    1u,     "PDE(0x400000) == 1");
    CHECK_EQ(LIMA_PTE(0x400000u),    0u,     "PTE(0x400000) == 0");
    CHECK_EQ(LIMA_PBE(0x400000u),    0u,     "PBE(0x400000) == 0");
    CHECK_EQ(LIMA_BTE(0x400000u),  0x400u,   "BTE(0x400000) == 0x400");

    /* va = 0x2000000 (32 MiB, PBE boundary) */
    CHECK_EQ(LIMA_PDE(0x2000000u),   8u,     "PDE(0x2000000) == 8");
    CHECK_EQ(LIMA_PTE(0x2000000u),   0u,     "PTE(0x2000000) == 0");
    CHECK_EQ(LIMA_PBE(0x2000000u),   1u,     "PBE(0x2000000) == 1");
    CHECK_EQ(LIMA_BTE(0x2000000u),   0u,     "BTE(0x2000000) == 0");

    /* va = 0x100000 (1 MiB, crosses multiple PTE entries) */
    CHECK_EQ(LIMA_PDE(0x100000u),    0u,     "PDE(0x100000) == 0");
    CHECK_EQ(LIMA_PTE(0x100000u),  0x100u,   "PTE(0x100000) == 0x100");
    CHECK_EQ(LIMA_PBE(0x100000u),    0u,     "PBE(0x100000) == 0");
    CHECK_EQ(LIMA_BTE(0x100000u),  0x100u,   "BTE(0x100000) == 0x100");

    /* va = 0x1234000 (an arbitrary mid-range address) */
    /* PDE = 0x1234000 >> 22 = 4; PTE = (0x1234000 & 0x3FFFFF) >> 12 = 0x234 */
    CHECK_EQ(LIMA_PDE(0x1234000u),   4u,     "PDE(0x1234000) == 4");
    CHECK_EQ(LIMA_PTE(0x1234000u), 0x234u,   "PTE(0x1234000) == 0x234");

    /* va = 0 (zero address, edge case) */
    CHECK_EQ(LIMA_PDE(0u), 0u,  "PDE(0) == 0");
    CHECK_EQ(LIMA_PTE(0u), 0u,  "PTE(0) == 0");
    CHECK_EQ(LIMA_PBE(0u), 0u,  "PBE(0) == 0");
    CHECK_EQ(LIMA_BTE(0u), 0u,  "BTE(0) == 0");

    /* ── Struct sizes (stable ABI for crash-dump parsing) ─────────────── */
    printf("\n-- Struct layout --\n");
    CHECK_EQ(sizeof(struct lima_dump_head),         16u,  "sizeof(lima_dump_head) == 16");
    CHECK_EQ(sizeof(struct lima_dump_task),         12u,  "sizeof(lima_dump_task) == 12");
    CHECK_EQ(sizeof(struct lima_dump_chunk),         8u,  "sizeof(lima_dump_chunk) == 8");
    CHECK_EQ(sizeof(struct lima_dump_chunk_pid),    12u,  "sizeof(lima_dump_chunk_pid) == 12");
    CHECK_EQ(sizeof(struct lima_dump_chunk_buffer), 16u,  "sizeof(lima_dump_chunk_buffer) == 16");

    /* ── Struct field offsets ─────────────────────────────────────────── */
    CHECK_EQ(offsetof(struct lima_dump_head, magic),          0u,  "dump_head.magic @ 0");
    CHECK_EQ(offsetof(struct lima_dump_head, version_major),  4u,  "dump_head.version_major @ 4");
    CHECK_EQ(offsetof(struct lima_dump_head, version_minor),  6u,  "dump_head.version_minor @ 6");
    CHECK_EQ(offsetof(struct lima_dump_head, size),           8u,  "dump_head.size @ 8");
    CHECK_EQ(offsetof(struct lima_dump_head, num_tasks),     12u,  "dump_head.num_tasks @ 12");
    CHECK_EQ(offsetof(struct lima_dump_chunk_buffer, va),     8u,  "dump_chunk_buffer.va @ 8");

    /* ── Magic string decodes as "LIMA" ──────────────────────────────── */
    printf("\n-- Magic string --\n");
    {
        uint32_t magic = LIMA_DUMP_MAGIC;
        char magic_str[5] = {0};
        memcpy(magic_str, &magic, 4);
        CHECK(strcmp(magic_str, "LIMA") == 0, "LIMA_DUMP_MAGIC decodes to 'LIMA' (LE)");
    }

    /* ── Version constants ───────────────────────────────────────────── */
    CHECK_EQ(LIMA_DUMP_MAJOR, 1u, "LIMA_DUMP_MAJOR == 1");
    CHECK_EQ(LIMA_DUMP_MINOR, 1u, "LIMA_DUMP_MINOR == 1");

    /* ── Summary ─────────────────────────────────────────────────────── */
    printf("\n=== %s: %d passed, %d failed ===\n",
           fail_count ? "FAIL" : "PASS", pass_count, fail_count);
    return fail_count ? 1 : 0;
}
