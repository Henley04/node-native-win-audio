{
  "targets": [
    {
      "target_name": "win_audio",
      "sources": [
        "src/addon.cc",
        "src/util.cc",
        "src/wasapi_engine.cc",
        "src/mme_engine.cc",
        "src/wdm_engine.cc",
        "src/asio_engine.cc",
        "src/audiograph_engine.cc"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "ExceptionHandling": 1,
          "RuntimeTypeInfo": "true",
          "EnablePREfast": "true",
          "WarningLevel": 3,
          "ConformanceMode": "true",
          "LanguageStandard": "stdcpp17",
          "AdditionalOptions": [
            "/permissive-",
            "/Zc:__cplusplus",
            "/wd4996"
          ]
        },
        "VCLinkerTool": {
          "AdditionalDependencies": [
            "ole32.lib",
            "mmdevapi.lib",
            "winmm.lib",
            "setupapi.lib",
            "ksuser.lib",
            "avrt.lib",
            "uuid.lib",
            "mfplat.lib",
            "mfuuid.lib",
            "windowsapp.lib",
            "RuntimeObject.lib"
          ],
          "SubSystem": "1"
        }
      },
      "msvs_disabled_warnings": [
        "4996",
        "4267",
        "4244",
        "4100",
        "4127",
        "4505"
      ],
      "defines": [
        "NAPI_CPP_EXCEPTIONS",
        "_WIN32_WINNT=0x0A00",
        "WINVER=0x0A00",
        "NTDDI_VERSION=0x0A00000C",
        "_WINSOCKAPI_"
      ],
      "conditions": [
        [
          "OS=='win32'",
          {
            "libraries": [
              "-lOle32.lib",
              "-lMmdevapi.lib",
              "-lWinmm.lib",
              "-lSetupapi.lib",
              "-lKsuser.lib",
              "-lAvrt.lib",
              "-lMfplat.lib",
              "-lMfuuid.lib",
              "-lWindowsapp.lib",
              "-lRuntimeObject.lib"
            ]
          }
        ]
      ]
    }
  ]
}
