
/*
Copyright 2014-2022 Celtoys Ltd

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

/*
Modified for EDGE-Classic (https://github.com/scramshackle/EDGE-classic), 2026-10-08.
Based on upstream https://github.com/Celtoys/Remotery commit 5483f65b60334059933ff82464d8ea6882d20073 (2024-08-28).

Changes from upstream:
- Removed CUDA, Direct3D 11, Direct3D 12, Metal, OpenGL and Vulkan GPU sampling and their public API.
- RMT_ASSUME_LITTLE_ENDIAN defaults to 1.
- Removed Xbox One, Android and TinyCRT support.
- Removed the upstream compiling instructions (they referred to lib/ and sample/ paths not shipped here).
- Public types use <stdint.h> fixed-width types; the rmtU8..rmtS64/rmtF32/rmtF64 aliases are removed.
- See Remotery.c for the full list of changes.
*/

/*
You can define some extra macros to modify what features are compiled into Remotery. These are
documented just below this comment.
*/


#ifndef RMT_INCLUDED_H
#define RMT_INCLUDED_H


// Set to 0 to not include any bits of Remotery in your build
#ifndef RMT_ENABLED
#define RMT_ENABLED 1
#endif

// Help performance of the server sending data to the client by marking this machine as little-endian 
#ifndef RMT_ASSUME_LITTLE_ENDIAN
#define RMT_ASSUME_LITTLE_ENDIAN 1
#endif

// Initially use POSIX thread names to name threads instead of Thread0, 1, ...
#ifndef RMT_USE_POSIX_THREADNAMES
#define RMT_USE_POSIX_THREADNAMES 0
#endif

// If RMT_USE_INTERNAL_HASH_FUNCTION is defined to 1, the internal hash function for strings is used.
// This is the default setting.
// If you set RMT_USE_INTERNAL_HASH_FUNCTION to 0, you must implement rmt_HashString32 yourself.
#ifndef RMT_USE_INTERNAL_HASH_FUNCTION
#define RMT_USE_INTERNAL_HASH_FUNCTION 1
#endif

// If RMT_USE_LEGACY_ATOMICS is defined to 1, the implementation will use the legacy fallback atomic functions
// The default setting is 0
#ifndef RMT_USE_LEGACY_ATOMICS
#define RMT_USE_LEGACY_ATOMICS 0
#endif

/*--------------------------------------------------------------------------------------------------------------------------------
   Compiler/Platform Detection and Preprocessor Utilities
---------------------------------------------------------------------------------------------------------------------------------*/


// Platform identification
#if defined(_WINDOWS) || defined(_WIN32)
    #define RMT_PLATFORM_WINDOWS
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    #define RMT_PLATFORM_LINUX
    #define RMT_PLATFORM_POSIX
#elif defined(__APPLE__)
    #define RMT_PLATFORM_MACOS
    #define RMT_PLATFORM_POSIX
#endif

// Architecture identification
#ifdef RMT_PLATFORM_WINDOWS
#if defined(_M_AMD64) || defined(__x86_64__) // MSVC defines _M_AMD64 and MinGW-64 defines __x86_64__
#define RMT_ARCH_64BIT
#else
#define RMT_ARCH_32BIT
#endif
#endif

#if __GNUC__ || __clang__
#if __x86_64__ || __ppc64__ || __amd64__ || __arm64__
#define RMT_ARCH_64BIT
#else
#define RMT_ARCH_32BIT
#endif
#endif


#ifdef RMT_DLL
    #if defined (RMT_PLATFORM_WINDOWS)
        #if defined (RMT_IMPL)
            #define RMT_API __declspec(dllexport)
        #else
            #define RMT_API __declspec(dllimport)
        #endif
    #elif defined (RMT_PLATFORM_POSIX)
        #if defined (RMT_IMPL)
            #define RMT_API __attribute__((visibility("default")))
        #else
            #define RMT_API
        #endif
    #endif
#else
    #define RMT_API
#endif

// Allows macros to be written that can work around the inability to do: #define(x) #ifdef x
// with the C preprocessor.
#if RMT_ENABLED
    #define IFDEF_RMT_ENABLED(t, f) t
#else
    #define IFDEF_RMT_ENABLED(t, f) f
#endif


// Public interface is written in terms of these macros to easily enable/disable itself
#define RMT_OPTIONAL(macro, x) IFDEF_ ## macro(x, )
#define RMT_OPTIONAL_RET(macro, x, y) IFDEF_ ## macro(x, (y))


/*--------------------------------------------------------------------------------------------------------------------------------
   Types
--------------------------------------------------------------------------------------------------------------------------------*/


#include <stdint.h>

// Boolean
typedef uint32_t rmtBool;
#define RMT_TRUE ((rmtBool)1)
#define RMT_FALSE ((rmtBool)0)

// Const, null-terminated string pointer
typedef const char* rmtPStr;

// Opaque pointer for a sample graph tree
typedef struct Msg_SampleTree rmtSampleTree;

// Opaque pointer to a node in the sample graph tree
typedef struct Sample rmtSample;

// Handle to the main remotery instance
typedef struct Remotery Remotery;

// Forward declaration
struct rmtProperty;

typedef enum rmtSampleType
{
    RMT_SampleType_CPU,
    RMT_SampleType_Count,
} rmtSampleType;

// All possible error codes
// clang-format off
typedef enum rmtError
{
    RMT_ERROR_NONE,
    RMT_ERROR_RECURSIVE_SAMPLE,                 // Not an error but an internal message to calling code
    RMT_ERROR_UNKNOWN,                          // An error with a message yet to be defined, only for internal error handling
    RMT_ERROR_INVALID_INPUT,                    // An invalid input to a function call was provided
    RMT_ERROR_RESOURCE_CREATE_FAIL,             // Creation of an internal resource failed
    RMT_ERROR_RESOURCE_ACCESS_FAIL,             // Access of an internal resource failed
    RMT_ERROR_TIMEOUT,                          // Internal system timeout

    // System errors
    RMT_ERROR_MALLOC_FAIL,                      // Malloc call within remotery failed
    RMT_ERROR_TLS_ALLOC_FAIL,                   // Attempt to allocate thread local storage failed
    RMT_ERROR_VIRTUAL_MEMORY_BUFFER_FAIL,       // Failed to create a virtual memory mirror buffer
    RMT_ERROR_CREATE_THREAD_FAIL,               // Failed to create a thread for the server
    RMT_ERROR_OPEN_THREAD_HANDLE_FAIL,          // Failed to open a thread handle, given a thread id

    // Network TCP/IP socket errors
    RMT_ERROR_SOCKET_INVALID_POLL,              // Poll attempt on an invalid socket
    RMT_ERROR_SOCKET_SELECT_FAIL,               // Server failed to call select on socket
    RMT_ERROR_SOCKET_POLL_ERRORS,               // Poll notified that the socket has errors
    RMT_ERROR_SOCKET_SEND_FAIL,                 // Unrecoverable error occured while client/server tried to send data
    RMT_ERROR_SOCKET_RECV_NO_DATA,              // No data available when attempting a receive
    RMT_ERROR_SOCKET_RECV_TIMEOUT,              // Timed out trying to receive data
    RMT_ERROR_SOCKET_RECV_FAILED,               // Unrecoverable error occured while client/server tried to receive data

    // WebSocket errors
    RMT_ERROR_WEBSOCKET_HANDSHAKE_NOT_GET,      // WebSocket server handshake failed, not HTTP GET
    RMT_ERROR_WEBSOCKET_HANDSHAKE_NO_VERSION,   // WebSocket server handshake failed, can't locate WebSocket version
    RMT_ERROR_WEBSOCKET_HANDSHAKE_BAD_VERSION,  // WebSocket server handshake failed, unsupported WebSocket version
    RMT_ERROR_WEBSOCKET_HANDSHAKE_NO_HOST,      // WebSocket server handshake failed, can't locate host
    RMT_ERROR_WEBSOCKET_HANDSHAKE_BAD_HOST,     // WebSocket server handshake failed, host is not allowed to connect
    RMT_ERROR_WEBSOCKET_HANDSHAKE_NO_KEY,       // WebSocket server handshake failed, can't locate WebSocket key
    RMT_ERROR_WEBSOCKET_HANDSHAKE_BAD_KEY,      // WebSocket server handshake failed, WebSocket key is ill-formed
    RMT_ERROR_WEBSOCKET_HANDSHAKE_STRING_FAIL,  // WebSocket server handshake failed, internal error, bad string code
    RMT_ERROR_WEBSOCKET_DISCONNECTED,           // WebSocket server received a disconnect request and closed the socket
    RMT_ERROR_WEBSOCKET_BAD_FRAME_HEADER,       // Couldn't parse WebSocket frame header
    RMT_ERROR_WEBSOCKET_BAD_FRAME_HEADER_SIZE,  // Partially received wide frame header size
    RMT_ERROR_WEBSOCKET_BAD_FRAME_HEADER_MASK,  // Partially received frame header data mask
    RMT_ERROR_WEBSOCKET_RECEIVE_TIMEOUT,        // Timeout receiving frame header

    RMT_ERROR_REMOTERY_NOT_CREATED,             // Remotery object has not been created
    RMT_ERROR_SEND_ON_INCOMPLETE_PROFILE,       // An attempt was made to send an incomplete profile tree to the client

    RMT_ERROR_ERROR_NOT_READY,                  // This indicates that a resource handle passed to the API call was not valid

} rmtError;
// clang-format on

#ifdef __cplusplus
extern "C" {
#endif

    // Gets the last error message issued on the calling thread
    RMT_API rmtPStr rmt_GetLastErrorMessage();

#ifdef __cplusplus
}
#endif




/*--------------------------------------------------------------------------------------------------------------------------------
   Runtime Settings
--------------------------------------------------------------------------------------------------------------------------------*/


// Callback function pointer types
typedef void* (*rmtMallocPtr)(void* mm_context, uint32_t size);
typedef void* (*rmtReallocPtr)(void* mm_context, void* ptr, uint32_t size);
typedef void (*rmtFreePtr)(void* mm_context, void* ptr);
typedef void (*rmtInputHandlerPtr)(const char* text, void* context);
typedef void (*rmtSampleTreeHandlerPtr)(void* cbk_context, rmtSampleTree* sample_tree);
typedef void (*rmtPropertyHandlerPtr)(void* cbk_context, struct rmtProperty* root);

// Struture to fill in to modify Remotery default settings
typedef struct rmtSettings
{
    // Which port to listen for incoming connections on
    uint16_t port;

    // When this server exits it can leave the port open in TIME_WAIT state for a while. This forces
    // subsequent server bind attempts to fail when restarting. If you find restarts fail repeatedly
    // with bind attempts, set this to true to forcibly reuse the open port.
    rmtBool reuse_open_port;

    // Only allow connections on localhost?
    // For dev builds you may want to access your game from other devices but if
    // you distribute a game to your players with Remotery active, probably best
    // to limit connections to localhost.
    rmtBool limit_connections_to_localhost;

    // Whether to enable runtime thread sampling that discovers which processors a thread is running
    // on. This will suspend and resume threads from outside repeatdly and inject code into each
    // thread that automatically instruments the processor.
    // Default: Enabled
    rmtBool enableThreadSampler;

    // How long to sleep between server updates, hopefully trying to give
    // a little CPU back to other threads.
    uint32_t msSleepBetweenServerUpdates;

    // Size of the internal message queues Remotery uses
    // Will be rounded to page granularity of 64k
    uint32_t messageQueueSizeInBytes;

    // If the user continuously pushes to the message queue, the server network
    // code won't get a chance to update unless there's an upper-limit on how
    // many messages can be consumed per loop.
    uint32_t maxNbMessagesPerUpdate;

    // Callback pointers for memory allocation
    rmtMallocPtr malloc;
    rmtReallocPtr realloc;
    rmtFreePtr free;
    void* mm_context;

    // Callback pointer for receiving input from the Remotery console
    rmtInputHandlerPtr input_handler;

    // Callback pointer for traversing the sample tree graph
    rmtSampleTreeHandlerPtr sampletree_handler;
    void* sampletree_context;

    // Callback pointer for traversing the prpperty graph
    rmtPropertyHandlerPtr snapshot_callback;
    void* snapshot_context;

    // Context pointer that gets sent to Remotery console callback function
    void* input_handler_context;

    rmtPStr logPath;
} rmtSettings;

// Retrieve and configure the global rmtSettings object; returns `rmtSettings*`.
// This can be done before or after Remotery is initialised, however some fields are only referenced on initialisation.
#define rmt_Settings()                                                              \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_Settings(), NULL )


/*--------------------------------------------------------------------------------------------------------------------------------
   Initialisation/Shutdown
--------------------------------------------------------------------------------------------------------------------------------*/


// Can call remotery functions on a null pointer
// TODO: Can embed extern "C" in these macros?

// Initialises Remotery and sets its internal global instance pointer.
// Parameter is `Remotery**`, returning you the pointer for further use.
#define rmt_CreateGlobalInstance(rmt)                                               \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_CreateGlobalInstance(rmt), RMT_ERROR_NONE)

// Shutsdown Remotery, requiring its pointer to be passed to ensure you are destroying the correct instance.
#define rmt_DestroyGlobalInstance(rmt)                                              \
    RMT_OPTIONAL(RMT_ENABLED, _rmt_DestroyGlobalInstance(rmt))

// For use in the presence of DLLs/SOs if each of them are linking Remotery statically.
// If Remotery is hosted in its own DLL and linked dynamically then there is no need to use this.
// Otherwise, pass the result of `rmt_CreateGlobalInstance` from your main DLL to this in your other DLLs.
#define rmt_SetGlobalInstance(rmt)                                                  \
    RMT_OPTIONAL(RMT_ENABLED, _rmt_SetGlobalInstance(rmt))

// Get a pointer to the current global Remotery instance.
#define rmt_GetGlobalInstance()                                                     \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_GetGlobalInstance(), NULL)


/*--------------------------------------------------------------------------------------------------------------------------------
   CPU Sampling
--------------------------------------------------------------------------------------------------------------------------------*/


#define rmt_SetCurrentThreadName(rmt)                                               \
    RMT_OPTIONAL(RMT_ENABLED, _rmt_SetCurrentThreadName(rmt))

#define rmt_LogText(text)                                                           \
    RMT_OPTIONAL(RMT_ENABLED, _rmt_LogText(text))

#define rmt_BeginCPUSample(name, flags)                                             \
    RMT_OPTIONAL(RMT_ENABLED, {                                                     \
        static uint32_t rmt_sample_hash_##name = 0;                                   \
        _rmt_BeginCPUSample(#name, flags, &rmt_sample_hash_##name);                 \
    })

#define rmt_BeginCPUSampleDynamic(namestr, flags)                                   \
    RMT_OPTIONAL(RMT_ENABLED, _rmt_BeginCPUSample(namestr, flags, NULL))

#define rmt_EndCPUSample()                                                          \
    RMT_OPTIONAL(RMT_ENABLED, _rmt_EndCPUSample())

#define rmt_MarkFrame()                                                             \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_MarkFrame(), RMT_ERROR_NONE)


/*--------------------------------------------------------------------------------------------------------------------------------
   Runtime Properties
--------------------------------------------------------------------------------------------------------------------------------*/


/* --- Public API --------------------------------------------------------------------------------------------------------------*/


// Flags that control property behaviour
typedef enum
{
    RMT_PropertyFlags_NoFlags = 0,

    // Reset property back to its default value on each new frame
    RMT_PropertyFlags_FrameReset = 1,
} rmtPropertyFlags;

// All possible property types that can be recorded and sent to the viewer
typedef enum
{
    RMT_PropertyType_rmtGroup,
    RMT_PropertyType_rmtBool,
    RMT_PropertyType_rmtS32,
    RMT_PropertyType_rmtU32,
    RMT_PropertyType_rmtF32,
    RMT_PropertyType_rmtS64,
    RMT_PropertyType_rmtU64,
    RMT_PropertyType_rmtF64,
} rmtPropertyType;

// A property value as a union of all its possible types
typedef union rmtPropertyValue
{
    // C++ requires function-based construction of property values because it has no designated initialiser support until C++20
    #ifdef __cplusplus
        // These are static Make calls, rather than overloaded constructors, because `rmtBool` is the same type as `uint32_t`
        static rmtPropertyValue MakeBool(rmtBool v) { rmtPropertyValue pv; pv.Bool = v; return pv; }
        static rmtPropertyValue MakeS32(int32_t v) { rmtPropertyValue pv; pv.S32 = v; return pv; }
        static rmtPropertyValue MakeU32(uint32_t v) { rmtPropertyValue pv; pv.U32 = v; return pv; }
        static rmtPropertyValue MakeF32(float v) { rmtPropertyValue pv; pv.F32 = v; return pv; }
        static rmtPropertyValue MakeS64(int64_t v) { rmtPropertyValue pv; pv.S64 = v; return pv; }
        static rmtPropertyValue MakeU64(uint64_t v) { rmtPropertyValue pv; pv.U64 = v; return pv; }
        static rmtPropertyValue MakeF64(double v) { rmtPropertyValue pv; pv.F64 = v; return pv; }
    #endif

    rmtBool Bool;
    int32_t S32;
    uint32_t U32;
    float F32;
    int64_t S64;
    uint64_t U64;
    double F64;
} rmtPropertyValue;

// Definition of a property that should be stored globally
// Note:
//  Use the callback api and the rmt_PropertyGetxxx accessors to traverse this structure
typedef struct rmtProperty
{
    // Gets set to RMT_TRUE after a property has been modified, when it gets initialised for the first time
    rmtBool initialised;

    // Runtime description
    rmtPropertyType type;
    rmtPropertyFlags flags;

    // Current value
    rmtPropertyValue value;

    // Last frame value to see if previous value needs to be updated
    rmtPropertyValue lastFrameValue;
    
    // Previous value only if it's different from the current value, and when it changed
    rmtPropertyValue prevValue;
    uint32_t prevValueFrame;

    // Text description
    const char* name;
    const char* description;

    // Default value for Reset calls
    rmtPropertyValue defaultValue;

    // Parent link specifically placed after default value so that variadic macro can initialise it
    struct rmtProperty* parent;

    // Links within the property tree
    struct rmtProperty* firstChild;
    struct rmtProperty* lastChild;
    struct rmtProperty* nextSibling;

    // Hash for efficient sending of properties to the viewer
    uint32_t nameHash;

    // Unique, persistent ID among all properties
    uint32_t uniqueID;
} rmtProperty;

// Define properties of different types at global scope:
//
//    * Never define properties in a header file that gets included multiple times.
//    * The property gets defined exactly as `name` in the global scope.
//    * `flag` is specified without the `RMT_PropertyFlags_` prefix.
//    * Property parents are optional and can be specified as the last parameter, referencing `&name`.
//
#define rmt_PropertyDefine_Group(name, desc, ...) _rmt_PropertyDefine(rmtGroup, name, _rmt_MakePropertyValue(Bool, 0), RMT_PropertyFlags_NoFlags, desc, __VA_ARGS__)
#define rmt_PropertyDefine_Bool(name, default_value, flag, desc, ...) _rmt_PropertyDefine(rmtBool, name, _rmt_MakePropertyValue(Bool, default_value), RMT_PropertyFlags_##flag, desc, __VA_ARGS__)
#define rmt_PropertyDefine_S32(name, default_value, flag, desc, ...) _rmt_PropertyDefine(int32_t, name, _rmt_MakePropertyValue(S32, default_value), RMT_PropertyFlags_##flag, desc, __VA_ARGS__)
#define rmt_PropertyDefine_U32(name, default_value, flag, desc, ...) _rmt_PropertyDefine(uint32_t, name, _rmt_MakePropertyValue(U32, default_value), RMT_PropertyFlags_##flag, desc, __VA_ARGS__)
#define rmt_PropertyDefine_F32(name, default_value, flag, desc, ...) _rmt_PropertyDefine(float, name, _rmt_MakePropertyValue(F32, default_value), RMT_PropertyFlags_##flag, desc, __VA_ARGS__)
#define rmt_PropertyDefine_S64(name, default_value, flag, desc, ...) _rmt_PropertyDefine(int64_t, name, _rmt_MakePropertyValue(S64, default_value), RMT_PropertyFlags_##flag, desc, __VA_ARGS__)
#define rmt_PropertyDefine_U64(name, default_value, flag, desc, ...) _rmt_PropertyDefine(uint64_t, name, _rmt_MakePropertyValue(U64, default_value), RMT_PropertyFlags_##flag, desc, __VA_ARGS__)
#define rmt_PropertyDefine_F64(name, default_value, flag, desc, ...) _rmt_PropertyDefine(double, name, _rmt_MakePropertyValue(F64, default_value), RMT_PropertyFlags_##flag, desc, __VA_ARGS__)

// As properties need to be defined at global scope outside header files, use this to declare properties in header files to be
// modified in other translation units.
//
// If you don't want to include Remotery.h in your shared header you can forward declare the `rmtProperty` type and then forward
// declare the property name yourself.
#define rmt_PropertyExtern(name) extern rmtProperty name;

// Set properties to the given value
#define rmt_PropertySet_Bool(name, set_value) _rmt_PropertySet(Bool, name, set_value)
#define rmt_PropertySet_S32(name, set_value) _rmt_PropertySet(S32, name, set_value)
#define rmt_PropertySet_U32(name, set_value) _rmt_PropertySet(U32, name, set_value)
#define rmt_PropertySet_F32(name, set_value) _rmt_PropertySet(F32, name, set_value)
#define rmt_PropertySet_S64(name, set_value) _rmt_PropertySet(S64, name, set_value)
#define rmt_PropertySet_U64(name, set_value) _rmt_PropertySet(U64, name, set_value)
#define rmt_PropertySet_F64(name, set_value) _rmt_PropertySet(F64, name, set_value)

// Add the given value to properties
#define rmt_PropertyAdd_S32(name, add_value) _rmt_PropertyAdd(S32, name, add_value)
#define rmt_PropertyAdd_U32(name, add_value) _rmt_PropertyAdd(U32, name, add_value)
#define rmt_PropertyAdd_F32(name, add_value) _rmt_PropertyAdd(F32, name, add_value)
#define rmt_PropertyAdd_S64(name, add_value) _rmt_PropertyAdd(S64, name, add_value)
#define rmt_PropertyAdd_U64(name, add_value) _rmt_PropertyAdd(U64, name, add_value)
#define rmt_PropertyAdd_F64(name, add_value) _rmt_PropertyAdd(F64, name, add_value)

// Reset properties to their default value
#define rmt_PropertyReset(name) \
    { \
        name.value = name.defaultValue; \
        _rmt_PropertySetValue(&name); \
    }

// Send all properties and their values to the viewer and log to file
#define rmt_PropertySnapshotAll() _rmt_PropertySnapshotAll()

// Reset all RMT_PropertyFlags_FrameReset properties to their default value
#define rmt_PropertyFrameResetAll() _rmt_PropertyFrameResetAll()

/* --- Private Details ---------------------------------------------------------------------------------------------------------*/


// Used to define properties from typed macro callers
#define _rmt_PropertyDefine(type, name, default_value, flags, desc, ...) \
    rmtProperty name = { RMT_FALSE, RMT_PropertyType_##type, flags, default_value, default_value, default_value, 0, #name, desc, default_value, __VA_ARGS__ };

// C++ doesn't support designated initialisers until C++20
// Worth checking for C++ designated initialisers to remove the function call in debug builds
#ifdef __cplusplus
#define _rmt_MakePropertyValue(field, value) rmtPropertyValue::Make##field(value)
#else
#define _rmt_MakePropertyValue(field, value) { .field = value }
#endif

// Used to set properties from typed macro callers
#define _rmt_PropertySet(field, name, set_value) \
    { \
        name.value.field = set_value; \
        _rmt_PropertySetValue(&name); \
    }

// Used to add properties from typed macro callers
#define _rmt_PropertyAdd(field, name, add_value) \
    { \
        name.value.field += add_value; \
        rmtPropertyValue delta_value = _rmt_MakePropertyValue(field, add_value); \
        _rmt_PropertyAddValue(&name, delta_value); \
    }


#ifdef __cplusplus
extern "C" {
#endif

RMT_API void _rmt_PropertySetValue(rmtProperty* property);
RMT_API void _rmt_PropertyAddValue(rmtProperty* property, rmtPropertyValue add_value);
RMT_API rmtError _rmt_PropertySnapshotAll();
RMT_API void _rmt_PropertyFrameResetAll();
RMT_API uint32_t _rmt_HashString32(const char* s, int len, uint32_t seed);

#ifdef __cplusplus
}
#endif


/*--------------------------------------------------------------------------------------------------------------------------------
   Sample Tree API for walking `rmtSampleTree` Objects in the Sample Tree Handler.
--------------------------------------------------------------------------------------------------------------------------------*/


typedef enum rmtSampleFlags
{
    // Default behaviour
    RMTSF_None = 0,

    // Search parent for same-named samples and merge timing instead of adding a new sample
    RMTSF_Aggregate = 1,

    // Merge sample with parent if it's the same sample
    RMTSF_Recursive = 2,

    // Set this flag on any of your root samples so that Remotery will assert if it ends up *not* being the root sample.
    // This will quickly allow you to detect Begin/End mismatches causing a sample tree imbalance.
    RMTSF_Root = 4,

    // Mainly for platforms other than Windows that don't support the thread sampler and can't detect stalling samples.
    // Where you have a non-root sample that stays open indefinitely and never sends its contents to log/viewer.
    // Send this sample to log/viewer when it closes.
    // You can not have more than one sample open with this flag on the same thread at a time.
    // This flag will be removed in a future version when all platforms support stalling samples.
    RMTSF_SendOnClose = 8,
} rmtSampleFlags;

// Struct to hold iterator info
typedef struct rmtSampleIterator
{
// public
    rmtSample* sample;
// private
    rmtSample* initial;
} rmtSampleIterator;

#define rmt_IterateChildren(iter, sample)                                           \
    RMT_OPTIONAL(RMT_ENABLED, _rmt_IterateChildren(iter, sample))

#define rmt_IterateNext(iter)                                                       \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_IterateNext(iter), RMT_FALSE)

#define rmt_SampleTreeGetThreadName(sample_tree)                                    \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_SampleTreeGetThreadName(sample_tree), NULL)

#define rmt_SampleTreeGetRootSample(sample_tree)                                    \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_SampleTreeGetRootSample(sample_tree), NULL)

// Should only called from within the sample tree callback,
// when the internal string lookup table is valid (i.e. on the main Remotery thread)
#define rmt_SampleGetName(sample)                                                   \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_SampleGetName(sample), NULL)

#define rmt_SampleGetNameHash(sample)                                               \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_SampleGetNameHash(sample), 0U)

#define rmt_SampleGetCallCount(sample)                                              \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_SampleGetCallCount(sample), 0U)

#define rmt_SampleGetStart(sample)                                                  \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_SampleGetStart(sample), 0LLU)

#define rmt_SampleGetTime(sample)                                                   \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_SampleGetTime(sample), 0LLU)

#define rmt_SampleGetSelfTime(sample)                                               \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_SampleGetSelfTime(sample), 0LLU)

#define rmt_SampleGetColour(sample, r, g, b)                                        \
    RMT_OPTIONAL(RMT_ENABLED, _rmt_SampleGetColour(sample, r, g, b))

#define rmt_SampleGetType(sample)                                                   \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_SampleGetType(sample), RMT_SampleType_Count)


// Struct to hold iterator info
typedef struct rmtPropertyIterator
{
// public
    rmtProperty* property;
// private
    rmtProperty* initial;
} rmtPropertyIterator;

#define rmt_PropertyIterateChildren(iter, property)                                     \
    RMT_OPTIONAL(RMT_ENABLED, _rmt_PropertyIterateChildren(iter, property))

#define rmt_PropertyIterateNext(iter)                                                   \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_PropertyIterateNext(iter), RMT_FALSE)

// Should only called from within the property callback,
// when the internal string lookup table is valid (i.e. on the main Remotery thread)

#define rmt_PropertyGetType(property)                                                   \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_PropertyGetType(property), RMT_PropertyType_Count)

#define rmt_PropertyGetName(property)                                                   \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_PropertyGetName(property), NULL)

#define rmt_PropertyGetDescription(property)                                            \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_PropertyGetDescription(property), 0U)

#define rmt_PropertyGetValue(property)                                                  \
    RMT_OPTIONAL_RET(RMT_ENABLED, _rmt_PropertyGetValue(property), 0U)



/*--------------------------------------------------------------------------------------------------------------------------------
   C++ Public Interface Extensions
--------------------------------------------------------------------------------------------------------------------------------*/


#ifdef __cplusplus


#if RMT_ENABLED

// Types that end samples in their destructors
extern "C" RMT_API void _rmt_EndCPUSample(void);
struct rmt_EndCPUSampleOnScopeExit
{
    ~rmt_EndCPUSampleOnScopeExit()
    {
        _rmt_EndCPUSample();
    }
};






#endif


// Pairs a call to rmt_Begin<TYPE>Sample with its call to rmt_End<TYPE>Sample when leaving scope
#define rmt_ScopedCPUSample(name, flags)                                                                \
        RMT_OPTIONAL(RMT_ENABLED, rmt_BeginCPUSample(name, flags));                                     \
        RMT_OPTIONAL(RMT_ENABLED, rmt_EndCPUSampleOnScopeExit rmt_ScopedCPUSample##name);

#endif


/*--------------------------------------------------------------------------------------------------------------------------------
   Private Interface - don't directly call these
--------------------------------------------------------------------------------------------------------------------------------*/


#if RMT_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

RMT_API rmtSettings* _rmt_Settings( void );
RMT_API enum rmtError _rmt_CreateGlobalInstance(Remotery** remotery);
RMT_API void _rmt_DestroyGlobalInstance(Remotery* remotery);
RMT_API void _rmt_SetGlobalInstance(Remotery* remotery);
RMT_API Remotery* _rmt_GetGlobalInstance(void);
RMT_API void _rmt_SetCurrentThreadName(rmtPStr thread_name);
RMT_API void _rmt_LogText(rmtPStr text);
RMT_API void _rmt_BeginCPUSample(rmtPStr name, uint32_t flags, uint32_t* hash_cache);
RMT_API void _rmt_EndCPUSample(void);
RMT_API rmtError _rmt_MarkFrame(void);







// Sample iterator
RMT_API void                _rmt_IterateChildren(rmtSampleIterator* iter, rmtSample* sample);
RMT_API rmtBool             _rmt_IterateNext(rmtSampleIterator* iter);

// SampleTree accessors
RMT_API const char*         _rmt_SampleTreeGetThreadName(rmtSampleTree* sample_tree);
RMT_API rmtSample*          _rmt_SampleTreeGetRootSample(rmtSampleTree* sample_tree);

// Sample accessors
RMT_API const char*         _rmt_SampleGetName(rmtSample* sample);
RMT_API uint32_t              _rmt_SampleGetNameHash(rmtSample* sample);
RMT_API uint32_t              _rmt_SampleGetCallCount(rmtSample* sample);
RMT_API uint64_t              _rmt_SampleGetStart(rmtSample* sample);
RMT_API uint64_t              _rmt_SampleGetTime(rmtSample* sample);
RMT_API uint64_t              _rmt_SampleGetSelfTime(rmtSample* sample);
RMT_API void                _rmt_SampleGetColour(rmtSample* sample, uint8_t* r, uint8_t* g, uint8_t* b);
RMT_API rmtSampleType       _rmt_SampleGetType(rmtSample* sample);

// Property iterator
RMT_API void                _rmt_PropertyIterateChildren(rmtPropertyIterator* iter, rmtProperty* property);
RMT_API rmtBool             _rmt_PropertyIterateNext(rmtPropertyIterator* iter);

// Property accessors
RMT_API rmtPropertyType     _rmt_PropertyGetType(rmtProperty* property);
RMT_API uint32_t              _rmt_PropertyGetNameHash(rmtProperty* property);
RMT_API const char*         _rmt_PropertyGetName(rmtProperty* property);
RMT_API const char*         _rmt_PropertyGetDescription(rmtProperty* property);
RMT_API rmtPropertyValue    _rmt_PropertyGetValue(rmtProperty* property);




#ifdef __cplusplus
}
#endif

#endif  // RMT_ENABLED


#endif
