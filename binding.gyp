{
  "targets": [
    {
      "target_name": "ssh2ng",
      "sources": [
        "src/binding.cc"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")",
        "vendor/install/include",
        "vendor/autossh/include"
      ],
      "libraries": [
        "<(module_root_dir)/vendor/autossh/target/release/libautossh.a",
        "<(module_root_dir)/vendor/install/lib/libssh.a",
        "-lssl",
        "-lcrypto",
        "-lz",
        "-lpthread",
        "-ldl",
        "-lm"
      ],
      "cflags": [
        "-fexceptions"
      ],
      "cflags_cc": [
        "-fexceptions",
        "-std=c++17"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS=0",
        "NAPI_VERSION=8"
      ],
      "actions": [
        {
          "action_name": "build_autossh_static",
          "inputs": [ "scripts/build-autossh.sh" ],
          "outputs": [ "vendor/autossh/target/release/libautossh.a" ],
          "action": [ "bash", "scripts/build-autossh.sh" ],
          "message": "Building static autossh from vendor/autossh"
        },
        {
          "action_name": "build_libssh_static",
          "inputs": [ "scripts/build-libssh.sh" ],
          "outputs": [ "vendor/install/lib/libssh.a" ],
          "action": [ "bash", "scripts/build-libssh.sh" ],
          "message": "Building static libssh from vendor/libssh"
        }
      ],
      "conditions": [
        ["OS=='mac'", {
          "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "CLANG_CXX_LANGUAGE_STANDARD": "c++17",
            "MACOSX_DEPLOYMENT_TARGET": "10.15",
            "OTHER_LDFLAGS": [
              "-framework", "Security",
              "-framework", "CoreFoundation"
            ]
          }
        }],
        ["OS!='mac' and OS!='win'", {
          "ldflags": [
            "-Wl,--as-needed"
          ]
        }]
      ]
    }
  ]
}
