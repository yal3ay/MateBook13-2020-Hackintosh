#include <mach/kmod.h>
extern "C" {
extern kern_return_t MatebookUVCFix_kern_start(kmod_info_t *, void *);
extern kern_return_t MatebookUVCFix_kern_stop(kmod_info_t *, void *);
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(local.hardware.MatebookUVCFix, "0.3.0", _start, _stop)
__attribute__((visibility("default"))) kmod_start_func_t *_realmain = MatebookUVCFix_kern_start;
__attribute__((visibility("default"))) kmod_stop_func_t *_antimain = MatebookUVCFix_kern_stop;
__attribute__((visibility("default"))) int _kext_apple_cc = __APPLE_CC__;
}
