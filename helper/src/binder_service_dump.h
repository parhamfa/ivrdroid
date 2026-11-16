#pragma once

namespace ivrdroid {

// Resolves libbinder_ndk at run time; the helper itself targets android-23 and
// must not link against it. Returns false when any symbol is missing.
bool InitializeBinderServiceDump();

// ServiceDumpFunction for the dump worker. Keeps one binder reference per
// allowed service, so the process opens the binder driver only once.
bool BinderServiceDump(const char* service, int fd, void* context);

}  // namespace ivrdroid
