{
  "targets": [
    {
      "target_name": "ssh2ng",
      "sources": [
        "src/binding.cc"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "libraries": [
        "<!@(pkg-config --libs libssh)"
      ],
      "cflags": [
        "<!@(pkg-config --cflags libssh)",
        "-fexceptions"
      ],
      "cflags_cc": [
        "<!@(pkg-config --cflags libssh)",
        "-fexceptions",
        "-std=c++17"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS=0",
        "NAPI_VERSION=8"
      ],
      "conditions": [
        ["OS=='mac'", {
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
            "MACOSX_DEPLOYMENT_TARGET": "10.15"
          }
        }]
      ]
    }
  ]
}
