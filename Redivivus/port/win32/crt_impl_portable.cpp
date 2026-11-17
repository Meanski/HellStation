// Non-x86 counterpart of upstream's Redivivus/d2_crt_impl.cpp, which a port
// excludes there because its __ftol2 is x87 inline assembly. The tree only
// declares __ftol2 (an x87-stack register contract with no portable meaning) and
// never calls it, so it is left out; everything else is upstream's behaviour.
#include <windows.h>
#include <cstring>

#if defined(HS_FAKE_I386)
namespace _Wrappers {
uint32_t CRT_StrLen(char* pText) { return pText ? (uint32_t)std::strlen(pText) : 0u; }
void CRT_SecurityCookieStub2(void) {}
}  // namespace _Wrappers

extern "C" void _eh_vector_constructor_iterator_(void* pArray, uint32_t nElementSize, int nCount,
                                                void(__fastcall* pfnConstructor)(void*),
                                                void(__fastcall* pfnDestructor)(void*)) {
    (void)pfnDestructor;
    if (!pArray || !pfnConstructor) return;
    char* p = (char*)pArray;
    for (int i = 0; i < nCount; ++i) pfnConstructor(p + (size_t)i * nElementSize);
}

extern "C" void _eh_vector_destructor_iterator_(void* pArray, uint32_t nElementSize, int nCount,
                                               void(__fastcall* pfnDestructor)(void*)) {
    if (!pArray || !pfnDestructor) return;
    char* p = (char*)pArray;
    for (int i = nCount - 1; i >= 0; --i) pfnDestructor(p + (size_t)i * nElementSize);
}
#endif
