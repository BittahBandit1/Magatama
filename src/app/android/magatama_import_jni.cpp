// JNI side of the Android launcher (dev.magatama.LauncherActivity): the disc importer (src/app/magatama_disc_import.h)
// in its own small library, libmagatama_import.so, so the launcher never loads the game (libmain.so).
#include "../magatama_disc_import.h"

#include <jni.h>

#include <string>

namespace {
struct Callback
{
    JNIEnv *env;
    jobject listener;
    jmethodID onProgress; // boolean onProgress(long done, long total, String file)
};

int progress(uint64_t done, uint64_t total, const char *file, void *context)
{
    auto *callback = static_cast<Callback *>(context);
    JNIEnv *env = callback->env;
    jstring name = env->NewStringUTF(file ? file : "");
    const jboolean keepGoing = env->CallBooleanMethod(callback->listener, callback->onProgress,
                                                      static_cast<jlong>(done), static_cast<jlong>(total), name);
    env->DeleteLocalRef(name);
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return 0;
    }
    return keepGoing ? 1 : 0;
}

std::string utf8(JNIEnv *env, jstring text)
{
    if (!text)
        return {};
    const char *chars = env->GetStringUTFChars(text, nullptr);
    std::string out = chars ? chars : "";
    if (chars)
        env->ReleaseStringUTFChars(text, chars);
    return out;
}
} // namespace

// Returns "<status> <message>" (status: MagatamaImportStatus).
extern "C" JNIEXPORT jstring JNICALL Java_dev_magatama_DiscImport_nativeImport(JNIEnv *env, jclass, jstring image,
                                                                               jstring discDir, jobject listener)
{
    const std::string imagePath = utf8(env, image), dir = utf8(env, discDir);
    Callback callback{env, listener, nullptr};
    jclass type = env->GetObjectClass(listener);
    callback.onProgress = env->GetMethodID(type, "onProgress", "(JJLjava/lang/String;)Z");
    env->DeleteLocalRef(type);
    char message[512] = {};
    const int status = callback.onProgress
                           ? magatama_disc_import(imagePath.c_str(), dir.c_str(), progress, &callback, message,
                                                  sizeof message)
                           : magatama_disc_import(imagePath.c_str(), dir.c_str(), nullptr, nullptr, message,
                                                  sizeof message);
    return env->NewStringUTF((std::to_string(status) + " " + message).c_str());
}

extern "C" JNIEXPORT jboolean JNICALL Java_dev_magatama_DiscImport_nativeInstalled(JNIEnv *env, jclass, jstring discDir)
{
    return magatama_disc_installed(utf8(env, discDir).c_str()) == 1 ? JNI_TRUE : JNI_FALSE;
}
