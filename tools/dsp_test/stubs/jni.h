#pragma once
#include <cstdint>
typedef int jint; typedef float jfloat; typedef unsigned char jboolean; typedef int jsize;
typedef void* jobject; typedef float* jfloatArray_t; struct _jfa{}; typedef _jfa* jfloatArray;
#define JNI_TRUE 1
#define JNI_FALSE 0
#define JNIEXPORT
#define JNICALL
struct JNIEnv {
  jsize GetArrayLength(jfloatArray) { return 0; }
  void GetFloatArrayRegion(jfloatArray, jsize, jsize, float*) {}
  void SetFloatArrayRegion(jfloatArray, jsize, jsize, const float*) {}
  float* GetFloatArrayElements(jfloatArray, void*) { return nullptr; }
  void ReleaseFloatArrayElements(jfloatArray, float*, int) {}
};
