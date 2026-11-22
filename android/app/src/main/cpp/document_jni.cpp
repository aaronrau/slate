#include "slate_document.h"
#include <jni.h>
#include <string>
#include <vector>

namespace {
SlateDocument* doc(jlong pointer) { return reinterpret_cast<SlateDocument*>(pointer); }

void error(JNIEnv* env, const char* message) {
    env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), message);
}

std::string bytes(JNIEnv* env, jbyteArray data) {
    std::string text(env->GetArrayLength(data), '\0');
    env->GetByteArrayRegion(data, 0, static_cast<jsize>(text.size()),
                            reinterpret_cast<jbyte*>(text.data()));
    return text;
}
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_slate_editor_NativeDocument_create(JNIEnv* env, jclass) {
    auto* result = slate_document_new();
    if (!result) error(env, "Could not allocate document");
    return reinterpret_cast<jlong>(result);
}

extern "C" JNIEXPORT void JNICALL
Java_org_slate_editor_NativeDocument_destroy(JNIEnv*, jclass, jlong handle) {
    slate_document_free(doc(handle));
}

extern "C" JNIEXPORT void JNICALL
Java_org_slate_editor_NativeDocument_openFd(JNIEnv* env, jclass, jlong handle, jint fd) {
    if (!slate_document_open_fd(doc(handle), fd, "Android document")) error(env, slate_document_error(doc(handle)));
}

extern "C" JNIEXPORT void JNICALL
Java_org_slate_editor_NativeDocument_openFile(JNIEnv* env, jclass, jlong handle, jbyteArray path) {
    if (!slate_document_open(doc(handle), bytes(env, path).c_str())) error(env, slate_document_error(doc(handle)));
}

extern "C" JNIEXPORT void JNICALL
Java_org_slate_editor_NativeDocument_saveFile(JNIEnv* env, jclass, jlong handle, jbyteArray path) {
    if (!slate_document_save(doc(handle), bytes(env, path).c_str())) error(env, slate_document_error(doc(handle)));
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_slate_editor_NativeDocument_length(JNIEnv*, jclass, jlong handle) {
    return static_cast<jlong>(slate_document_size(doc(handle)));
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_slate_editor_NativeDocument_align(JNIEnv*, jclass, jlong handle, jlong offset) {
    return static_cast<jlong>(slate_document_boundary(doc(handle), offset));
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_slate_editor_NativeDocument_readBytes(JNIEnv* env, jclass, jlong handle, jlong offset, jint count) {
    std::vector<uint8_t> buffer(static_cast<size_t>(count));
    auto size = slate_document_read(doc(handle), offset, buffer.data(), buffer.size());
    if (size < 0) {
        error(env, slate_document_error(doc(handle)));
        return nullptr;
    }
    auto data = env->NewByteArray(static_cast<jsize>(size));
    if (data) env->SetByteArrayRegion(data, 0, static_cast<jsize>(size),
                                      reinterpret_cast<const jbyte*>(buffer.data()));
    return data;
}

extern "C" JNIEXPORT void JNICALL
Java_org_slate_editor_NativeDocument_edit(JNIEnv* env, jclass, jlong handle, jlong offset,
                                         jlong erased, jbyteArray data) {
    auto text = bytes(env, data);
    if (!slate_document_replace(doc(handle), offset, erased,
                                reinterpret_cast<const uint8_t*>(text.data()), text.size()))
        error(env, slate_document_error(doc(handle)));
}
