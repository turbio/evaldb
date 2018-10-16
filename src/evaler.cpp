#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <libplatform/libplatform.h>
#include <v8.h>

void run() {
  v8::Platform *platform = v8::platform::CreateDefaultPlatform();
  v8::V8::InitializePlatform(platform);
  v8::V8::Initialize();

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  v8::Isolate *isolate = v8::Isolate::New(create_params);

  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);

    auto context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);

    for (;;) {
      std::string code;
      std::cin >> code;
      std::cout << "o" << std::endl;

      auto source = v8::String::NewFromUtf8(
                        isolate, code.c_str(), v8::NewStringType::kNormal)
                        .ToLocalChecked();

      auto script = v8::Script::Compile(context, source).ToLocalChecked();

      auto result = script->Run(context).ToLocalChecked();

      v8::String::Utf8Value utf8(isolate, result);
      printf("%s\n", *utf8);
    }
  }

  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::ShutdownPlatform();
  delete platform;
  delete create_params.array_buffer_allocator;
}
