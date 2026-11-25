#include "client_engine.hpp"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#define LOG_TAG "DeskMateJni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

deskmate::android::ClientEngine* from_handle(jlong handle) {
  return reinterpret_cast<deskmate::android::ClientEngine*>(handle);
}

void append_json_escaped(std::string& out, const std::string& input) {
  out.push_back('"');
  for (char ch : input) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          out.push_back(' ');
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  out.push_back('"');
}

void append_json_str_field(std::string& out, const char* key,
                           const std::string& value, bool first = false) {
  if (!first) {
    out.push_back(',');
  }
  out.push_back('"');
  out += key;
  out += "\":";
  append_json_escaped(out, value);
}

void append_json_u64_field(std::string& out, const char* key,
                           std::uint64_t value) {
  out += ",\"";
  out += key;
  out += "\":";
  out += std::to_string(value);
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_dev_deskmate_client_NativeBridge_create(JNIEnv*, jobject) {
  return reinterpret_cast<jlong>(new deskmate::android::ClientEngine());
}

JNIEXPORT jboolean JNICALL
Java_dev_deskmate_client_NativeBridge_start(JNIEnv* env, jobject, jlong handle,
                                            jint accessory_fd, jobject surface,
                                            jint width, jint height,
                                            jint refresh_hz) {
  auto* engine = from_handle(handle);
  if (engine == nullptr) {
    return JNI_FALSE;
  }
  ANativeWindow* window = nullptr;
  if (surface != nullptr) {
    window = ANativeWindow_fromSurface(env, surface);
  }
  std::string error;
  const bool ok = engine->start(accessory_fd, window, width, height,
                                refresh_hz, error);
  if (!ok) {
    LOGI("start failed: %s", error.c_str());
  }
  // ClientEngine owns the acquired ANativeWindow reference until stop().
  return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_dev_deskmate_client_NativeBridge_requestStop(JNIEnv*, jobject,
                                                  jlong handle) {
  if (auto* engine = from_handle(handle); engine != nullptr) {
    engine->request_stop();
  }
}

JNIEXPORT void JNICALL
Java_dev_deskmate_client_NativeBridge_stop(JNIEnv*, jobject, jlong handle) {
  if (auto* engine = from_handle(handle); engine != nullptr) {
    engine->stop();
  }
}

JNIEXPORT void JNICALL
Java_dev_deskmate_client_NativeBridge_destroy(JNIEnv*, jobject,
                                              jlong handle) {
  delete from_handle(handle);
}

JNIEXPORT void JNICALL
Java_dev_deskmate_client_NativeBridge_setSurface(JNIEnv* env, jobject,
                                                 jlong handle,
                                                 jobject surface) {
  auto* engine = from_handle(handle);
  if (engine == nullptr) {
    return;
  }
  engine->set_surface(surface != nullptr ? ANativeWindow_fromSurface(env, surface)
                                         : nullptr);
}

JNIEXPORT void JNICALL
Java_dev_deskmate_client_NativeBridge_setViewport(JNIEnv*, jobject,
                                                  jlong handle, jint width,
                                                  jint height) {
  if (auto* engine = from_handle(handle); engine != nullptr) {
    engine->set_viewport(width, height);
  }
}

JNIEXPORT void JNICALL
Java_dev_deskmate_client_NativeBridge_setPreferredCodec(JNIEnv* env, jobject,
                                                        jlong handle,
                                                        jstring codec) {
  auto* engine = from_handle(handle);
  if (engine == nullptr || codec == nullptr) {
    return;
  }
  const char* raw = env->GetStringUTFChars(codec, nullptr);
  if (raw != nullptr) {
    engine->set_preferred_codec(raw);
    env->ReleaseStringUTFChars(codec, raw);
  }
}

JNIEXPORT void JNICALL
Java_dev_deskmate_client_NativeBridge_setTouchEnabled(JNIEnv*, jobject,
                                                      jlong handle,
                                                      jboolean enabled) {
  if (auto* engine = from_handle(handle); engine != nullptr) {
    engine->set_touch_enabled(enabled == JNI_TRUE);
  }
}

JNIEXPORT void JNICALL
Java_dev_deskmate_client_NativeBridge_touch(JNIEnv*, jobject, jlong handle,
                                            jint action, jint pointer_id,
                                            jfloat x, jfloat y) {
  auto* engine = from_handle(handle);
  if (engine == nullptr) {
    return;
  }
  switch (action) {
    case 0:
      engine->touch_down(pointer_id, x, y);
      break;
    case 1:
      engine->touch_move(pointer_id, x, y);
      break;
    case 2:
      engine->touch_up(pointer_id, x, y);
      break;
    default:
      engine->touch_cancel();
      break;
  }
}

JNIEXPORT void JNICALL
Java_dev_deskmate_client_NativeBridge_touchFrame(JNIEnv* env, jobject,
                                                 jlong handle, jboolean cancel,
                                                 jint count, jintArray ids,
                                                 jfloatArray xs,
                                                 jfloatArray ys) {
  auto* engine = from_handle(handle);
  if (engine == nullptr) {
    return;
  }
  if (cancel == JNI_TRUE) {
    engine->touch_snapshot({}, true);
    return;
  }
  if (count <= 0) {
    // Releasing the final contact notifies TouchTracker so the pointer-up
    // frame is transmitted instead of silently dropped.
    engine->touch_snapshot({}, false);
    return;
  }
  if (ids == nullptr || xs == nullptr || ys == nullptr ||
      env->GetArrayLength(ids) < count || env->GetArrayLength(xs) < count ||
      env->GetArrayLength(ys) < count) {
    return;
  }

  constexpr jsize kStackCap = 16;
  if (count <= kStackCap) {
    std::array<jint, kStackCap> id_values{};
    std::array<jfloat, kStackCap> x_values{};
    std::array<jfloat, kStackCap> y_values{};
    env->GetIntArrayRegion(ids, 0, count, id_values.data());
    env->GetFloatArrayRegion(xs, 0, count, x_values.data());
    env->GetFloatArrayRegion(ys, 0, count, y_values.data());
    if (env->ExceptionCheck() == JNI_TRUE) {
      return;
    }
    std::array<deskmate::platform::TouchSample, kStackCap> samples{};
    for (jsize index = 0; index < count; ++index) {
      samples[static_cast<std::size_t>(index)] = deskmate::platform::TouchSample{
          id_values[static_cast<std::size_t>(index)],
          x_values[static_cast<std::size_t>(index)],
          y_values[static_cast<std::size_t>(index)],
          true,
      };
    }
    engine->touch_snapshot(
        std::span<const deskmate::platform::TouchSample>(
            samples.data(), static_cast<std::size_t>(count)),
        false);
    return;
  }

  std::vector<jint> id_values(static_cast<std::size_t>(count));
  std::vector<jfloat> x_values(static_cast<std::size_t>(count));
  std::vector<jfloat> y_values(static_cast<std::size_t>(count));
  env->GetIntArrayRegion(ids, 0, count, id_values.data());
  env->GetFloatArrayRegion(xs, 0, count, x_values.data());
  env->GetFloatArrayRegion(ys, 0, count, y_values.data());
  if (env->ExceptionCheck() == JNI_TRUE) {
    return;
  }
  std::vector<deskmate::platform::TouchSample> samples(
      static_cast<std::size_t>(count));
  for (jsize index = 0; index < count; ++index) {
    samples[static_cast<std::size_t>(index)] = deskmate::platform::TouchSample{
        id_values[static_cast<std::size_t>(index)],
        x_values[static_cast<std::size_t>(index)],
        y_values[static_cast<std::size_t>(index)],
        true,
    };
  }
  engine->touch_snapshot(samples, false);
}

JNIEXPORT jstring JNICALL
Java_dev_deskmate_client_NativeBridge_snapshot(JNIEnv* env, jobject,
                                               jlong handle) {
  auto* engine = from_handle(handle);
  if (engine == nullptr) {
    return env->NewStringUTF("{}");
  }
  const auto snapshot = engine->snapshot();
  std::string json;
  json.reserve(512);
  json.push_back('{');
  append_json_str_field(json, "session", snapshot.state, true);
  append_json_str_field(json, "state", snapshot.state);
  append_json_str_field(json, "failure", snapshot.failure);
  append_json_str_field(json, "profile", snapshot.profile);
  append_json_str_field(json, "codec", snapshot.codec);
  append_json_str_field(json, "resolution", snapshot.resolution);
  append_json_str_field(json, "preferredCodec", snapshot.preferred_codec);
  append_json_str_field(json, "surface", snapshot.surface);
  append_json_str_field(json, "error", snapshot.error);
  append_json_u64_field(json, "frames", snapshot.frames);
  append_json_u64_field(json, "framesReceived", snapshot.frames_received);
  append_json_u64_field(json, "framesDropped", snapshot.frames_dropped);
  append_json_u64_field(json, "decoderRendered", snapshot.decoder_rendered);
  append_json_u64_field(json, "decoderSubmitted", snapshot.decoder_submitted);
  append_json_u64_field(json, "decoderDropped", snapshot.decoder_dropped);
  append_json_u64_field(json, "decoderOpens", snapshot.decoder_opens);
  append_json_u64_field(json, "inputSent", snapshot.input_events);
  append_json_u64_field(json, "inputRejected", snapshot.input_rejected);
  append_json_u64_field(json, "input", snapshot.input_events);
  append_json_u64_field(json, "rxBytes", snapshot.rx_bytes);
  append_json_u64_field(json, "rx", snapshot.rx_bytes);
  append_json_u64_field(json, "txBytes", snapshot.tx_bytes);
  append_json_u64_field(json, "tx", snapshot.tx_bytes);
  json.push_back('}');
  return env->NewStringUTF(json.c_str());
}

}  // extern "C"
