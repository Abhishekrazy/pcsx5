#include <jni.h>
#include "../src/self_test.h"
extern "C" JNIEXPORT jboolean JNICALL
Java_org_pcsx5_experimental_MainActivity_nativeSelfTest(JNIEnv*,jclass) {
    return pcsx5::frontend::self_test()?JNI_TRUE:JNI_FALSE;
}
