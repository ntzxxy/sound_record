add_subdirectory(csrc)

# voice demo 需要 portaudio_static + zmq，我们不使用，跳过
# add_subdirectory(voice)
if(SHERPA_ONNX_ENABLE_PYTHON)
  add_subdirectory(python)
endif()

if(SHERPA_ONNX_ENABLE_JNI)
  add_subdirectory(jni)
endif()

if(SHERPA_ONNX_ENABLE_C_API)
  add_subdirectory(c-api)
endif()
