/* x86_64 instruction helpers. CPUID is defined in Intel SDM Vol. 2A and AMD APM Vol. 3. */
#ifndef CATO_BOOT_CPU_H
#define CATO_BOOT_CPU_H

#include <stdint.h>

static inline void cpu_cpuid(uint32_t leaf, uint32_t regs[4])
{
    __asm__ volatile("cpuid"
                     : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                     : "a"(leaf), "c"(0));
}

/* CPUID.1:ECX bit 31 is set by hypervisors and is zero on physical CPUs. */
static inline int cpu_in_vm(void)
{
    uint32_t regs[4];
    cpu_cpuid(1, regs);
    return (regs[2] >> 31) & 1;
}

/*
 * Random value for network IDs and sequence numbers. It uses RDRAND when the CPU
 * has it (CPUID.1:ECX bit 30), and falls back to the time-stamp counter otherwise.
 * It is not suitable for cryptographic keys.
 */
static inline uint32_t cpu_random32(void)
{
    uint32_t regs[4];
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    uint32_t mixed = (low * 2654435761u) ^ high ^ (low >> 13);

    cpu_cpuid(1, regs);
    if (regs[2] & (1u << 30)) {
        for (int attempt = 0; attempt < 10; attempt++) {
            uint32_t value;
            uint8_t ok;
            __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok) : : "cc");
            if (ok)
                return value ^ mixed;
        }
    }
    return mixed;
}

/*
 * Fills out with hardware randomness from RDSEED (CPUID.7:EBX bit 18) or RDRAND
 * (CPUID.1:ECX bit 30). Returns 0 if the CPU has neither or it keeps failing,
 * because TLS keys must never come from a guessable source.
 */
static inline int cpu_entropy(uint8_t *out, int length)
{
    uint32_t regs[4];
    cpu_cpuid(0, regs);
    int has_seed = 0;
    if (regs[0] >= 7) {
        cpu_cpuid(7, regs);
        has_seed = (regs[1] >> 18) & 1;
    }
    cpu_cpuid(1, regs);
    int has_rand = (regs[2] >> 30) & 1;
    if (!has_seed && !has_rand)
        return 0;

    for (int filled = 0; filled < length;) {
        uint64_t value = 0;
        uint8_t ok = 0;
        for (int attempt = 0; attempt < 100 && !ok; attempt++) {
            if (has_seed)
                __asm__ volatile("rdseed %0; setc %1" : "=r"(value), "=qm"(ok) : : "cc");
            else
                __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok) : : "cc");
        }
        if (!ok)
            return 0;
        for (int i = 0; i < 8 && filled < length; i++)
            out[filled++] = (uint8_t)(value >> (8 * i));
    }
    return 1;
}

static inline uint8_t port_in8(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %w1, %b0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void port_out8(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %b0, %w1" : : "a"(value), "Nd"(port));
}

#endif
