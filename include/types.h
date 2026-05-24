#ifndef NYXIS_TYPES_H
#define NYXIS_TYPES_H

#include <stdint.h>

#define pack __attribute__((packed));
#define interrupt __attribute__((interrupt))

// fixed size
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

typedef char      i8;
typedef short     i16;
typedef int       i32;
typedef long long i64;

// Boolean
typedef _Bool bool;

#define true 1
#define false 0


// pointer-sized
#ifdef NYXIS_64BITS
    typedef u64 usize;
    typedef i64 isize;
#else
    typedef u32 usize;
    typedef i32 isize;
#endif


// NULL
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
        // C23
    #define nNULL nullptr
#else
    #define nNULL ((void*)0)
#endif

// chars
typedef u8  utf8;
typedef u16 utf16;


// =====================
// error system
// =====================

// status type
typedef i32 Nstatus;

// MSB = error flag
#define NSTATUS_ERR_FLAG ((i32)1 << 31)

// helpers
#define NSTATUS_OK 0
#define NSTATUS_MAKE_ERR(code) (NSTATUS_ERR_FLAG | (code))
#define NSTATUS_IS_ERR(x) ((x) < 0)
#define NSTATUS_CODE(x) ((x) & 0x7FFFFFFF)


typedef struct {
    usize       version;
    utf16*      id;

    // Memory
    usize       memory_size;

    // Framebuffer
    void*       framebuffer_base;
    usize       framebuffer_size;
    u32         width;
    u32         height;
    u32         pixels_per_scan_line;

    // ACPI
    void*       Rsdp;

} NTBLI;

extern bool multicore_enabled;

// low-level operations from lowlevel.h
static inline void cli(void);
static inline void sti(void);

// atomic type
typedef struct {
    volatile i32 value;
} atomic_t;

static inline void atomic_inc(atomic_t* v) {

    if (!multicore_enabled) {
        v->value++;
        return;
    }

    __asm__ volatile(
        "lock incl %0"
        : "+m"(v->value)
        :
        : "memory"
    );
}

static inline int atomic_cmpxchg(
    atomic_t* v,
    int old,
    int new
) {

    if (!multicore_enabled) {

        int prev = v->value;

        if (prev == old)
            v->value = new;

        return prev;
    }

    int prev;

    __asm__ volatile(
        "lock cmpxchgl %2, %1"
        : "=a"(prev), "+m"(v->value)
        : "r"(new), "0"(old)
        : "memory"
    );

    return prev;
}

static inline void atomic_set(atomic_t* v, int new)
{
    if (!multicore_enabled) {
        v->value = new;
        return;
    }

    __asm__ volatile(
        "lock xchgl %0, %1"
        : "+r"(new), "+m"(v->value)
        :
        : "memory"
    );
}

// spinlock type
typedef struct {
    atomic_t locked;
} spinlock_t;

static inline void spin_lock(spinlock_t* lock) {

    if (!multicore_enabled) {
        cli();
        return;
    }

    while (1) {

        if (atomic_cmpxchg(
                &lock->locked,
                0,
                1
            ) == 0)
        {
            break;
        }

        while (lock->locked.value) {
            __asm__ volatile("pause");
        }
    }

    cli();
}

static inline void spin_unlock(spinlock_t* lock) {

    if (!multicore_enabled) {
        sti();
        return;
    }

    atomic_set(&lock->locked, 0);

    sti();
}

typedef uint64_t hkey_t;

typedef struct hashmap_node {
    hkey_t key;

    void* value;

    struct hashmap_node* next;
} hashmap_node_t;

// 64 error codes should be more than enough for now, and we can always expand to 128 or 256 if needed
typedef enum {
    Nok              = 0,

    // General
    NnotFound        = NSTATUS_MAKE_ERR(1),
    NinvalidArg      = NSTATUS_MAKE_ERR(2),
    NoutOfMemory     = NSTATUS_MAKE_ERR(3),
    Npermission      = NSTATUS_MAKE_ERR(4),
    Ntimeout         = NSTATUS_MAKE_ERR(5),
    Nbusy            = NSTATUS_MAKE_ERR(6),
    NalreadyExists   = NSTATUS_MAKE_ERR(7),
    Noverflow        = NSTATUS_MAKE_ERR(8),
    Nunderflow       = NSTATUS_MAKE_ERR(9),
    Nio              = NSTATUS_MAKE_ERR(10),
    Ndisconnected    = NSTATUS_MAKE_ERR(11),
    Nunsupported     = NSTATUS_MAKE_ERR(12),
    Ncorrupted       = NSTATUS_MAKE_ERR(13),
    Ninterrupted     = NSTATUS_MAKE_ERR(14),
    Nunknown         = NSTATUS_MAKE_ERR(15),

    // Arithmetic
    NdivideByZero    = NSTATUS_MAKE_ERR(16),
    Narithmetic      = NSTATUS_MAKE_ERR(17),
    NoverflowMath    = NSTATUS_MAKE_ERR(18),
    NunderflowMath   = NSTATUS_MAKE_ERR(19),
    Nnan             = NSTATUS_MAKE_ERR(20),

    // Memory
    NnullPointer     = NSTATUS_MAKE_ERR(21),
    NinvalidPointer  = NSTATUS_MAKE_ERR(22),
    NdoubleFree      = NSTATUS_MAKE_ERR(23),
    NuseAfterFree    = NSTATUS_MAKE_ERR(24),
    NstackOverflow   = NSTATUS_MAKE_ERR(25),
    NheapCorruption  = NSTATUS_MAKE_ERR(26),
    NbufferOverflow  = NSTATUS_MAKE_ERR(27),
    NbufferUnderflow = NSTATUS_MAKE_ERR(28),

    // Filesystem
    NfileNotFound    = NSTATUS_MAKE_ERR(29),
    NpathTooLong     = NSTATUS_MAKE_ERR(30),
    NreadOnly        = NSTATUS_MAKE_ERR(31),
    NdiskFull        = NSTATUS_MAKE_ERR(32),
    NbadFilesystem   = NSTATUS_MAKE_ERR(33),
    NfileCorrupted   = NSTATUS_MAKE_ERR(34),
    NtooManyFileSystem = NSTATUS_MAKE_ERR(35),

    // Process / Thread
    NprocessFailed   = NSTATUS_MAKE_ERR(36),
    NthreadFailed    = NSTATUS_MAKE_ERR(37),
    Ndeadlock        = NSTATUS_MAKE_ERR(38),
    NraceCondition   = NSTATUS_MAKE_ERR(39),

    // Network
    NconnectionLost  = NSTATUS_MAKE_ERR(40),
    NconnectionRefused = NSTATUS_MAKE_ERR(41),
    NhostUnreachable = NSTATUS_MAKE_ERR(42),
    NprotocolError   = NSTATUS_MAKE_ERR(43),

    // Security
    Nunauthorized    = NSTATUS_MAKE_ERR(44),
    Nauthentication  = NSTATUS_MAKE_ERR(45),
    Nencryption      = NSTATUS_MAKE_ERR(46),

    // Parsing / Format
    NinvalidFormat   = NSTATUS_MAKE_ERR(47),
    NparseError      = NSTATUS_MAKE_ERR(48),
    NsyntaxError     = NSTATUS_MAKE_ERR(49),
    NencodingError   = NSTATUS_MAKE_ERR(50),

    // Device / Hardware
    NdeviceFailure   = NSTATUS_MAKE_ERR(51),
    NdeviceMissing   = NSTATUS_MAKE_ERR(52),
    NdeviceBusy      = NSTATUS_MAKE_ERR(53),
    NhardwareFault   = NSTATUS_MAKE_ERR(54),

    // State
    NnotInitialized  = NSTATUS_MAKE_ERR(55),
    NalreadyInitialized = NSTATUS_MAKE_ERR(56),
    NinvalidState    = NSTATUS_MAKE_ERR(57),

    // System
    NsyscallFailed   = NSTATUS_MAKE_ERR(58),
    NkernelFault     = NSTATUS_MAKE_ERR(59),
    Npanic           = NSTATUS_MAKE_ERR(60),
    Nfatal           = NSTATUS_MAKE_ERR(61),

    // Reserved for future use
    Nreserved1       = NSTATUS_MAKE_ERR(62),
    Nreserved2       = NSTATUS_MAKE_ERR(63),
} Nerror;

#endif
