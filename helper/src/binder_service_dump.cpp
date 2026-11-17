#include "binder_service_dump.h"

#include "service_dump_worker.h"

#include <cstdint>
#include <cstring>
#include <dlfcn.h>

struct AIBinder;

namespace ivrdroid {
namespace {

using CheckServiceFunction = AIBinder* (*)(const char* instance);
using DumpFunction = int32_t (*)(AIBinder* binder, int fd, const char** args, uint32_t numArgs);
using PingFunction = int32_t (*)(AIBinder* binder);
using DecStrongFunction = void (*)(AIBinder* binder);

struct BinderLibrary {
    CheckServiceFunction checkService = nullptr;
    DumpFunction dump = nullptr;
    PingFunction ping = nullptr;
    DecStrongFunction decStrong = nullptr;
};

struct CachedService {
    const char* name;
    AIBinder* binder;
};

BinderLibrary gLibrary;
bool gInitialized = false;
CachedService gServices[] = {
    {"telecom", nullptr},
    {"audio", nullptr},
};

template <typename Function>
bool Resolve(void* handle, const char* symbol, Function* function) {
    void* address = dlsym(handle, symbol);
    if (address == nullptr) return false;
    *function = reinterpret_cast<Function>(address);
    return true;
}

CachedService* FindService(const char* service) {
    if (!IsAllowedDumpService(service)) return nullptr;
    for (auto& cached : gServices) {
        if (std::strcmp(cached.name, service) == 0) return &cached;
    }
    return nullptr;
}

void DropBinder(CachedService* cached) {
    if (cached->binder != nullptr) gLibrary.decStrong(cached->binder);
    cached->binder = nullptr;
}

}  // namespace

bool InitializeBinderServiceDump() {
    if (gInitialized) return true;
    void* handle = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) return false;
    BinderLibrary library;
    if (!Resolve(handle, "AServiceManager_checkService", &library.checkService) ||
        !Resolve(handle, "AIBinder_dump", &library.dump) ||
        !Resolve(handle, "AIBinder_ping", &library.ping) ||
        !Resolve(handle, "AIBinder_decStrong", &library.decStrong)) {
        dlclose(handle);
        return false;
    }
    gLibrary = library;
    gInitialized = true;
    return true;
}

bool BinderServiceDump(const char* service, int fd, void*) {
    if (!gInitialized || fd < 0) return false;
    CachedService* cached = FindService(service);
    if (cached == nullptr) return false;

    if (cached->binder != nullptr && gLibrary.ping(cached->binder) != 0) {
        DropBinder(cached);
    }
    if (cached->binder == nullptr) {
        // checkService returns an owned strong reference or nullptr.
        cached->binder = gLibrary.checkService(cached->name);
    }
    if (cached->binder == nullptr) return false;

    // Never retry the dump itself: a retry could write the dump twice.
    if (gLibrary.dump(cached->binder, fd, nullptr, 0) != 0) {
        DropBinder(cached);
        return false;
    }
    return true;
}

}  // namespace ivrdroid
