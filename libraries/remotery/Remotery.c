//
// Copyright 2014-2022 Celtoys Ltd
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Modified for EDGE-Classic (https://github.com/scramshackle/EDGE-classic), 2026-10-08.
// Based on upstream https://github.com/Celtoys/Remotery commit 5483f65b60334059933ff82464d8ea6882d20073 (2024-08-28).
//
// Changes from upstream:
// - Removed CUDA, Direct3D 11, Direct3D 12, Metal, OpenGL and Vulkan GPU sampling and their public API.
// - Thread sampler: 32-bit Windows callback injection is compiled only with MSVC (its inline assembly is
//   MSVC-only); GetCurrentProcessorNumber is resolved at runtime and injection is skipped when it is
//   unavailable (Windows XP).
// - rmtOpenFile: UTF-8 filenames are converted to UTF-16 for _wfopen/_wfopen_s on Windows.
// - Trace log files are opened in binary mode (text mode corrupted .rbin files on Windows).
// - rmtWriteFile: success check compares fwrite's item count, not the byte size.
// - Trace log filename timestamps are zero-padded so files sort by name.
// - MakeWideString: Windows-only, converts UTF-8 with MultiByteToWideChar instead of mbstowcs_s.
// - gmtime_s, #pragma comment and the THREADNAME_INFO exception fallback are limited to MSVC.
// - Removed unused helper functions and fixed GCC/MinGW warnings.
// - RMT_ASSUME_LITTLE_ENDIAN defaults to 1, with a compile-time error on big-endian targets; the
//   little-endian buffer writes use memcpy instead of unaligned pointer stores.
// - Replaced the rmtU8..rmtS64/rmtF32/rmtF64 aliases and raw unsigned types with <stdint.h> fixed-width
//   types (float/double for the float aliases); rmtBool is now uint32_t.
// - Removed Xbox One (Durango) and Android code, and the unreleased Celtoys TinyCRT (RMT_USE_TINYCRT).
// - Remotery.h: removed the upstream compiling instructions (they referred to lib/ and sample/ paths not
//   shipped here).
//

/*
@Contents:

    @DEPS:          External Dependencies
    @TIMERS:        Platform-specific timers
    @TLS:           Thread-Local Storage
    @ERROR:         Error handling
    @ATOMIC:        Atomic Operations
    @RNG:           Random Number Generator
    @LFSR:          Galois Linear-feedback Shift Register
    @VMBUFFER:      Mirror Buffer using Virtual Memory for auto-wrap
    @NEW:           New/Delete operators with error values for simplifying object create/destroy
    @SAFEC:         Safe C Library excerpts
    @OSTHREADS:     Wrappers around OS-specific thread functions
    @THREADS:       Cross-platform thread object
    @OBJALLOC:      Reusable Object Allocator
    @DYNBUF:        Dynamic Buffer
    @HASHTABLE:     Integer pair hash map for inserts/finds. No removes for added simplicity.
    @STRINGTABLE:	Map from string hash to string offset in local buffer
    @SOCKETS:       Sockets TCP/IP Wrapper
    @SHA1:          SHA-1 Cryptographic Hash Function
    @BASE64:        Base-64 encoder
    @MURMURHASH:    Murmur-Hash 3
    @WEBSOCKETS:    WebSockets
    @MESSAGEQ:      Multiple producer, single consumer message queue
    @NETWORK:       Network Server
    @SAMPLE:        Base Sample Description (CPU by default)
    @SAMPLETREE:    A tree of samples with their allocator
    @TPROFILER:     Thread Profiler data, storing both sampling and instrumentation results
    @TGATHER:       Thread Gatherer, periodically polling for newly created threads
    @TSAMPLER:      Sampling thread contexts
    @REMOTERY:      Remotery
    @SAMPLEAPI:     Sample API for user callbacks
    @PROPERTYAPI:   Property API for user callbacks
    @PROPERTIES:    Property API
*/

#define RMT_IMPL
#include "Remotery.h"

#if RMT_ASSUME_LITTLE_ENDIAN && defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error "RMT_ASSUME_LITTLE_ENDIAN is set but the target is big-endian"
#endif

#if defined(RMT_PLATFORM_WINDOWS) && defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#endif

#if RMT_ENABLED

// Global settings
static rmtSettings g_Settings;
static rmtBool g_SettingsInitialized = RMT_FALSE;

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @DEPS: External Dependencies
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

// clang-format off

//
// Required CRT dependencies
//

    #ifdef RMT_PLATFORM_MACOS
        #include <mach/mach_time.h>
        #include <mach/vm_map.h>
        #include <mach/mach.h>
        #include <sys/time.h>
    #else
        #if !defined(__FreeBSD__) && !defined(__OpenBSD__)
            #include <malloc.h>
        #endif
    #endif

    #include <assert.h>
    #include <stdio.h>
    #include <time.h>
    #include <limits.h>
    #include <stdlib.h>
    #include <stdint.h>
    #include <math.h>

    #ifdef RMT_PLATFORM_WINDOWS
        #include <winsock2.h>
        #include <timeapi.h>
        #ifndef __MINGW32__
            #include <intrin.h>
        #endif
        #undef min
        #undef max
        #include <tlhelp32.h>
        #include <winnt.h>
        #include <processthreadsapi.h>
        typedef long NTSTATUS;  // winternl.h

        #define RMT_ENABLE_THREAD_SAMPLER

    #endif

    #ifdef RMT_PLATFORM_LINUX
        #if defined(__FreeBSD__) || defined(__OpenBSD__)
            #include <pthread_np.h>
        #else
            #include <sys/prctl.h>
        #endif
    #endif

    #if defined(RMT_PLATFORM_POSIX)
        #include <pthread.h>
        #include <unistd.h>
        #include <string.h>
        #include <arpa/inet.h>
        #include <sys/select.h>
        #include <sys/socket.h>
        #include <sys/mman.h>
        #include <netinet/in.h>
        #include <fcntl.h>
        #include <errno.h>
        #include <dlfcn.h>
    #endif

    #ifdef __MINGW32__
        #include <pthread.h>
    #endif



#if RMT_USE_LEGACY_ATOMICS==0
    #if __cplusplus >= 199711L
        #if !defined(RMT_USE_CPP_ATOMICS)
            #define RMT_USE_CPP_ATOMICS
        #endif
    #elif __STDC_VERSION__ >= 201112L
        #if !defined(__STDC_NO_ATOMICS__)
            #if !defined(RMT_USE_C11_ATOMICS)
                #define RMT_USE_C11_ATOMICS
            #endif
        #endif
    #endif
#endif

#if defined(RMT_USE_C11_ATOMICS)
    #include <stdatomic.h>
#elif defined(RMT_USE_CPP_ATOMICS)
    #include <atomic>
#endif

// clang-format on

#if defined(_MSC_VER) && !defined(__clang__)
    #define RMT_UNREFERENCED_PARAMETER(i) (i)
#else
    #define RMT_UNREFERENCED_PARAMETER(i) (void)(1 ? (void)0 : ((void)i))
#endif

// Executes the given statement and returns from the calling function if it fails, returning the error with it
#define rmtTry(stmt)                   \
    {                                   \
        rmtError error = stmt;          \
        if (error != RMT_ERROR_NONE)    \
            return error;               \
    }

static uint16_t maxU16(uint16_t a, uint16_t b)
{
    return a > b ? a : b;
}
static int32_t minS32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}
static int32_t maxS32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}
static uint32_t minU32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}
static int64_t maxS64(int64_t a, int64_t b)
{
    return a > b ? a : b;
}

// Memory management functions
static void* rmtMalloc(uint32_t size)
{
    return g_Settings.malloc(g_Settings.mm_context, size);
}

static void* rmtRealloc(void* ptr, uint32_t size)
{
    return g_Settings.realloc(g_Settings.mm_context, ptr, size);
}

static void rmtFree(void* ptr)
{
    g_Settings.free(g_Settings.mm_context, ptr);
}

// File system functions
static FILE* rmtOpenFile(const char* filename, const char* mode)
{
#if defined(RMT_PLATFORM_WINDOWS)
    wchar_t wide_filename[1024];
    wchar_t wide_mode[16];
    if (MultiByteToWideChar(CP_UTF8, 0, filename, -1, wide_filename, sizeof(wide_filename) / sizeof(wide_filename[0])) == 0 ||
        MultiByteToWideChar(CP_UTF8, 0, mode, -1, wide_mode, sizeof(wide_mode) / sizeof(wide_mode[0])) == 0)
    {
        return NULL;
    }
#ifdef _MSC_VER
    {
        FILE* fp;
        return _wfopen_s(&fp, wide_filename, wide_mode) == 0 ? fp : NULL;
    }
#else
    return _wfopen(wide_filename, wide_mode);
#endif
#else
    return fopen(filename, mode);
#endif
}

void rmtCloseFile(FILE* fp)
{
    if (fp != NULL)
    {
        fclose(fp);
    }
}

rmtBool rmtWriteFile(FILE* fp, const void* data, uint32_t size)
{
    assert(fp != NULL);
    return fwrite(data, size, 1, fp) == 1 ? RMT_TRUE : RMT_FALSE;
}


/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @TIMERS: Platform-specific timers
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

//
// Get millisecond timer value that has only one guarantee: multiple calls are consistently comparable.
// On some platforms, even though this returns milliseconds, the timer may be far less accurate.
//
static uint32_t msTimer_Get()
{
#ifdef RMT_PLATFORM_WINDOWS

    return (uint32_t)GetTickCount();

#else

    clock_t time = clock();

// CLOCKS_PER_SEC is 128 on FreeBSD, causing div/0
#if defined(__FreeBSD__) || defined(__OpenBSD__)
    uint32_t msTime = (uint32_t)(time * 1000 / CLOCKS_PER_SEC);
#else
    uint32_t msTime = (uint32_t)(time / (CLOCKS_PER_SEC / 1000));
#endif

    return msTime;

#endif
}

//
// Micro-second accuracy high performance counter
//
#ifndef RMT_PLATFORM_WINDOWS
typedef uint64_t LARGE_INTEGER;
#endif
typedef struct
{
    LARGE_INTEGER counter_start;
    double counter_scale;
} usTimer;

static void usTimer_Init(usTimer* timer)
{
#if defined(RMT_PLATFORM_WINDOWS)
    LARGE_INTEGER performance_frequency;

    assert(timer != NULL);

    // Calculate the scale from performance counter to microseconds
    QueryPerformanceFrequency(&performance_frequency);
    timer->counter_scale = 1000000.0 / performance_frequency.QuadPart;

    // Record the offset for each read of the counter
    QueryPerformanceCounter(&timer->counter_start);

#elif defined(RMT_PLATFORM_MACOS)

    mach_timebase_info_data_t nsScale;
    mach_timebase_info(&nsScale);
    const double ns_per_us = 1.0e3;
    timer->counter_scale = (double)(nsScale.numer) / ((double)nsScale.denom * ns_per_us);

    timer->counter_start = mach_absolute_time();

#elif defined(RMT_PLATFORM_LINUX)

    struct timespec tv;
    clock_gettime(CLOCK_REALTIME, &tv);
    timer->counter_start = (uint64_t)(tv.tv_sec * (uint64_t)1000000) + (uint64_t)(tv.tv_nsec * 0.001);

#endif
}

#if defined(RMT_PLATFORM_WINDOWS)
    #define usTimer_FromRawTicks(timer, ticks) (uint64_t)(((ticks) - (timer)->counter_start.QuadPart) * (timer)->counter_scale)
#elif defined(RMT_PLATFORM_MACOS)
    #define usTimer_FromRawTicks(timer, ticks) (uint64_t)(((ticks) - (timer)->counter_start) * (timer)->counter_scale)
#elif defined(RMT_PLATFORM_LINUX)
    #define usTimer_FromRawTicks(timer, ticks) (uint64_t)((ticks) - (timer)->counter_start)
#endif

static uint64_t usTimer_Get(usTimer* timer)
{
#if defined(RMT_PLATFORM_WINDOWS)
    LARGE_INTEGER performance_count;

    assert(timer != NULL);

    // Read counter and convert to microseconds
    QueryPerformanceCounter(&performance_count);
    return usTimer_FromRawTicks(timer, performance_count.QuadPart);

#elif defined(RMT_PLATFORM_MACOS)

    uint64_t curr_time = mach_absolute_time();
    return usTimer_FromRawTicks(timer, curr_time);

#elif defined(RMT_PLATFORM_LINUX)

    struct timespec tv;
    clock_gettime(CLOCK_REALTIME, &tv);
    uint64_t ticks = (uint64_t)(tv.tv_sec * (uint64_t)1000000) + (uint64_t)(tv.tv_nsec * 0.001);
    return usTimer_FromRawTicks(timer, ticks);

#endif
}

static void msSleep(uint32_t time_ms)
{
#ifdef RMT_PLATFORM_WINDOWS
    Sleep(time_ms);
#elif defined(RMT_PLATFORM_POSIX)
    usleep(time_ms * 1000);
#endif
}

static struct tm* TimeDateNow()
{
    time_t time_now = time(NULL);

#if defined(RMT_PLATFORM_WINDOWS) && defined(_MSC_VER)
    // Discard the thread-safety benefit of gmtime_s
    static struct tm tm_now;
    gmtime_s(&tm_now, &time_now);
    return &tm_now;
#else
    return gmtime(&time_now);
#endif
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @TLS: Thread-Local Storage
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

#define TLS_INVALID_HANDLE 0xFFFFFFFF

#if defined(RMT_PLATFORM_WINDOWS)
typedef uint32_t rmtTLS;
#else
typedef pthread_key_t rmtTLS;
#endif

static rmtError tlsAlloc(rmtTLS* handle)
{
    assert(handle != NULL);

#if defined(RMT_PLATFORM_WINDOWS)
    *handle = (rmtTLS)TlsAlloc();
    if (*handle == TLS_OUT_OF_INDEXES)
    {
        *handle = TLS_INVALID_HANDLE;
        return RMT_ERROR_TLS_ALLOC_FAIL;
    }
#elif defined(RMT_PLATFORM_POSIX)
    if (pthread_key_create(handle, NULL) != 0)
    {
        *handle = TLS_INVALID_HANDLE;
        return RMT_ERROR_TLS_ALLOC_FAIL;
    }
#endif

    return RMT_ERROR_NONE;
}

static void tlsFree(rmtTLS handle)
{
    assert(handle != TLS_INVALID_HANDLE);
#if defined(RMT_PLATFORM_WINDOWS)
    TlsFree(handle);
#elif defined(RMT_PLATFORM_POSIX)
    pthread_key_delete((pthread_key_t)handle);
#endif
}

static void tlsSet(rmtTLS handle, void* value)
{
    assert(handle != TLS_INVALID_HANDLE);
#if defined(RMT_PLATFORM_WINDOWS)
    TlsSetValue(handle, value);
#elif defined(RMT_PLATFORM_POSIX)
    pthread_setspecific((pthread_key_t)handle, value);
#endif
}

static void* tlsGet(rmtTLS handle)
{
    assert(handle != TLS_INVALID_HANDLE);
#if defined(RMT_PLATFORM_WINDOWS)
    return TlsGetValue(handle);
#elif defined(RMT_PLATFORM_POSIX)
    return pthread_getspecific((pthread_key_t)handle);
#endif
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @ERROR: Error handling
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

// Used to store per-thread error messages
// Static so that we can set error messages from code the Remotery object depends on
static rmtTLS g_lastErrorMessageTlsHandle = TLS_INVALID_HANDLE;
static const uint32_t g_errorMessageSize = 1024;

static rmtError rmtMakeError(rmtError in_error, rmtPStr error_message)
{
    char* thread_message_ptr;
    uint32_t error_len;

    // Allocate the TLS on-demand
    // TODO(don): Make this thread-safe
    if (g_lastErrorMessageTlsHandle == TLS_INVALID_HANDLE)
    {
        rmtTry(tlsAlloc(&g_lastErrorMessageTlsHandle));
    }

    // Allocate the string storage for the error message on-demand
    thread_message_ptr = (char*)tlsGet(g_lastErrorMessageTlsHandle);
    if (thread_message_ptr == NULL)
    {
        thread_message_ptr = (char*)rmtMalloc(g_errorMessageSize);
        if (thread_message_ptr == NULL)
        {
            return RMT_ERROR_MALLOC_FAIL;
        }

        tlsSet(g_lastErrorMessageTlsHandle, (void*)thread_message_ptr);
    }

    // Safe copy of the error text without going via strcpy_s down below
    error_len = (uint32_t)strlen(error_message);
    error_len = error_len >= g_errorMessageSize ? g_errorMessageSize - 1 : error_len;
    memcpy(thread_message_ptr, error_message, error_len);
    thread_message_ptr[error_len] = 0;

    return in_error;
}

RMT_API rmtPStr rmt_GetLastErrorMessage()
{
    rmtPStr thread_message_ptr;

    // No message to specify if `rmtMakeError` failed or one hasn't been set yet
    if (g_lastErrorMessageTlsHandle == TLS_INVALID_HANDLE)
    {
        return "No error message";
    }
    thread_message_ptr = (rmtPStr)tlsGet(g_lastErrorMessageTlsHandle);
    if (thread_message_ptr == NULL)
    {
        return "No error message";
    }

    return thread_message_ptr;
}


/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @MUTEX: Mutexes
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

#ifdef RMT_PLATFORM_WINDOWS
typedef CRITICAL_SECTION rmtMutex;
#else
typedef pthread_mutex_t rmtMutex;
#endif

static void mtxInit(rmtMutex* mutex)
{
    assert(mutex != NULL);
#if defined(RMT_PLATFORM_WINDOWS)
    InitializeCriticalSection(mutex);
#elif defined(RMT_PLATFORM_POSIX)
    pthread_mutex_init(mutex, NULL);
#endif
}

static void mtxLock(rmtMutex* mutex)
{
    assert(mutex != NULL);
#if defined(RMT_PLATFORM_WINDOWS)
    EnterCriticalSection(mutex);
#elif defined(RMT_PLATFORM_POSIX)
    pthread_mutex_lock(mutex);
#endif
}

static void mtxUnlock(rmtMutex* mutex)
{
    assert(mutex != NULL);
#if defined(RMT_PLATFORM_WINDOWS)
    LeaveCriticalSection(mutex);
#elif defined(RMT_PLATFORM_POSIX)
    pthread_mutex_unlock(mutex);
#endif
}

static void mtxDelete(rmtMutex* mutex)
{
    assert(mutex != NULL);
#if defined(RMT_PLATFORM_WINDOWS)
    DeleteCriticalSection(mutex);
#elif defined(RMT_PLATFORM_POSIX)
    pthread_mutex_destroy(mutex);
#endif
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @ATOMIC: Atomic Operations
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

// TODO(don): The CAS loops possible with this API are suboptimal. For example, AtomicCompareAndSwapU32 discards the
// return value which tells you the current (potentially mismatching) value of the location you want to modify. This
// means the CAS loop has to explicitly re-load this location on each modify attempt. Instead, the return value should
// be used to update the old value and an initial load only made once before the loop starts.

// TODO(don): Vary these types across versions of C and C++
#if defined(RMT_USE_C11_ATOMICS)
    typedef _Atomic(int32_t)     rmtAtomicS32;
    typedef _Atomic(uint32_t)     rmtAtomicU32;
    typedef _Atomic(uint64_t)     rmtAtomicU64;
    typedef _Atomic(rmtBool)    rmtAtomicBool;
    #define rmtAtomicPtr(type)  _Atomic(type *)
#elif defined(RMT_USE_CPP_ATOMICS)
    typedef std::atomic< int32_t >   rmtAtomicS32;
    typedef std::atomic< uint32_t >   rmtAtomicU32;
    typedef std::atomic< uint64_t >   rmtAtomicU64;
    typedef std::atomic< rmtBool >  rmtAtomicBool;
    #define rmtAtomicPtr(type)      std::atomic< type * >
#else
    typedef volatile int32_t     rmtAtomicS32;
    typedef volatile uint32_t     rmtAtomicU32;
    typedef volatile uint64_t     rmtAtomicU64;
    typedef volatile rmtBool    rmtAtomicBool;
    #define rmtAtomicPtr(type)  volatile type*
#endif

typedef rmtAtomicPtr(void)      rmtAtomicVoidPtr;

static rmtBool AtomicCompareAndSwapU32(rmtAtomicU32 volatile* val, uint32_t old_val, uint32_t new_val)
{
#if defined(RMT_USE_C11_ATOMICS)
    return atomic_compare_exchange_strong(val, &old_val, new_val);
#elif defined(RMT_USE_CPP_ATOMICS)
    return val->compare_exchange_strong(old_val, new_val);
#elif defined(RMT_PLATFORM_WINDOWS) && !defined(__MINGW32__)
    return _InterlockedCompareExchange((long volatile*)val, new_val, old_val) == old_val ? RMT_TRUE : RMT_FALSE;
#elif defined(RMT_PLATFORM_POSIX) || defined(__MINGW32__)
    return __sync_bool_compare_and_swap(val, old_val, new_val) ? RMT_TRUE : RMT_FALSE;
#endif
}


static rmtBool AtomicCompareAndSwapPointer(rmtAtomicVoidPtr volatile* ptr, void* old_ptr, void* new_ptr)
{
#if defined(RMT_USE_C11_ATOMICS)
    return atomic_compare_exchange_strong(ptr, &old_ptr, new_ptr);
#elif defined(RMT_USE_CPP_ATOMICS)
    return ptr->compare_exchange_strong(old_ptr, new_ptr);
#elif defined(RMT_PLATFORM_WINDOWS) && !defined(__MINGW32__)
#ifdef _WIN64
    return _InterlockedCompareExchange64((__int64 volatile*)ptr, (__int64)new_ptr, (__int64)old_ptr) == (__int64)old_ptr
               ? RMT_TRUE
               : RMT_FALSE;
#else
    return _InterlockedCompareExchange((long volatile*)ptr, (long)new_ptr, (long)old_ptr) == (long)old_ptr ? RMT_TRUE
                                                                                                           : RMT_FALSE;
#endif
#elif defined(RMT_PLATFORM_POSIX) || defined(__MINGW32__)
    return __sync_bool_compare_and_swap(ptr, old_ptr, new_ptr) ? RMT_TRUE : RMT_FALSE;
#endif
}

//
// NOTE: Does not guarantee a memory barrier
// TODO: Make sure all platforms don't insert a memory barrier as this is only for stats
//       Alternatively, add strong/weak memory order equivalents
//
static int32_t AtomicAddS32(rmtAtomicS32* value, int32_t add)
{
#if defined(RMT_USE_C11_ATOMICS)
    return atomic_fetch_add(value, add);
#elif defined(RMT_USE_CPP_ATOMICS)
    return value->fetch_add(add);
#elif defined(RMT_PLATFORM_WINDOWS) && !defined(__MINGW32__)
    return _InterlockedExchangeAdd((long volatile*)value, (long)add);
#elif defined(RMT_PLATFORM_POSIX) || defined(__MINGW32__)
    return __sync_fetch_and_add(value, add);
#endif
}

static void AtomicSubS32(rmtAtomicS32* value, int32_t sub)
{
    // Not all platforms have an implementation so just negate and add
    AtomicAddS32(value, -sub);
}

static uint32_t AtomicStoreU32(rmtAtomicU32* value, uint32_t set)
{
#if defined(RMT_USE_C11_ATOMICS)
    return atomic_exchange(value, set);
#elif defined(RMT_USE_CPP_ATOMICS)
    return value->exchange(set);
#elif defined(RMT_PLATFORM_WINDOWS) && !defined(__MINGW32__)
    return (uint32_t)_InterlockedExchange((long volatile*)value, (long) set);
#elif defined(RMT_PLATFORM_POSIX) || defined(__MINGW32__)
    return (uint32_t)__sync_lock_test_and_set(value, set);
#endif
}

static uint32_t AtomicLoadU32(rmtAtomicU32* value)
{
#if defined(RMT_USE_C11_ATOMICS)
    return atomic_load(value);
#elif defined(RMT_USE_CPP_ATOMICS)
    return value->load();
#elif defined(RMT_PLATFORM_WINDOWS) && !defined(__MINGW32__)
    return (uint32_t)_InterlockedExchangeAdd((long volatile*)value, (long)0);
#elif defined(RMT_PLATFORM_POSIX) || defined(__MINGW32__)
    return (uint32_t)__sync_fetch_and_add(value, 0);
#endif
}

static void CompilerWriteFence()
{
#if defined(__clang__)
    __asm__ volatile("" : : : "memory");
#elif defined(RMT_PLATFORM_WINDOWS) && !defined(__MINGW32__)
    _WriteBarrier();
#else
    asm volatile("" : : : "memory");
#endif
}

static void CompilerReadFence()
{
#if defined(__clang__)
    __asm__ volatile("" : : : "memory");
#elif defined(RMT_PLATFORM_WINDOWS) && !defined(__MINGW32__)
    _ReadBarrier();
#else
    asm volatile("" : : : "memory");
#endif
}

static uint32_t LoadAcquire(rmtAtomicU32* address)
{
    uint32_t value = *address;
    CompilerReadFence();
    return value;
}

static long* LoadAcquirePointer(long* volatile* ptr)
{
    long* value = *ptr;
    CompilerReadFence();
    return value;
}

static void StoreRelease(rmtAtomicU32* address, uint32_t value)
{
    CompilerWriteFence();
    *address = value;
}

static void StoreReleasePointer(long* volatile* ptr, long* value)
{
    CompilerWriteFence();
    *ptr = value;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @RNG: Random Number Generator
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

//
// WELL: Well Equidistributed Long-period Linear
// These algorithms produce numbers with better equidistribution than MT19937 and improve upon "bit-mixing" properties. They are
// fast, come in many sizes, and produce higher quality random numbers.
//
// This implementation has a period of 2^512, or 10^154.
//
// Implementation from: Game Programming Gems 7, Random Number Generation Chris Lomont
// Documentation: http://www.lomont.org/Math/Papers/2008/Lomont_PRNG_2008.pdf
//

// Global RNG state for now
// Far better than interfering with the user's rand()
#define Well512_StateSize 16
static uint32_t Well512_State[Well512_StateSize];
static uint32_t Well512_Index;

static void Well512_Init(uint32_t seed)
{
    uint32_t i;

    // Generate initial state from seed
    Well512_State[0] = seed;
    for (i = 1; i < Well512_StateSize; i++)
    {
        uint32_t prev = Well512_State[i - 1];
        Well512_State[i] = (1812433253 * (prev ^ (prev >> 30)) + i);
    }
    Well512_Index = 0;
}

static uint32_t Well512_RandomU32()
{
    uint32_t a, b, c, d;

    a = Well512_State[Well512_Index];
    c = Well512_State[(Well512_Index + 13) & 15];
    b = a ^ c ^ (a << 16) ^ (c << 15);
    c = Well512_State[(Well512_Index + 9) & 15];
    c ^= (c >> 11);
    a = Well512_State[Well512_Index] = b ^ c;
    d = a ^ ((a << 5) & 0xDA442D24UL);
    Well512_Index = (Well512_Index + 15) & 15;
    a = Well512_State[Well512_Index];
    Well512_State[Well512_Index] = a ^ b ^ d ^ (a << 2) ^ (b << 18) ^ (c << 28);
    return Well512_State[Well512_Index];
}

static uint32_t Well512_RandomOpenLimit(uint32_t limit)
{
    // Using % to modulo with range is just masking out the higher bits, leaving a result that's objectively biased.
    // Dividing by RAND_MAX is better but leads to increased repetition at low ranges due to very large bucket sizes.
    // Instead use multiple passes with smaller bucket sizes, rejecting results that don't fit into this smaller range.
    uint32_t bucket_size = UINT_MAX / limit;
    uint32_t bucket_limit = bucket_size * limit;
    uint32_t r;
    do
    {
        r = Well512_RandomU32();
    } while(r >= bucket_limit);

    return r / bucket_size;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @LFSR: Galois Linear-feedback Shift Register
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

static uint32_t Log2i(uint32_t x)
{
	static const uint8_t MultiplyDeBruijnBitPosition[32] =
	{
		0, 9, 1, 10, 13, 21, 2, 29, 11, 14, 16, 18, 22, 25, 3, 30,
		8, 12, 20, 28, 15, 17, 24, 7, 19, 27, 23, 6, 26, 5, 4, 31
	};

    // First round down to one less than a power of two
	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;

	return MultiplyDeBruijnBitPosition[(uint32_t)(x * 0x07C4ACDDU) >> 27];
}

static uint32_t GaloisLFSRMask(uint32_t table_size_log2)
{
    // Taps for 4 to 8 bit ranges
    static const uint8_t XORMasks[] =
    {
        ((1 << 0) | (1 << 1)),                          // 2
        ((1 << 1) | (1 << 2)),                          // 3
        ((1 << 2) | (1 << 3)),                          // 4
        ((1 << 2) | (1 << 4)),                          // 5
        ((1 << 4) | (1 << 5)),                          // 6
        ((1 << 5) | (1 << 6)),                          // 7
        ((1 << 3) | (1 << 4) | (1 << 5) | (1 << 7)),    // 8
    };

    // Map table size to required XOR mask
    assert(table_size_log2 >= 2);
    assert(table_size_log2 <= 8);
    return XORMasks[table_size_log2 - 2];
}

static uint32_t GaloisLFSRNext(uint32_t value, uint32_t xor_mask)
{
    // Output bit
    uint32_t lsb = value & 1;

    // Apply the register shift
    value >>= 1;

    // Apply toggle mask if the output bit is set
    if (lsb != 0)
    {
        value ^= xor_mask;
    }

    return value;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @NEW: New/Delete operators with error values for simplifying object create/destroy
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

#define rmtTryMalloc(type, obj) \
    obj = (type*)rmtMalloc(sizeof(type)); \
    if (obj == NULL) \
    { \
        return RMT_ERROR_MALLOC_FAIL; \
    }

#define rmtTryMallocArray(type, obj, count) \
    obj = (type*)rmtMalloc((count) * sizeof(type)); \
    if (obj == NULL) \
    { \
        return RMT_ERROR_MALLOC_FAIL; \
    }

// Ensures the pointer is non-NULL, calls the destructor, frees memory and sets the pointer to NULL
#define rmtDelete(type, obj)    \
    if (obj != NULL)            \
    {                           \
        type##_Destructor(obj); \
        rmtFree(obj);           \
        obj = NULL;             \
    }

// New will allocate enough space for the object and call the constructor
// If allocation fails the constructor won't be called
// If the constructor fails, the destructor is called and memory is released
// NOTE: Use of sizeof() requires that the type be defined at the point of call
// This is a disadvantage over requiring only a custom Create function
#define rmtTryNew(type, obj, ...)                              \
    {                                                          \
        obj = (type*)rmtMalloc(sizeof(type));                  \
        if (obj == NULL)                                       \
        {                                                      \
            return RMT_ERROR_MALLOC_FAIL;                      \
        }                                                      \
        rmtError error = type##_Constructor(obj, ##__VA_ARGS__); \
        if (error != RMT_ERROR_NONE)                           \
        {                                                      \
            rmtDelete(type, obj);                              \
            return error;                                      \
        }                                                      \
    }

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @VMBUFFER: Mirror Buffer using Virtual Memory for auto-wrap
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

typedef struct VirtualMirrorBuffer
{
    // Page-rounded size of the buffer without mirroring
    uint32_t size;

    // Pointer to the first part of the mirror
    // The second part comes directly after at ptr+size bytes
    uint8_t* ptr;

#ifdef RMT_PLATFORM_WINDOWS
    HANDLE file_map_handle;
#endif

} VirtualMirrorBuffer;


static rmtError VirtualMirrorBuffer_Constructor(VirtualMirrorBuffer* buffer, uint32_t size, int nb_attempts)
{
    static const uint32_t k_64 = 64 * 1024;
    RMT_UNREFERENCED_PARAMETER(nb_attempts);

#ifdef RMT_PLATFORM_LINUX
#if defined(__FreeBSD__) || defined(__OpenBSD__)
    char path[] = "/tmp/ring-buffer-XXXXXX";
#else
    char path[] = "/dev/shm/ring-buffer-XXXXXX";
#endif
    int file_descriptor;
#endif

    // Round up to page-granulation; the nearest 64k boundary for now
    size = (size + k_64 - 1) / k_64 * k_64;

    // Set defaults
    buffer->size = size;
    buffer->ptr = NULL;
#ifdef RMT_PLATFORM_WINDOWS
    buffer->file_map_handle = INVALID_HANDLE_VALUE;
#endif

#ifdef RMT_PLATFORM_WINDOWS

    // Windows version based on https://gist.github.com/rygorous/3158316

    while (nb_attempts-- > 0)
    {
        uint8_t* desired_addr;

        // Create a file mapping for pointing to its physical address with multiple virtual pages
        buffer->file_map_handle = CreateFileMapping(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0, size, 0);
        if (buffer->file_map_handle == NULL)
            break;

#ifndef _UWP // NON-UWP Windows Desktop Version

        // Reserve two contiguous pages of virtual memory
        desired_addr = (uint8_t*)VirtualAlloc(0, size * 2, MEM_RESERVE, PAGE_NOACCESS);
        if (desired_addr == NULL)
            break;

        // Release the range immediately but retain the address for the next sequence of code to
        // try and map to it. In the mean-time some other OS thread may come along and allocate this
        // address range from underneath us so multiple attempts need to be made.
        VirtualFree(desired_addr, 0, MEM_RELEASE);

        // Immediately try to point both pages at the file mapping
        if (MapViewOfFileEx(buffer->file_map_handle, FILE_MAP_ALL_ACCESS, 0, 0, size, desired_addr) == desired_addr &&
            MapViewOfFileEx(buffer->file_map_handle, FILE_MAP_ALL_ACCESS, 0, 0, size, desired_addr + size) ==
                desired_addr + size)
        {
            buffer->ptr = desired_addr;
            break;
        }

#else // UWP

        // Implementation based on example from:
        // https://docs.microsoft.com/en-us/windows/desktop/api/memoryapi/nf-memoryapi-virtualalloc2
        //
        // Notes
        //  - just replaced the non-uwp functions by the uwp variants.
        //  - Both versions could be rewritten to not need the try-loop, see the example mentioned above. I just keep it
        //  as is for now.
        //  - Successfully tested on Hololens
        desired_addr = (uint8_t*)VirtualAlloc2FromApp(NULL, NULL, 2 * size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                    PAGE_NOACCESS, NULL, 0);

        // Split the placeholder region into two regions of equal size.
        VirtualFree(desired_addr, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);

        // Immediately try to point both pages at the file mapping.
        if (MapViewOfFile3FromApp(buffer->file_map_handle, NULL, desired_addr, 0, size, MEM_REPLACE_PLACEHOLDER,
                                  PAGE_READWRITE, NULL, 0) == desired_addr &&
            MapViewOfFile3FromApp(buffer->file_map_handle, NULL, desired_addr + size, 0, size, MEM_REPLACE_PLACEHOLDER,
                                  PAGE_READWRITE, NULL, 0) == desired_addr + size)
        {
            buffer->ptr = desired_addr;
            break;
        }
#endif
        // Failed to map the virtual pages; cleanup and try again
        CloseHandle(buffer->file_map_handle);
        buffer->file_map_handle = NULL;
    }


#endif

#ifdef RMT_PLATFORM_MACOS

    //
    // Mac version based on https://github.com/mikeash/MAMirroredQueue
    //
    // Copyright (c) 2010, Michael Ash
    // All rights reserved.
    //
    // Redistribution and use in source and binary forms, with or without modification, are permitted provided that
    // the following conditions are met:
    //
    // Redistributions of source code must retain the above copyright notice, this list of conditions and the following
    // disclaimer.
    //
    // Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
    // following disclaimer in the documentation and/or other materials provided with the distribution.
    // Neither the name of Michael Ash nor the names of its contributors may be used to endorse or promote products
    // derived from this software without specific prior written permission.
    //
    // THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
    // WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
    // PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
    // INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    // SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    // THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
    // IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
    //

    while (nb_attempts-- > 0)
    {
        vm_prot_t cur_prot, max_prot;
        kern_return_t mach_error;
        uint8_t* ptr = NULL;
        uint8_t* target = NULL;

        // Allocate 2 contiguous pages of virtual memory
        if (vm_allocate(mach_task_self(), (vm_address_t*)&ptr, size * 2, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
            break;

        // Try to deallocate the last page, leaving its virtual memory address free
        target = ptr + size;
        if (vm_deallocate(mach_task_self(), (vm_address_t)target, size) != KERN_SUCCESS)
        {
            vm_deallocate(mach_task_self(), (vm_address_t)ptr, size * 2);
            break;
        }

        // Attempt to remap the page just deallocated to the buffer again
        mach_error = vm_remap(mach_task_self(), (vm_address_t*)&target, size,
                              0, // mask
                              0, // anywhere
                              mach_task_self(), (vm_address_t)ptr,
                              0, // copy
                              &cur_prot, &max_prot, VM_INHERIT_COPY);

        if (mach_error == KERN_NO_SPACE)
        {
            // Failed on this pass, cleanup and make another attempt
            if (vm_deallocate(mach_task_self(), (vm_address_t)ptr, size) != KERN_SUCCESS)
                break;
        }

        else if (mach_error == KERN_SUCCESS)
        {
            // Leave the loop on success
            buffer->ptr = ptr;
            break;
        }

        else
        {
            // Unknown error, can't recover
            vm_deallocate(mach_task_self(), (vm_address_t)ptr, size);
            break;
        }
    }

#endif

#ifdef RMT_PLATFORM_LINUX

    // Linux version based on now-defunct Wikipedia section
    // http://en.wikipedia.org/w/index.php?title=Circular_buffer&oldid=600431497

    // Create a unique temporary filename in the shared memory folder
    file_descriptor = mkstemp(path);
    if (file_descriptor < 0)
        return RMT_ERROR_VIRTUAL_MEMORY_BUFFER_FAIL;

    // Delete the name
    if (unlink(path))
        return RMT_ERROR_VIRTUAL_MEMORY_BUFFER_FAIL;

    // Set the file size to twice the buffer size
    // TODO: this 2x behaviour can be avoided with similar solution to Win/Mac
    if (ftruncate(file_descriptor, size * 2))
        return RMT_ERROR_VIRTUAL_MEMORY_BUFFER_FAIL;

    // Map 2 contiguous pages
    buffer->ptr = (uint8_t*)mmap(NULL, size * 2, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (buffer->ptr == MAP_FAILED)
    {
        buffer->ptr = NULL;
        return RMT_ERROR_VIRTUAL_MEMORY_BUFFER_FAIL;
    }

    // Point both pages to the same memory file
    if (mmap(buffer->ptr, size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, file_descriptor, 0) != buffer->ptr ||
        mmap(buffer->ptr + size, size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, file_descriptor, 0) !=
            buffer->ptr + size)
        return RMT_ERROR_VIRTUAL_MEMORY_BUFFER_FAIL;

#endif

    // Cleanup if exceeded number of attempts or failed
    if (buffer->ptr == NULL)
        return RMT_ERROR_VIRTUAL_MEMORY_BUFFER_FAIL;

    return RMT_ERROR_NONE;
}

static void VirtualMirrorBuffer_Destructor(VirtualMirrorBuffer* buffer)
{
    assert(buffer != 0);

#ifdef RMT_PLATFORM_WINDOWS
    if (buffer->file_map_handle != NULL)
    {
        // FIXME, don't we need to unmap the file views obtained in VirtualMirrorBuffer_Constructor, both for
        // uwp/non-uwp See example
        // https://docs.microsoft.com/en-us/windows/desktop/api/memoryapi/nf-memoryapi-virtualalloc2

        CloseHandle(buffer->file_map_handle);
        buffer->file_map_handle = NULL;
    }
#endif

#ifdef RMT_PLATFORM_MACOS
    if (buffer->ptr != NULL)
        vm_deallocate(mach_task_self(), (vm_address_t)buffer->ptr, buffer->size * 2);
#endif

#ifdef RMT_PLATFORM_LINUX
    if (buffer->ptr != NULL)
        munmap(buffer->ptr, buffer->size * 2);
#endif

    buffer->ptr = NULL;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @SAFEC: Safe C Library excerpts
   http://sourceforge.net/projects/safeclib/
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

/*------------------------------------------------------------------
 *
 * November 2008, Bo Berry
 *
 * Copyright (c) 2008-2011 by Cisco Systems, Inc
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *------------------------------------------------------------------
 */

// NOTE: Microsoft also has its own version of these functions so I'm do some hacky PP to remove them
#define strnlen_s strnlen_s_safe_c
#define strncat_s strncat_s_safe_c
#define strcpy_s strcpy_s_safe_c

#define RSIZE_MAX_STR (4UL << 10) /* 4KB */
#define RCNEGATE(x) x

#define EOK (0)
#define ESNULLP (400) /* null ptr                    */
#define ESZEROL (401) /* length is zero              */
#define ESLEMAX (403) /* length exceeds max          */
#define ESOVRLP (404) /* overlap undefined           */
#define ESNOSPC (406) /* not enough space for s2     */
#define ESUNTERM (407) /* unterminated string         */
#define ESNOTFND (409) /* not found                   */

#ifndef _ERRNO_T_DEFINED
#define _ERRNO_T_DEFINED
typedef int errno_t;
#endif

// rsize_t equivalent without going to the hassle of detecting if a platform has implemented C11/K3.2
typedef uint32_t r_size_t;

static r_size_t strnlen_s(const char* dest, r_size_t dmax)
{
    r_size_t count;

    if (dest == NULL)
    {
        return RCNEGATE(0);
    }

    if (dmax == 0)
    {
        return RCNEGATE(0);
    }

    if (dmax > RSIZE_MAX_STR)
    {
        return RCNEGATE(0);
    }

    count = 0;
    while (*dest && dmax)
    {
        count++;
        dmax--;
        dest++;
    }

    return RCNEGATE(count);
}

static errno_t strstr_s(char* dest, r_size_t dmax, const char* src, r_size_t slen, char** substring)
{
    r_size_t len;
    r_size_t dlen;
    int i;

    if (substring == NULL)
    {
        return RCNEGATE(ESNULLP);
    }
    *substring = NULL;

    if (dest == NULL)
    {
        return RCNEGATE(ESNULLP);
    }

    if (dmax == 0)
    {
        return RCNEGATE(ESZEROL);
    }

    if (dmax > RSIZE_MAX_STR)
    {
        return RCNEGATE(ESLEMAX);
    }

    if (src == NULL)
    {
        return RCNEGATE(ESNULLP);
    }

    if (slen == 0)
    {
        return RCNEGATE(ESZEROL);
    }

    if (slen > RSIZE_MAX_STR)
    {
        return RCNEGATE(ESLEMAX);
    }

    /*
     * src points to a string with zero length, or
     * src equals dest, return dest
     */
    if (*src == '\0' || dest == src)
    {
        *substring = dest;
        return RCNEGATE(EOK);
    }

    while (*dest && dmax)
    {
        i = 0;
        len = slen;
        dlen = dmax;

        while (src[i] && dlen)
        {

            /* not a match, not a substring */
            if (dest[i] != src[i])
            {
                break;
            }

            /* move to the next char */
            i++;
            len--;
            dlen--;

            if (src[i] == '\0' || !len)
            {
                *substring = dest;
                return RCNEGATE(EOK);
            }
        }
        dest++;
        dmax--;
    }

    /*
     * substring was not found, return NULL
     */
    *substring = NULL;
    return RCNEGATE(ESNOTFND);
}

static errno_t strncat_s(char* dest, r_size_t dmax, const char* src, r_size_t slen)
{
    const char* overlap_bumper;

    if (dest == NULL)
    {
        return RCNEGATE(ESNULLP);
    }

    if (src == NULL)
    {
        return RCNEGATE(ESNULLP);
    }

    if (slen > RSIZE_MAX_STR)
    {
        return RCNEGATE(ESLEMAX);
    }

    if (dmax == 0)
    {
        return RCNEGATE(ESZEROL);
    }

    if (dmax > RSIZE_MAX_STR)
    {
        return RCNEGATE(ESLEMAX);
    }

    /* hold base of dest in case src was not copied */

    if (dest < src)
    {
        overlap_bumper = src;

        /* Find the end of dest */
        while (*dest != '\0')
        {

            if (dest == overlap_bumper)
            {
                return RCNEGATE(ESOVRLP);
            }

            dest++;
            dmax--;
            if (dmax == 0)
            {
                return RCNEGATE(ESUNTERM);
            }
        }

        while (dmax > 0)
        {
            if (dest == overlap_bumper)
            {
                return RCNEGATE(ESOVRLP);
            }

            /*
             * Copying truncated before the source null is encountered
             */
            if (slen == 0)
            {
                *dest = '\0';
                return RCNEGATE(EOK);
            }

            *dest = *src;
            if (*dest == '\0')
            {
                return RCNEGATE(EOK);
            }

            dmax--;
            slen--;
            dest++;
            src++;
        }
    }
    else
    {
        overlap_bumper = dest;

        /* Find the end of dest */
        while (*dest != '\0')
        {

            /*
             * NOTE: no need to check for overlap here since src comes first
             * in memory and we're not incrementing src here.
             */
            dest++;
            dmax--;
            if (dmax == 0)
            {
                return RCNEGATE(ESUNTERM);
            }
        }

        while (dmax > 0)
        {
            if (src == overlap_bumper)
            {
                return RCNEGATE(ESOVRLP);
            }

            /*
             * Copying truncated
             */
            if (slen == 0)
            {
                *dest = '\0';
                return RCNEGATE(EOK);
            }

            *dest = *src;
            if (*dest == '\0')
            {
                return RCNEGATE(EOK);
            }

            dmax--;
            slen--;
            dest++;
            src++;
        }
    }

    /*
     * the entire src was not copied, so the string will be nulled.
     */
    return RCNEGATE(ESNOSPC);
}

errno_t strcpy_s(char* dest, r_size_t dmax, const char* src)
{
    const char* overlap_bumper;

    if (dest == NULL)
    {
        return RCNEGATE(ESNULLP);
    }

    if (dmax == 0)
    {
        return RCNEGATE(ESZEROL);
    }

    if (dmax > RSIZE_MAX_STR)
    {
        return RCNEGATE(ESLEMAX);
    }

    if (src == NULL)
    {
        *dest = '\0';
        return RCNEGATE(ESNULLP);
    }

    if (dest == src)
    {
        return RCNEGATE(EOK);
    }

    if (dest < src)
    {
        overlap_bumper = src;

        while (dmax > 0)
        {
            if (dest == overlap_bumper)
            {
                return RCNEGATE(ESOVRLP);
            }

            *dest = *src;
            if (*dest == '\0')
            {
                return RCNEGATE(EOK);
            }

            dmax--;
            dest++;
            src++;
        }
    }
    else
    {
        overlap_bumper = dest;

        while (dmax > 0)
        {
            if (src == overlap_bumper)
            {
                return RCNEGATE(ESOVRLP);
            }

            *dest = *src;
            if (*dest == '\0')
            {
                return RCNEGATE(EOK);
            }

            dmax--;
            dest++;
            src++;
        }
    }

    /*
     * the entire src must have been copied, if not reset dest
     * to null the string.
     */
    return RCNEGATE(ESNOSPC);
}

/* very simple integer to hex */
static const char* hex_encoding_table = "0123456789ABCDEF";

static void itoahex_s(char* dest, r_size_t dmax, int32_t value)
{
    r_size_t len;
    int32_t halfbytepos;

    halfbytepos = 8;

    /* strip leading 0's */
    while (halfbytepos > 1)
    {
        --halfbytepos;
        if (value >> (4 * halfbytepos) & 0xF)
        {
            ++halfbytepos;
            break;
        }
    }

    len = 0;
    while (len + 1 < dmax && halfbytepos > 0)
    {
        --halfbytepos;
        dest[len] = hex_encoding_table[value >> (4 * halfbytepos) & 0xF];
        ++len;
    }

    if (len < dmax)
    {
        dest[len] = 0;
    }
}

static const char* itoa_s(int32_t value)
{
    static char temp_dest[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    int pos = 10;

    // Work back with the absolute value
    int32_t abs_value = abs(value);
    while (abs_value > 0)
    {
        temp_dest[pos--] = '0' + (abs_value % 10);
        abs_value /= 10;
    }

    // Place the negative
    if (value < 0)
    {
        temp_dest[pos--] = '-';
    }

    return temp_dest + pos + 1;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @OSTHREADS: Wrappers around OS-specific thread functions
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

#ifdef RMT_PLATFORM_WINDOWS
typedef DWORD rmtThreadId;
typedef HANDLE rmtThreadHandle;
#else
typedef uintptr_t rmtThreadId;
typedef pthread_t rmtThreadHandle;
#endif

#ifdef RMT_PLATFORM_WINDOWS
typedef CONTEXT rmtCpuContext;
#else
typedef int rmtCpuContext;
#endif

static uint32_t rmtGetNbProcessors()
{
#ifdef RMT_PLATFORM_WINDOWS
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    return system_info.dwNumberOfProcessors;
#else
    // TODO: get_nprocs_conf / get_nprocs
    return 0;
#endif
}

static rmtThreadId rmtGetCurrentThreadId()
{
#ifdef RMT_PLATFORM_WINDOWS
    return GetCurrentThreadId();
#else
    return (rmtThreadId)pthread_self();
#endif
}

static rmtBool rmtSuspendThread(rmtThreadHandle thread_handle)
{
#ifdef RMT_PLATFORM_WINDOWS
    // SuspendThread is an async call to the scheduler and upon return the thread is not guaranteed to be suspended.
    // Calling GetThreadContext will serialise that.
    // See: https://github.com/mono/mono/blob/master/mono/utils/mono-threads-windows.c#L203
    return SuspendThread(thread_handle) == 0 ? RMT_TRUE : RMT_FALSE;
#else
    return RMT_FALSE;
#endif
}

static void rmtResumeThread(rmtThreadHandle thread_handle)
{
#ifdef RMT_PLATFORM_WINDOWS
    ResumeThread(thread_handle);
#endif
}

#ifdef RMT_PLATFORM_WINDOWS
#ifndef CONTEXT_EXCEPTION_REQUEST
// These seem to be guarded by a _AMD64_ macro in winnt.h, which doesn't seem to be defined in older MSVC compilers.
// Which makes sense given this was a post-Vista/Windows 7 patch around errors in the WoW64 context switch.
// This bug was never fixed in the OS so defining these will only get this code to compile on Old Windows systems, with no
// guarantee of being stable at runtime.
#define CONTEXT_EXCEPTION_ACTIVE 0x8000000L
#define CONTEXT_SERVICE_ACTIVE 0x10000000L
#define CONTEXT_EXCEPTION_REQUEST 0x40000000L
#define CONTEXT_EXCEPTION_REPORTING 0x80000000L
#endif
#endif

static rmtBool rmtGetUserModeThreadContext(rmtThreadHandle thread, rmtCpuContext* context)
{
#ifdef RMT_PLATFORM_WINDOWS
    DWORD kernel_mode_mask;

    // Request thread context with exception reporting
    context->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_EXCEPTION_REQUEST;
    if (GetThreadContext(thread, context) == 0)
    {
        return RMT_FALSE;
    }

    // Context on WoW64 is only valid and can only be set if the thread isn't in kernel mode
    // Typical reference to this appears to be: http://zachsaw.blogspot.com/2010/11/wow64-bug-getthreadcontext-may-return.html
    // Confirmed by MS here: https://social.msdn.microsoft.com/Forums/vstudio/en-US/aa176c36-6624-4776-9380-1c9cf37a314e/getthreadcontext-returns-stale-register-values-on-wow64?forum=windowscompatibility
    kernel_mode_mask = CONTEXT_EXCEPTION_REPORTING | CONTEXT_EXCEPTION_ACTIVE | CONTEXT_SERVICE_ACTIVE;
    return (context->ContextFlags & kernel_mode_mask) == CONTEXT_EXCEPTION_REPORTING ? RMT_TRUE : RMT_FALSE;
#else
    return RMT_FALSE;
#endif
}

static void rmtSetThreadContext(rmtThreadHandle thread_handle, rmtCpuContext* context)
{
#ifdef RMT_PLATFORM_WINDOWS
    SetThreadContext(thread_handle, context);
#endif
}

static rmtError rmtOpenThreadHandle(rmtThreadId thread_id, rmtThreadHandle* out_thread_handle)
{
#ifdef RMT_PLATFORM_WINDOWS
    // Open the thread with required access rights to get the thread handle
    *out_thread_handle = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_SET_CONTEXT | THREAD_GET_CONTEXT, FALSE, thread_id);
    if (*out_thread_handle == NULL)
    {
        return RMT_ERROR_OPEN_THREAD_HANDLE_FAIL;
    }
#endif

    return RMT_ERROR_NONE;
}

static void rmtCloseThreadHandle(rmtThreadHandle thread_handle)
{
#ifdef RMT_PLATFORM_WINDOWS
    if (thread_handle != NULL)
    {
        CloseHandle(thread_handle);
    }
#endif
}

#ifdef RMT_ENABLE_THREAD_SAMPLER
DWORD_PTR GetThreadStartAddress(rmtThreadHandle thread_handle)
{
    // Get NtQueryInformationThread from ntdll
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll != NULL)
    {
        typedef NTSTATUS (WINAPI *NTQUERYINFOMATIONTHREAD)(HANDLE, LONG, PVOID, ULONG, PULONG);
        NTQUERYINFOMATIONTHREAD NtQueryInformationThread = (NTQUERYINFOMATIONTHREAD)GetProcAddress(ntdll, "NtQueryInformationThread");

        // Use it to query the start address
        DWORD_PTR start_address;
        NTSTATUS status = NtQueryInformationThread(thread_handle, 9, &start_address, sizeof(DWORD), NULL);
        if (status == 0)
        {
            return start_address;
        }
    }

    return 0;
}

const char* GetStartAddressModuleName(DWORD_PTR start_address)
{
    BOOL success;
    MODULEENTRY32 module_entry;

    // Snapshot the modules
    HANDLE handle = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return NULL;
    }

    module_entry.dwSize = sizeof(MODULEENTRY32);
    module_entry.th32ModuleID = 1;

    // Enumerate modules checking start address against their loaded address range
    success = Module32First(handle, &module_entry);
    while (success == TRUE)
    {
        if (start_address >= (DWORD_PTR)module_entry.modBaseAddr && start_address <= ((DWORD_PTR)module_entry.modBaseAddr + module_entry.modBaseSize))
        {
            static char name[MAX_MODULE_NAME32 + 1];
#ifdef UNICODE
            int size = WideCharToMultiByte(CP_ACP, 0, module_entry.szModule, -1, name, MAX_MODULE_NAME32, NULL, NULL);
            if (size < 1)
            {
                name[0] = '\0';
            }
#else
            strcpy_s(name, sizeof(name), module_entry.szModule);
#endif
            CloseHandle(handle);
            return name;
        }

        success = Module32Next(handle, &module_entry);
    }

    CloseHandle(handle);

    return NULL;
}
#endif

static void rmtGetThreadNameFallback(char* out_thread_name, uint32_t thread_name_size);

static void rmtGetThreadName(rmtThreadId thread_id, rmtThreadHandle thread_handle, char* out_thread_name, uint32_t thread_name_size)
{
#ifdef RMT_PLATFORM_WINDOWS
    DWORD_PTR address;
    const char* module_name;
    uint32_t len;

    // Use the new Windows 10 GetThreadDescription function
    HMODULE kernel32 = GetModuleHandleA("Kernel32.dll");
    if (kernel32 != NULL)
    {
        typedef HRESULT(WINAPI* GETTHREADDESCRIPTION)(HANDLE hThread, PWSTR *ppszThreadDescription);
        GETTHREADDESCRIPTION GetThreadDescription = (GETTHREADDESCRIPTION)GetProcAddress(kernel32, "GetThreadDescription");
        if (GetThreadDescription != NULL)
        {
            int size;

            WCHAR* thread_name_w;
            GetThreadDescription(thread_handle, &thread_name_w);

            // Returned size is the byte size, so will be 1 for a null-terminated strings
            size = WideCharToMultiByte(CP_ACP, 0, thread_name_w, -1, out_thread_name, thread_name_size, NULL, NULL);
            if (size > 1)
            {
                return;
            }
        }
    }

    // At this point GetThreadDescription hasn't returned anything so let's get the thread module name and use that
    address = GetThreadStartAddress(thread_handle);
    if (address == 0)
    {
        rmtGetThreadNameFallback(out_thread_name, thread_name_size);
        return;
    }
    module_name = GetStartAddressModuleName(address);
    if (module_name == NULL)
    {
        rmtGetThreadNameFallback(out_thread_name, thread_name_size);
        return;
    }

    // Concatenate thread name with then thread ID as that will be unique, whereas the start address won't be
    memset(out_thread_name, 0, thread_name_size);
    strcpy_s(out_thread_name, thread_name_size, module_name);
    strncat_s(out_thread_name, thread_name_size, "!", 1);
    len = strnlen_s(out_thread_name, thread_name_size);
    itoahex_s(out_thread_name + len, thread_name_size - len, thread_id);

#elif defined(RMT_PLATFORM_MACOS)

    int ret = pthread_getname_np(pthread_self(), out_thread_name, thread_name_size);
    if (ret != 0 || out_thread_name[0] == '\0')
    {
        rmtGetThreadNameFallback(out_thread_name, thread_name_size);
    }

#elif defined(RMT_PLATFORM_LINUX) && RMT_USE_POSIX_THREADNAMES && !defined(__FreeBSD__) && !defined(__OpenBSD__)

    prctl(PR_GET_NAME, out_thread_name, 0, 0, 0);

#else

    rmtGetThreadNameFallback(out_thread_name, thread_name_size);

#endif
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @THREADS: Cross-platform thread object
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

typedef struct Thread_t rmtThread;
typedef rmtError (*ThreadProc)(rmtThread* thread);

struct Thread_t
{
    rmtThreadHandle handle;

    // Callback executed when the thread is created
    ThreadProc callback;

    // Caller-specified parameter passed to Thread_Create
    void* param;

    // Error state returned from callback
    rmtError error;

    // External threads can set this to request an exit
    rmtAtomicBool request_exit;
};

#if defined(RMT_PLATFORM_WINDOWS)

static DWORD WINAPI ThreadProcWindows(LPVOID lpParameter)
{
    rmtThread* thread = (rmtThread*)lpParameter;
    assert(thread != NULL);
    thread->error = thread->callback(thread);
    return thread->error == RMT_ERROR_NONE ? 0 : 1;
}

#else
static void* StartFunc(void* pArgs)
{
    rmtThread* thread = (rmtThread*)pArgs;
    assert(thread != NULL);
    thread->error = thread->callback(thread);
    return NULL; // returned error not use, check thread->error.
}
#endif

static int rmtThread_Valid(rmtThread* thread)
{
    assert(thread != NULL);

#if defined(RMT_PLATFORM_WINDOWS)
    return thread->handle != NULL;
#else
    return !pthread_equal(thread->handle, pthread_self());
#endif
}

static rmtError rmtThread_Constructor(rmtThread* thread, ThreadProc callback, void* param)
{
    assert(thread != NULL);

    thread->callback = callback;
    thread->param = param;
    thread->error = RMT_ERROR_NONE;
    thread->request_exit = RMT_FALSE;

    // OS-specific thread creation

#if defined(RMT_PLATFORM_WINDOWS)

    thread->handle = CreateThread(NULL,              // lpThreadAttributes
                                  0,                 // dwStackSize
                                  ThreadProcWindows, // lpStartAddress
                                  thread,            // lpParameter
                                  0,                 // dwCreationFlags
                                  NULL);             // lpThreadId

    if (thread->handle == NULL)
        return RMT_ERROR_CREATE_THREAD_FAIL;

#else

    int32_t error = pthread_create(&thread->handle, NULL, StartFunc, thread);
    if (error)
    {
        // Contents of 'thread' parameter to pthread_create() are undefined after
        // failure call so can't pre-set to invalid value before hand.
        thread->handle = pthread_self();
        return RMT_ERROR_CREATE_THREAD_FAIL;
    }

#endif

    return RMT_ERROR_NONE;
}

static void rmtThread_RequestExit(rmtThread* thread)
{
    // Not really worried about memory barriers or delayed visibility to the target thread
    assert(thread != NULL);
    thread->request_exit = RMT_TRUE;
}

static void rmtThread_Join(rmtThread* thread)
{
    assert(rmtThread_Valid(thread));

#if defined(RMT_PLATFORM_WINDOWS)
    WaitForSingleObject(thread->handle, INFINITE);
#else
    pthread_join(thread->handle, NULL);
#endif
}

static void rmtThread_Destructor(rmtThread* thread)
{
    assert(thread != NULL);

    if (rmtThread_Valid(thread))
    {
        // Shutdown the thread
        rmtThread_RequestExit(thread);
        rmtThread_Join(thread);

        // OS-specific release of thread resources

#if defined(RMT_PLATFORM_WINDOWS)
        CloseHandle(thread->handle);
        thread->handle = NULL;
#endif
    }
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @OBJALLOC: Reusable Object Allocator
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

//
// All objects that require free-list-backed allocation need to inherit from this type.
//
typedef struct ObjectLink_s
{
    struct ObjectLink_s* volatile next;
} ObjectLink;

typedef rmtAtomicPtr(ObjectLink)    rmtAtomicObjectLinkPtr;

static void ObjectLink_Constructor(ObjectLink* link)
{
    assert(link != NULL);
    link->next = NULL;
}

typedef rmtError (*ObjConstructor)(void*);
typedef void (*ObjDestructor)(void*);

typedef struct
{
    // Object create/destroy parameters
    uint32_t object_size;
    ObjConstructor constructor;
    ObjDestructor destructor;

    // Number of objects in the free list
    rmtAtomicS32 nb_free;

    // Number of objects used by callers
    rmtAtomicS32 nb_inuse;

    // Total allocation count
    rmtAtomicS32 nb_allocated;

    rmtAtomicObjectLinkPtr first_free;
} ObjectAllocator;

static rmtError ObjectAllocator_Constructor(ObjectAllocator* allocator, uint32_t object_size, ObjConstructor constructor,
                                            ObjDestructor destructor)
{
    allocator->object_size = object_size;
    allocator->constructor = constructor;
    allocator->destructor = destructor;
    allocator->nb_free = 0;
    allocator->nb_inuse = 0;
    allocator->nb_allocated = 0;
    allocator->first_free = (ObjectLink*)0;
    return RMT_ERROR_NONE;
}

static void ObjectAllocator_Destructor(ObjectAllocator* allocator)
{
    // Ensure everything has been released to the allocator
    assert(allocator != NULL);
    assert(allocator->nb_inuse == 0);

    // Destroy all objects released to the allocator
    while (allocator->first_free != NULL)
    {
        ObjectLink* next = ((ObjectLink*)allocator->first_free)->next;
        assert(allocator->destructor != NULL);
        allocator->destructor((void*)allocator->first_free);
        rmtFree((void*)allocator->first_free);
        allocator->first_free = next;
    }
}

static void ObjectAllocator_Push(ObjectAllocator* allocator, ObjectLink* start, ObjectLink* end)
{
    assert(allocator != NULL);
    assert(start != NULL);
    assert(end != NULL);

    // CAS pop add range to the front of the list
    for (;;)
    {
        ObjectLink* old_link = (ObjectLink*)allocator->first_free;
        end->next = old_link;
        if (AtomicCompareAndSwapPointer((rmtAtomicVoidPtr*)&allocator->first_free, (void*)old_link, (void*)start) ==
            RMT_TRUE)
            break;
    }
}

static ObjectLink* ObjectAllocator_Pop(ObjectAllocator* allocator)
{
    ObjectLink* link;

    assert(allocator != NULL);

    // CAS pop from the front of the list
    for (;;)
    {
        ObjectLink* old_link = (ObjectLink*)allocator->first_free;
        if (old_link == NULL)
        {
            return NULL;
        }
        ObjectLink* next_link = old_link->next;
        if (AtomicCompareAndSwapPointer((rmtAtomicVoidPtr*)&allocator->first_free, (void*)old_link, (void*)next_link) ==
            RMT_TRUE)
        {
            link = (ObjectLink*)old_link;
            break;
        }
    }

    link->next = NULL;

    return link;
}

static rmtError ObjectAllocator_Alloc(ObjectAllocator* allocator, void** object)
{
    // This function only calls the object constructor on initial malloc of an object

    assert(allocator != NULL);
    assert(object != NULL);

    // Pull available objects from the free list
    *object = ObjectAllocator_Pop(allocator);

    // Has the free list run out?
    if (*object == NULL)
    {
        rmtError error;

        // Allocate/construct a new object
        *object = rmtMalloc(allocator->object_size);
        if (*object == NULL)
            return RMT_ERROR_MALLOC_FAIL;
        assert(allocator->constructor != NULL);
        error = allocator->constructor(*object);
        if (error != RMT_ERROR_NONE)
        {
            // Auto-teardown on failure
            assert(allocator->destructor != NULL);
            allocator->destructor(*object);
            rmtFree(*object);
            return error;
        }

        AtomicAddS32(&allocator->nb_allocated, 1);
    }
    else
    {
        AtomicSubS32(&allocator->nb_free, 1);
    }

    AtomicAddS32(&allocator->nb_inuse, 1);

    return RMT_ERROR_NONE;
}

static void ObjectAllocator_Free(ObjectAllocator* allocator, void* object)
{
    // Add back to the free-list
    assert(allocator != NULL);
    ObjectAllocator_Push(allocator, (ObjectLink*)object, (ObjectLink*)object);
    AtomicSubS32(&allocator->nb_inuse, 1);
    AtomicAddS32(&allocator->nb_free, 1);
}

static void ObjectAllocator_FreeRange(ObjectAllocator* allocator, void* start, void* end, uint32_t count)
{
    assert(allocator != NULL);
    ObjectAllocator_Push(allocator, (ObjectLink*)start, (ObjectLink*)end);
    AtomicSubS32(&allocator->nb_inuse, count);
    AtomicAddS32(&allocator->nb_free, count);
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @DYNBUF: Dynamic Buffer
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

typedef struct
{
    uint32_t alloc_granularity;

    uint32_t bytes_allocated;
    uint32_t bytes_used;

    uint8_t* data;
} Buffer;

static rmtError Buffer_Constructor(Buffer* buffer, uint32_t alloc_granularity)
{
    assert(buffer != NULL);
    buffer->alloc_granularity = alloc_granularity;
    buffer->bytes_allocated = 0;
    buffer->bytes_used = 0;
    buffer->data = NULL;
    return RMT_ERROR_NONE;
}

static void Buffer_Destructor(Buffer* buffer)
{
    assert(buffer != NULL);

    if (buffer->data != NULL)
    {
        rmtFree(buffer->data);
        buffer->data = NULL;
    }
}

static rmtError Buffer_Grow(Buffer* buffer, uint32_t length)
{
    // Calculate size increase rounded up to the requested allocation granularity
    uint32_t granularity = buffer->alloc_granularity;
    uint32_t allocate = buffer->bytes_allocated + length;
    allocate = allocate + ((granularity - 1) - ((allocate - 1) % granularity));

    buffer->bytes_allocated = allocate;
    buffer->data = (uint8_t*)rmtRealloc(buffer->data, buffer->bytes_allocated);
    if (buffer->data == NULL)
        return RMT_ERROR_MALLOC_FAIL;

    return RMT_ERROR_NONE;
}

static rmtError Buffer_Pad(Buffer* buffer, uint32_t length)
{
    assert(buffer != NULL);

    // Reallocate the buffer on overflow
    if (buffer->bytes_used + length > buffer->bytes_allocated)
    {
        rmtTry(Buffer_Grow(buffer, length));
    }

    // Step by the pad amount
    buffer->bytes_used += length;

    return RMT_ERROR_NONE;
}

static rmtError Buffer_AlignedPad(Buffer* buffer, uint32_t start_pos)
{
    return Buffer_Pad(buffer, (4 - ((buffer->bytes_used - start_pos) & 3)) & 3);
}

static rmtError Buffer_Write(Buffer* buffer, const void* data, uint32_t length)
{
    assert(buffer != NULL);

    // Reallocate the buffer on overflow
    if (buffer->bytes_used + length > buffer->bytes_allocated)
    {
        rmtTry(Buffer_Grow(buffer, length));
    }

    // Copy all bytes
    memcpy(buffer->data + buffer->bytes_used, data, length);
    buffer->bytes_used += length;

    return RMT_ERROR_NONE;
}

static rmtError Buffer_WriteStringZ(Buffer* buffer, rmtPStr string)
{
    assert(string != NULL);
    return Buffer_Write(buffer, (void*)string, (uint32_t)strnlen_s(string, 2048) + 1);
}

static void U32ToByteArray(uint8_t* dest, uint32_t value)
{
    // Commit as little-endian
    dest[0] = value & 255;
    dest[1] = (value >> 8) & 255;
    dest[2] = (value >> 16) & 255;
    dest[3] = value >> 24;
}

static rmtError Buffer_WriteU32(Buffer* buffer, uint32_t value)
{
    assert(buffer != NULL);

    // Reallocate the buffer on overflow
    if (buffer->bytes_used + sizeof(value) > buffer->bytes_allocated)
    {
        rmtTry(Buffer_Grow(buffer, sizeof(value)));
    }

// Copy all bytes
#if RMT_ASSUME_LITTLE_ENDIAN
    memcpy(buffer->data + buffer->bytes_used, &value, sizeof(value));
#else
    U32ToByteArray(buffer->data + buffer->bytes_used, value);
#endif

    buffer->bytes_used += sizeof(value);

    return RMT_ERROR_NONE;
}

#if !RMT_ASSUME_LITTLE_ENDIAN
static rmtBool IsLittleEndian()
{
    // Not storing this in a global variable allows the compiler to more easily optimise
    // this away altogether.
    union {
        uint32_t i;
        uint8_t c[sizeof(uint32_t)];
    } u;
    u.i = 1;
    return u.c[0] == 1 ? RMT_TRUE : RMT_FALSE;
}
#endif

static rmtError Buffer_WriteF64(Buffer* buffer, double value)
{
    assert(buffer != NULL);

    // Reallocate the buffer on overflow
    if (buffer->bytes_used + sizeof(value) > buffer->bytes_allocated)
    {
        rmtTry(Buffer_Grow(buffer, sizeof(value)));
    }

// Copy all bytes
#if RMT_ASSUME_LITTLE_ENDIAN
    memcpy(buffer->data + buffer->bytes_used, &value, sizeof(value));
#else
    {
        union {
            double d;
            uint8_t c[sizeof(double)];
        } u;
        uint8_t* dest = buffer->data + buffer->bytes_used;
        u.d = value;
        if (IsLittleEndian())
        {
            dest[0] = u.c[0];
            dest[1] = u.c[1];
            dest[2] = u.c[2];
            dest[3] = u.c[3];
            dest[4] = u.c[4];
            dest[5] = u.c[5];
            dest[6] = u.c[6];
            dest[7] = u.c[7];
        }
        else
        {
            dest[0] = u.c[7];
            dest[1] = u.c[6];
            dest[2] = u.c[5];
            dest[3] = u.c[4];
            dest[4] = u.c[3];
            dest[5] = u.c[2];
            dest[6] = u.c[1];
            dest[7] = u.c[0];
        }
    }
#endif

    buffer->bytes_used += sizeof(value);

    return RMT_ERROR_NONE;
}

static rmtError Buffer_WriteU64(Buffer* buffer, uint64_t value)
{
    // Write as a double as Javascript DataView doesn't have a 64-bit integer read
    return Buffer_WriteF64(buffer, (double)value);
}

static rmtError Buffer_WriteStringWithLength(Buffer* buffer, rmtPStr string)
{
    uint32_t length = (uint32_t)strnlen_s(string, 2048);
    rmtTry(Buffer_WriteU32(buffer, length));
    return Buffer_Write(buffer, (void*)string, length);
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @HASHTABLE: Integer pair hash map for inserts/finds. No removes for added simplicity.
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

#define RMT_NOT_FOUND 0xffffffffffffffff

typedef struct
{
    // Non-zero, pre-hashed key
    uint32_t key;

    // Value that's not equal to RMT_NOT_FOUND
    uint64_t value;
} HashSlot;

typedef struct
{
    // Stats
    uint32_t maxNbSlots;
    uint32_t nbSlots;

    // Data
    HashSlot* slots;
} rmtHashTable;

static rmtError rmtHashTable_Constructor(rmtHashTable* table, uint32_t max_nb_slots)
{
    // Default initialise
    assert(table != NULL);
    table->maxNbSlots = max_nb_slots;
    table->nbSlots = 0;

    // Allocate and clear the hash slots
    rmtTryMallocArray(HashSlot, table->slots, table->maxNbSlots);
    memset(table->slots, 0, table->maxNbSlots * sizeof(HashSlot));

    return RMT_ERROR_NONE;
}

static void rmtHashTable_Destructor(rmtHashTable* table)
{
    assert(table != NULL);

    if (table->slots != NULL)
    {
        rmtFree(table->slots);
        table->slots = NULL;
    }
}

static rmtError rmtHashTable_Resize(rmtHashTable* table);

static rmtError rmtHashTable_Insert(rmtHashTable* table, uint32_t key, uint64_t value)
{
    HashSlot* slot = NULL;
    rmtError error = RMT_ERROR_NONE;

    // Calculate initial slot location for this key
    uint32_t index_mask = table->maxNbSlots - 1;
    uint32_t index = key & index_mask;

    assert(key != 0);
    assert(value != RMT_NOT_FOUND);

    // Linear probe for free slot, reusing any existing key matches
    // There will always be at least one free slot due to load factor management
    while (table->slots[index].key)
    {
        if (table->slots[index].key == key)
        {
            // Counter occupied slot increments below
            table->nbSlots--;
            break;
        }

        index = (index + 1) & index_mask;
    }

    // Just verify that I've got no errors in the code above
    assert(index < table->maxNbSlots);

    // Add to the table
    slot = table->slots + index;
    slot->key = key;
    slot->value = value;
    table->nbSlots++;

    // Resize when load factor is greater than 2/3
    if (table->nbSlots > (table->maxNbSlots * 2) / 3)
    {
        error = rmtHashTable_Resize(table);
    }

    return error;
}

static rmtError rmtHashTable_Resize(rmtHashTable* table)
{
    uint32_t old_max_nb_slots = table->maxNbSlots;
    HashSlot* new_slots = NULL;
    HashSlot* old_slots = table->slots;
    uint32_t i;

    // Increase the table size
    uint32_t new_max_nb_slots = table->maxNbSlots;
    if (new_max_nb_slots < 8192 * 4)
    {
        new_max_nb_slots *= 4;
    }
    else
    {
        new_max_nb_slots *= 2;
    }

    // Allocate and clear a new table
    rmtTryMallocArray(HashSlot, new_slots, new_max_nb_slots);
    memset(new_slots, 0, new_max_nb_slots * sizeof(HashSlot));

    // Update fields of the table after successful allocation only
    table->slots = new_slots;
    table->maxNbSlots = new_max_nb_slots;
    table->nbSlots = 0;

    // Reinsert all objects into the new table
    for (i = 0; i < old_max_nb_slots; i++)
    {
        HashSlot* slot = old_slots + i;
        if (slot->key != 0)
        {
            rmtHashTable_Insert(table, slot->key, slot->value);
        }
    }

    rmtFree(old_slots);

    return RMT_ERROR_NONE;
}

static uint64_t rmtHashTable_Find(rmtHashTable* table, uint32_t key)
{
    // Calculate initial slot location for this key
    uint32_t index_mask = table->maxNbSlots - 1;
    uint32_t index = key & index_mask;

    // Linear probe for matching hash
    while (table->slots[index].key)
    {
        HashSlot* slot = table->slots + index;

        if (slot->key == key)
        {
            return slot->value;
        }

        index = (index + 1) & index_mask;
    }

    return RMT_NOT_FOUND;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @STRINGTABLE: Map from string hash to string offset in local buffer
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

typedef struct
{
    // Growable dynamic array of strings added so far
    Buffer* text;

    // Map from text hash to text location in the buffer
    rmtHashTable* text_map;
} StringTable;

static rmtError StringTable_Constructor(StringTable* table)
{
    // Default initialise
    assert(table != NULL);
    table->text = NULL;
    table->text_map = NULL;

    // Allocate reasonably storage for initial sample names
    rmtTryNew(Buffer, table->text, 8 * 1024);
    rmtTryNew(rmtHashTable, table->text_map, 1 * 1024);

    return RMT_ERROR_NONE;
}

static void StringTable_Destructor(StringTable* table)
{
    assert(table != NULL);

    rmtDelete(rmtHashTable, table->text_map);
    rmtDelete(Buffer, table->text);
}

static rmtPStr StringTable_Find(StringTable* table, uint32_t name_hash)
{
    uint64_t text_offset = rmtHashTable_Find(table->text_map, name_hash);
    if (text_offset != RMT_NOT_FOUND)
    {
        return (rmtPStr)(table->text->data + text_offset);
    }
    return NULL;
}

static rmtBool StringTable_Insert(StringTable* table, uint32_t name_hash, rmtPStr name)
{
    // Only add to the buffer if the string isn't already there
    uint64_t text_offset = rmtHashTable_Find(table->text_map, name_hash);
    if (text_offset == RMT_NOT_FOUND)
    {
        // TODO: Allocation errors aren't being passed on to the caller
        text_offset = table->text->bytes_used;
        Buffer_WriteStringZ(table->text, name);
        rmtHashTable_Insert(table->text_map, name_hash, text_offset);
        return RMT_TRUE;
    }

    return RMT_FALSE;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @SOCKETS: Sockets TCP/IP Wrapper
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

#ifndef RMT_PLATFORM_WINDOWS
typedef int SOCKET;
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define SD_SEND SHUT_WR
#define closesocket close
#endif

typedef struct
{
    SOCKET socket;
} TCPSocket;

typedef struct
{
    rmtBool can_read;
    rmtBool can_write;
    rmtError error_state;
} SocketStatus;

//
// Function prototypes
//
static void TCPSocket_Close(TCPSocket* tcp_socket);

static rmtError InitialiseNetwork()
{
#ifdef RMT_PLATFORM_WINDOWS

    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data))
    {
        return rmtMakeError(RMT_ERROR_RESOURCE_CREATE_FAIL, "WSAStartup failed");
    }
    if (LOBYTE(wsa_data.wVersion) != 2 || HIBYTE(wsa_data.wVersion) != 2)
    {
        return rmtMakeError(RMT_ERROR_RESOURCE_CREATE_FAIL, "WSAStartup returned incorrect version number");
    }

    return RMT_ERROR_NONE;

#else

    return RMT_ERROR_NONE;

#endif
}

static void ShutdownNetwork()
{
#ifdef RMT_PLATFORM_WINDOWS
    WSACleanup();
#endif
}

static rmtError TCPSocket_Constructor(TCPSocket* tcp_socket)
{
    assert(tcp_socket != NULL);
    tcp_socket->socket = INVALID_SOCKET;
    return InitialiseNetwork();
}

static void TCPSocket_Destructor(TCPSocket* tcp_socket)
{
    assert(tcp_socket != NULL);
    TCPSocket_Close(tcp_socket);
    ShutdownNetwork();
}

static rmtError TCPSocket_RunServer(TCPSocket* tcp_socket, uint16_t port, rmtBool reuse_open_port,
                                    rmtBool limit_connections_to_localhost)
{
    SOCKET s = INVALID_SOCKET;
    struct sockaddr_in sin;
#ifdef RMT_PLATFORM_WINDOWS
    u_long nonblock = 1;
#endif

    memset(&sin, 0, sizeof(sin));
    assert(tcp_socket != NULL);

    // Try to create the socket
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == SOCKET_ERROR)
    {
        return rmtMakeError(RMT_ERROR_RESOURCE_CREATE_FAIL, "Can't create a socket for connection to the remote viewer");
    }

    if (reuse_open_port)
    {
        int enable = 1;

// set SO_REUSEADDR so binding doesn't fail when restarting the application
// (otherwise the same port can't be reused within TIME_WAIT)
// I'm not checking for errors because if this fails (unlikely) we might still
// be able to bind to the socket anyway
#ifdef RMT_PLATFORM_POSIX
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
#elif defined(RMT_PLATFORM_WINDOWS)
        // windows also needs SO_EXCLUSEIVEADDRUSE,
        // see http://www.andy-pearce.com/blog/posts/2013/Feb/so_reuseaddr-on-windows/
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&enable, sizeof(enable));
        enable = 1;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&enable, sizeof(enable));
#endif
    }

    // Bind the socket to the incoming port
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(limit_connections_to_localhost ? INADDR_LOOPBACK : INADDR_ANY);
    sin.sin_port = htons(port);
    if (bind(s, (struct sockaddr*)&sin, sizeof(sin)) == SOCKET_ERROR)
    {
        return rmtMakeError(RMT_ERROR_RESOURCE_ACCESS_FAIL, "Can't bind a socket for the server");
    }

    // Connection is valid, remaining code is socket state modification
    tcp_socket->socket = s;

    // Enter a listening state with a backlog of 1 connection
    if (listen(s, 1) == SOCKET_ERROR)
    {
        return rmtMakeError(RMT_ERROR_RESOURCE_ACCESS_FAIL, "Created server socket failed to enter a listen state");
    }

// Set as non-blocking
#ifdef RMT_PLATFORM_WINDOWS
    if (ioctlsocket(tcp_socket->socket, FIONBIO, &nonblock) == SOCKET_ERROR)
    {
        return rmtMakeError(RMT_ERROR_RESOURCE_ACCESS_FAIL, "Created server socket failed to switch to a non-blocking state");
    }
#else
    if (fcntl(tcp_socket->socket, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
    {
        return rmtMakeError(RMT_ERROR_RESOURCE_ACCESS_FAIL, "Created server socket failed to switch to a non-blocking state");
    }
#endif

    return RMT_ERROR_NONE;
}

static void TCPSocket_Close(TCPSocket* tcp_socket)
{
    assert(tcp_socket != NULL);

    if (tcp_socket->socket != INVALID_SOCKET)
    {
        // Shutdown the connection, stopping all sends
        int result = shutdown(tcp_socket->socket, SD_SEND);
        if (result != SOCKET_ERROR)
        {
            // Keep receiving until the peer closes the connection
            int total = 0;
            char temp_buf[128];
            while (result > 0)
            {
                result = (int)recv(tcp_socket->socket, temp_buf, sizeof(temp_buf), 0);
                total += result;
            }
        }

        // Close the socket and issue a network shutdown request
        closesocket(tcp_socket->socket);
        tcp_socket->socket = INVALID_SOCKET;
    }
}

static SocketStatus TCPSocket_PollStatus(TCPSocket* tcp_socket)
{
    SocketStatus status;
    fd_set fd_read, fd_write, fd_errors;
    struct timeval tv;

    status.can_read = RMT_FALSE;
    status.can_write = RMT_FALSE;
    status.error_state = RMT_ERROR_NONE;

    assert(tcp_socket != NULL);
    if (tcp_socket->socket == INVALID_SOCKET)
    {
        status.error_state = RMT_ERROR_SOCKET_INVALID_POLL;
        return status;
    }

    // Set read/write/error markers for the socket
    FD_ZERO(&fd_read);
    FD_ZERO(&fd_write);
    FD_ZERO(&fd_errors);
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4127) // warning C4127: conditional expression is constant
#endif // _MSC_VER
    FD_SET(tcp_socket->socket, &fd_read);
    FD_SET(tcp_socket->socket, &fd_write);
    FD_SET(tcp_socket->socket, &fd_errors);
#ifdef _MSC_VER
#pragma warning(pop)
#endif // _MSC_VER

    // Poll socket status without blocking
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (select(((int)tcp_socket->socket) + 1, &fd_read, &fd_write, &fd_errors, &tv) == SOCKET_ERROR)
    {
        status.error_state = RMT_ERROR_SOCKET_SELECT_FAIL;
        return status;
    }

    status.can_read = FD_ISSET(tcp_socket->socket, &fd_read) != 0 ? RMT_TRUE : RMT_FALSE;
    status.can_write = FD_ISSET(tcp_socket->socket, &fd_write) != 0 ? RMT_TRUE : RMT_FALSE;
    status.error_state = FD_ISSET(tcp_socket->socket, &fd_errors) != 0 ? RMT_ERROR_SOCKET_POLL_ERRORS : RMT_ERROR_NONE;
    return status;
}

static rmtError TCPSocket_AcceptConnection(TCPSocket* tcp_socket, TCPSocket** client_socket)
{
    SocketStatus status;
    SOCKET s;

    // Ensure there is an incoming connection
    assert(tcp_socket != NULL);
    status = TCPSocket_PollStatus(tcp_socket);
    if (status.error_state != RMT_ERROR_NONE || !status.can_read)
        return status.error_state;

    // Accept the connection
    s = accept(tcp_socket->socket, 0, 0);
    if (s == SOCKET_ERROR)
    {
        return rmtMakeError(RMT_ERROR_RESOURCE_CREATE_FAIL, "Server failed to accept connection from client");
    }

#ifdef SO_NOSIGPIPE
    // On POSIX systems, send() may send a SIGPIPE signal when writing to an
    // already closed connection. By setting this option, we prevent the
    // signal from being emitted and send will instead return an error and set
    // errno to EPIPE.
    //
    // This is supported on BSD platforms and not on Linux.
    {
        int flag = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof(flag));
    }
#endif
    // Create a client socket for the new connection
    assert(client_socket != NULL);
    rmtTryNew(TCPSocket, *client_socket);
    (*client_socket)->socket = s;

    return RMT_ERROR_NONE;
}

static int TCPTryAgain()
{
#ifdef RMT_PLATFORM_WINDOWS
    DWORD error = WSAGetLastError();
    return error == WSAEWOULDBLOCK;
#else
#if EAGAIN == EWOULDBLOCK
    return errno == EAGAIN;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
#endif
}

static rmtError TCPSocket_Send(TCPSocket* tcp_socket, const void* data, uint32_t length, uint32_t timeout_ms)
{
    SocketStatus status;
    char* cur_data = NULL;
    char* end_data = NULL;
    uint32_t start_ms = 0;
    uint32_t cur_ms = 0;

    assert(tcp_socket != NULL);

    start_ms = msTimer_Get();

    // Loop until timeout checking whether data can be written
    status.can_write = RMT_FALSE;
    while (!status.can_write)
    {
        status = TCPSocket_PollStatus(tcp_socket);
        if (status.error_state != RMT_ERROR_NONE)
            return status.error_state;

        cur_ms = msTimer_Get();
        if (cur_ms - start_ms > timeout_ms)
        {
            return rmtMakeError(RMT_ERROR_TIMEOUT, "Timed out trying to send data");
        }
    }

    cur_data = (char*)data;
    end_data = cur_data + length;

    while (cur_data < end_data)
    {
        // Attempt to send the remaining chunk of data
        int bytes_sent;
        int send_flags = 0;
#ifdef MSG_NOSIGNAL
        // On Linux this prevents send from emitting a SIGPIPE signal
        // Equivalent on BSD to the SO_NOSIGPIPE option.
        send_flags = MSG_NOSIGNAL;
#endif
        bytes_sent = (int)send(tcp_socket->socket, cur_data, (int)(end_data - cur_data), send_flags);

        if (bytes_sent == SOCKET_ERROR || bytes_sent == 0)
        {
            // Close the connection if sending fails for any other reason other than blocking
            if (bytes_sent != 0 && !TCPTryAgain())
                return RMT_ERROR_SOCKET_SEND_FAIL;

            // First check for tick-count overflow and reset, giving a slight hitch every 49.7 days
            cur_ms = msTimer_Get();
            if (cur_ms < start_ms)
            {
                start_ms = cur_ms;
                continue;
            }

            //
            // Timeout can happen when:
            //
            //    1) endpoint is no longer there
            //    2) endpoint can't consume quick enough
            //    3) local buffers overflow
            //
            // As none of these are actually errors, we have to pass this timeout back to the caller.
            //
            // TODO: This strategy breaks down if a send partially completes and then times out!
            //
            if (cur_ms - start_ms > timeout_ms)
            {
                return rmtMakeError(RMT_ERROR_TIMEOUT, "Timed out trying to send data");
            }
        }
        else
        {
            // Jump over the data sent
            cur_data += bytes_sent;
        }
    }

    return RMT_ERROR_NONE;
}

static rmtError TCPSocket_Receive(TCPSocket* tcp_socket, void* data, uint32_t length, uint32_t timeout_ms)
{
    SocketStatus status;
    char* cur_data = NULL;
    char* end_data = NULL;
    uint32_t start_ms = 0;
    uint32_t cur_ms = 0;

    assert(tcp_socket != NULL);

    // Ensure there is data to receive
    status = TCPSocket_PollStatus(tcp_socket);
    if (status.error_state != RMT_ERROR_NONE)
        return status.error_state;
    if (!status.can_read)
        return RMT_ERROR_SOCKET_RECV_NO_DATA;

    cur_data = (char*)data;
    end_data = cur_data + length;

    // Loop until all data has been received
    start_ms = msTimer_Get();
    while (cur_data < end_data)
    {
        int bytes_received = (int)recv(tcp_socket->socket, cur_data, (int)(end_data - cur_data), 0);

        if (bytes_received == SOCKET_ERROR || bytes_received == 0)
        {
            // Close the connection if receiving fails for any other reason other than blocking
            if (bytes_received != 0 && !TCPTryAgain())
                return RMT_ERROR_SOCKET_RECV_FAILED;

            // First check for tick-count overflow and reset, giving a slight hitch every 49.7 days
            cur_ms = msTimer_Get();
            if (cur_ms < start_ms)
            {
                start_ms = cur_ms;
                continue;
            }

            //
            // Timeout can happen when:
            //
            //    1) data is delayed by sender
            //    2) sender fails to send a complete set of packets
            //
            // As not all of these scenarios are errors, we need to pass this information back to the caller.
            //
            // TODO: This strategy breaks down if a receive partially completes and then times out!
            //
            if (cur_ms - start_ms > timeout_ms)
            {
                return RMT_ERROR_SOCKET_RECV_TIMEOUT;
            }
        }
        else
        {
            // Jump over the data received
            cur_data += bytes_received;
        }
    }

    return RMT_ERROR_NONE;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @SHA1: SHA-1 Cryptographic Hash Function
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

//
// Typed to allow enforced data size specification
//
typedef struct
{
    uint8_t data[20];
} SHA1;

/*
 Copyright (c) 2011, Micael Hildenborg
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Micael Hildenborg nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY Micael Hildenborg ''AS IS'' AND ANY
 EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL Micael Hildenborg BE LIABLE FOR ANY
 DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 Contributors:
 Gustav
 Several members in the gamedev.se forum.
 Gregory Petrosyan
 */

// Rotate an integer value to left.
static uint32_t rol(const uint32_t value, const uint32_t steps)
{
    return ((value << steps) | (value >> (32 - steps)));
}

// Sets the first 16 integers in the buffert to zero.
// Used for clearing the W buffert.
static void clearWBuffert(uint32_t* buffert)
{
    int pos;
    for (pos = 16; --pos >= 0;)
    {
        buffert[pos] = 0;
    }
}

static void innerHash(uint32_t* result, uint32_t* w)
{
    uint32_t a = result[0];
    uint32_t b = result[1];
    uint32_t c = result[2];
    uint32_t d = result[3];
    uint32_t e = result[4];

    int round = 0;

#define sha1macro(func, val)                                            \
    {                                                                   \
        const uint32_t t = rol(a, 5) + (func) + e + val + w[round]; \
        e = d;                                                          \
        d = c;                                                          \
        c = rol(b, 30);                                                 \
        b = a;                                                          \
        a = t;                                                          \
    }

    while (round < 16)
    {
        sha1macro((b & c) | (~b & d), 0x5a827999);
        ++round;
    }
    while (round < 20)
    {
        w[round] = rol((w[round - 3] ^ w[round - 8] ^ w[round - 14] ^ w[round - 16]), 1);
        sha1macro((b & c) | (~b & d), 0x5a827999);
        ++round;
    }
    while (round < 40)
    {
        w[round] = rol((w[round - 3] ^ w[round - 8] ^ w[round - 14] ^ w[round - 16]), 1);
        sha1macro(b ^ c ^ d, 0x6ed9eba1);
        ++round;
    }
    while (round < 60)
    {
        w[round] = rol((w[round - 3] ^ w[round - 8] ^ w[round - 14] ^ w[round - 16]), 1);
        sha1macro((b & c) | (b & d) | (c & d), 0x8f1bbcdc);
        ++round;
    }
    while (round < 80)
    {
        w[round] = rol((w[round - 3] ^ w[round - 8] ^ w[round - 14] ^ w[round - 16]), 1);
        sha1macro(b ^ c ^ d, 0xca62c1d6);
        ++round;
    }

#undef sha1macro

    result[0] += a;
    result[1] += b;
    result[2] += c;
    result[3] += d;
    result[4] += e;
}

static void calc(const void* src, const int bytelength, uint8_t* hash)
{
    int roundPos;
    int lastBlockBytes;
    int hashByte;

    // Init the result array.
    uint32_t result[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};

    // Cast the void src pointer to be the byte array we can work with.
    const uint8_t* sarray = (const uint8_t*)src;

    // The reusable round buffer
    uint32_t w[80];

    // Loop through all complete 64byte blocks.
    const int endOfFullBlocks = bytelength - 64;
    int endCurrentBlock;
    int currentBlock = 0;

    while (currentBlock <= endOfFullBlocks)
    {
        endCurrentBlock = currentBlock + 64;

        // Init the round buffer with the 64 byte block data.
        for (roundPos = 0; currentBlock < endCurrentBlock; currentBlock += 4)
        {
            // This line will swap endian on big endian and keep endian on little endian.
            w[roundPos++] = (uint32_t)sarray[currentBlock + 3] | (((uint32_t)sarray[currentBlock + 2]) << 8) |
                            (((uint32_t)sarray[currentBlock + 1]) << 16) |
                            (((uint32_t)sarray[currentBlock]) << 24);
        }
        innerHash(result, w);
    }

    // Handle the last and not full 64 byte block if existing.
    endCurrentBlock = bytelength - currentBlock;
    clearWBuffert(w);
    lastBlockBytes = 0;
    for (; lastBlockBytes < endCurrentBlock; ++lastBlockBytes)
    {
        w[lastBlockBytes >> 2] |= (uint32_t)sarray[lastBlockBytes + currentBlock]
                                  << ((3 - (lastBlockBytes & 3)) << 3);
    }
    w[lastBlockBytes >> 2] |= 0x80U << ((3 - (lastBlockBytes & 3)) << 3);
    if (endCurrentBlock >= 56)
    {
        innerHash(result, w);
        clearWBuffert(w);
    }
    w[15] = bytelength << 3;
    innerHash(result, w);

    // Store hash in result pointer, and make sure we get in in the correct order on both endian models.
    for (hashByte = 20; --hashByte >= 0;)
    {
        hash[hashByte] = (result[hashByte >> 2] >> (((3 - hashByte) & 0x3) << 3)) & 0xff;
    }
}

static SHA1 SHA1_Calculate(const void* src, uint32_t length)
{
    SHA1 hash;
    assert((int)length >= 0);
    calc(src, length, hash.data);
    return hash;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @BASE64: Base-64 encoder
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

static const char* b64_encoding_table = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                        "abcdefghijklmnopqrstuvwxyz"
                                        "0123456789+/";

static uint32_t Base64_CalculateEncodedLength(uint32_t length)
{
    // ceil(l * 4/3)
    return 4 * ((length + 2) / 3);
}

static void Base64_Encode(const uint8_t* in_bytes, uint32_t length, uint8_t* out_bytes)
{
    uint32_t i;
    uint32_t encoded_length;
    uint32_t remaining_bytes;

    uint8_t* optr = out_bytes;

    for (i = 0; i < length;)
    {
        // Read input 3 values at a time, null terminating
        uint32_t c0 = i < length ? in_bytes[i++] : 0;
        uint32_t c1 = i < length ? in_bytes[i++] : 0;
        uint32_t c2 = i < length ? in_bytes[i++] : 0;

        // Encode 4 bytes for ever 3 input bytes
        uint32_t triple = (c0 << 0x10) + (c1 << 0x08) + c2;
        *optr++ = b64_encoding_table[(triple >> 3 * 6) & 0x3F];
        *optr++ = b64_encoding_table[(triple >> 2 * 6) & 0x3F];
        *optr++ = b64_encoding_table[(triple >> 1 * 6) & 0x3F];
        *optr++ = b64_encoding_table[(triple >> 0 * 6) & 0x3F];
    }

    // Pad output to multiple of 3 bytes with terminating '='
    encoded_length = Base64_CalculateEncodedLength(length);
    remaining_bytes = (3 - ((length + 2) % 3)) - 1;
    for (i = 0; i < remaining_bytes; i++)
        out_bytes[encoded_length - 1 - i] = '=';

    // Null terminate
    out_bytes[encoded_length] = 0;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @MURMURHASH: MurmurHash3
   https://code.google.com/p/smhasher
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

//-----------------------------------------------------------------------------
// MurmurHash3 was written by Austin Appleby, and is placed in the public
// domain. The author hereby disclaims copyright to this source code.
//-----------------------------------------------------------------------------

#if RMT_USE_INTERNAL_HASH_FUNCTION

static uint32_t rotl32(uint32_t x, int8_t r)
{
    return (x << r) | (x >> (32 - r));
}

// Block read - if your platform needs to do endian-swapping, do the conversion here
static uint32_t getblock32(const uint32_t* p, int i)
{
    uint32_t result;
    const uint8_t* src = ((const uint8_t*)p) + i * (int)sizeof(uint32_t);
    memcpy(&result, src, sizeof(result));
    return result;
}

// Finalization mix - force all bits of a hash block to avalanche
static uint32_t fmix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

static uint32_t MurmurHash3_x86_32(const void* key, int len, uint32_t seed)
{
    const uint8_t* data = (const uint8_t*)key;
    const int nblocks = len / 4;

    uint32_t h1 = seed;

    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;

    int i;

    const uint32_t* blocks = (const uint32_t*)(data + nblocks * 4);
    const uint8_t* tail = (const uint8_t*)(data + nblocks * 4);

    uint32_t k1 = 0;

    //----------
    // body

    for (i = -nblocks; i; i++)
    {
        uint32_t k2 = getblock32(blocks, i);

        k2 *= c1;
        k2 = rotl32(k2, 15);
        k2 *= c2;

        h1 ^= k2;
        h1 = rotl32(h1, 13);
        h1 = h1 * 5 + 0xe6546b64;
    }

    //----------
    // tail

    switch (len & 3)
    {
        case 3:
            k1 ^= tail[2] << 16; // fallthrough
        case 2:
            k1 ^= tail[1] << 8; // fallthrough
        case 1:
            k1 ^= tail[0];
            k1 *= c1;
            k1 = rotl32(k1, 15);
            k1 *= c2;
            h1 ^= k1;
    };

    //----------
    // finalization

    h1 ^= len;

    h1 = fmix32(h1);

    return h1;
}

RMT_API uint32_t _rmt_HashString32(const char* s, int len, uint32_t seed)
{
    return MurmurHash3_x86_32(s, len, seed);
}

#else
    #if defined(__cplusplus)
    extern "C"
    #endif
    RMT_API uint32_t _rmt_HashString32(const char* s, int len, uint32_t seed);

#endif // RMT_USE_INTERNAL_HASH_FUNCTION

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @WEBSOCKETS: WebSockets
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

enum WebSocketMode
{
    WEBSOCKET_NONE = 0,
    WEBSOCKET_TEXT = 1,
    WEBSOCKET_BINARY = 2,
};

typedef struct
{
    TCPSocket* tcp_socket;

    enum WebSocketMode mode;

    uint32_t frame_bytes_remaining;
    uint32_t mask_offset;

    union {
        uint8_t mask[4];
        uint32_t mask_u32;
    } data;

} WebSocket;

static void WebSocket_Close(WebSocket* web_socket);

static char* GetField(char* buffer, r_size_t buffer_length, rmtPStr field_name)
{
    char* field = NULL;
    char* buffer_end = buffer + buffer_length - 1;

    r_size_t field_length = strnlen_s(field_name, buffer_length);
    if (field_length == 0)
        return NULL;

    // Search for the start of the field
    if (strstr_s(buffer, buffer_length, field_name, field_length, &field) != EOK)
        return NULL;

    // Field name is now guaranteed to be in the buffer so its safe to jump over it without hitting the bounds
    field += strlen(field_name);

    // Skip any trailing whitespace
    while (*field == ' ')
    {
        if (field >= buffer_end)
            return NULL;
        field++;
    }

    return field;
}

static const char websocket_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const char websocket_response[] = "HTTP/1.1 101 Switching Protocols\r\n"
                                         "Upgrade: websocket\r\n"
                                         "Connection: Upgrade\r\n"
                                         "Sec-WebSocket-Accept: ";

static rmtError WebSocketHandshake(TCPSocket* tcp_socket, rmtPStr limit_host)
{
    uint32_t start_ms, now_ms;

    // Parsing scratchpad
    char buffer[1024];
    char* buffer_ptr = buffer;
    int buffer_len = sizeof(buffer) - 1;
    char* buffer_end = buffer + buffer_len;

    char response_buffer[256];
    int response_buffer_len = sizeof(response_buffer) - 1;

    char* version;
    char* host;
    char* key;
    char* key_end;
    SHA1 hash;

    assert(tcp_socket != NULL);

    start_ms = msTimer_Get();

    // Really inefficient way of receiving the handshake data from the browser
    // Not really sure how to do this any better, as the termination requirement is \r\n\r\n
    while (buffer_ptr - buffer < buffer_len)
    {
        rmtError error = TCPSocket_Receive(tcp_socket, buffer_ptr, 1, 20);
        if (error == RMT_ERROR_SOCKET_RECV_FAILED)
            return error;

        // If there's a stall receiving the data, check for a handshake timeout
        if (error == RMT_ERROR_SOCKET_RECV_NO_DATA || error == RMT_ERROR_SOCKET_RECV_TIMEOUT)
        {
            now_ms = msTimer_Get();
            if (now_ms - start_ms > 1000)
                return RMT_ERROR_SOCKET_RECV_TIMEOUT;

            continue;
        }

        // Just in case new enums are added...
        assert(error == RMT_ERROR_NONE);

        if (buffer_ptr - buffer >= 4)
        {
            if (*(buffer_ptr - 3) == '\r' && *(buffer_ptr - 2) == '\n' && *(buffer_ptr - 1) == '\r' &&
                *(buffer_ptr - 0) == '\n')
                break;
        }

        buffer_ptr++;
    }
    *buffer_ptr = 0;

    // HTTP GET instruction
    if (memcmp(buffer, "GET", 3) != 0)
        return RMT_ERROR_WEBSOCKET_HANDSHAKE_NOT_GET;

    // Look for the version number and verify that it's supported
    version = GetField(buffer, buffer_len, "Sec-WebSocket-Version:");
    if (version == NULL)
        return RMT_ERROR_WEBSOCKET_HANDSHAKE_NO_VERSION;
    if (buffer_end - version < 2 || (version[0] != '8' && (version[0] != '1' || version[1] != '3')))
        return RMT_ERROR_WEBSOCKET_HANDSHAKE_BAD_VERSION;

    // Make sure this connection comes from a known host
    host = GetField(buffer, buffer_len, "Host:");
    if (host == NULL)
        return RMT_ERROR_WEBSOCKET_HANDSHAKE_NO_HOST;
    if (limit_host != NULL)
    {
        r_size_t limit_host_len = strnlen_s(limit_host, 128);
        char* found = NULL;
        if (strstr_s(host, (r_size_t)(buffer_end - host), limit_host, limit_host_len, &found) != EOK)
            return RMT_ERROR_WEBSOCKET_HANDSHAKE_BAD_HOST;
    }

    // Look for the key start and null-terminate it within the receive buffer
    key = GetField(buffer, buffer_len, "Sec-WebSocket-Key:");
    if (key == NULL)
        return RMT_ERROR_WEBSOCKET_HANDSHAKE_NO_KEY;
    if (strstr_s(key, (r_size_t)(buffer_end - key), "\r\n", 2, &key_end) != EOK)
        return RMT_ERROR_WEBSOCKET_HANDSHAKE_BAD_KEY;
    *key_end = 0;

    // Concatenate the browser's key with the WebSocket Protocol GUID and base64 encode
    // the hash, to prove to the browser that this is a bonafide WebSocket server
    buffer[0] = 0;
    if (strncat_s(buffer, buffer_len, key, (r_size_t)(key_end - key)) != EOK)
        return RMT_ERROR_WEBSOCKET_HANDSHAKE_STRING_FAIL;
    if (strncat_s(buffer, buffer_len, websocket_guid, sizeof(websocket_guid)) != EOK)
        return RMT_ERROR_WEBSOCKET_HANDSHAKE_STRING_FAIL;
    hash = SHA1_Calculate(buffer, (uint32_t)strnlen_s(buffer, buffer_len));
    Base64_Encode(hash.data, sizeof(hash.data), (uint8_t*)buffer);

    // Send the response back to the server with a longer timeout than usual
    response_buffer[0] = 0;
    if (strncat_s(response_buffer, response_buffer_len, websocket_response, sizeof(websocket_response)) != EOK)
        return RMT_ERROR_WEBSOCKET_HANDSHAKE_STRING_FAIL;
    if (strncat_s(response_buffer, response_buffer_len, buffer, buffer_len) != EOK)
        return RMT_ERROR_WEBSOCKET_HANDSHAKE_STRING_FAIL;
    if (strncat_s(response_buffer, response_buffer_len, "\r\n\r\n", 4) != EOK)
        return RMT_ERROR_WEBSOCKET_HANDSHAKE_STRING_FAIL;

    return TCPSocket_Send(tcp_socket, response_buffer, (uint32_t)strnlen_s(response_buffer, response_buffer_len), 1000);
}

static rmtError WebSocket_Constructor(WebSocket* web_socket, TCPSocket* tcp_socket)
{
    assert(web_socket != NULL);
    web_socket->tcp_socket = tcp_socket;
    web_socket->mode = WEBSOCKET_NONE;
    web_socket->frame_bytes_remaining = 0;
    web_socket->mask_offset = 0;
    web_socket->data.mask[0] = 0;
    web_socket->data.mask[1] = 0;
    web_socket->data.mask[2] = 0;
    web_socket->data.mask[3] = 0;

    // Caller can optionally specify which TCP socket to use
    if (web_socket->tcp_socket == NULL)
        rmtTryNew(TCPSocket, web_socket->tcp_socket);

    return RMT_ERROR_NONE;
}

static void WebSocket_Destructor(WebSocket* web_socket)
{
    WebSocket_Close(web_socket);
}

static rmtError WebSocket_RunServer(WebSocket* web_socket, uint16_t port, rmtBool reuse_open_port,
                                    rmtBool limit_connections_to_localhost, enum WebSocketMode mode)
{
    // Create the server's listening socket
    assert(web_socket != NULL);
    web_socket->mode = mode;
    return TCPSocket_RunServer(web_socket->tcp_socket, port, reuse_open_port, limit_connections_to_localhost);
}

static void WebSocket_Close(WebSocket* web_socket)
{
    assert(web_socket != NULL);
    rmtDelete(TCPSocket, web_socket->tcp_socket);
}

static SocketStatus WebSocket_PollStatus(WebSocket* web_socket)
{
    assert(web_socket != NULL);
    return TCPSocket_PollStatus(web_socket->tcp_socket);
}

static rmtError WebSocket_AcceptConnection(WebSocket* web_socket, WebSocket** client_socket)
{
    TCPSocket* tcp_socket = NULL;

    // Is there a waiting connection?
    assert(web_socket != NULL);
    rmtTry(TCPSocket_AcceptConnection(web_socket->tcp_socket, &tcp_socket));
    if (tcp_socket == NULL)
        return RMT_ERROR_NONE;

    // Need a successful handshake between client/server before allowing the connection
    // TODO: Specify limit_host
    rmtTry(WebSocketHandshake(tcp_socket, NULL));

    // Allocate and return a new client socket
    assert(client_socket != NULL);
    rmtTryNew(WebSocket, *client_socket, tcp_socket);

    (*client_socket)->mode = web_socket->mode;

    return RMT_ERROR_NONE;
}

static void WriteSize(uint32_t size, uint8_t* dest, uint32_t dest_size, uint32_t dest_offset)
{
    int size_size = dest_size - dest_offset;
    uint32_t i;
    for (i = 0; i < dest_size; i++)
    {
        int j = i - dest_offset;
        dest[i] = (j < 0) ? 0 : (size >> ((size_size - j - 1) * 8)) & 0xFF;
    }
}

// For send buffers to preallocate
#define WEBSOCKET_MAX_FRAME_HEADER_SIZE 10

static void WebSocket_PrepareBuffer(Buffer* buffer)
{
    char empty_frame_header[WEBSOCKET_MAX_FRAME_HEADER_SIZE] = { 0 };

    assert(buffer != NULL);

    // Reset to start
    buffer->bytes_used = 0;

    // Allocate enough space for a maximum-sized frame header
    Buffer_Write(buffer, empty_frame_header, sizeof(empty_frame_header));
}

static uint32_t WebSocket_FrameHeaderSize(uint32_t length)
{
    if (length <= 125)
        return 2;
    if (length <= 65535)
        return 4;
    return 10;
}

static void WebSocket_WriteFrameHeader(WebSocket* web_socket, uint8_t* dest, uint32_t length)
{
    uint8_t final_fragment = 0x1 << 7;
    uint8_t frame_type = (uint8_t)web_socket->mode;

    dest[0] = final_fragment | frame_type;

    // Construct the frame header, correctly applying the narrowest size
    if (length <= 125)
    {
        dest[1] = (uint8_t)length;
    }
    else if (length <= 65535)
    {
        dest[1] = 126;
        WriteSize(length, dest + 2, 2, 0);
    }
    else
    {
        dest[1] = 127;
        WriteSize(length, dest + 2, 8, 4);
    }
}

static rmtError WebSocket_Send(WebSocket* web_socket, const void* data, uint32_t length, uint32_t timeout_ms)
{
    rmtError error;
    SocketStatus status;
    uint32_t payload_length, frame_header_size, delta;

    assert(web_socket != NULL);
    assert(data != NULL);

    // Can't send if there are socket errors
    status = WebSocket_PollStatus(web_socket);
    if (status.error_state != RMT_ERROR_NONE)
        return status.error_state;

    // Assume space for max frame header has been allocated in the incoming data
    payload_length = length - WEBSOCKET_MAX_FRAME_HEADER_SIZE;
    frame_header_size = WebSocket_FrameHeaderSize(payload_length);
    delta = WEBSOCKET_MAX_FRAME_HEADER_SIZE - frame_header_size;
    data = (void*)((uint8_t*)data + delta);
    length -= delta;
    WebSocket_WriteFrameHeader(web_socket, (uint8_t*)data, payload_length);

    // Send frame header and data together
    error = TCPSocket_Send(web_socket->tcp_socket, data, length, timeout_ms);
    return error;
}

static rmtError ReceiveFrameHeader(WebSocket* web_socket)
{
    // TODO: Specify infinite timeout?

    uint8_t msg_header[2] = {0, 0};
    int msg_length, size_bytes_remaining, i;
    rmtBool mask_present;

    assert(web_socket != NULL);

    // Get message header
    rmtTry(TCPSocket_Receive(web_socket->tcp_socket, msg_header, 2, 20));

    // Check for WebSocket Protocol disconnect
    if (msg_header[0] == 0x88)
        return RMT_ERROR_WEBSOCKET_DISCONNECTED;

    // Check that the client isn't sending messages we don't understand
    if (msg_header[0] != 0x81 && msg_header[0] != 0x82)
        return RMT_ERROR_WEBSOCKET_BAD_FRAME_HEADER;

    // Get message length and check to see if it's a marker for a wider length
    msg_length = msg_header[1] & 0x7F;
    size_bytes_remaining = 0;
    switch (msg_length)
    {
        case 126:
            size_bytes_remaining = 2;
            break;
        case 127:
            size_bytes_remaining = 8;
            break;
    }

    if (size_bytes_remaining > 0)
    {
        // Receive the wider bytes of the length
        uint8_t size_bytes[8];
        rmtTry(TCPSocket_Receive(web_socket->tcp_socket, size_bytes, size_bytes_remaining, 20));

        // Calculate new length, MSB first
        msg_length = 0;
        for (i = 0; i < size_bytes_remaining; i++)
            msg_length |= size_bytes[i] << ((size_bytes_remaining - 1 - i) * 8);
    }

    // Receive any message data masks
    mask_present = (msg_header[1] & 0x80) != 0 ? RMT_TRUE : RMT_FALSE;
    if (mask_present)
    {
        rmtTry(TCPSocket_Receive(web_socket->tcp_socket, web_socket->data.mask, 4, 20));
    }

    web_socket->frame_bytes_remaining = msg_length;
    web_socket->mask_offset = 0;

    return RMT_ERROR_NONE;
}

static rmtError WebSocket_Receive(WebSocket* web_socket, void* data, uint32_t* msg_len, uint32_t length, uint32_t timeout_ms)
{
    SocketStatus status;
    char* cur_data;
    char* end_data;
    uint32_t start_ms;
    uint32_t now_ms;
    uint32_t bytes_to_read;

    assert(web_socket != NULL);

    // Can't read with any socket errors
    status = WebSocket_PollStatus(web_socket);
    if (status.error_state != RMT_ERROR_NONE)
    {
        return status.error_state;
    }

    cur_data = (char*)data;
    end_data = cur_data + length;

    start_ms = msTimer_Get();
    while (cur_data < end_data)
    {
        // Get next WebSocket frame if we've run out of data to read from the socket
        if (web_socket->frame_bytes_remaining == 0)
        {
            rmtTry(ReceiveFrameHeader(web_socket));

            // Set output message length only on initial receive
            if (msg_len != NULL)
            {
                *msg_len = web_socket->frame_bytes_remaining;
            }
        }

        {
            rmtError error;

            // Read as much required data as possible
            bytes_to_read = web_socket->frame_bytes_remaining < length ? web_socket->frame_bytes_remaining : length;
            error = TCPSocket_Receive(web_socket->tcp_socket, cur_data, bytes_to_read, 20);
            if (error == RMT_ERROR_SOCKET_RECV_FAILED)
            {
                return error;
            }

            // If there's a stall receiving the data, check for timeout
            if (error == RMT_ERROR_SOCKET_RECV_NO_DATA || error == RMT_ERROR_SOCKET_RECV_TIMEOUT)
            {
                now_ms = msTimer_Get();
                if (now_ms - start_ms > timeout_ms)
                {
                    return RMT_ERROR_SOCKET_RECV_TIMEOUT;
                }
                continue;
            }
        }

        // Apply data mask
        if (web_socket->data.mask_u32 != 0)
        {
            uint32_t i;
            for (i = 0; i < bytes_to_read; i++)
            {
                *((uint8_t*)cur_data + i) ^= web_socket->data.mask[web_socket->mask_offset & 3];
                web_socket->mask_offset++;
            }
        }

        cur_data += bytes_to_read;
        web_socket->frame_bytes_remaining -= bytes_to_read;
    }

    return RMT_ERROR_NONE;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @MESSAGEQ: Multiple producer, single consumer message queue
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

typedef enum MessageID
{
    MsgID_NotReady,
    MsgID_AddToStringTable,
    MsgID_LogText,
    MsgID_SampleTree,
    MsgID_ProcessorThreads,
    MsgID_None,
    MsgID_PropertySnapshot,
    MsgID_Force32Bits = 0xFFFFFFFF,
} MessageID;

typedef struct Message
{
    MessageID id;

    uint32_t payload_size;

    // For telling which thread the message came from in the debugger
    struct ThreadProfiler* threadProfiler;

    uint8_t payload[1];
} Message;

// Multiple producer, single consumer message queue that uses its own data buffer
// to store the message data.
typedef struct rmtMessageQueue
{
    uint32_t size;

    // The physical address of this data buffer is pointed to by two sequential
    // virtual memory pages, allowing automatic wrap-around of any reads or writes
    // that exceed the limits of the buffer.
    VirtualMirrorBuffer* data;

    // Read/write position never wrap allowing trivial overflow checks
    // with easier debugging
    rmtAtomicU32 read_pos;
    rmtAtomicU32 write_pos;

} rmtMessageQueue;

static rmtError rmtMessageQueue_Constructor(rmtMessageQueue* queue, uint32_t size)
{
    assert(queue != NULL);

    // Set defaults
    queue->size = 0;
    queue->data = NULL;
    queue->read_pos = 0;
    queue->write_pos = 0;

    rmtTryNew(VirtualMirrorBuffer, queue->data, size, 10);

    // The mirror buffer needs to be page-aligned and will change the requested
    // size to match that.
    queue->size = queue->data->size;

    // Set the entire buffer to not ready message
    memset(queue->data->ptr, MsgID_NotReady, queue->size);

    return RMT_ERROR_NONE;
}

static void rmtMessageQueue_Destructor(rmtMessageQueue* queue)
{
    assert(queue != NULL);
    rmtDelete(VirtualMirrorBuffer, queue->data);
}

static uint32_t rmtMessageQueue_SizeForPayload(uint32_t payload_size)
{
    // Add message header and align for ARM platforms
    uint32_t size = sizeof(Message) + payload_size;
#if defined(RMT_ARCH_64BIT)
    size = (size + 7) & ~7U;
#else
    size = (size + 3) & ~3U;
#endif
    return size;
}

static Message* rmtMessageQueue_AllocMessage(rmtMessageQueue* queue, uint32_t payload_size,
                                             struct ThreadProfiler* thread_profiler)
{
    Message* msg;

    uint32_t write_size = rmtMessageQueue_SizeForPayload(payload_size);

    assert(queue != NULL);

    for (;;)
    {
        // Check for potential overflow
        // Order of loads means allocation failure can happen when enough space has just been freed
        // However, incorrect overflows are not possible
        uint32_t s = queue->size;
        uint32_t w = LoadAcquire(&queue->write_pos);
        uint32_t r = LoadAcquire(&queue->read_pos);
        if ((int)(w - r) > ((int)(s - write_size)))
            return NULL;

        // Point to the newly allocated space
        msg = (Message*)(queue->data->ptr + (w & (s - 1)));

        // Increment the write position, leaving the loop if this is the thread that succeeded
        if (AtomicCompareAndSwapU32(&queue->write_pos, w, w + write_size) == RMT_TRUE)
        {
            // Safe to set payload size after thread claims ownership of this allocated range
            msg->payload_size = payload_size;
            msg->threadProfiler = thread_profiler;
            break;
        }
    }

    return msg;
}

static void rmtMessageQueue_CommitMessage(Message* message, MessageID id)
{
    MessageID r;
    assert(message != NULL);

    // Setting the message ID signals to the consumer that the message is ready
    r = (MessageID)LoadAcquire((rmtAtomicU32*)&message->id);
    RMT_UNREFERENCED_PARAMETER(r);
    assert(r == MsgID_NotReady);
    StoreRelease((rmtAtomicU32*)&message->id, id);
}

Message* rmtMessageQueue_PeekNextMessage(rmtMessageQueue* queue)
{
    Message* ptr;
    uint32_t r, w;
    MessageID id;

    assert(queue != NULL);

    // First check that there are bytes queued
    w = LoadAcquire(&queue->write_pos);
    r = queue->read_pos;
    if (w - r == 0)
        return NULL;

    // Messages are in the queue but may not have been commit yet
    // Messages behind this one may have been commit but it's not reachable until
    // the next one in the queue is ready.
    r = r & (queue->size - 1);
    ptr = (Message*)(queue->data->ptr + r);
    id = (MessageID)LoadAcquire((rmtAtomicU32*)&ptr->id);
    if (id != MsgID_NotReady)
        return ptr;

    return NULL;
}

static void rmtMessageQueue_ConsumeNextMessage(rmtMessageQueue* queue, Message* message)
{
    uint32_t message_size, read_pos;

    assert(queue != NULL);
    assert(message != NULL);

    // Setting the message ID to "not ready" serves as a marker to the consumer that even though
    // space has been allocated for a message, the message isn't ready to be consumed
    // yet.
    //
    // We can't do that when allocating the message because multiple threads will be fighting for
    // the same location. Instead, clear out any messages just read by the consumer before advancing
    // the read position so that a winning thread's allocation will inherit the "not ready" state.
    //
    // This costs some write bandwidth and has the potential to flush cache to other cores.
    message_size = rmtMessageQueue_SizeForPayload(message->payload_size);
    memset(message, MsgID_NotReady, message_size);

    // Advance read position
    read_pos = queue->read_pos + message_size;
    StoreRelease(&queue->read_pos, read_pos);
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @NETWORK: Network Server
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

typedef rmtError (*Server_ReceiveHandler)(void*, char*, uint32_t);

typedef struct
{
    WebSocket* listen_socket;

    WebSocket* client_socket;

    uint32_t last_ping_time;

    uint16_t port;

    rmtBool reuse_open_port;
    rmtBool limit_connections_to_localhost;

    // A dynamically-sized buffer used for binary-encoding messages and sending to the client
    Buffer* bin_buf;

    // Handler for receiving messages from the client
    Server_ReceiveHandler receive_handler;
    void* receive_handler_context;
} Server;

static rmtError Server_CreateListenSocket(Server* server, uint16_t port, rmtBool reuse_open_port,
                                          rmtBool limit_connections_to_localhost)
{
    rmtTryNew(WebSocket, server->listen_socket, NULL);
    rmtTry(WebSocket_RunServer(server->listen_socket, port, reuse_open_port, limit_connections_to_localhost, WEBSOCKET_BINARY));
    return RMT_ERROR_NONE;
}

static rmtError Server_Constructor(Server* server, uint16_t port, rmtBool reuse_open_port,
                                   rmtBool limit_connections_to_localhost)
{
    assert(server != NULL);
    server->listen_socket = NULL;
    server->client_socket = NULL;
    server->last_ping_time = 0;
    server->port = port;
    server->reuse_open_port = reuse_open_port;
    server->limit_connections_to_localhost = limit_connections_to_localhost;
    server->bin_buf = NULL;
    server->receive_handler = NULL;
    server->receive_handler_context = NULL;

    // Create the binary serialisation buffer
    rmtTryNew(Buffer, server->bin_buf, 4096);

    // Create the listening WebSocket
    return Server_CreateListenSocket(server, port, reuse_open_port, limit_connections_to_localhost);
}

static void Server_Destructor(Server* server)
{
    assert(server != NULL);
    rmtDelete(WebSocket, server->client_socket);
    rmtDelete(WebSocket, server->listen_socket);
    rmtDelete(Buffer, server->bin_buf);
}

static rmtBool Server_IsClientConnected(Server* server)
{
    assert(server != NULL);
    return server->client_socket != NULL ? RMT_TRUE : RMT_FALSE;
}

static void Server_DisconnectClient(Server* server)
{
    WebSocket* client_socket;

    assert(server != NULL);

    // NULL the variable before destroying the socket
    client_socket = server->client_socket;
    server->client_socket = NULL;
    CompilerWriteFence();
    rmtDelete(WebSocket, client_socket);
}

static rmtError Server_Send(Server* server, const void* data, uint32_t length, uint32_t timeout)
{
    assert(server != NULL);
    if (Server_IsClientConnected(server))
    {
        rmtError error = WebSocket_Send(server->client_socket, data, length, timeout);
        if (error == RMT_ERROR_SOCKET_SEND_FAIL)
            Server_DisconnectClient(server);

        return error;
    }

    return RMT_ERROR_NONE;
}

static rmtError Server_ReceiveMessage(Server* server, char message_first_byte, uint32_t message_length)
{
    char message_data[1024];

    // Check for potential message data overflow
    if (message_length >= sizeof(message_data) - 1)
    {
        rmt_LogText("Ignoring console input bigger than internal receive buffer (1024 bytes)");
        return RMT_ERROR_NONE;
    }

    // Receive the rest of the message
    message_data[0] = message_first_byte;
    rmtTry(WebSocket_Receive(server->client_socket, message_data + 1, NULL, message_length - 1, 100));
    message_data[message_length] = 0;

    // Each message must have a descriptive 4 byte header
    if (message_length < 4)
        return RMT_ERROR_NONE;

    // Dispatch to handler
    if (server->receive_handler)
        rmtTry(server->receive_handler(server->receive_handler_context, message_data, message_length));

    return RMT_ERROR_NONE;
}

static rmtError bin_MessageHeader(Buffer* buffer, const char* id, uint32_t* out_write_start_offset)
{
    // Record where the header starts before writing it
    *out_write_start_offset = buffer->bytes_used;
    rmtTry(Buffer_Write(buffer, (void*)id, 4));
    rmtTry(Buffer_Write(buffer, (void*)"    ", 4));
    return RMT_ERROR_NONE;
}

static rmtError bin_MessageFooter(Buffer* buffer, uint32_t write_start_offset)
{
    // Align message size to 32-bits so that the viewer can alias float arrays within log files
    rmtTry(Buffer_AlignedPad(buffer, write_start_offset));

    // Patch message size, including padding at the end
    U32ToByteArray(buffer->data + write_start_offset + 4, (buffer->bytes_used - write_start_offset));

    return RMT_ERROR_NONE;
}

static void Server_Update(Server* server)
{
    uint32_t cur_time;

    assert(server != NULL);

    // Recreate the listening socket if it's been destroyed earlier
    if (server->listen_socket == NULL)
        Server_CreateListenSocket(server, server->port, server->reuse_open_port,
                                  server->limit_connections_to_localhost);

    if (server->listen_socket != NULL && server->client_socket == NULL)
    {
        // Accept connections as long as there is no client connected
        WebSocket* client_socket = NULL;
        rmtError error = WebSocket_AcceptConnection(server->listen_socket, &client_socket);
        if (error == RMT_ERROR_NONE)
        {
            server->client_socket = client_socket;
        }
        else
        {
            // Destroy the listen socket on failure to accept
            // It will get recreated in another update
            rmtDelete(WebSocket, server->listen_socket);
        }
    }

    else
    {
        // Loop checking for incoming messages
        for (;;)
        {
            // Inspect first byte to see if a message is there
            char message_first_byte;
            uint32_t message_length;
            rmtError error = WebSocket_Receive(server->client_socket, &message_first_byte, &message_length, 1, 0);
            if (error == RMT_ERROR_NONE)
            {
                // Parse remaining message
                error = Server_ReceiveMessage(server, message_first_byte, message_length);
                if (error != RMT_ERROR_NONE)
                {
                    Server_DisconnectClient(server);
                    break;
                }

                // Check for more...
                continue;
            }

            // Passable errors...
            if (error == RMT_ERROR_SOCKET_RECV_NO_DATA)
            {
                // No data available
                break;
            }

            if (error == RMT_ERROR_SOCKET_RECV_TIMEOUT)
            {
                // Data not available yet, can afford to ignore as we're only reading the first byte
                break;
            }

            // Anything else is an error that may have closed the connection
            Server_DisconnectClient(server);
            break;
        }
    }

    // Send pings to the client every second
    cur_time = msTimer_Get();
    if (cur_time - server->last_ping_time > 1000)
    {
        Buffer* bin_buf = server->bin_buf;
        uint32_t write_start_offset;
        WebSocket_PrepareBuffer(bin_buf);
        bin_MessageHeader(bin_buf, "PING", &write_start_offset);
        bin_MessageFooter(bin_buf, write_start_offset);
        Server_Send(server, bin_buf->data, bin_buf->bytes_used, 10);
        server->last_ping_time = cur_time;
    }
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @SAMPLE: Base Sample Description for CPU by default
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

#define SAMPLE_NAME_LEN 128

typedef struct Sample
{
    // Inherit so that samples can be quickly allocated
    ObjectLink Link;

    enum rmtSampleType type;

    // Hash generated from sample name
    uint32_t name_hash;

    // Unique, persistent ID among all samples
    uint32_t unique_id;

    // RGB8 unique colour generated from the unique ID
    uint8_t uniqueColour[3];

    // Links to related samples in the tree
    struct Sample* parent;
    struct Sample* first_child;
    struct Sample* last_child;
    struct Sample* next_sibling;

    // Keep track of child count to distinguish from repeated calls to the same function at the same stack level
    // This is also mixed with the callstack hash to allow consistent addressing of any point in the tree
    uint32_t nb_children;

    // Sample end points and length in microseconds
    uint64_t us_start;
    uint64_t us_end;
    uint64_t us_length;

    // Total sampled length of all children
    uint64_t us_sampled_length;

    // If this is a GPU sample, when the sample was issued on the GPU
    uint64_t usGpuIssueOnCpu;

    // Number of times this sample was used in a call in aggregate mode, 1 otherwise
    uint32_t call_count;

    // Current and maximum sample recursion depths
    uint16_t recurse_depth;
    uint16_t max_recurse_depth;

} Sample;

static rmtError Sample_Constructor(Sample* sample)
{
    assert(sample != NULL);

    ObjectLink_Constructor((ObjectLink*)sample);

    sample->type = RMT_SampleType_CPU;
    sample->name_hash = 0;
    sample->unique_id = 0;
    sample->uniqueColour[0] = 0;
    sample->uniqueColour[1] = 0;
    sample->uniqueColour[2] = 0;
    sample->parent = NULL;
    sample->first_child = NULL;
    sample->last_child = NULL;
    sample->next_sibling = NULL;
    sample->nb_children = 0;
    sample->us_start = 0;
    sample->us_end = 0;
    sample->us_length = 0;
    sample->us_sampled_length = 0;
    sample->usGpuIssueOnCpu = 0;
    sample->call_count = 0;
    sample->recurse_depth = 0;
    sample->max_recurse_depth = 0;

    return RMT_ERROR_NONE;
}

static void Sample_Destructor(Sample* sample)
{
    RMT_UNREFERENCED_PARAMETER(sample);
}

static void Sample_Prepare(Sample* sample, uint32_t name_hash, Sample* parent)
{
    sample->name_hash = name_hash;
    sample->unique_id = 0;
    sample->parent = parent;
    sample->first_child = NULL;
    sample->last_child = NULL;
    sample->next_sibling = NULL;
    sample->nb_children = 0;
    sample->us_start = 0;
    sample->us_end = 0;
    sample->us_length = 0;
    sample->us_sampled_length = 0;
    sample->usGpuIssueOnCpu = 0;
    sample->call_count = 1;
    sample->recurse_depth = 0;
    sample->max_recurse_depth = 0;
}

static void Sample_Close(Sample* sample, int64_t us_end)
{
    // Aggregate samples use us_end to store start so that us_start is preserved
    int64_t us_length = 0;
    if (sample->call_count > 1 && sample->max_recurse_depth == 0)
    {
        us_length = maxS64(us_end - sample->us_end, 0);
    }
    else
    {
        us_length = maxS64(us_end - sample->us_start, 0);
    }

    sample->us_length += us_length;

    // Sum length on the parent to track un-sampled time in the parent
    if (sample->parent != NULL)
    {
        sample->parent->us_sampled_length += us_length;
    }
}

static void Sample_CopyState(Sample* dst_sample, const Sample* src_sample)
{
    // Copy fields that don't override destination allocator links or transfer source sample tree positioning
    // Also ignoring uniqueColour as that's calculated in the Remotery thread
    dst_sample->type = src_sample->type;
    dst_sample->name_hash = src_sample->name_hash;
    dst_sample->unique_id = src_sample->unique_id;
    dst_sample->nb_children = src_sample->nb_children;
    dst_sample->us_start = src_sample->us_start;
    dst_sample->us_end = src_sample->us_end;
    dst_sample->us_length = src_sample->us_length;
    dst_sample->us_sampled_length = src_sample->us_sampled_length;
    dst_sample->usGpuIssueOnCpu = src_sample->usGpuIssueOnCpu;
    dst_sample->call_count = src_sample->call_count;
    dst_sample->recurse_depth = src_sample->recurse_depth;
    dst_sample->max_recurse_depth = src_sample->max_recurse_depth;

    // Prepare empty tree links
    dst_sample->parent = NULL;
    dst_sample->first_child = NULL;
    dst_sample->last_child = NULL;
    dst_sample->next_sibling = NULL;
}

static rmtError bin_SampleArray(Buffer* buffer, Sample* parent_sample, uint8_t depth);

static rmtError bin_Sample(Buffer* buffer, Sample* sample, uint8_t depth)
{
    assert(sample != NULL);

    rmtTry(Buffer_WriteU32(buffer, sample->name_hash));
    rmtTry(Buffer_WriteU32(buffer, sample->unique_id));
    rmtTry(Buffer_Write(buffer, sample->uniqueColour, 3));
    rmtTry(Buffer_Write(buffer, &depth, 1));
    rmtTry(Buffer_WriteU64(buffer, sample->us_start));
    rmtTry(Buffer_WriteU64(buffer, sample->us_length));
    rmtTry(Buffer_WriteU64(buffer, maxS64(sample->us_length - sample->us_sampled_length, 0)));
    rmtTry(Buffer_WriteU64(buffer, sample->usGpuIssueOnCpu));
    rmtTry(Buffer_WriteU32(buffer, sample->call_count));
    rmtTry(Buffer_WriteU32(buffer, sample->max_recurse_depth));
    rmtTry(bin_SampleArray(buffer, sample, depth + 1));

    return RMT_ERROR_NONE;
}

static rmtError bin_SampleArray(Buffer* buffer, Sample* parent_sample, uint8_t depth)
{
    Sample* sample;

    rmtTry(Buffer_WriteU32(buffer, parent_sample->nb_children));
    for (sample = parent_sample->first_child; sample != NULL; sample = sample->next_sibling)
        rmtTry(bin_Sample(buffer, sample, depth));

    return RMT_ERROR_NONE;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @SAMPLETREE: A tree of samples with their allocator
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

typedef struct SampleTree
{
    // Allocator for all samples
    ObjectAllocator* allocator;

    // Root sample for all samples created by this thread
    Sample* root;

    // Most recently pushed sample
    Sample* currentParent;

    // Last time this sample tree was completed and sent to listeners, for stall detection
    rmtAtomicU32 msLastTreeSendTime;

    // Lightweight flag, changed with release/acquire semantics to inform the stall detector the state of the tree is unreliable
    rmtAtomicU32 treeBeingModified;

    // Send this popped sample to the log/viewer on close?
    Sample* sendSampleOnClose;

} SampleTree;

// Notify tree watchers that its structure is in the process of being changed
#define ModifySampleTree(tree, statements)      \
    StoreRelease(&tree->treeBeingModified, 1);  \
    statements;                                 \
    StoreRelease(&tree->treeBeingModified, 0);

static rmtError SampleTree_Constructor(SampleTree* tree, uint32_t sample_size, ObjConstructor constructor,
                                       ObjDestructor destructor)
{
    assert(tree != NULL);

    tree->allocator = NULL;
    tree->root = NULL;
    tree->currentParent = NULL;
    StoreRelease(&tree->msLastTreeSendTime, 0);
    StoreRelease(&tree->treeBeingModified, 0);
    tree->sendSampleOnClose = NULL;

    // Create the sample allocator
    rmtTryNew(ObjectAllocator, tree->allocator, sample_size, constructor, destructor);

    // Create a root sample that's around for the lifetime of the thread
    rmtTry(ObjectAllocator_Alloc(tree->allocator, (void**)&tree->root));
    Sample_Prepare(tree->root, 0, NULL);
    tree->currentParent = tree->root;

    return RMT_ERROR_NONE;
}

static void SampleTree_Destructor(SampleTree* tree)
{
    assert(tree != NULL);

    if (tree->root != NULL)
    {
        ObjectAllocator_Free(tree->allocator, tree->root);
        tree->root = NULL;
    }

    rmtDelete(ObjectAllocator, tree->allocator);
}

static uint32_t HashCombine(uint32_t hash_a, uint32_t hash_b)
{
    // A sequence of 32 uniformly random bits so that each bit of the combined hash is changed on application
    // Derived from the golden ratio: UINT_MAX / ((1 + sqrt(5)) / 2)
    // In reality it's just an arbitrary value which happens to work well, avoiding mapping all zeros to zeros.
    // http://burtleburtle.net/bob/hash/doobs.html
    static uint32_t random_bits = 0x9E3779B9;
    hash_a ^= hash_b + random_bits + (hash_a << 6) + (hash_a >> 2);
    return hash_a;
}

static rmtError SampleTree_Push(SampleTree* tree, uint32_t name_hash, uint32_t flags, Sample** sample)
{
    Sample* parent;
    uint32_t unique_id;

    // As each tree has a root sample node allocated, a parent must always be present
    assert(tree != NULL);
    assert(tree->currentParent != NULL);
    parent = tree->currentParent;

    // Assume no flags is the common case and predicate branch checks
    if (flags != 0)
    {
        // Check root status
        if ((flags & RMTSF_Root) != 0)
        {
            assert(parent->parent == NULL);
        }

        if ((flags & RMTSF_Aggregate) != 0)
        {
            // Linear search for previous instance of this sample name
            Sample* sibling;
            for (sibling = parent->first_child; sibling != NULL; sibling = sibling->next_sibling)
            {
                if (sibling->name_hash == name_hash)
                {
                    tree->currentParent = sibling;
                    sibling->call_count++;
                    *sample = sibling;
                    return RMT_ERROR_NONE;
                }
            }
        }

        // Collapse sample on recursion
        if ((flags & RMTSF_Recursive) != 0 && parent->name_hash == name_hash)
        {
            parent->recurse_depth++;
            parent->max_recurse_depth = maxU16(parent->max_recurse_depth, parent->recurse_depth);
            parent->call_count++;
            *sample = parent;
            return RMT_ERROR_RECURSIVE_SAMPLE;
        }

        // Allocate a new sample for subsequent flag checks to reference
        rmtTry(ObjectAllocator_Alloc(tree->allocator, (void**)sample));
        Sample_Prepare(*sample, name_hash, parent);

        // Check for sending this sample on close
        if ((flags & RMTSF_SendOnClose) != 0)
        {
            assert(tree->currentParent != NULL);
            assert(tree->sendSampleOnClose == NULL);
            tree->sendSampleOnClose = *sample;
        }
    }

    else
    {
        // Allocate a new sample
        rmtTry(ObjectAllocator_Alloc(tree->allocator, (void**)sample));
        Sample_Prepare(*sample, name_hash, parent);
    }

    // Generate a unique ID for this sample in the tree
    unique_id = parent->unique_id;
    unique_id = HashCombine(unique_id, (*sample)->name_hash);
    unique_id = HashCombine(unique_id, parent->nb_children);
    (*sample)->unique_id = unique_id;

    // Add sample to its parent
    parent->nb_children++;
    if (parent->first_child == NULL)
    {
        parent->first_child = *sample;
        parent->last_child = *sample;
    }
    else
    {
        assert(parent->last_child != NULL);
        parent->last_child->next_sibling = *sample;
        parent->last_child = *sample;
    }

    // Make this sample the new parent of any newly created samples
    tree->currentParent = *sample;

    return RMT_ERROR_NONE;
}

static void SampleTree_Pop(SampleTree* tree, Sample* sample)
{
    assert(tree != NULL);
    assert(sample != NULL);
    assert(sample != tree->root);
    tree->currentParent = sample->parent;
}

static ObjectLink* FlattenSamples(Sample* sample, uint32_t* nb_samples)
{
    Sample* child;
    ObjectLink* cur_link = &sample->Link;

    assert(sample != NULL);
    assert(nb_samples != NULL);

    *nb_samples += 1;
    sample->Link.next = (ObjectLink*)sample->first_child;

    // Link all children together
    for (child = sample->first_child; child != NULL; child = child->next_sibling)
    {
        ObjectLink* last_link = FlattenSamples(child, nb_samples);
        last_link->next = (ObjectLink*)child->next_sibling;
        cur_link = last_link;
    }

    // Clear child info
    sample->first_child = NULL;
    sample->last_child = NULL;
    sample->nb_children = 0;

    return cur_link;
}

static void FreeSamples(Sample* sample, ObjectAllocator* allocator)
{
    // Chain all samples together in a flat list
    uint32_t nb_cleared_samples = 0;
    ObjectLink* last_link = FlattenSamples(sample, &nb_cleared_samples);

    // Release the complete sample memory range
    if (sample->Link.next != NULL)
    {
        ObjectAllocator_FreeRange(allocator, sample, last_link, nb_cleared_samples);
    }
    else
    {
        ObjectAllocator_Free(allocator, sample);
    }
}

static rmtError SampleTree_CopySample(Sample** out_dst_sample, Sample* dst_parent_sample, ObjectAllocator* allocator, const Sample* src_sample)
{
    Sample* src_child;

    // Allocate a copy of the sample
    Sample* dst_sample;
    rmtTry(ObjectAllocator_Alloc(allocator, (void**)&dst_sample));
    Sample_CopyState(dst_sample, src_sample);

    // Link the newly created/copied sample to its parent
    // Note that metrics including nb_children have already been copied by the Sample_CopyState call
    if (dst_parent_sample != NULL)
    {
        if (dst_parent_sample->first_child == NULL)
        {
            dst_parent_sample->first_child = dst_sample;
            dst_parent_sample->last_child = dst_sample;
        }
        else
        {
            assert(dst_parent_sample->last_child != NULL);
            dst_parent_sample->last_child->next_sibling = dst_sample;
            dst_parent_sample->last_child = dst_sample;
        }
    }

    // Copy all children
    for (src_child = src_sample->first_child; src_child != NULL; src_child = src_child->next_sibling)
    {
        Sample* dst_child;
        rmtTry(SampleTree_CopySample(&dst_child, dst_sample, allocator, src_child));
    }

    *out_dst_sample = dst_sample;

    return RMT_ERROR_NONE;
}

static rmtError SampleTree_Copy(SampleTree* dst_tree, const SampleTree* src_tree)
{
    // Sample trees are allocated at startup and their allocators are persistent for the lifetime of the Remotery object.
    // It's safe to reference the allocator and use it for sample lifetime.
    ObjectAllocator* allocator = src_tree->allocator;
    dst_tree->allocator = allocator;

    // Copy from the root
    rmtTry(SampleTree_CopySample(&dst_tree->root, NULL, allocator, src_tree->root));
    dst_tree->currentParent = dst_tree->root;

    return RMT_ERROR_NONE;
}

typedef struct Msg_SampleTree
{
    Sample* rootSample;

    ObjectAllocator* allocator;

    rmtPStr threadName;

    // Data specific to the sample tree that downstream users can inspect/use
    uint32_t userData;

    rmtBool partialTree;
} Msg_SampleTree;

static void QueueSampleTree(rmtMessageQueue* queue, Sample* sample, ObjectAllocator* allocator, rmtPStr thread_name, uint32_t user_data,
                            struct ThreadProfiler* thread_profiler, rmtBool partial_tree)
{
    Msg_SampleTree* payload;

    // Attempt to allocate a message for sending the tree to the viewer
    Message* message = rmtMessageQueue_AllocMessage(queue, sizeof(Msg_SampleTree), thread_profiler);
    if (message == NULL)
    {
        // Discard tree samples on failure
        FreeSamples(sample, allocator);
        return;
    }

    // Populate and commit
    payload = (Msg_SampleTree*)message->payload;
    payload->rootSample = sample;
    payload->allocator = allocator;
    payload->threadName = thread_name;
    payload->userData = user_data;
    payload->partialTree = partial_tree;
    rmtMessageQueue_CommitMessage(message, MsgID_SampleTree);
}

typedef struct Msg_AddToStringTable
{
    uint32_t hash;
    uint32_t length;
} Msg_AddToStringTable;

static rmtBool QueueAddToStringTable(rmtMessageQueue* queue, uint32_t hash, const char* string, size_t length, struct ThreadProfiler* thread_profiler)
{
    Msg_AddToStringTable* payload;

    // Attempt to allocate a message om the queue
    size_t nb_string_bytes = length + 1;
    Message* message = rmtMessageQueue_AllocMessage(queue, sizeof(Msg_AddToStringTable) + nb_string_bytes, thread_profiler);
    if (message == NULL)
    {
        return RMT_FALSE;
    }

    // Populate and commit
    payload = (Msg_AddToStringTable*)message->payload;
    payload->hash = hash;
    payload->length = length;
    memcpy(payload + 1, string, nb_string_bytes);
    rmtMessageQueue_CommitMessage(message, MsgID_AddToStringTable);

    return RMT_TRUE;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @TPROFILER: Thread Profiler data, storing both sampling and instrumentation results
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/




typedef struct ThreadProfiler
{
    // Storage for backing up initial register values when modifying a thread's context
    uint64_t registerBackup0;                                                                         // 0
    uint64_t registerBackup1;                                                                         // 8
    uint64_t registerBackup2;                                                                         // 16

    // Used to schedule callbacks taking into account some threads may be sleeping
    rmtAtomicS32 nbSamplesWithoutCallback;                                                          // 24

    // Index of the processor the thread was last seen running on
    uint32_t processorIndex;                                                                          // 28
    uint32_t lastProcessorIndex;

    // OS thread ID/handle
    rmtThreadId threadId;
    rmtThreadHandle threadHandle;

    // Thread name stored for sending to the viewer
    char threadName[64];
    uint32_t threadNameHash;

    // Store a unique sample tree for each type
    SampleTree* sampleTrees[RMT_SampleType_Count];



} ThreadProfiler;

static rmtError ThreadProfiler_Constructor(rmtMessageQueue* mq_to_rmt, ThreadProfiler* thread_profiler, rmtThreadId thread_id)
{
    uint32_t name_length;

    // Set defaults
    thread_profiler->nbSamplesWithoutCallback = 0;
    thread_profiler->processorIndex = (uint32_t)-1;
    thread_profiler->lastProcessorIndex = (uint32_t)-1;
    thread_profiler->threadId = thread_id;
    memset(thread_profiler->sampleTrees, 0, sizeof(thread_profiler->sampleTrees));




    // Pre-open the thread handle
    rmtTry(rmtOpenThreadHandle(thread_id, &thread_profiler->threadHandle));

    // Name the thread and add to the string table
    // Users can override this at a later point with the Remotery thread name API
    rmtGetThreadName(thread_id, thread_profiler->threadHandle, thread_profiler->threadName, sizeof(thread_profiler->threadName));
    name_length = strnlen_s(thread_profiler->threadName, 64);
    thread_profiler->threadNameHash = _rmt_HashString32(thread_profiler->threadName, name_length, 0);
    QueueAddToStringTable(mq_to_rmt, thread_profiler->threadNameHash, thread_profiler->threadName, name_length, thread_profiler);

    // Create the CPU sample tree only. The rest are created on-demand as they need extra context to function correctly.
    rmtTryNew(SampleTree, thread_profiler->sampleTrees[RMT_SampleType_CPU], sizeof(Sample), (ObjConstructor)Sample_Constructor,
          (ObjDestructor)Sample_Destructor);




    return RMT_ERROR_NONE;
}

static void ThreadProfiler_Destructor(ThreadProfiler* thread_profiler)
{
    uint32_t index;




    for (index = 0; index < RMT_SampleType_Count; index++)
    {
        rmtDelete(SampleTree, thread_profiler->sampleTrees[index]);
    }

    rmtCloseThreadHandle(thread_profiler->threadHandle);
}

static rmtError ThreadProfiler_Push(SampleTree* tree, uint32_t name_hash, uint32_t flags, Sample** sample)
{
    rmtError error;
    ModifySampleTree(tree,
        error = SampleTree_Push(tree, name_hash, flags, sample);
    );
    return error;
}

static void CloseOpenSamples(Sample* sample, uint64_t sample_time_us, uint32_t parents_are_last)
{
    Sample* child_sample;

    // Depth-first search into children as we want to close child samples before their parents
    for (child_sample = sample->first_child; child_sample != NULL; child_sample = child_sample->next_sibling)
    {
        uint32_t is_last = parents_are_last & (child_sample == sample->last_child ? 1 : 0);
        CloseOpenSamples(child_sample, sample_time_us, is_last);
    }

    // A chain of open samples will be linked from the root to the deepest, currently open sample
    if (parents_are_last > 0)
    {
        Sample_Close(sample, sample_time_us);
    }
}

static rmtError MakePartialTreeCopy(SampleTree* sample_tree, uint64_t sample_time_us, SampleTree* out_sample_tree_copy)
{
    uint32_t sample_time_s = (uint32_t)(sample_time_us / 1000);
    StoreRelease(&sample_tree->msLastTreeSendTime, sample_time_s);

    // Make a local copy of the tree as we want to keep the current tree for active profiling
    rmtTry(SampleTree_Copy(out_sample_tree_copy, sample_tree));

    // Close all samples from the deepest open sample, right back to the root
    CloseOpenSamples(out_sample_tree_copy->root, sample_time_us, 1);

    return RMT_ERROR_NONE;
}

static rmtBool ThreadProfiler_Pop(ThreadProfiler* thread_profiler, rmtMessageQueue* queue, Sample* sample, uint32_t msg_user_data)
{
    SampleTree* tree = thread_profiler->sampleTrees[sample->type];
    SampleTree_Pop(tree, sample);

    // Are we back at the root?
    if (tree->currentParent == tree->root)
    {
        Sample* root;

        // Disconnect all samples from the root and pack in the chosen message queue
        ModifySampleTree(tree,
        root = tree->root;
        root->first_child = NULL;
        root->last_child = NULL;
        root->nb_children = 0;
        );
        QueueSampleTree(queue, sample, tree->allocator, thread_profiler->threadName, msg_user_data, thread_profiler, RMT_FALSE);

        // Update the last send time for this tree, for stall detection
        StoreRelease(&tree->msLastTreeSendTime, (uint32_t)(sample->us_end / 1000));

        return RMT_TRUE;
    }

    if (tree->sendSampleOnClose == sample)
    {
        // Copy the sample tree as it is and send as a partial tree
        SampleTree partial_tree;
        if (MakePartialTreeCopy(tree, sample->us_start + sample->us_length, &partial_tree) == RMT_ERROR_NONE)
        {
            Sample* root_sample = partial_tree.root->first_child;
            assert(root_sample != NULL);
            QueueSampleTree(queue, root_sample, partial_tree.allocator, thread_profiler->threadName, msg_user_data, thread_profiler, RMT_TRUE);
        }

        // Tree has been copied away to the message queue so free up the samples
        if (partial_tree.root != NULL)
        {
            FreeSamples(partial_tree.root, partial_tree.allocator);
        }

        tree->sendSampleOnClose = NULL;
    }

    return RMT_FALSE;
}

static uint32_t ThreadProfiler_GetNameHash(ThreadProfiler* thread_profiler, rmtMessageQueue* queue, rmtPStr name, uint32_t* hash_cache)
{
    size_t name_len;
    uint32_t name_hash;

    // Hash cache provided?
    if (hash_cache != NULL)
    {
        // Calculate the hash first time round only
        name_hash = AtomicLoadU32((rmtAtomicU32*)hash_cache);
        if (name_hash == 0)
        {
            assert(name != NULL);
            name_len = strnlen_s(name, 256);
            name_hash = _rmt_HashString32(name, name_len, 0);

            // Queue the string for the string table and only cache the hash if it succeeds
            if (QueueAddToStringTable(queue, name_hash, name, name_len, thread_profiler) == RMT_TRUE)
            {
                AtomicStoreU32((rmtAtomicU32*)hash_cache, name_hash);
            }
        }

        return name_hash;
    }

    // Have to recalculate and speculatively insert the name every time when no cache storage exists
    name_len = strnlen_s(name, 256);
    name_hash = _rmt_HashString32(name, name_len, 0);
    QueueAddToStringTable(queue, name_hash, name, name_len, thread_profiler);
    return name_hash;
}

typedef struct ThreadProfilers
{
    // Timer shared with Remotery threads
    usTimer* timer;

    // Queue between clients and main remotery thread
    rmtMessageQueue* mqToRmtThread;

    // On x64 machines this points to the sample function
    void* compiledSampleFn;
    uint32_t compiledSampleFnSize;

    // Used to store thread profilers bound to an OS thread
    rmtTLS threadProfilerTlsHandle;

    // Array of preallocated ThreadProfiler objects
    // Read iteration is safe given that no incomplete ThreadProfiler objects will be encountered during iteration.
    // The ThreadProfiler count is only incremented once a new ThreadProfiler is fully defined and ready to be used.
    // Do not use this list to verify if a ThreadProfiler exists for a given thread. Use the mutex-guarded Get functions instead.
    ThreadProfiler threadProfilers[256];
    rmtAtomicU32 nbThreadProfilers;
    uint32_t maxNbThreadProfilers;

    // Guards creation and existence-testing of the ThreadProfiler list
    rmtMutex threadProfilerMutex;

    // Periodic thread sampling thread
    rmtThread* threadSampleThread;

    // Periodic thread to processor gatherer
    rmtThread* threadGatherThread;
} ThreadProfilers;

static rmtError SampleThreadsLoop(rmtThread* rmt_thread);

#ifdef RMT_PLATFORM_WINDOWS
#ifdef RMT_ARCH_64BIT
static void* CreateSampleCallback(uint32_t* out_size);
#endif
static FARPROC g_GetCurrentProcessorNumber = NULL;
#endif

static rmtError ThreadProfilers_Constructor(ThreadProfilers* thread_profilers, usTimer* timer, rmtMessageQueue* mq_to_rmt_thread)
{
    // Set to default
    thread_profilers->timer = timer;
    thread_profilers->mqToRmtThread = mq_to_rmt_thread;
    thread_profilers->compiledSampleFn = NULL;
    thread_profilers->compiledSampleFnSize = 0;
    thread_profilers->threadProfilerTlsHandle = TLS_INVALID_HANDLE;
    thread_profilers->nbThreadProfilers = 0;
    thread_profilers->maxNbThreadProfilers = sizeof(thread_profilers->threadProfilers) / sizeof(thread_profilers->threadProfilers[0]);
    mtxInit(&thread_profilers->threadProfilerMutex);
    thread_profilers->threadSampleThread = NULL;
    thread_profilers->threadGatherThread = NULL;

#ifdef RMT_PLATFORM_WINDOWS
    g_GetCurrentProcessorNumber = GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetCurrentProcessorNumber");
#ifdef RMT_ARCH_64BIT
    thread_profilers->compiledSampleFn = CreateSampleCallback(&thread_profilers->compiledSampleFnSize);
    if (thread_profilers->compiledSampleFn == NULL)
    {
        return RMT_ERROR_MALLOC_FAIL;
    }
#endif
#endif

    // Allocate a TLS handle for the thread profilers
    rmtTry(tlsAlloc(&thread_profilers->threadProfilerTlsHandle));

    // Kick-off the thread sampler
    if (g_Settings.enableThreadSampler == RMT_TRUE)
    {
        rmtTryNew(rmtThread, thread_profilers->threadSampleThread, SampleThreadsLoop, thread_profilers);
    }

    return RMT_ERROR_NONE;
}

static void ThreadProfilers_Destructor(ThreadProfilers* thread_profilers)
{
    uint32_t thread_index;

    rmtDelete(rmtThread, thread_profilers->threadSampleThread);

    // Delete all profilers
    for (thread_index = 0; thread_index < thread_profilers->nbThreadProfilers; thread_index++)
    {
        ThreadProfiler* thread_profiler = thread_profilers->threadProfilers + thread_index;
        ThreadProfiler_Destructor(thread_profiler);
    }

    if (thread_profilers->threadProfilerTlsHandle != TLS_INVALID_HANDLE)
    {
        tlsFree(thread_profilers->threadProfilerTlsHandle);
    }

#ifdef RMT_PLATFORM_WINDOWS
#ifdef RMT_ARCH_64BIT
    if (thread_profilers->compiledSampleFn != NULL)
    {
        VirtualFree(thread_profilers->compiledSampleFn, 0, MEM_RELEASE);
    }
#endif
#endif

    mtxDelete(&thread_profilers->threadProfilerMutex);
}

static rmtError ThreadProfilers_GetThreadProfiler(ThreadProfilers* thread_profilers, rmtThreadId thread_id, ThreadProfiler** out_thread_profiler)
{
    uint32_t profiler_index;
    ThreadProfiler* thread_profiler;
    rmtError error;

    mtxLock(&thread_profilers->threadProfilerMutex);

    // Linear search for a matching thread id
    for (profiler_index = 0; profiler_index < thread_profilers->nbThreadProfilers; profiler_index++)
    {
        thread_profiler = thread_profilers->threadProfilers + profiler_index;
        if (thread_profiler->threadId == thread_id)
        {
            *out_thread_profiler = thread_profiler;
            mtxUnlock(&thread_profilers->threadProfilerMutex);
            return RMT_ERROR_NONE;
        }
    }

    if (thread_profilers->nbThreadProfilers+1 > thread_profilers->maxNbThreadProfilers)
    {
        mtxUnlock(&thread_profilers->threadProfilerMutex);
        return RMT_ERROR_MALLOC_FAIL;
    }

    // Thread info not found so create a new one at the end
    thread_profiler = thread_profilers->threadProfilers + thread_profilers->nbThreadProfilers;
    error = ThreadProfiler_Constructor(thread_profilers->mqToRmtThread, thread_profiler, thread_id);
    if (error != RMT_ERROR_NONE)
    {
        ThreadProfiler_Destructor(thread_profiler);
        mtxUnlock(&thread_profilers->threadProfilerMutex);
        return error;
    }
    *out_thread_profiler = thread_profiler;

    // Increment count for consume by read iterators
    // Within the mutex so that there are no race conditions creating thread profilers
    // Using release semantics to ensure a memory barrier for read iterators
    StoreRelease(&thread_profilers->nbThreadProfilers, thread_profilers->nbThreadProfilers + 1);

    mtxUnlock(&thread_profilers->threadProfilerMutex);

    return RMT_ERROR_NONE;
}

static rmtError ThreadProfilers_GetCurrentThreadProfiler(ThreadProfilers* thread_profilers, ThreadProfiler** out_thread_profiler)
{
    // Is there a thread profiler associated with this thread yet?
    *out_thread_profiler = (ThreadProfiler*)tlsGet(thread_profilers->threadProfilerTlsHandle);
    if (*out_thread_profiler == NULL)
    {
        // Allocate on-demand
        rmtTry(ThreadProfilers_GetThreadProfiler(thread_profilers, rmtGetCurrentThreadId(), out_thread_profiler));

        // Bind to the curren thread
        tlsSet(thread_profilers->threadProfilerTlsHandle, *out_thread_profiler);
    }

    return RMT_ERROR_NONE;
}

static rmtBool ThreadProfilers_ThreadInCallback(ThreadProfilers* thread_profilers, rmtCpuContext* context)
{
#ifdef RMT_PLATFORM_WINDOWS
#ifdef RMT_ARCH_32BIT
    if (context->Eip >= (DWORD)thread_profilers->compiledSampleFn &&
        context->Eip < (DWORD)((char*)thread_profilers->compiledSampleFn + thread_profilers->compiledSampleFnSize))
    {
        return RMT_TRUE;
    }
#else
    if (context->Rip >= (DWORD64)thread_profilers->compiledSampleFn &&
        context->Rip < (DWORD64)((char*)thread_profilers->compiledSampleFn + thread_profilers->compiledSampleFnSize))
    {
        return RMT_TRUE;
    }
#endif
#endif
    return RMT_FALSE;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @TGATHER: Thread Gatherer, periodically polling for newly created threads
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

static void GatherThreads(ThreadProfilers* thread_profilers)
{
#ifdef RMT_ENABLE_THREAD_SAMPLER
    rmtThreadHandle handle;
#endif

    assert(thread_profilers != NULL);

#ifdef RMT_ENABLE_THREAD_SAMPLER

    // Create the snapshot - this is a slow call
    handle = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (handle != INVALID_HANDLE_VALUE)
    {
        BOOL success;

        THREADENTRY32 thread_entry;
        thread_entry.dwSize = sizeof(thread_entry);

        // Loop through all threads owned by this process
        success = Thread32First(handle, &thread_entry);
        while (success == TRUE)
        {
            if (thread_entry.th32OwnerProcessID == GetCurrentProcessId())
            {
                // Create thread profilers on-demand if there're not already there
                ThreadProfiler* thread_profiler;
                rmtError error = ThreadProfilers_GetThreadProfiler(thread_profilers, thread_entry.th32ThreadID, &thread_profiler);
                if (error != RMT_ERROR_NONE)
                {
                    // Not really worth bringing the whole profiler down here
                    rmt_LogText("REMOTERY ERROR: Failed to create Thread Profiler");
                }
            }

            success = Thread32Next(handle, &thread_entry);
        }

        CloseHandle(handle);
    }

#endif
}

static rmtError GatherThreadsLoop(rmtThread* thread)
{
    ThreadProfilers* thread_profilers = (ThreadProfilers*)thread->param;
    uint32_t sleep_time = 100;

    assert(thread_profilers != NULL);

    rmt_SetCurrentThreadName("RemoteryGatherThreads");

    while (thread->request_exit == RMT_FALSE)
    {
        // We want a long period of time between scanning for new threads as the process is a little expensive (~30ms here).
        // However not too long so as to miss potentially detailed process startup data.
        // Use reduced sleep time at startup to catch as many early thread creations as possible.
        // TODO(don): We could get processes to register themselves to ensure no startup data is lost but the scan must still
        // be present, to catch threads in a process that the user doesn't create (e.g. graphics driver threads).
        GatherThreads(thread_profilers);
        msSleep(sleep_time);
        sleep_time = minU32(sleep_time * 2, 2000);
    }

    return RMT_ERROR_NONE;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @TSAMPLER: Sampling thread contexts
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

typedef struct Processor
{
    // Current thread profiler sampling this processor
    ThreadProfiler* threadProfiler;

    uint32_t sampleCount;
    uint64_t sampleTime;
} Processor;

typedef struct Msg_ProcessorThreads
{
    // Running index of processor messages
    uint64_t messageIndex;

    // Processor array, leaking into the memory behind the struct
    uint32_t nbProcessors;
    Processor processors[1];
} Msg_ProcessorThreads;

static void QueueProcessorThreads(rmtMessageQueue* queue, uint64_t message_index, uint32_t nb_processors, Processor* processors)
{
    Msg_ProcessorThreads* payload;

    // Attempt to allocate a message for sending processors to the viewer
    uint32_t array_size = (nb_processors - 1) * sizeof(Processor);
    Message* message = rmtMessageQueue_AllocMessage(queue, sizeof(Msg_ProcessorThreads) + array_size, NULL);
    if (message == NULL)
    {
        return;
    }

    // Populate and commit
    payload = (Msg_ProcessorThreads*)message->payload;
    payload->messageIndex = message_index;
    payload->nbProcessors = nb_processors;
    memcpy(payload->processors, processors, nb_processors * sizeof(Processor));
    rmtMessageQueue_CommitMessage(message, MsgID_ProcessorThreads);
}

#ifdef RMT_PLATFORM_WINDOWS
#if defined(RMT_ARCH_32BIT) && defined(_MSC_VER)
__declspec(naked) static void SampleCallback()
{
    //
    // It's important to realise that this call can be pre-empted by the scheduler and shifted to another processor *while we are
    // sampling which processor this thread is on*.
    //
    // This has two very important implications:
    //
    //    * What we are sampling here is an *approximation* of the path of threads across processors.
    //    * These samples can't be used to "open" and "close" sample periods on a processor as it's highly likely you'll get many
    //      open events without a close, or vice versa.
    //
    // As such, we can only choose a sampling period and for each sample register which threads are on which processor.
    //
    // This is very different to hooking up the Event Tracing API (requiring Administrator elevation), which raises events for
    // each context switch, directly from the kernel.
    //

    __asm
    {
        // Push the EIP return address used by the final ret instruction
        push ebx

        // We might be in the middle of something like a cmp/jmp instruction pair so preserve EFLAGS
        // (Classic example which seems to pop up regularly is _RTC_CheckESP, with cmp/call/jne)
        pushfd

        // Push all volatile registers as we don't know what the function calls below will destroy
        push eax
        push ecx
        push edx

        // Retrieve and store the current processor index
        call esi
        mov [edi].processorIndex, eax

        // Mark as ready for scheduling another callback
        // Intel x86 store release
        mov [edi].nbSamplesWithoutCallback, 0

        // Restore preserved register state
        pop edx
        pop ecx
        pop eax

        // Restore registers used to provide parameters to the callback
        mov ebx, dword ptr [edi].registerBackup0
        mov esi, dword ptr [edi].registerBackup1
        mov edi, dword ptr [edi].registerBackup2

        // Restore EFLAGS
        popfd

        // Pops the original EIP off the stack and jmps to origin suspend point in the thread
        ret
    }
}
#elif defined(RMT_ARCH_64BIT)
// Generated with https://defuse.ca/online-x86-assembler.htm
static uint8_t SampleCallbackBytes[] =
{
    // Push the RIP return address used by the final ret instruction
    0x53,                                           // push rbx

    // We might be in the middle of something like a cmp/jmp instruction pair so preserve RFLAGS
    // (Classic example which seems to pop up regularly is _RTC_CheckESP, with cmp/call/jne)
    0x9C,                                           // pushfq

    // Push all volatile registers as we don't know what the function calls below will destroy
    0x50,                                           // push rax
    0x51,                                           // push rcx
    0x52,                                           // push rdx
    0x41, 0x50,                                     // push r8
    0x41, 0x51,                                     // push r9
    0x41, 0x52,                                     // push r10
    0x41, 0x53,                                     // push r11

    // Retrieve and store the current processor index
    0xFF, 0xD6,                                     // call rsi
    0x89, 0x47, 0x1C,                               // mov dword ptr [rdi + 28], eax

    // Mark as ready for scheduling another callback
    // Intel x64 store release
    0xC7, 0x47, 0x18, 0x00, 0x00, 0x00, 0x00,       // mov dword ptr [rdi + 24], 0

    // Restore preserved register state
    0x41, 0x5B,                                     // pop r11
    0x41, 0x5A,                                     // pop r10
    0x41, 0x59,                                     // pop r9
    0x41, 0x58,                                     // pop r8
    0x5A,                                           // pop rdx
    0x59,                                           // pop rcx
    0x58,                                           // pop rax

    // Restore registers used to provide parameters to the callback
    0x48, 0x8B, 0x1F,                               // mov rbx, qword ptr [rdi + 0]
    0x48, 0x8B, 0x77, 0x08,                         // mov rsi, qword ptr [rdi + 8]
    0x48, 0x8B, 0x7F, 0x10,                         // mov rdi, qword ptr [rdi + 16]

    // Restore RFLAGS
    0x9D,                                           // popfq

    // Pops the original EIP off the stack and jmps to origin suspend point in the thread
    0xC3                                            // ret
};
static void* CreateSampleCallback(uint32_t* out_size)
{
    // Allocate page for the generated code
    DWORD size = 4096;
    DWORD old_protect;
    void* function = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (function == NULL)
    {
        return NULL;
    }

    // Clear whole allocation to int 3h
    memset(function, 0xCC, size);

    // Copy over the generated code
    memcpy(function, SampleCallbackBytes, sizeof(SampleCallbackBytes));
    *out_size = sizeof(SampleCallbackBytes);

    // Enable execution
    VirtualProtect(function, size, PAGE_EXECUTE_READ, &old_protect);
    return function;
}
#endif
#endif

#if defined(__cplusplus) && __cplusplus >= 201103L
static_assert(offsetof(ThreadProfiler, nbSamplesWithoutCallback) == 24, "");
static_assert(offsetof(ThreadProfiler, processorIndex) == 28, "");
#endif

static rmtError CheckForStallingSamples(SampleTree* stalling_sample_tree, ThreadProfiler* thread_profiler, uint64_t sample_time_us)
{
    SampleTree* sample_tree;
    uint32_t sample_time_s = (uint32_t)(sample_time_us / 1000);

    // Initialise to empty
    stalling_sample_tree->root = NULL;
    stalling_sample_tree->allocator = NULL;

    // Skip the stall check if the tree is being modified
    sample_tree = thread_profiler->sampleTrees[RMT_SampleType_CPU];
    if (LoadAcquire(&sample_tree->treeBeingModified) != 0)
    {
        return RMT_ERROR_NONE;
    }

    if (sample_tree != NULL)
    {
        // The root is a dummy root inserted on tree creation so check that for children
        Sample* root_sample = sample_tree->root;
        if (root_sample != NULL && root_sample->nb_children > 0)
        {
            if (sample_time_s - LoadAcquire(&sample_tree->msLastTreeSendTime) > 1000)
            {
                rmtTry(MakePartialTreeCopy(sample_tree, sample_time_us, stalling_sample_tree));
            }
        }
    }

    return RMT_ERROR_NONE;
}

static rmtError InitThreadSampling(ThreadProfilers* thread_profilers)
{
    rmt_SetCurrentThreadName("RemoterySampleThreads");

    // Make an initial gather so that we have something to work with
    GatherThreads(thread_profilers);

#ifdef RMT_ENABLE_THREAD_SAMPLER
    // Ensure we can wake up every millisecond
    if (timeBeginPeriod(1) != TIMERR_NOERROR)
    {
        return RMT_ERROR_UNKNOWN;
    }
#endif

    // Kick-off the background thread that watches for new threads
    rmtTryNew(rmtThread, thread_profilers->threadGatherThread, GatherThreadsLoop, thread_profilers);

    // We're going to be shuffling thread visits to avoid the scheduler trying to predict a work-load based on sampling
    // Use the global RNG with a random seed to start the shuffle
    Well512_Init((uint32_t)time(NULL));

    return RMT_ERROR_NONE;
}

static rmtError SampleThreadsLoop(rmtThread* rmt_thread)
{
    rmtCpuContext context;
    uint32_t processor_message_index = 0;
    uint32_t nb_processors;
    Processor* processors;
    uint32_t processor_index;

    ThreadProfilers* thread_profilers = (ThreadProfilers*)rmt_thread->param;

    // If we can't figure out how many processors there are then we are running on an unsupported platform
    nb_processors = rmtGetNbProcessors();
    if (nb_processors == 0)
    {
        return RMT_ERROR_UNKNOWN;
    }

    rmtTry(InitThreadSampling(thread_profilers));

    // An array entry for each processor
    rmtTryMallocArray(Processor, processors, nb_processors);
    for (processor_index = 0; processor_index < nb_processors; processor_index++)
    {
        processors[processor_index].threadProfiler = NULL;
        processors[processor_index].sampleTime = 0;
    }

    while (rmt_thread->request_exit == RMT_FALSE)
    {
        uint32_t lfsr_seed;
        uint32_t lfsr_value;

        // Query how many threads the gather knows about this time round
        uint32_t nb_thread_profilers = LoadAcquire(&thread_profilers->nbThreadProfilers);

        // Calculate table size log2 required to fit count entries. Normally we would adjust the log2 input by -1 so that
        // power-of-2 counts map to their exact bit offset and don't require a twice larger table. You can iterate indices
        // 0 to (1<<b)-1 and efficiently store the whole range.
        //
        // However, the LFSR will never return 0. If you have a table count of 7 then it will return indices 1 to 7 within the
        // 3=bit range required. Normally this is fine as subtracting one for your index iterates the complete indices 0 to 6.
        //
        // This becomes a problem when your table size is 8 (or any other power-of-2). With the log2 -1 adjustment a count of 8
        // calculates a table size of 8. LFSR iteration will only return 1-7, visiting indices 0-6, missing out the last one.
        //
        // So to counter that let an input count of 8 require a table size of 16. LFSR iteration will return 1-15, visiting
        // indices 0-14 and the required range test will limit that visit to 0-7.
        //
        // Implementation playground: https://godbolt.org/z/qG8T4E
        uint32_t highest_bit_set = Log2i(nb_thread_profilers);
        uint32_t table_size_log2 = highest_bit_set + 1;
        uint32_t xor_mask = GaloisLFSRMask(table_size_log2);

        // Use a LFSR to visit threads in shuffled order
        lfsr_seed = Well512_RandomOpenLimit(nb_thread_profilers);
        lfsr_value = lfsr_seed;
        do
        {
            uint32_t thread_index;
            rmtThreadId thread_id;
            ThreadProfiler* thread_profiler;
            rmtThreadHandle thread_handle;
            uint64_t sample_time_us;
            uint32_t sample_count;
            SampleTree stalling_sample_tree;

            lfsr_value = GaloisLFSRNext(lfsr_value, xor_mask);

            // Apply the value-to-index bias and see if this index is within range before processing
            thread_index = lfsr_value - 1;
            if (thread_index >= nb_thread_profilers)
            {
                continue;
            }

            // Ignore our own thread
            thread_id = rmtGetCurrentThreadId();
            thread_profiler = thread_profilers->threadProfilers + thread_index;
            if (thread_profiler->threadId == thread_id)
            {
                continue;
            }

            // Suspend the thread so we can insert a callback
            thread_handle = thread_profiler->threadHandle;
            if (rmtSuspendThread(thread_handle) == RMT_FALSE)
            {
                continue;
            }

            // Mark the processor this thread was last recorded as running on.
            // Note that a thread might be pre-empted multiple times in-between sampling. Given a sampling rate equal to the
            // scheduling quantum, this doesn't happen too often. However in such cases, whoever marks the processor last is
            // the one that gets recorded.
            sample_time_us = usTimer_Get(thread_profilers->timer);
            sample_count = AtomicAddS32(&thread_profiler->nbSamplesWithoutCallback, 1);
            processor_index = thread_profiler->processorIndex;
            if (processor_index != (uint32_t)-1)
            {
                assert(processor_index < nb_processors);
                processors[processor_index].threadProfiler = thread_profiler;
                processors[processor_index].sampleCount = sample_count;
                processors[processor_index].sampleTime = sample_time_us;
            }

            // Swap in a new context with our callback if one is not already scheduled on this thread
            if (sample_count == 0)
            {
                if (rmtGetUserModeThreadContext(thread_handle, &context) == RMT_TRUE &&
                    // There is a slight window of opportunity, after which the callback sets nbSamplesWithoutCallback=0,
                    // for this loop to suspend a thread while it's executing the last instructions of the callback.
                    ThreadProfilers_ThreadInCallback(thread_profilers, &context) == RMT_FALSE)
                {
                #ifdef RMT_PLATFORM_WINDOWS
                    if (g_GetCurrentProcessorNumber != NULL)
                    {
                #ifdef RMT_ARCH_64BIT
                    thread_profiler->registerBackup0 = context.Rbx;
                    thread_profiler->registerBackup1 = context.Rsi;
                    thread_profiler->registerBackup2 = context.Rdi;
                    context.Rbx = context.Rip;
                    context.Rsi = (uint64_t)g_GetCurrentProcessorNumber;
                    context.Rdi = (uint64_t)thread_profiler;
                    context.Rip = (DWORD64)thread_profilers->compiledSampleFn;
                #endif
                #if defined(RMT_ARCH_32BIT) && defined(_MSC_VER)
                    thread_profiler->registerBackup0 = context.Ebx;
                    thread_profiler->registerBackup1 = context.Esi;
                    thread_profiler->registerBackup2 = context.Edi;
                    context.Ebx = context.Eip;
                    context.Esi = (uint32_t)g_GetCurrentProcessorNumber;
                    context.Edi = (uint32_t)thread_profiler;
                    context.Eip = (DWORD)&SampleCallback;
                #endif
                    }
                #endif

                    rmtSetThreadContext(thread_handle, &context);
                }
                else
                {
                    AtomicAddS32(&thread_profiler->nbSamplesWithoutCallback, -1);
                }
            }

            // While the thread is suspended take the chance to check for samples trees that may never complete
            // Because SuspendThread on Windows is an async request, this needs to be placed at a point where the request completes
            // Calling GetThreadContext will ensure the request is completed so this stall check is placed after that
            if (RMT_ERROR_NONE != CheckForStallingSamples(&stalling_sample_tree, thread_profiler, sample_time_us))
            {
                assert(stalling_sample_tree.allocator != NULL);
                if (stalling_sample_tree.root != NULL)
                {
                    FreeSamples(stalling_sample_tree.root, stalling_sample_tree.allocator);
                }
            }

            rmtResumeThread(thread_handle);

            if (stalling_sample_tree.root != NULL)
            {
                // If there is stalling sample tree on this thread then send it to listeners.
                // Do the send *outside* of all Suspend/Resume calls as we have no way of knowing who is reading/writing the queue
                // Mark this as partial so that the listeners know it will be overwritten.
                Sample* sample = stalling_sample_tree.root->first_child;
                assert(sample != NULL);
                QueueSampleTree(thread_profilers->mqToRmtThread, sample, stalling_sample_tree.allocator, thread_profiler->threadName, 0, thread_profiler, RMT_TRUE);

                // The stalling_sample_tree.root->first_child has been sent to the main Remotery thread. This will get released later
                // when the Remotery thread has processed it. This leaves the stalling_sample_tree.root here that must be freed.
                // Before freeing the root sample we have to detach the children though.
                stalling_sample_tree.root->first_child = NULL;
                stalling_sample_tree.root->last_child = NULL;
                stalling_sample_tree.root->nb_children = 0;
                assert(stalling_sample_tree.allocator != NULL);
                FreeSamples(stalling_sample_tree.root, stalling_sample_tree.allocator);
            }


        } while (lfsr_value != lfsr_seed);

        // Filter all processor samples made in this pass
        for (processor_index = 0; processor_index < nb_processors; processor_index++)
        {
            Processor* processor = processors + processor_index;
            ThreadProfiler* thread_profiler = processor->threadProfiler;

            if (thread_profiler != NULL)
            {
                // If this thread was on another processor on a previous pass and that processor is still tracking that thread,
                // remove the thread from it.
                uint32_t last_processor_index = thread_profiler->lastProcessorIndex;
                if (last_processor_index != (uint32_t)-1 && last_processor_index != processor_index)
                {
                    assert(last_processor_index < nb_processors);
                    if (processors[last_processor_index].threadProfiler == thread_profiler)
                    {
                        processors[last_processor_index].threadProfiler = NULL;
                    }
                }

                // When the thread is still on the same processor, check to see if it hasn't triggered the callback within another
                // pass. This suggests the thread has gone to sleep and is no longer assigned to any thread.
                else if (processor->sampleCount > 1)
                {
                    processor->threadProfiler = NULL;
                }

                thread_profiler->lastProcessorIndex = thread_profiler->processorIndex;
            }
        }

        // Send current processor state off to remotery
        QueueProcessorThreads(thread_profilers->mqToRmtThread, processor_message_index++, nb_processors, processors);
    }

    rmtDelete(rmtThread, thread_profilers->threadGatherThread);

#ifdef RMT_ENABLE_THREAD_SAMPLER
    timeEndPeriod(1);
#endif

    rmtFree(processors);

    return RMT_ERROR_NONE;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
   @REMOTERY: Remotery
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/



typedef struct PropertySnapshot
{
    // Inherit so that property states can be quickly allocated
    ObjectLink Link;

    // Data copied from the property at the time of the snapshot
    rmtPropertyType type;
    rmtPropertyValue value;
    rmtPropertyValue prevValue;
    uint32_t prevValueFrame;
    uint32_t nameHash;
    uint32_t uniqueID;

    // Depth calculated as part of the walk
    uint8_t depth;

    // Link to the next property snapshot
    uint32_t nbChildren;
    struct PropertySnapshot* nextSnapshot;
} PropertySnapshot;

typedef struct Msg_PropertySnapshot
{
    PropertySnapshot* rootSnapshot;
    uint32_t nbSnapshots;
    uint32_t propertyFrame;
} Msg_PropertySnapshot;

static rmtError PropertySnapshot_Constructor(PropertySnapshot* snapshot)
{
    assert(snapshot != NULL);

    ObjectLink_Constructor((ObjectLink*)snapshot);

    snapshot->type = RMT_PropertyType_rmtBool;
    snapshot->value.Bool = RMT_FALSE;
    snapshot->nameHash = 0;
    snapshot->uniqueID = 0;
    snapshot->nbChildren = 0;
    snapshot->depth = 0;
    snapshot->nextSnapshot = NULL;

    return RMT_ERROR_NONE;
}

static void PropertySnapshot_Destructor(PropertySnapshot* snapshot)
{
    RMT_UNREFERENCED_PARAMETER(snapshot);
}

struct Remotery
{
    Server* server;

    // Microsecond accuracy timer for CPU timestamps
    usTimer timer;

    // Queue between clients and main remotery thread
    rmtMessageQueue* mq_to_rmt_thread;

    // The main server thread
    rmtThread* thread;

    // String table shared by all threads
    StringTable* string_table;

    // Open logfile handle to append events to
    FILE* logfile;

    // Set to trigger a map of each message on the remotery thread message queue
    void (*map_message_queue_fn)(Remotery* rmt, Message*);
    void* map_message_queue_data;






    ThreadProfilers* threadProfilers;

    // Root of all registered properties, guarded by mutex as property register can come from any thread
    rmtMutex propertyMutex;
    rmtProperty rootProperty;

    // Allocator for property values that get sent to the viewer
    ObjectAllocator* propertyAllocator;

    // Frame used to determine age of property changes
    uint32_t propertyFrame;

    rmtAtomicS32 countThreads;
};

//
// Global remotery context
//
static Remotery* g_Remotery = NULL;

//
// This flag marks the EXE/DLL that created the global remotery instance. We want to allow
// only the creating EXE/DLL to destroy the remotery instance.
//
static rmtBool g_RemoteryCreated = RMT_FALSE;

static void rmtGetThreadNameFallback(char* out_thread_name, uint32_t thread_name_size)
{
    // In cases where we can't get a thread name from the OS
    out_thread_name[0] = 0;
    strncat_s(out_thread_name, thread_name_size, "Thread", 6);
    itoahex_s(out_thread_name + 6, thread_name_size - 6, AtomicAddS32(&g_Remotery->countThreads, 1));
}

static double saturate(double v)
{
    if (v < 0)
    {
        return 0;
    }
    if (v > 1)
    {
        return 1;
    }
    return v;
}

static void PostProcessSamples(Sample* sample, uint32_t* nb_samples)
{
    Sample* child;

    assert(sample != NULL);
    assert(nb_samples != NULL);

    (*nb_samples)++;

    {
        // Hash integer line position to full hue
        double h = (double)sample->name_hash / (double)0xFFFFFFFF;
        double r = saturate(fabs(fmod(h * 6 + 0, 6) - 3) - 1);
        double g = saturate(fabs(fmod(h * 6 + 4, 6) - 3) - 1);
        double b = saturate(fabs(fmod(h * 6 + 2, 6) - 3) - 1);

        // Cubic smooth
        r = r * r * (3 - 2 * r);
        g = g * g * (3 - 2 * g);
        b = b * b * (3 - 2 * b);

        // Lerp to HSV lightness a little
        double k = 0.4;
        r = r * k + (1 - k);
        g = g * k + (1 - k);
        b = b * k + (1 - k);

        // To RGB8
        sample->uniqueColour[0] = (uint8_t)maxS32(minS32((int32_t)(r * 255), 255), 0);
        sample->uniqueColour[1] = (uint8_t)maxS32(minS32((int32_t)(g * 255), 255), 0);
        sample->uniqueColour[2] = (uint8_t)maxS32(minS32((int32_t)(b * 255), 255), 0);

        //uint32_t hash = sample->name_hash;
        //sample->uniqueColour[0] = 127 + ((hash & 255) >> 1);
        //sample->uniqueColour[1] = 127 + (((hash >> 4) & 255) >> 1);
        //sample->uniqueColour[2] = 127 + (((hash >> 8) & 255) >> 1);
    }

    // Concatenate children
    for (child = sample->first_child; child != NULL; child = child->next_sibling)
    {
        PostProcessSamples(child, nb_samples);
    }
}

static rmtError Remotery_SendLogTextMessage(Remotery* rmt, Message* message)
{
    Buffer* bin_buf;
    uint32_t write_start_offset;

    // Build the buffer as if it's being sent to the server
    assert(rmt != NULL);
    assert(message != NULL);
    bin_buf = rmt->server->bin_buf;
    WebSocket_PrepareBuffer(bin_buf);
    rmtTry(bin_MessageHeader(bin_buf, "LOGM", &write_start_offset));
    rmtTry(Buffer_Write(bin_buf, message->payload, message->payload_size));
    rmtTry(bin_MessageFooter(bin_buf, write_start_offset));

    // Pass to either the server or the log file
    if (rmt->logfile != NULL)
    {
        rmtWriteFile(rmt->logfile, bin_buf->data + WEBSOCKET_MAX_FRAME_HEADER_SIZE, bin_buf->bytes_used - WEBSOCKET_MAX_FRAME_HEADER_SIZE);
    }
    if (Server_IsClientConnected(rmt->server) == RMT_TRUE)
    {
        rmtTry(Server_Send(rmt->server, bin_buf->data, bin_buf->bytes_used, 20));
    }

    return RMT_ERROR_NONE;
}

static rmtError bin_SampleName(Buffer* buffer, const char* name, uint32_t name_hash, uint32_t name_length)
{
    uint32_t write_start_offset;
    rmtTry(bin_MessageHeader(buffer, "SSMP", &write_start_offset));
    rmtTry(Buffer_WriteU32(buffer, name_hash));
    rmtTry(Buffer_WriteU32(buffer, name_length));
    rmtTry(Buffer_Write(buffer, (void*)name, name_length));
    rmtTry(bin_MessageFooter(buffer, write_start_offset));

    return RMT_ERROR_NONE;
}

static rmtError Remotery_AddToStringTable(Remotery* rmt, Message* message)
{
    // Add to the string table
    Msg_AddToStringTable* payload = (Msg_AddToStringTable*)message->payload;
    const char* name = (const char*)(payload + 1);
    rmtBool name_inserted = StringTable_Insert(rmt->string_table, payload->hash, name);

    // Emit to log file if one is open
    if (name_inserted == RMT_TRUE && rmt->logfile != NULL)
    {
        Buffer* bin_buf = rmt->server->bin_buf;
        bin_buf->bytes_used = 0;
        rmtTry(bin_SampleName(bin_buf, name, payload->hash, payload->length));

        rmtWriteFile(rmt->logfile, bin_buf->data, bin_buf->bytes_used);
    }

    return RMT_ERROR_NONE;
}

static rmtError bin_SampleTree(Buffer* buffer, Msg_SampleTree* msg)
{
    Sample* root_sample;
    char thread_name[256];
    uint32_t nb_samples = 0;
    uint32_t write_start_offset = 0;

    assert(buffer != NULL);
    assert(msg != NULL);

    // Get the message root sample
    root_sample = msg->rootSample;
    assert(root_sample != NULL);

    // Add any sample types as a thread name post-fix to ensure they get their own viewer
    thread_name[0] = 0;
    strncat_s(thread_name, sizeof(thread_name), msg->threadName, strnlen_s(msg->threadName, 255));

    // Get digest hash of samples so that viewer can efficiently rebuild its tables
    PostProcessSamples(root_sample, &nb_samples);

    // Write sample message header
    rmtTry(bin_MessageHeader(buffer, "SMPL", &write_start_offset));
    rmtTry(Buffer_WriteStringWithLength(buffer, thread_name));
    rmtTry(Buffer_WriteU32(buffer, nb_samples));
    rmtTry(Buffer_WriteU32(buffer, msg->partialTree ? 1 : 0));

    // Align serialised sample tree to 32-bit boundary
    rmtTry(Buffer_AlignedPad(buffer, write_start_offset));

    // Write entire sample tree
    rmtTry(bin_Sample(buffer, root_sample, 0));

    rmtTry(bin_MessageFooter(buffer, write_start_offset));

    return RMT_ERROR_NONE;
}


static rmtError Remotery_SendToViewerAndLog(Remotery* rmt, Buffer* bin_buf, uint32_t timeout)
{
    rmtError error = RMT_ERROR_NONE;

    if (Server_IsClientConnected(rmt->server) == RMT_TRUE)
    {
        rmt_BeginCPUSample(Server_Send, RMTSF_Aggregate);
        error = Server_Send(rmt->server, bin_buf->data, bin_buf->bytes_used, timeout);
        rmt_EndCPUSample();
    }

    if (rmt->logfile != NULL)
    {
        // Write the data after the websocket header
        rmtWriteFile(rmt->logfile, bin_buf->data + WEBSOCKET_MAX_FRAME_HEADER_SIZE, bin_buf->bytes_used - WEBSOCKET_MAX_FRAME_HEADER_SIZE);
    }

    return error;
}

static rmtError Remotery_SendSampleTreeMessage(Remotery* rmt, Message* message)
{
    rmtError error = RMT_ERROR_NONE;

    Msg_SampleTree* sample_tree;
    Sample* sample;
    Buffer* bin_buf;

    assert(rmt != NULL);
    assert(message != NULL);

    // Get the message root sample
    sample_tree = (Msg_SampleTree*)message->payload;
    sample = sample_tree->rootSample;
    assert(sample != NULL);


    // Reset the buffer for sending a websocket message
    bin_buf = rmt->server->bin_buf;
    WebSocket_PrepareBuffer(bin_buf);

    // Serialise the sample tree
    rmt_BeginCPUSample(bin_SampleTree, RMTSF_Aggregate);
    error = bin_SampleTree(bin_buf, sample_tree);
    rmt_EndCPUSample();

    if (g_Settings.sampletree_handler != NULL)
    {
        g_Settings.sampletree_handler(g_Settings.sampletree_context, sample_tree);
    }

    // Release sample tree samples back to their allocator
    FreeSamples(sample, sample_tree->allocator);

    if (error != RMT_ERROR_NONE)
    {
        return error;
    }

    // Send to the viewer with a reasonably long timeout as the size of the sample data may be large
    return Remotery_SendToViewerAndLog(rmt, bin_buf, 50000);
}

static rmtError Remotery_SendProcessorThreads(Remotery* rmt, Message* message)
{
    uint32_t processor_index;

    Msg_ProcessorThreads* processor_threads = (Msg_ProcessorThreads*)message->payload;

    Buffer* bin_buf;
    uint32_t write_start_offset;

    // Reset the buffer for sending a websocket message
    bin_buf = rmt->server->bin_buf;
    WebSocket_PrepareBuffer(bin_buf);

    // Serialise the message
    rmtTry(bin_MessageHeader(bin_buf, "PRTH", &write_start_offset));
    rmtTry(Buffer_WriteU32(bin_buf, processor_threads->nbProcessors));
    rmtTry(Buffer_WriteU64(bin_buf, processor_threads->messageIndex));
    for (processor_index = 0; processor_index < processor_threads->nbProcessors; processor_index++)
    {
        Processor* processor = processor_threads->processors + processor_index;
        if (processor->threadProfiler != NULL)
        {
            rmtTry(Buffer_WriteU32(bin_buf, processor->threadProfiler->threadId));
            rmtTry(Buffer_WriteU32(bin_buf, processor->threadProfiler->threadNameHash));
            rmtTry(Buffer_WriteU64(bin_buf, processor->sampleTime));
        }
        else
        {
            rmtTry(Buffer_WriteU32(bin_buf, (uint32_t)-1));
            rmtTry(Buffer_WriteU32(bin_buf, 0));
            rmtTry(Buffer_WriteU64(bin_buf, 0));
        }
    }

    rmtTry(bin_MessageFooter(bin_buf, write_start_offset));

    return Remotery_SendToViewerAndLog(rmt, bin_buf, 50);
}

static void FreePropertySnapshots(PropertySnapshot* snapshot)
{
    // Allows root call to pass null
    if (snapshot == NULL)
    {
        return;
    }

    // Depth first free
    if (snapshot->nextSnapshot != NULL)
    {
        FreePropertySnapshots(snapshot->nextSnapshot);
    }

    ObjectAllocator_Free(g_Remotery->propertyAllocator, snapshot);
}

static rmtError Remotery_SerialisePropertySnapshots(Buffer* bin_buf, Msg_PropertySnapshot* msg_snapshot)
{
    PropertySnapshot* snapshot;
    uint8_t empty_group[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t write_start_offset;

    // Header
    rmtTry(bin_MessageHeader(bin_buf, "PSNP", &write_start_offset));
    rmtTry(Buffer_WriteU32(bin_buf, msg_snapshot->nbSnapshots));
    rmtTry(Buffer_WriteU32(bin_buf, msg_snapshot->propertyFrame));

    // Linearised snapshots
    for (snapshot = msg_snapshot->rootSnapshot; snapshot != NULL; snapshot = snapshot->nextSnapshot)
    {
        uint8_t colour_depth[4] = {0, 0, 0};

        // Same place as samples so that the GPU renderer can easily pick them out
        rmtTry(Buffer_WriteU32(bin_buf, snapshot->nameHash));
        rmtTry(Buffer_WriteU32(bin_buf, snapshot->uniqueID));

        // 3 byte place holder for viewer-side colour, with snapshot depth packed next to it
        colour_depth[3] = snapshot->depth;
        rmtTry(Buffer_Write(bin_buf, colour_depth, 4));

        // Dispatch on property type, but maintaining 64-bits per value
        rmtTry(Buffer_WriteU32(bin_buf, snapshot->type));
        switch (snapshot->type)
        {
            // Empty
            case RMT_PropertyType_rmtGroup:
                rmtTry(Buffer_Write(bin_buf, empty_group, 16));
                break;
            
            // All value ranges here are double-representable, so convert them early in C where it's cheap
            case RMT_PropertyType_rmtBool:
                rmtTry(Buffer_WriteF64(bin_buf, snapshot->value.Bool));
                rmtTry(Buffer_WriteF64(bin_buf, snapshot->prevValue.Bool));
                break;
            case RMT_PropertyType_rmtS32:
                rmtTry(Buffer_WriteF64(bin_buf, snapshot->value.S32));
                rmtTry(Buffer_WriteF64(bin_buf, snapshot->prevValue.S32));
                break;
            case RMT_PropertyType_rmtU32:
                rmtTry(Buffer_WriteF64(bin_buf, snapshot->value.U32));
                rmtTry(Buffer_WriteF64(bin_buf, snapshot->prevValue.U32));
                break;
            case RMT_PropertyType_rmtF32:
                rmtTry(Buffer_WriteF64(bin_buf, snapshot->value.F32));
                rmtTry(Buffer_WriteF64(bin_buf, snapshot->prevValue.F32));
                break;

            // The high end of these are not double representable but store their full pattern so we don't lose data
            case RMT_PropertyType_rmtS64:
            case RMT_PropertyType_rmtU64:
                rmtTry(Buffer_WriteU64(bin_buf, snapshot->value.U64));
                rmtTry(Buffer_WriteU64(bin_buf, snapshot->prevValue.U64));
                break;

            case RMT_PropertyType_rmtF64:
                rmtTry(Buffer_WriteF64(bin_buf, snapshot->value.F64));
                rmtTry(Buffer_WriteF64(bin_buf, snapshot->prevValue.F64));
                break;
        }

        rmtTry(Buffer_WriteU32(bin_buf, snapshot->prevValueFrame));
        rmtTry(Buffer_WriteU32(bin_buf, snapshot->nbChildren));
    }

    rmtTry(bin_MessageFooter(bin_buf, write_start_offset));

    return RMT_ERROR_NONE;
}

static rmtError Remotery_SendPropertySnapshot(Remotery* rmt, Message* message)
{
    Msg_PropertySnapshot* msg_snapshot = (Msg_PropertySnapshot*)message->payload;

    rmtError error = RMT_ERROR_NONE;

    Buffer* bin_buf;

    // Reset the buffer for sending a websocket message
    bin_buf = rmt->server->bin_buf;
    WebSocket_PrepareBuffer(bin_buf);

    // Serialise the message and send
    error = Remotery_SerialisePropertySnapshots(bin_buf, msg_snapshot);
    if (error == RMT_ERROR_NONE)
    {
        error = Remotery_SendToViewerAndLog(rmt, bin_buf, 50);
    }

    FreePropertySnapshots(msg_snapshot->rootSnapshot);

    return error;
}

static rmtError Remotery_ConsumeMessageQueue(Remotery* rmt)
{
    uint32_t nb_messages_sent = 0;
    const uint32_t maxNbMessagesPerUpdate = g_Settings.maxNbMessagesPerUpdate;

    assert(rmt != NULL);

    // Loop reading the max number of messages for this update
    // Note some messages don't consume the sent message count as they are small enough to not cause performance issues
    while (nb_messages_sent < maxNbMessagesPerUpdate)
    {
        rmtError error = RMT_ERROR_NONE;
        Message* message = rmtMessageQueue_PeekNextMessage(rmt->mq_to_rmt_thread);
        if (message == NULL)
            break;

        switch (message->id)
        {
            // This shouldn't be possible
            case MsgID_NotReady:
                assert(RMT_FALSE);
                break;

            // Dispatch to message handler
            case MsgID_AddToStringTable:
                error = Remotery_AddToStringTable(rmt, message);
                break;
            case MsgID_LogText:
                error = Remotery_SendLogTextMessage(rmt, message);
                nb_messages_sent++;
                break;
            case MsgID_SampleTree:
                rmt_BeginCPUSample(SendSampleTreeMessage, RMTSF_Aggregate);
                error = Remotery_SendSampleTreeMessage(rmt, message);
                nb_messages_sent++;
                rmt_EndCPUSample();
                break;
            case MsgID_ProcessorThreads:
                Remotery_SendProcessorThreads(rmt, message);
                nb_messages_sent++;
                break;
            case MsgID_PropertySnapshot:
                error = Remotery_SendPropertySnapshot(rmt, message);
                break;

            default:
                break;
        }

        // Consume the message before reacting to any errors
        rmtMessageQueue_ConsumeNextMessage(rmt->mq_to_rmt_thread, message);
        if (error != RMT_ERROR_NONE)
        {
            return error;
        }
    }

    return RMT_ERROR_NONE;
}

static void Remotery_FlushMessageQueue(Remotery* rmt)
{
    assert(rmt != NULL);

    // Loop reading all remaining messages
    for (;;)
    {
        Message* message = rmtMessageQueue_PeekNextMessage(rmt->mq_to_rmt_thread);
        if (message == NULL)
            break;

        switch (message->id)
        {
            // These can be safely ignored
            case MsgID_NotReady:
            case MsgID_AddToStringTable:
            case MsgID_LogText:
                break;

            // Release all samples back to their allocators
            case MsgID_SampleTree: {
                Msg_SampleTree* sample_tree = (Msg_SampleTree*)message->payload;
                FreeSamples(sample_tree->rootSample, sample_tree->allocator);
                break;
            }

            case MsgID_PropertySnapshot: {
                Msg_PropertySnapshot* msg_snapshot = (Msg_PropertySnapshot*)message->payload;
                FreePropertySnapshots(msg_snapshot->rootSnapshot);
                break;
            }

            default:
                break;
        }

        rmtMessageQueue_ConsumeNextMessage(rmt->mq_to_rmt_thread, message);
    }
}

static void Remotery_MapMessageQueue(Remotery* rmt)
{
    uint32_t read_pos, write_pos;
    rmtMessageQueue* queue;

    assert(rmt != NULL);

    // Wait until the caller sets the custom data
    while (LoadAcquirePointer((long* volatile*)&rmt->map_message_queue_data) == NULL)
        msSleep(1);

    // Snapshot the current write position so that we're not constantly chasing other threads
    // that can have no effect on the thread requesting the map.
    queue = rmt->mq_to_rmt_thread;
    write_pos = LoadAcquire(&queue->write_pos);

    // Walk every message in the queue and call the map function
    read_pos = queue->read_pos;
    while (read_pos < write_pos)
    {
        uint32_t r = read_pos & (queue->size - 1);
        Message* message = (Message*)(queue->data->ptr + r);
        uint32_t message_size = rmtMessageQueue_SizeForPayload(message->payload_size);
        rmt->map_message_queue_fn(rmt, message);
        read_pos += message_size;
    }

    StoreReleasePointer((long* volatile*)&rmt->map_message_queue_data, NULL);
}

static rmtError Remotery_ThreadMain(rmtThread* thread)
{
    Remotery* rmt = (Remotery*)thread->param;
    assert(rmt != NULL);

    rmt_SetCurrentThreadName("Remotery");

    while (thread->request_exit == RMT_FALSE)
    {
        rmt_BeginCPUSample(Wakeup, 0);

        rmt_BeginCPUSample(ServerUpdate, 0);
        Server_Update(rmt->server);
        rmt_EndCPUSample();

        rmt_BeginCPUSample(ConsumeMessageQueue, 0);
        Remotery_ConsumeMessageQueue(rmt);
        rmt_EndCPUSample();

        rmt_EndCPUSample();

        // Process any queue map requests
        if (LoadAcquirePointer((long* volatile*)&rmt->map_message_queue_fn) != NULL)
        {
            Remotery_MapMessageQueue(rmt);
            StoreReleasePointer((long* volatile*)&rmt->map_message_queue_fn, NULL);
        }

        //
        // [NOTE-A]
        //
        // Possible sequence of user events at this point:
        //
        //    1. Add samples to the queue.
        //    2. Shutdown remotery.
        //
        // This loop will exit with unrelease samples.
        //

        msSleep(g_Settings.msSleepBetweenServerUpdates);
    }

    // Release all samples to their allocators as a consequence of [NOTE-A]
    Remotery_FlushMessageQueue(rmt);

    return RMT_ERROR_NONE;
}

static rmtError Remotery_ReceiveMessage(void* context, char* message_data, uint32_t message_length)
{
    Remotery* rmt = (Remotery*)context;

// Manual dispatch on 4-byte message headers (message ID is little-endian encoded)
#define FOURCC(a, b, c, d) (uint32_t)(((d) << 24) | ((c) << 16) | ((b) << 8) | (a))
    uint32_t message_id = *(uint32_t*)message_data;

    switch (message_id)
    {
        case FOURCC('C', 'O', 'N', 'I'): {
            rmt_LogText("Console message received...");
            rmt_LogText(message_data + 4);

            // Pass on to any registered handler
            if (g_Settings.input_handler != NULL)
                g_Settings.input_handler(message_data + 4, g_Settings.input_handler_context);

            break;
        }

        case FOURCC('G', 'S', 'M', 'P'): {
            rmtPStr name;

            // Convert name hash to integer
            uint32_t name_hash = 0;
            const char* cur = message_data + 4;
            const char* end = cur + message_length - 4;
            while (cur < end)
                name_hash = name_hash * 10 + *cur++ - '0';

            // Search for a matching string hash
            name = StringTable_Find(rmt->string_table, name_hash);
            if (name != NULL)
            {
                uint32_t name_length = (uint32_t)strnlen_s_safe_c(name, 256 - 12);

                // Construct a response message containing the matching name
                Buffer* bin_buf = rmt->server->bin_buf;
                WebSocket_PrepareBuffer(bin_buf);
                bin_SampleName(bin_buf, name, name_hash, name_length);

                // Send back immediately as we're on the server thread
                return Server_Send(rmt->server, bin_buf->data, bin_buf->bytes_used, 10);
            }

            break;
        }
    }

#undef FOURCC

    return RMT_ERROR_NONE;
}

static rmtError Remotery_Constructor(Remotery* rmt)
{
    assert(rmt != NULL);

    // Set default state
    rmt->server = NULL;
    rmt->mq_to_rmt_thread = NULL;
    rmt->thread = NULL;
    rmt->string_table = NULL;
    rmt->logfile = NULL;
    rmt->map_message_queue_fn = NULL;
    rmt->map_message_queue_data = NULL;
    rmt->threadProfilers = NULL;
    mtxInit(&rmt->propertyMutex);
    rmt->propertyAllocator = NULL;
    rmt->propertyFrame = 0;

    // Set default state on the root property
    rmtProperty* root_property = &rmt->rootProperty;
    root_property->initialised = RMT_TRUE;
    root_property->type = RMT_PropertyType_rmtGroup;
    root_property->value.Bool = RMT_FALSE;
    root_property->flags = RMT_PropertyFlags_NoFlags;
    root_property->name = "Root Property";
    root_property->description = "";
    root_property->defaultValue.Bool = RMT_FALSE;
    root_property->parent = NULL;
    root_property->firstChild = NULL;
    root_property->lastChild = NULL;
    root_property->nextSibling = NULL;
    root_property->nameHash = 0;
    root_property->uniqueID = 0;






    // Kick-off the timer
    usTimer_Init(&rmt->timer);

    // Create the server
    rmtTryNew(Server, rmt->server, g_Settings.port, g_Settings.reuse_open_port, g_Settings.limit_connections_to_localhost);

    // Setup incoming message handler
    rmt->server->receive_handler = Remotery_ReceiveMessage;
    rmt->server->receive_handler_context = rmt;

    // Create the main message thread with only one page
    rmtTryNew(rmtMessageQueue, rmt->mq_to_rmt_thread, g_Settings.messageQueueSizeInBytes);

    // Create sample name string table
    rmtTryNew(StringTable, rmt->string_table);

    if (g_Settings.logPath != NULL)
    {
        // Get current date/time
        struct tm* now_tm = TimeDateNow();

        // Start the log path off
        char filename[512] = { 0 };
        strncat_s(filename, sizeof(filename), g_Settings.logPath, 512);
        strncat_s(filename, sizeof(filename), "/remotery-log-", 14);

        // Append current date and time
        strncat_s(filename, sizeof(filename), itoa_s(now_tm->tm_year + 1900), 11);
        {
            int fields[5];
            int field_index;
            fields[0] = now_tm->tm_mon + 1;
            fields[1] = now_tm->tm_mday;
            fields[2] = now_tm->tm_hour;
            fields[3] = now_tm->tm_min;
            fields[4] = now_tm->tm_sec;
            for (field_index = 0; field_index < 5; field_index++)
            {
                strncat_s(filename, sizeof(filename), fields[field_index] < 10 ? "-0" : "-", 2);
                strncat_s(filename, sizeof(filename), itoa_s(fields[field_index]), 11);
            }
        }

        // Just append a custom extension
        strncat_s(filename, sizeof(filename), ".rbin", 5);

        // Open and assume any failure simply sets NULL and the file isn't written
        rmt->logfile = rmtOpenFile(filename, "wb");

        // Write the header
        if (rmt->logfile != NULL)
        {
            rmtWriteFile(rmt->logfile, "RMTBLOGF", 8);
        }
    }



    // Create the thread profilers container
    rmtTryNew(ThreadProfilers, rmt->threadProfilers, &rmt->timer, rmt->mq_to_rmt_thread);

    // Create the property state allocator
    rmtTryNew(ObjectAllocator, rmt->propertyAllocator, sizeof(PropertySnapshot), (ObjConstructor)PropertySnapshot_Constructor, (ObjDestructor)PropertySnapshot_Destructor);

    // Set as the global instance before creating any threads that uses it for sampling itself
    assert(g_Remotery == NULL);
    g_Remotery = rmt;
    g_RemoteryCreated = RMT_TRUE;
    g_Remotery->countThreads = 0;

    // Ensure global instance writes complete before other threads get a chance to use it
    CompilerWriteFence();

    // Create the main update thread once everything has been defined for the global remotery object
    rmtTryNew(rmtThread, rmt->thread, Remotery_ThreadMain, rmt);

    return RMT_ERROR_NONE;
}

static void Remotery_Destructor(Remotery* rmt)
{
    assert(rmt != NULL);


    // Join the remotery thread before clearing the global object as the thread is profiling itself
    rmtDelete(rmtThread, rmt->thread);

    rmtDelete(ThreadProfilers, rmt->threadProfilers);

    rmtDelete(ObjectAllocator, rmt->propertyAllocator);




    if (g_RemoteryCreated)
    {
        g_Remotery = NULL;
        g_RemoteryCreated = RMT_FALSE;
    }

    rmtCloseFile(rmt->logfile);

    rmtDelete(StringTable, rmt->string_table);
    rmtDelete(rmtMessageQueue, rmt->mq_to_rmt_thread);

    rmtDelete(Server, rmt->server);
    
    // Free the error message TLS
    // TODO(don): The allocated messages will need to be freed as well
    if (g_lastErrorMessageTlsHandle != TLS_INVALID_HANDLE)
    {
        tlsFree(g_lastErrorMessageTlsHandle);
        g_lastErrorMessageTlsHandle = TLS_INVALID_HANDLE;
    }
    
    mtxDelete(&rmt->propertyMutex);
}

static void* CRTMalloc(void* mm_context, uint32_t size)
{
    RMT_UNREFERENCED_PARAMETER(mm_context);
    return malloc((size_t)size);
}

static void CRTFree(void* mm_context, void* ptr)
{
    RMT_UNREFERENCED_PARAMETER(mm_context);
    free(ptr);
}

static void* CRTRealloc(void* mm_context, void* ptr, uint32_t size)
{
    RMT_UNREFERENCED_PARAMETER(mm_context);
    return realloc(ptr, size);
}

RMT_API rmtSettings* _rmt_Settings(void)
{
    // Default-initialize on first call
    if (g_SettingsInitialized == RMT_FALSE)
    {
        g_Settings.port = 0x4597;
        g_Settings.reuse_open_port = RMT_FALSE;
        g_Settings.limit_connections_to_localhost = RMT_FALSE;
        g_Settings.enableThreadSampler = RMT_TRUE;
        g_Settings.msSleepBetweenServerUpdates = 4;
        g_Settings.messageQueueSizeInBytes = 1024 * 1024;
        g_Settings.maxNbMessagesPerUpdate = 1000;
        g_Settings.malloc = CRTMalloc;
        g_Settings.free = CRTFree;
        g_Settings.realloc = CRTRealloc;
        g_Settings.input_handler = NULL;
        g_Settings.input_handler_context = NULL;
        g_Settings.logPath = NULL;
        g_Settings.sampletree_handler = NULL;
        g_Settings.sampletree_context = NULL;
        g_Settings.snapshot_callback = NULL;
        g_Settings.snapshot_context = NULL;

        g_SettingsInitialized = RMT_TRUE;
    }

    return &g_Settings;
}

RMT_API rmtError _rmt_CreateGlobalInstance(Remotery** remotery)
{
    // Ensure load/acquire store/release operations match this enum size
    assert(sizeof(MessageID) == sizeof(uint32_t));

    // Default-initialise if user has not set values
    rmt_Settings();

    // Creating the Remotery instance also records it as the global instance
    assert(remotery != NULL);
    rmtTryNew(Remotery, *remotery);
    return RMT_ERROR_NONE;
}

RMT_API void _rmt_DestroyGlobalInstance(Remotery* remotery)
{
    // Ensure this is the module that created it
    assert(g_RemoteryCreated == RMT_TRUE);
    assert(g_Remotery == remotery);
    rmtDelete(Remotery, remotery);
}

RMT_API void _rmt_SetGlobalInstance(Remotery* remotery)
{
    // Default-initialise if user has not set values
    rmt_Settings();

    g_Remotery = remotery;
}

RMT_API Remotery* _rmt_GetGlobalInstance(void)
{
    return g_Remotery;
}

#ifdef RMT_PLATFORM_WINDOWS
#pragma pack(push, 8)
typedef struct tagTHREADNAME_INFO
{
    DWORD dwType;     // Must be 0x1000.
    LPCSTR szName;    // Pointer to name (in user addr space).
    DWORD dwThreadID; // Thread ID (-1=caller thread).
    DWORD dwFlags;    // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)
#endif

#ifdef RMT_PLATFORM_WINDOWS
static wchar_t* MakeWideString(const char* string)
{
    int wlen;
    wchar_t* wstr;

    wlen = MultiByteToWideChar(CP_UTF8, 0, string, -1, NULL, 0);
    if (wlen <= 0)
    {
        return NULL;
    }

    wstr = (wchar_t*)(rmtMalloc((uint32_t)wlen * sizeof(wchar_t)));
    if (wstr == NULL)
    {
        return NULL;
    }

    if (MultiByteToWideChar(CP_UTF8, 0, string, -1, wstr, wlen) == 0)
    {
        rmtFree(wstr);
        return NULL;
    }

    return wstr;
}
#endif

static void SetDebuggerThreadName(const char* name)
{
#ifdef RMT_PLATFORM_WINDOWS
#ifndef __MINGW32__
    THREADNAME_INFO info;
#endif

    // See if SetThreadDescription is available in this version of Windows
    // Introduced in Windows 10 build 1607
    HMODULE kernel32 = GetModuleHandleA("Kernel32.dll");
    if (kernel32 != NULL)
    {
        typedef HRESULT(WINAPI* SETTHREADDESCRIPTION)(HANDLE hThread, PCWSTR lpThreadDescription);
        SETTHREADDESCRIPTION SetThreadDescription = (SETTHREADDESCRIPTION)GetProcAddress(kernel32, "SetThreadDescription");
        if (SetThreadDescription != NULL)
        {
            // Create a wide-string version of the thread name
            wchar_t* wstr = MakeWideString(name);
            if (wstr != NULL)
            {
                // Set and return, leaving a fall-through for any failure cases to use the old exception method
                SetThreadDescription(GetCurrentThread(), wstr);
                rmtFree(wstr);
                return;
            }
        }
    }

#ifndef __MINGW32__
    info.dwType = 0x1000;
    info.szName = name;
    info.dwThreadID = (DWORD)-1;
    info.dwFlags = 0;

    __try
    {
        RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    __except (1 /* EXCEPTION_EXECUTE_HANDLER */)
    {
    }
#endif
#elif defined(RMT_PLATFORM_MACOS)
    pthread_setname_np(name);
#else
    RMT_UNREFERENCED_PARAMETER(name);
#endif

#ifdef RMT_PLATFORM_LINUX
    // pthread_setname_np is a non-standard GNU extension.
    char name_clamp[16];
    name_clamp[0] = 0;
    strncat_s(name_clamp, sizeof(name_clamp), name, 15);
#if defined(__FreeBSD__) || defined(__OpenBSD__)
    pthread_set_name_np(pthread_self(), name_clamp);
#else
    prctl(PR_SET_NAME, name_clamp, 0, 0, 0);
#endif
#endif
}

RMT_API void _rmt_SetCurrentThreadName(rmtPStr thread_name)
{
    ThreadProfiler* thread_profiler;

    if (g_Remotery == NULL)
    {
        return;
    }

    // Get data for this thread
    if (ThreadProfilers_GetCurrentThreadProfiler(g_Remotery->threadProfilers, &thread_profiler) != RMT_ERROR_NONE)
    {
        return;
    }

    // Copy name and apply to the debugger
    strcpy_s(thread_profiler->threadName, sizeof(thread_profiler->threadName), thread_name);
    thread_profiler->threadNameHash = _rmt_HashString32(thread_name, strnlen_s(thread_name, 64), 0);
    SetDebuggerThreadName(thread_name);

    // Send the thread name for lookup
#if defined(RMT_PLATFORM_WINDOWS) || defined(RMT_PLATFORM_MACOS)
    {
        uint32_t name_length = strnlen_s(thread_profiler->threadName, 64);
        QueueAddToStringTable(g_Remotery->mq_to_rmt_thread, thread_profiler->threadNameHash, thread_name, name_length, NULL);
    }
#endif
}

static rmtBool QueueLine(rmtMessageQueue* queue, uint8_t* text, uint32_t size, struct ThreadProfiler* thread_profiler)
{
    Message* message;
    uint32_t text_size;

    assert(queue != NULL);

    // Patch line size
    text_size = size - 4;
    U32ToByteArray(text, text_size);

    // Allocate some space for the line
    message = rmtMessageQueue_AllocMessage(queue, size, thread_profiler);
    if (message == NULL)
        return RMT_FALSE;

    // Copy the text and commit the message
    memcpy(message->payload, text, size);
    rmtMessageQueue_CommitMessage(message, MsgID_LogText);

    return RMT_TRUE;
}

RMT_API void _rmt_LogText(rmtPStr text)
{
    int start_offset, offset, i;
    uint8_t line_buffer[1024] = {0};
    ThreadProfiler* thread_profiler;

    if (g_Remotery == NULL)
        return;

    if (ThreadProfilers_GetCurrentThreadProfiler(g_Remotery->threadProfilers, &thread_profiler) != RMT_ERROR_NONE)
    {
        return;
    }

    // Start with empty line size
    // Fill with spaces to enable viewing line_buffer without offset in a debugger
    // (will be overwritten later by QueueLine/rmtMessageQueue_AllocMessage)
    line_buffer[0] = ' ';
    line_buffer[1] = ' ';
    line_buffer[2] = ' ';
    line_buffer[3] = ' ';
    start_offset = 4;

    // There might be newlines in the buffer, so split them into multiple network calls
    offset = start_offset;
    for (i = 0; text[i] != 0; i++)
    {
        char c = text[i];

        // Line wrap when too long or newline encountered
        if (offset == sizeof(line_buffer) - 1 || c == '\n')
        {
            // Send the line up to now
            if (QueueLine(g_Remotery->mq_to_rmt_thread, line_buffer, offset, thread_profiler) == RMT_FALSE)
                return;

            // Restart line
            offset = start_offset;

            // Don't add the newline character (if this was the reason for the flush)
            // to the restarted line_buffer, let's skip it
            if (c == '\n')
                continue;
        }

        line_buffer[offset++] = c;
    }

    // Send the last line
    if (offset > start_offset)
    {
        assert(offset < (int)sizeof(line_buffer));
        QueueLine(g_Remotery->mq_to_rmt_thread, line_buffer, offset, thread_profiler);
    }
}

RMT_API void _rmt_BeginCPUSample(rmtPStr name, uint32_t flags, uint32_t* hash_cache)
{
    // 'hash_cache' stores a pointer to a sample name's hash value. Internally this is used to identify unique
    // callstacks and it would be ideal that it's not recalculated each time the sample is used. This can be statically
    // cached at the point of call or stored elsewhere when dynamic names are required.
    //
    // If 'hash_cache' is NULL then this call becomes more expensive, as it has to recalculate the hash of the name.

    ThreadProfiler* thread_profiler;

    if (g_Remotery == NULL)
        return;

    // TODO: Time how long the bits outside here cost and subtract them from the parent

    if (ThreadProfilers_GetCurrentThreadProfiler(g_Remotery->threadProfilers, &thread_profiler) == RMT_ERROR_NONE)
    {
        Sample* sample;
        uint32_t name_hash = ThreadProfiler_GetNameHash(thread_profiler, g_Remotery->mq_to_rmt_thread, name, hash_cache);
        if (ThreadProfiler_Push(thread_profiler->sampleTrees[RMT_SampleType_CPU], name_hash, flags, &sample) == RMT_ERROR_NONE)
        {
            // If this is an aggregate sample, store the time in 'end' as we want to preserve 'start'
            if (sample->call_count > 1)
                sample->us_end = usTimer_Get(&g_Remotery->timer);
            else
                sample->us_start = usTimer_Get(&g_Remotery->timer);
        }
    }
}

RMT_API void _rmt_EndCPUSample(void)
{
    ThreadProfiler* thread_profiler;

    if (g_Remotery == NULL)
        return;

    if (ThreadProfilers_GetCurrentThreadProfiler(g_Remotery->threadProfilers, &thread_profiler) == RMT_ERROR_NONE)
    {
        Sample* sample = thread_profiler->sampleTrees[RMT_SampleType_CPU]->currentParent;

        if (sample->recurse_depth > 0)
        {
            sample->recurse_depth--;
        }
        else
        {
            uint64_t us_end = usTimer_Get(&g_Remotery->timer);
            Sample_Close(sample, us_end);
            ThreadProfiler_Pop(thread_profiler, g_Remotery->mq_to_rmt_thread, sample, 0);
        }
    }
}



RMT_API rmtError _rmt_MarkFrame(void)
{
    if (g_Remotery == NULL)
    {
        return RMT_ERROR_REMOTERY_NOT_CREATED;
    }

    return RMT_ERROR_NONE;
}


/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
@SAMPLEAPI: Sample API for user callbacks
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

// Iterator
RMT_API void _rmt_IterateChildren(rmtSampleIterator* iterator, rmtSample* sample)
{
    iterator->sample = 0;
    iterator->initial = sample != NULL ? sample->first_child : 0;
}

RMT_API rmtBool _rmt_IterateNext(rmtSampleIterator* iter)
{
    if (iter->initial != NULL)
    {
        iter->sample = iter->initial;
        iter->initial = 0;
    }
    else
    {
        if (iter->sample != NULL)
            iter->sample = iter->sample->next_sibling;
    }

    return iter->sample != NULL ? RMT_TRUE : RMT_FALSE;
}

// Sample tree accessors
RMT_API const char* _rmt_SampleTreeGetThreadName(rmtSampleTree* sample_tree)
{
    return sample_tree->threadName;
}

RMT_API rmtSample* _rmt_SampleTreeGetRootSample(rmtSampleTree* sample_tree)
{
    return sample_tree->rootSample;
}

// Sample accessors
RMT_API const char* _rmt_SampleGetName(rmtSample* sample)
{
    const char* name = StringTable_Find(g_Remotery->string_table, sample->name_hash);
    if (name == NULL)
    {
        return "null";
    }
    return name;
}

RMT_API uint32_t _rmt_SampleGetNameHash(rmtSample* sample)
{
    return sample->name_hash;
}

RMT_API uint32_t _rmt_SampleGetCallCount(rmtSample* sample)
{
    return sample->call_count;
}

RMT_API uint64_t _rmt_SampleGetStart(rmtSample* sample)
{
    return sample->us_start;
}

RMT_API uint64_t _rmt_SampleGetTime(rmtSample* sample)
{
    return sample->us_length;
}

RMT_API uint64_t _rmt_SampleGetSelfTime(rmtSample* sample)
{
    return (uint64_t)maxS64(sample->us_length - sample->us_sampled_length, 0);
}

RMT_API rmtSampleType _rmt_SampleGetType(rmtSample* sample)
{
    return sample->type;
}

RMT_API void _rmt_SampleGetColour(rmtSample* sample, uint8_t* r, uint8_t* g, uint8_t* b)
{
    *r = sample->uniqueColour[0];
    *g = sample->uniqueColour[1];
    *b = sample->uniqueColour[2];
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
@PROPERTYAPI: Property API for user callbacks
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

// Iterator
RMT_API void _rmt_PropertyIterateChildren(rmtPropertyIterator* iterator, rmtProperty* property)
{
    iterator->property = 0;
    iterator->initial = property != NULL ? property->firstChild : 0;
}

RMT_API rmtBool _rmt_PropertyIterateNext(rmtPropertyIterator* iter)
{
    if (iter->initial != NULL)
    {
        iter->property = iter->initial;
        iter->initial = 0;
    }
    else
    {
        if (iter->property != NULL)
            iter->property = iter->property->nextSibling;
    }

    return iter->property != NULL ? RMT_TRUE : RMT_FALSE;
}

// Property accessors
RMT_API const char* _rmt_PropertyGetName(rmtProperty* property)
{
    return property->name;
}

RMT_API const char* _rmt_PropertyGetDescription(rmtProperty* property)
{
    return property->description;
}

RMT_API uint32_t _rmt_PropertyGetNameHash(rmtProperty* property)
{
    return property->nameHash;
}

RMT_API rmtPropertyType _rmt_PropertyGetType(rmtProperty* property)
{
    return property->type;
}

RMT_API rmtPropertyValue _rmt_PropertyGetValue(rmtProperty* property)
{
    return property->value;
}

/*
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
@PROPERTIES: Property API
------------------------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------------------------
*/

static void RegisterProperty(rmtProperty* property, rmtBool can_lock)
{
    if (property->initialised == RMT_FALSE)
    {
        // Apply for a lock once at the start of the recursive walk
        if (can_lock)
        {
            mtxLock(&g_Remotery->propertyMutex);
        }

        // Multiple threads accessing the same property can apply for the lock at the same time as the `initialised` property for
        // each of them may not be set yet. One thread only will get the lock successfully while the others will only come through
        // here when the first thread has finished initialising. The first thread through will have `initialised` set to RMT_FALSE
        // while all other threads will see it in its initialised state. Skip those so that we don't register multiple times.
        if (property->initialised == RMT_FALSE)
        {
            uint32_t name_len;

            // With no parent, add this to the root property
            rmtProperty* parent_property = property->parent;
            if (parent_property == NULL)
            {
                property->parent = &g_Remotery->rootProperty;
                parent_property = property->parent;
            }

            // Walk up to parent properties first in case they haven't been registered
            RegisterProperty(parent_property, RMT_FALSE);

            // Link this property into the parent's list
            if (parent_property->firstChild != NULL)
            {
                parent_property->lastChild->nextSibling = property;
                parent_property->lastChild = property;
            }
            else
            {
                parent_property->firstChild = property;
                parent_property->lastChild = property;
            }

            // Calculate the name hash and send it to the viewer
            name_len = strnlen_s(property->name, 256);
            property->nameHash = _rmt_HashString32(property->name, name_len, 0);
            QueueAddToStringTable(g_Remotery->mq_to_rmt_thread, property->nameHash, property->name, name_len, NULL);

            // Generate a unique ID for this property in the tree
            property->uniqueID = parent_property->uniqueID;
            property->uniqueID = HashCombine(property->uniqueID, property->nameHash);

            property->initialised = RMT_TRUE;
        }

        // Unlock on the way out of recursive walk
        if (can_lock)
        {
            mtxUnlock(&g_Remotery->propertyMutex);
        }
    }
}

RMT_API void _rmt_PropertySetValue(rmtProperty* property)
{
    if (g_Remotery == NULL)
    {
        return;
    }

    RegisterProperty(property, RMT_TRUE);

    // on this thread, create a new sample that encodes the value just set

    // send the sample to remotery UI and disk log

    // value resets and sets don't have delta values, really
}

RMT_API void _rmt_PropertyAddValue(rmtProperty* property, rmtPropertyValue add_value)
{
    if (g_Remotery == NULL)
    {
        return;
    }

    RegisterProperty(property, RMT_TRUE);

    RMT_UNREFERENCED_PARAMETER(add_value);

    // use `add_value` to determine how much this property was changed

    // on this thread, create a new sample that encodes the delta and parents itself to `property`
    // could also encode the current value of the property at this point

    // send the sample to remotery UI and disk log
}

static rmtError TakePropertySnapshot(rmtProperty* property, PropertySnapshot* parent_snapshot, PropertySnapshot** first_snapshot, PropertySnapshot** prev_snapshot, uint32_t depth)
{
    rmtError error;
    rmtProperty* child_property;

    // Allocate some state for the property
    PropertySnapshot* snapshot;
    error = ObjectAllocator_Alloc(g_Remotery->propertyAllocator, (void**)&snapshot);
    if (error != RMT_ERROR_NONE)
    {
        return error;
    }

    // Snapshot the property
    snapshot->type = property->type;
    snapshot->value = property->value;
    snapshot->prevValue = property->prevValue;
    snapshot->prevValueFrame = property->prevValueFrame;
    snapshot->nameHash = property->nameHash;
    snapshot->uniqueID = property->uniqueID;
    snapshot->nbChildren = 0;
    snapshot->depth = (uint8_t)depth;
    snapshot->nextSnapshot = NULL;

    // Keep count of the number of children in the parent
    if (parent_snapshot != NULL)
    {
        parent_snapshot->nbChildren++;
    }

    // Link into the linear list
    if (*first_snapshot == NULL)
    {
        *first_snapshot = snapshot;
    }
    if (*prev_snapshot != NULL)
    {
        (*prev_snapshot)->nextSnapshot = snapshot;
    }
    *prev_snapshot = snapshot;

    // Snapshot the children
    for (child_property = property->firstChild; child_property != NULL; child_property = child_property->nextSibling)
    {
        error = TakePropertySnapshot(child_property, snapshot, first_snapshot, prev_snapshot, depth + 1);
        if (error != RMT_ERROR_NONE)
        {
            return error;
        }
    }

    return RMT_ERROR_NONE;
}

RMT_API rmtError _rmt_PropertySnapshotAll()
{
    rmtError error;
    PropertySnapshot* first_snapshot;
    PropertySnapshot* prev_snapshot;
    Msg_PropertySnapshot* payload;
    Message* message;
    uint32_t nb_snapshot_allocs;

    if (g_Remotery == NULL)
    {
        return RMT_ERROR_REMOTERY_NOT_CREATED;
    }

    // Don't do anything if any properties haven't been registered yet
    if (g_Remotery->rootProperty.firstChild == NULL)
    {
        return RMT_ERROR_NONE;
    }

    // Mark current allocation count so we can quickly calculate the number of snapshots being sent
    nb_snapshot_allocs = g_Remotery->propertyAllocator->nb_inuse;
    
    // Snapshot from the root into a linear list
    first_snapshot = NULL;
    prev_snapshot = NULL;
    mtxLock(&g_Remotery->propertyMutex);
    error = TakePropertySnapshot(&g_Remotery->rootProperty, NULL, &first_snapshot, &prev_snapshot, 0);

    if (g_Settings.snapshot_callback != NULL)
    {
        g_Settings.snapshot_callback(g_Settings.snapshot_context, &g_Remotery->rootProperty);
    }

    mtxUnlock(&g_Remotery->propertyMutex);
    if (error != RMT_ERROR_NONE)
    {
        FreePropertySnapshots(first_snapshot);
        return error;
    }

    // Attempt to allocate a message for sending the snapshot to the viewer
    message = rmtMessageQueue_AllocMessage(g_Remotery->mq_to_rmt_thread, sizeof(Msg_PropertySnapshot), NULL);
    if (message == NULL)
    {
        FreePropertySnapshots(first_snapshot);
        return RMT_ERROR_UNKNOWN;
    }

    // Populate and commit
    payload = (Msg_PropertySnapshot*)message->payload;
    payload->rootSnapshot = first_snapshot;
    payload->nbSnapshots = g_Remotery->propertyAllocator->nb_inuse - nb_snapshot_allocs;
    payload->propertyFrame = g_Remotery->propertyFrame;
    rmtMessageQueue_CommitMessage(message, MsgID_PropertySnapshot);

    return RMT_ERROR_NONE;
}

static void PropertyFrameReset(Remotery* rmt, rmtProperty* first_property)
{
    rmtProperty* property;
    for (property = first_property; property != NULL; property = property->nextSibling)
    {
        PropertyFrameReset(rmt, property->firstChild);

        // TODO(don): It might actually be quicker to sign-extend assignments but this gives me a nice debug hook for now
        rmtBool changed = RMT_FALSE;
        switch (property->type)
        {
            case RMT_PropertyType_rmtGroup:
                break;

            case RMT_PropertyType_rmtBool:
                changed = property->lastFrameValue.Bool != property->value.Bool;
                break;

            case RMT_PropertyType_rmtS32:
            case RMT_PropertyType_rmtU32:
            case RMT_PropertyType_rmtF32:
                changed = property->lastFrameValue.U32 != property->value.U32;
                break;

            case RMT_PropertyType_rmtS64:
            case RMT_PropertyType_rmtU64:
            case RMT_PropertyType_rmtF64:
                changed = property->lastFrameValue.U64 != property->value.U64;
                break;
        }

        if (changed)
        {
            property->prevValue = property->lastFrameValue;
            property->prevValueFrame = rmt->propertyFrame;
        }

        property->lastFrameValue = property->value;

        if ((property->flags & RMT_PropertyFlags_FrameReset) != 0)
        {
            property->value = property->defaultValue;
        }
    }
}

RMT_API void _rmt_PropertyFrameResetAll()
{
    if (g_Remotery == NULL)
    {
        return;
    }

    mtxLock(&g_Remotery->propertyMutex);
    PropertyFrameReset(g_Remotery, g_Remotery->rootProperty.firstChild);
    mtxUnlock(&g_Remotery->propertyMutex);

    g_Remotery->propertyFrame++;
}

#endif // RMT_ENABLED
